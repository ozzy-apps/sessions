// The guitar and bass devices' panels: the Tuner, the Guitar Amp, NAM and the pedals. The
// pedals are devices like the others (knobs, mode buttons, graphs): a drive pedal shows its
// clipper and its tone, the phaser its notches, the wah its rocker. Parameter indices follow
// core/DeviceSpecs.cpp.

#include "Bodies.h"
#include "audio/Devices.h"
#include "audio/Dsp.h"
#include "audio/Nam.h"
#include "audio/Builtin.h"
#include "core/Import.h"
#include "core/Model.h"
#include "core/Sample.h"
#include "core/Scales.h"
#include "ui/Browser.h"
#include "ui/InfoView.h"
#include "widgets/Knob.h"
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QLabel>
#include <cmath>
#include <functional>
#include <deque>

namespace sessions::devui {

namespace {

using T = DeviceType;
constexpr float kSr = 48000;

// The knob cells' name labels (not the value labels: refresh() styles those).
void inkLabels(QWidget* cell, const QColor& ink) {
    const QList<QLabel*> labels = cell->findChildren<QLabel*>();
    if (!labels.isEmpty())
        labels.first()->setStyleSheet(QString("color:%1;font-size:9px;font-weight:600;background:transparent;border:0;").arg(ink.name()));
}

// ---- the IR library row shared by the Guitar Amp, NAM and Cabinet: ◀ ▶ step through the browser's
// Impulse Responses (the built-in cabinets, the Impulse Responses folder and your IR folders; or the IR's
// own folder when it isn't in them), Load… picks a file. Mappable (device:<t>:<d>:irprev / irnext).
QToolButton* smallButton(QWidget* parent, const QString& text, const QString& title, const QString& help, std::function<void()> fn, int pad = 8) {
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setFocusPolicy(Qt::NoFocus);
    b->setStyleSheet(QString("QToolButton{font-size:9px;padding:1px %1px;}").arg(pad));
    InfoView::attach(b, title, help);
    QObject::connect(b, &QToolButton::clicked, parent, std::move(fn));
    return b;
}

void addIrSteps(DeviceBody* body, QHBoxLayout* row) {
    const DeviceContext& c = body->ctx();
    auto step = [body](int dir) {
        const DeviceContext& c = body->ctx();
        QString why;
        bool ok = false;
        { EditScope edit(c.project, dir > 0 ? "Next IR" : "Previous IR"); ok = import::stepIr(*c.project, c.track, c.deviceId, dir, &why); }
        if (!ok) InfoView::notify(dir > 0 ? "Next IR" : "Previous IR", why);
    };
    QToolButton* prev = smallButton(body, "◀", "Previous IR", "Loads the impulse response before this one in your library (the built-in cabinets, the Impulse Responses folder and your IR folders), or in its own folder. Wraps around. One undo step; MIDI-mappable.", [step] { step(-1); }, 4);
    QToolButton* next = smallButton(body, "▶", "Next IR", "Loads the next impulse response in your library (the built-in cabinets, the Impulse Responses folder and your IR folders), or in its own folder. Wraps around. One undo step; MIDI-mappable.", [step] { step(1); }, 4);
    prev->setProperty("mapTarget", QString("device:%1:%2:irprev").arg(c.trackId).arg(c.deviceId));
    next->setProperty("mapTarget", QString("device:%1:%2:irnext").arg(c.trackId).arg(c.deviceId));
    row->addWidget(prev);
    row->addWidget(next);
}

// "3 / 17" for the device's IR (scanned only when the IR changes), or "not in the library".
struct IrPlace {
    QString path, text;
    QString update(const Device* d) {
        if (!d) return {};
        if (d->samplePath != path) {
            path = d->samplePath;
            text.clear();
            if (!path.isEmpty()) {
                const QStringList library = import::irLibrary();
                const int at = import::irIndex(path, library);
                text = at > 0 ? QString("%1 / %2").arg(at).arg(library.size()) : QString("not in the library");
            }
        }
        return text;
    }
};

QString irFileName(const Device* d) {
    if (!d || d->samplePath.isEmpty()) return {};
    return audio::isBuiltin(d->samplePath) ? audio::builtinName(d->samplePath) : QFileInfo(d->samplePath).completeBaseName();
}

// ============================================================================ Tuner

class TunerDisplay : public Display {
public:
    TunerDisplay(DeviceBody* b, int w, int h) : Display(b, w, h) {
        InfoView::attach(this, "Tuner", "The note you play, how far off it is (cents: a hundredth of a semitone) and the last few seconds. "
                                        "Green: in tune. The needle (or the strobe's stripes) leans the way to turn the peg back.");
    }
    void tick() override {
        const float f = m_body->meter(0);
        const double ref = m_body->real(0);
        if (f > 20) {
            const double midi = 69 + 12 * std::log2(f / ref);
            const int note = int(std::lround(midi));
            const double cents = (midi - note) * 100;
            if (note != m_note || m_hold <= 0) m_cents = cents;
            m_note = note;
            m_freq = f;
            m_cents += (cents - m_cents) * 0.35;
            m_hold = 36;
            m_history.push_back(float(m_cents));
        } else {
            if (m_hold > 0) --m_hold;
            m_history.push_back(NAN);
        }
        while (m_history.size() > 160) m_history.pop_front();
        m_strobe += m_cents * 0.01;
        update();
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const bool on = m_hold > 0;
        const bool inTune = on && std::fabs(m_cents) < 3;
        const QColor good("#6fd35c");
        const QColor mark = !on ? col::gridText : inTune ? good : col::curve;
        // the note, big
        QFont big = font(); big.setPointSizeF(30); big.setBold(true); p.setFont(big);
        p.setPen(on ? (inTune ? good : col::value) : col::gridStrong);
        const QRectF noteBox(r.left() + 10, r.top() + 4, 120, 52);
        const QString name = on ? QString(noteName(m_note)) : QString("–");
        p.drawText(noteBox, Qt::AlignLeft | Qt::AlignVCenter, name);
        if (on) {
            const QFontMetricsF fm(big);
            QFont small = font(); small.setPointSizeF(12); small.setBold(true); p.setFont(small);
            p.drawText(QPointF(noteBox.left() + fm.horizontalAdvance(name) + 2, noteBox.bottom() - 10), QString::number(m_note / 12 - 1));
        }
        QFont mid = font(); mid.setPointSizeF(8.5); p.setFont(mid);
        p.setPen(col::label);
        const QString right = on ? QString("%1 Hz\n%2%3 ct").arg(m_freq, 0, 'f', 1).arg(m_cents >= 0 ? "+" : "").arg(m_cents, 0, 'f', 1)
                                 : QString("Play a note\nA = %1 Hz").arg(int(m_body->real(0)));
        p.drawText(QRectF(r.right() - 110, r.top() + 8, 102, 40), Qt::AlignRight | Qt::AlignTop, right);

        // the meter: needle or strobe, -50 .. +50 cents
        const QRectF meter(r.left() + 10, r.top() + 62, r.width() - 20, 50);
        const double cx = meter.center().x(), half = meter.width() / 2;
        p.fillRect(QRectF(cx - half * 3 / 50, meter.top(), half * 6 / 50, meter.height()), QColor(111, 211, 92, 40));
        QFont tiny = font(); tiny.setPointSizeF(6.5); p.setFont(tiny);
        for (int c = -50; c <= 50; c += 10) {
            const double x = cx + c / 50.0 * half;
            p.setPen(QPen(c == 0 ? col::label : col::gridStrong, c == 0 ? 1.5 : 1));
            p.drawLine(QPointF(x, meter.bottom() - (c % 50 == 0 ? 14 : 8)), QPointF(x, meter.bottom()));
            if (c % 50 == 0) { p.setPen(col::gridText); p.drawText(QRectF(x - 14, meter.bottom() - 26, 28, 10), Qt::AlignCenter, c == 0 ? "0" : QString("%1%2").arg(c > 0 ? "+" : "").arg(c)); }
        }
        if (m_body->pick(2) == 1) {   // strobe: stripes drift right when sharp, left when flat, still when in tune
            const double off = std::fmod(m_strobe, 1.0) * 14;
            p.save();
            p.setClipRect(QRectF(meter.left(), meter.top(), meter.width(), 26));
            for (double x = meter.left() - 14 + off; x < meter.right(); x += 14)
                p.fillRect(QRectF(x, meter.top() + 2, 7, 22), on ? (inTune ? good : col::curve) : col::gridStrong);
            p.restore();
        } else if (on) {
            const double x = cx + std::clamp(m_cents, -50.0, 50.0) / 50.0 * half;
            p.setPen(QPen(mark, 2.5, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(x, meter.top() + 2), QPointF(x, meter.bottom()));
            QPainterPath tri;
            tri.moveTo(x - 6, meter.top()); tri.lineTo(x + 6, meter.top()); tri.lineTo(x, meter.top() + 8); tri.closeSubpath();
            p.fillPath(tri, mark);
        }

        // the last few seconds
        const QRectF hist(r.left() + 10, r.bottom() - 46, r.width() - 20, 40);
        p.fillRect(hist, QColor(0, 0, 0, 60));
        p.setPen(QPen(col::gridStrong, 1));
        p.drawLine(QPointF(hist.left(), hist.center().y()), QPointF(hist.right(), hist.center().y()));
        const int n = int(m_history.size());
        for (int i = 0; i < n; ++i) {
            const float c = m_history[size_t(i)];
            if (std::isnan(c)) continue;
            const double x = hist.right() - (n - 1 - i) * hist.width() / 160.0;
            const double y = hist.center().y() - std::clamp(double(c), -50.0, 50.0) / 50.0 * (hist.height() / 2 - 2);
            p.fillRect(QRectF(x - 1, y - 1, 2.5, 2.5), std::fabs(c) < 3 ? good : col::curve);
        }
    }
private:
    int m_note = 0, m_hold = 0;
    double m_freq = 0, m_cents = 0, m_strobe = 0;
    std::deque<float> m_history;
};

class TunerBody : public DeviceBody {
public:
    TunerBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = rootRow(this);
        auto* disp = new TunerDisplay(this, 350, 178);
        animate(disp);
        root->addWidget(disp, 0, Qt::AlignTop);
        root->addWidget(vline(this));
        auto* side = vbox(5);
        side->addWidget(knob(0, 30, "Reference"));
        side->addWidget(choices(2, {}, {}, 92));
        side->addWidget(toggle(1, "Mute", 92));
        side->addStretch(1);
        root->addLayout(side);
        setFixedWidth(480);
    }
};

// ============================================================================ Guitar Amp

struct HeadLook { QColor panel, ink; const char* script; };
const HeadLook kHeadLooks[guitaramp::kHeads] = {
    {QColor("#d8ceb3"), QColor("#2a241a"), "Clean"},
    {QColor("#c9a24a"), QColor("#2a1f08"), "Crunch"},
    {QColor("#1e1e1e"), QColor("#d4af37"), "Lead"},
    {QColor("#30353d"), QColor("#e6e6e6"), "Metal"},
    {QColor("#9ea7b1"), QColor("#1c2026"), "Bass"},
};

// The five heads, each a little faceplate; click one to switch.
class HeadPicker : public QWidget {
public:
    explicit HeadPicker(DeviceBody* body) : QWidget(body), m_body(body) {
        setFixedSize(96, 5 * 30 + 4 * 3);
        setCursor(Qt::PointingHandCursor);
        setProperty("mapTarget", body->target(guitaramp::Head));
        InfoView::attach(this, "Head", "The amp: Clean (lots of headroom), Crunch (the classic British stack), Lead (hotter), Metal (three gain stages, tight), Bass.");
        body->watch(this);
    }
protected:
    QRectF row(int k) const { return QRectF(0.5, 0.5 + k * 33, width() - 1, 30); }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const int cur = m_body->pick(guitaramp::Head);
        for (int k = 0; k < guitaramp::kHeads; ++k) {
            const HeadLook& look = kHeadLooks[k];
            const QRectF r = row(k);
            p.setPen(QPen(k == cur ? col::buttonOn : QColor("#101010"), k == cur ? 2 : 1));
            p.setBrush(QColor("#141414"));
            p.drawRoundedRect(r, 3, 3);
            const QRectF plate = r.adjusted(4, 5, -4, -5);
            p.setPen(Qt::NoPen);
            p.setBrush(look.panel);
            p.drawRoundedRect(plate, 2, 2);
            QFont f = font(); f.setPointSizeF(8); f.setBold(true); f.setItalic(k != 3); f.setLetterSpacing(QFont::AbsoluteSpacing, 1);
            if (k == 3) f.setCapitalization(QFont::AllUppercase);
            p.setFont(f);
            p.setPen(look.ink);
            p.drawText(plate, Qt::AlignCenter, look.script);
            if (k != cur) { p.setBrush(QColor(0, 0, 0, 90)); p.drawRoundedRect(plate, 2, 2); }
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        for (int k = 0; k < guitaramp::kHeads; ++k)
            if (row(k).contains(e->position())) m_body->edit(guitaramp::Head, choicePosition(k, guitaramp::kHeads));
    }
private:
    DeviceBody* m_body;
};

// The amp's control panel: the six knobs on the head's faceplate colour.
class AmpPlate : public QWidget {
public:
    explicit AmpPlate(DeviceBody* body) : QWidget(body), m_body(body) { body->watch(this); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const HeadLook& look = kHeadLooks[std::clamp(m_body->pick(guitaramp::Head), 0, guitaramp::kHeads - 1)];
        p.setPen(QPen(QColor("#0e0e0e"), 1));
        p.setBrush(look.panel);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
        p.setPen(QPen(look.ink, 1));
        p.drawLine(QPointF(8, height() - 13.5), QPointF(width() - 8, height() - 13.5));
        QFont f = font(); f.setPointSizeF(7); f.setBold(true); f.setLetterSpacing(QFont::AbsoluteSpacing, 2);
        f.setCapitalization(QFont::AllUppercase);
        p.setFont(f);
        p.drawText(QRectF(8, height() - 12, width() - 16, 11), Qt::AlignRight | Qt::AlignVCenter, QString("Sessions · %1").arg(look.script));
    }
private:
    DeviceBody* m_body;
};

// The tone stack and the cabinet as one curve, with the input and output levels.
class AmpResponse : public Display {
public:
    AmpResponse(DeviceBody* b, int w, int h) : Display(b, w, h) {
        InfoView::attach(this, "Tone", "What the tone controls and the cabinet do to the sound (not the distortion). The bars: the level going in and coming out.");
    }
    void tick() override {
        m_in = std::max(m_body->meter(0), m_in * 0.85f);
        m_out = std::max(m_body->meter(1), m_out * 0.85f);
        update();
    }
protected:
    void draw(QPainter& p, const QRectF& rr) override {
        const QRectF r = rr.adjusted(0, 0, -16, 0);
        drawFreqGrid(p, r, 0);
        const std::vector<float> pos = m_body->positions();
        auto y = [&](double dbv) { return r.center().y() - std::clamp(dbv, -36.0, 18.0) / 30.0 * r.height() / 2; };
        QPainterPath path;
        for (int x = 0; x <= int(r.width()); x += 2) {
            const double dbv = audio::ampResponseDb(pos.data(), float(xToFreq(r.left() + x, r)), kSr);
            const QPointF pt(r.left() + x, y(dbv));
            if (x == 0) path.moveTo(pt); else path.lineTo(pt);
        }
        QPainterPath fill = path;
        fill.lineTo(r.right(), r.bottom()); fill.lineTo(r.left(), r.bottom()); fill.closeSubpath();
        p.fillPath(fill, col::curveFill);
        p.setPen(QPen(col::curve, 1.5));
        p.drawPath(path);
        const int cab = m_body->pick(guitaramp::Cab);
        if (cab == guitaramp::kLoadedIr) drawCaption(p, r, "+ your IR", Qt::AlignTop | Qt::AlignLeft);
        // in / out bars
        auto bar = [&](double x, float level, const QColor& c) {
            const double h = std::clamp((20.0 * std::log10(std::max(1e-5, double(level))) + 48) / 48, 0.0, 1.0) * (rr.height() - 6);
            p.fillRect(QRectF(x, rr.bottom() - 3 - h, 5, h), c);
        };
        bar(rr.right() - 14, m_in, col::cyan);
        bar(rr.right() - 7, m_out, col::curve);
    }
private:
    float m_in = 0, m_out = 0;
};

// The cabinet: its speakers behind the grille (1, 2, 4 or 8), the loaded IR's name, or nothing.
class CabView : public Display {
public:
    CabView(DeviceBody* b, int w, int h) : Display(b, w, h) {
        setAcceptDrops(true);
        InfoView::attach(this, "Cabinet", "The speaker cabinet the amp plays through, and where the mic is. Drop a WAV here (or use Load IR) to use your own impulse response.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const int cab = m_body->pick(guitaramp::Cab);
        const QRectF box = r.adjusted(8, 6, -8, -6);
        if (cab == 0) {
            p.setPen(col::gridText);
            p.drawText(box, Qt::AlignCenter, "No cabinet\n(straight from the amp)");
            return;
        }
        if (cab == guitaramp::kLoadedIr) {
            const Device* d = m_body->device();
            const SamplePtr s = d ? d->sample : nullptr;
            if (!s) { p.setPen(col::gridText); p.drawText(box, Qt::AlignCenter, "No IR loaded:\ndrop a WAV here"); return; }
            const double dur = s->seconds();
            const double mid = box.center().y() + 6, half = box.height() / 2 - 12;
            for (int x = 0; x < int(box.width()); ++x) {
                float lo, hi;
                s->range(dur * x / box.width(), dur * (x + 1) / box.width(), lo, hi);
                p.fillRect(QRectF(box.left() + x, mid - hi * half, 1, std::max(1.0, double(hi - lo) * half)), col::cyan);
            }
            p.setPen(col::value);
            p.drawText(box, Qt::AlignTop | Qt::AlignLeft, p.fontMetrics().elidedText(irFileName(d), Qt::ElideMiddle, int(box.width())));
            p.setPen(col::gridText);
            p.drawText(box, Qt::AlignBottom | Qt::AlignRight, QString("%1 ms").arg(int(dur * 1000)));
            return;
        }
        // tolex, grille, speakers
        p.setPen(QPen(QColor("#050505"), 1));
        p.setBrush(QColor("#1d1b19"));
        p.drawRoundedRect(box, 4, 4);
        const QRectF grille = box.adjusted(5, 5, -5, -5);
        p.setBrush(cab == 4 ? QColor("#5a4f3c") : QColor("#2b2b2b"));
        p.drawRect(grille);
        p.setPen(QPen(QColor(255, 255, 255, 14), 1));
        for (double x = grille.left() + 2; x < grille.right(); x += 3) p.drawLine(QPointF(x, grille.top()), QPointF(x, grille.bottom()));
        static const int counts[] = {0, 1, 2, 4, 4, 8};
        const int k = counts[std::clamp(cab, 0, 5)];
        const int cols = k >= 4 ? (k == 8 ? 4 : 2) : k, rows = k >= 4 ? 2 : 1;
        const double cw = grille.width() / cols, ch = grille.height() / rows;
        const double rad = std::min(cw, ch) * 0.4;
        for (int i = 0; i < k; ++i) {
            const QPointF c(grille.left() + cw * (i % cols + 0.5), grille.top() + ch * (i / cols + 0.5));
            QRadialGradient g(c, rad);
            g.setColorAt(0, QColor("#3a3a3a"));
            g.setColorAt(0.25, QColor("#151515"));
            g.setColorAt(1, QColor("#0a0a0a"));
            p.setPen(QPen(QColor(0, 0, 0, 160), 1));
            p.setBrush(g);
            p.drawEllipse(c, rad, rad);
        }
        // the mic: in front of the first speaker, on axis, off axis or back in the room
        const int mic = m_body->pick(guitaramp::Mic);
        const QPointF c0(grille.left() + cw * 0.5, grille.top() + ch * 0.5);
        const QPointF m = mic == 0 ? c0 : mic == 1 ? c0 + QPointF(rad * 0.7, -rad * 0.4) : QPointF(box.right() - 10, box.bottom() - 10);
        p.setPen(Qt::NoPen);
        p.setBrush(col::curve);
        p.drawEllipse(m, 3.5, 3.5);
    }
    void dragEnterEvent(QDragEnterEvent* e) override { if (!Browser::audioFilesIn(e->mimeData()).isEmpty()) e->acceptProposedAction(); else e->ignore(); }
    void dropEvent(QDropEvent* e) override;
};

class GuitarAmpBody : public DeviceBody {
public:
    GuitarAmpBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = rootRow(this);
        auto* heads = vbox(3);
        heads->addWidget(section("Head"));
        heads->addWidget(new HeadPicker(this));
        heads->addStretch(1);
        root->addLayout(heads);
        root->addWidget(vline(this));

        auto* mid = vbox(4);
        auto* plate = new AmpPlate(this);
        plate->setFixedSize(312, 70);
        auto* pl = new QHBoxLayout(plate);
        pl->setContentsMargins(6, 3, 6, 14);
        pl->setSpacing(0);
        for (int i : {guitaramp::Gain, guitaramp::Bass, guitaramp::Mid, guitaramp::Treble, guitaramp::Presence, guitaramp::Volume}) {
            QWidget* cell = knob(i, 28);
            cell->setParent(plate);
            pl->addWidget(cell);
            m_cells.push_back(cell);
        }
        mid->addWidget(plate);
        auto* resp = new AmpResponse(this, 312, 100);
        animate(resp);
        mid->addWidget(resp);
        mid->addStretch(1);
        root->addLayout(mid);
        root->addWidget(vline(this));

        auto* cab = vbox(3);
        cab->addWidget(section("Cabinet"));
        cab->addWidget(new CabView(this, 172, 70));
        cab->addWidget(chooser(guitaramp::Cab, 172));
        cab->addWidget(choices(guitaramp::Mic, {"On Axis", "Off Axis", "Far"}, {}, 172));
        auto* irRow = hbox(4);
        addIrSteps(this, irRow);
        irRow->addWidget(smallButton(this, "Load IR…", "Load IR", "Pick a cabinet impulse response (a WAV file). The cabinet switches to Loaded IR. Dropping a WAV or a browser IR on the cabinet does the same.", [this] {
            const QString path = QFileDialog::getOpenFileName(this, "Load Impulse Response", QString(),
                                                              "Audio files (" + samples::nameFilters().join(' ') + ")");
            if (!path.isEmpty()) loadIr(path);
        }));
        m_place = new QLabel(this);
        m_place->setStyleSheet(QString("color:%1;font-size:9px;background:transparent;border:0;").arg(col::label.name()));
        InfoView::attach(m_place, "Library Position", "Which impulse response this is of the ones the browser lists under Impulse Responses; ◀ ▶ step through them.");
        irRow->addWidget(m_place, 1);
        cab->addLayout(irRow);
        cab->addStretch(1);
        root->addLayout(cab);
        root->addWidget(vline(this));

        auto* out = vbox(2);
        out->addWidget(knob(guitaramp::Output, 26));
        out->addWidget(knob(guitaramp::DryWet, 24));
        out->addStretch(1);
        root->addLayout(out);
        setFixedWidth(712);
        setAcceptDrops(true);   // a WAV dropped anywhere on the amp is its cabinet IR
        refresh();
    }
    void refresh() override {
        DeviceBody::refresh();
        const HeadLook& look = kHeadLooks[std::clamp(pick(guitaramp::Head), 0, guitaramp::kHeads - 1)];
        for (QWidget* cell : m_cells) inkLabels(cell, look.ink);
        if (m_place) m_place->setText(m_irPlace.update(device()));
    }
    // A cabinet impulse response from a file (or a built-in one): loaded, and the cabinet switched to it (one undo step).
    void loadIr(const QString& path) {
        const DeviceContext& c = ctx();
        QString why;
        bool ok = false;
        { EditScope edit(c.project, "Load IR"); ok = import::loadIr(*c.project, c.track, c.deviceId, path, &why); }
        if (!ok) QMessageBox::warning(this, "Load IR", why);
    }
protected:
    void dragEnterEvent(QDragEnterEvent* e) override {
        if (!Browser::irFilesIn(e->mimeData()).isEmpty() || !Browser::audioFilesIn(e->mimeData()).isEmpty()) e->acceptProposedAction(); else e->ignore();
    }
    void dropEvent(QDropEvent* e) override {
        const QStringList files = Browser::irFilesIn(e->mimeData()) + Browser::audioFilesIn(e->mimeData());
        if (files.isEmpty()) { e->ignore(); return; }
        e->acceptProposedAction();
        loadIr(files.first());
    }
private:
    std::vector<QWidget*> m_cells;
    QLabel* m_place = nullptr;
    IrPlace m_irPlace;
};

void CabView::dropEvent(QDropEvent* e) {
    const QStringList files = Browser::irFilesIn(e->mimeData()) + Browser::audioFilesIn(e->mimeData());
    if (files.isEmpty()) { e->ignore(); return; }
    e->acceptProposedAction();
    if (auto* amp = dynamic_cast<GuitarAmpBody*>(m_body)) amp->loadIr(files.first());
}

// ============================================================================ NAM (Neural Amp Modeler)

class NamBody;

// The capture: its name, network, gear and loudness, or what's wrong with it. Drop a .nam here.
class CaptureView : public Display {
public:
    CaptureView(DeviceBody* b, int w, int h) : Display(b, w, h) {
        setAcceptDrops(true);
        InfoView::attach(this, "Capture", "The loaded Neural Amp Modeler capture: its name, its network (architecture, channels, layers), the gear it was made from and its loudness. "
                                          "Drop a .nam file here (or press Load…) to load one. A missing file shows in red and the device plays dry until you load another.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const Device* d = m_body->device();
        if (!d) return;
        const QRectF box = r.adjusted(8, 5, -8, -5);
        QFont f = p.font();
        if (d->modelPath.isEmpty()) {
            p.setPen(col::gridText);
            p.drawText(box, Qt::AlignCenter, "No capture loaded\nDrop a .nam file here, or press Load…");
            return;
        }
        if (!d->model) {
            f.setPointSizeF(7.5); f.setBold(true); p.setFont(f);
            p.setPen(theme::devMissing);
            p.drawText(box, Qt::AlignTop | Qt::AlignLeft, p.fontMetrics().elidedText("Missing: " + QFileInfo(d->modelPath).fileName(), Qt::ElideMiddle, int(box.width())));
            f.setPointSizeF(6.5); f.setBold(false); p.setFont(f);
            p.setPen(col::gridText);
            p.drawText(box.adjusted(0, 18, 0, 0), Qt::AlignTop | Qt::AlignLeft | Qt::TextWordWrap,
                       "The file isn't there, or isn't a capture this version reads. The sound passes through dry: put the file back, or load another.");
            return;
        }
        const nam::Model& m = *d->model;
        double y = box.top();
        auto line = [&](const QString& text, const QColor& c, double size, bool bold) {
            f.setPointSizeF(size); f.setBold(bold); p.setFont(f); p.setPen(c);
            const double h = p.fontMetrics().height();
            p.drawText(QRectF(box.left(), y, box.width(), h), Qt::AlignLeft | Qt::AlignVCenter, p.fontMetrics().elidedText(text, Qt::ElideRight, int(box.width())));
            y += h + 1;
        };
        line(m.name, col::value, 8, true);
        line(m.summary, col::cyan, 6.5, false);
        if (!m.gear.isEmpty()) line(m.gear, col::label, 6.5, false);
        if (!m.modeledBy.isEmpty()) line("by " + m.modeledBy, col::gridText, 6.5, false);
        QStringList facts;
        facts << (m.hasLoudness ? QString("Loudness %1 dB").arg(m.loudness, 0, 'f', 1) : QString("No loudness data"));
        facts << (m.rateKnown ? QString("%1 kHz%2").arg(m.sampleRate / 1000.0, 0, 'g', 4).arg(std::fabs(m.sampleRate - kSr) > 0.5 ? " (resampled)" : "") : QString("48 kHz assumed"));
        facts << QString("%1 weights").arg(m.weightCount);
        f.setPointSizeF(6.5); f.setBold(false); p.setFont(f);
        p.setPen(col::gridText);
        p.drawText(box, Qt::AlignBottom | Qt::AlignLeft, p.fontMetrics().elidedText(facts.join(" · "), Qt::ElideRight, int(box.width())));
    }
    void dragEnterEvent(QDragEnterEvent* e) override { if (!Browser::modelFilesIn(e->mimeData()).isEmpty()) e->acceptProposedAction(); else e->ignore(); }
    void dropEvent(QDropEvent* e) override;
};

// The cabinet IR after the capture: its waveform and name, or nothing. Drop a WAV here.
class IrView : public Display {
public:
    IrView(DeviceBody* b, int w, int h) : Display(b, w, h) {
        setAcceptDrops(true);
        InfoView::attach(this, "Cabinet IR", "A cabinet impulse response played after the capture (many captures are amp-only, without the speaker). "
                                             "Drop a WAV here or press Load IR…; the IR button switches it in and out, IR Mix blends it.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const Device* d = m_body->device();
        if (!d) return;
        const QRectF box = r.adjusted(8, 6, -8, -6);
        if (d->samplePath.isEmpty()) { p.setPen(col::gridText); p.drawText(box, Qt::AlignCenter, "No impulse response\nDrop one here, or press ▶"); return; }
        const SamplePtr s = d->sample;
        if (!s) {
            p.setPen(theme::devMissing);
            p.drawText(box, Qt::AlignCenter, p.fontMetrics().elidedText("Missing: " + QFileInfo(d->samplePath).fileName(), Qt::ElideMiddle, int(box.width())));
            return;
        }
        const double dur = s->seconds();
        const double mid = box.center().y() + 6, half = box.height() / 2 - 12;
        for (int x = 0; x < int(box.width()); ++x) {
            float lo, hi;
            s->range(dur * x / box.width(), dur * (x + 1) / box.width(), lo, hi);
            p.fillRect(QRectF(box.left() + x, mid - hi * half, 1, std::max(1.0, double(hi - lo) * half)), col::cyan);
        }
        p.setPen(col::value);
        p.drawText(box, Qt::AlignTop | Qt::AlignLeft, p.fontMetrics().elidedText(irFileName(d), Qt::ElideMiddle, int(box.width())));
        p.setPen(col::gridText);
        p.drawText(box, Qt::AlignBottom | Qt::AlignRight, QString("%1 ms").arg(int(dur * 1000)));
    }
    void dragEnterEvent(QDragEnterEvent* e) override { if (!Browser::irFilesIn(e->mimeData()).isEmpty() || !Browser::audioFilesIn(e->mimeData()).isEmpty()) e->acceptProposedAction(); else e->ignore(); }
    void dropEvent(QDropEvent* e) override;
};

// The level going in and coming out, as peaks with a dB scale.
class LevelMeter : public Display {
public:
    LevelMeter(DeviceBody* b, int w, int h) : Display(b, w, h) {
        InfoView::attach(this, "In / Out", "The level going into the device (before Input) and coming out of it (after Output and Mix), as peaks. Keep the output under 0 dB.");
    }
    void tick() override {
        m_in = std::max(m_body->meter(0), m_in * 0.85f);
        m_out = std::max(m_body->meter(1), m_out * 0.85f);
        update();
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const QRectF area = r.adjusted(4, 6, -4, -12);
        auto yOf = [&](double dbv) { return area.bottom() - std::clamp((dbv + 60) / 66.0, 0.0, 1.0) * area.height(); };   // -60 .. +6 dB
        p.setPen(QPen(col::grid, 1));
        for (int dbv : {6, 0, -12, -24, -36, -48}) p.drawLine(QPointF(area.left(), yOf(dbv)), QPointF(area.right(), yOf(dbv)));
        p.setPen(col::gridText);
        for (int dbv : {0, -24, -48}) p.drawText(QRectF(area.left(), yOf(dbv) - 8, area.width(), 8), Qt::AlignRight | Qt::AlignBottom, QString::number(dbv));
        auto bar = [&](double x, float level, const QColor& c) {
            const double dbv = 20.0 * std::log10(std::max(1e-5, double(level)));
            const double top = yOf(dbv);
            p.fillRect(QRectF(x, top, 9, area.bottom() - top), dbv > 0 ? theme::peakClip : c);
        };
        bar(area.left() + 3, m_in, col::cyan);
        bar(area.left() + 16, m_out, col::curve);
        p.setPen(col::gridText);
        p.drawText(QRectF(area.left(), area.bottom() + 1, 15, 10), Qt::AlignCenter, "In");
        p.drawText(QRectF(area.left() + 13, area.bottom() + 1, 18, 10), Qt::AlignCenter, "Out");
    }
private:
    float m_in = 0, m_out = 0;
};

// NAM: the capture (its view, Load…, Clear), Input / Output / Gate / Mix, Normalize, the in / out
// meter, and the cabinet IR slot (its view, Load IR…, Clear, IR on / off, IR Mix). A .nam or a WAV
// dropped anywhere on it loads the capture or the IR.
class NamBody : public DeviceBody {
public:
    NamBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        const QString labelStyle = QString("color:%1;font-size:9px;background:transparent;border:0;").arg(col::label.name());
        auto button = [this](const QString& text, const QString& title, const QString& help, std::function<void()> fn) {
            auto* b = new QToolButton(this);
            b->setText(text);
            b->setFocusPolicy(Qt::NoFocus);
            b->setStyleSheet("QToolButton{font-size:9px;padding:1px 8px;}");
            InfoView::attach(b, title, help);
            connect(b, &QToolButton::clicked, this, std::move(fn));
            return b;
        };
        auto help = [this](QWidget* cell, int i, const QString& text) {   // richer help than the generic knob text
            if (auto* k = cell->findChild<Knob*>()) InfoView::attach(k, name(i), text);
        };
        auto* root = rootRow(this);
        auto* cap = vbox(3);
        cap->addWidget(section("Capture"));
        cap->addWidget(new CaptureView(this, 240, 100));
        auto* capRow = hbox(4);
        // ◀ ▶ step through the captures the browser lists (the User Library and your sample folders;
        // or the capture's own folder when it isn't in them). Mappable, for a foot controller.
        QToolButton* prev = button("◀", "Previous Capture", "Loads the capture before this one in your library (the User Library folder and the browser's sample folders), "
                                                            "or in the capture's own folder. Wraps around. One undo step; MIDI-mappable.", [this] { stepModel(-1); });
        QToolButton* next = button("▶", "Next Capture", "Loads the next capture in your library (the User Library folder and the browser's sample folders), "
                                                        "or in the capture's own folder. Wraps around. One undo step; MIDI-mappable.", [this] { stepModel(1); });
        prev->setStyleSheet("QToolButton{font-size:9px;padding:1px 4px;}");
        next->setStyleSheet("QToolButton{font-size:9px;padding:1px 4px;}");
        prev->setProperty("mapTarget", QString("device:%1:%2:prev").arg(c.trackId).arg(c.deviceId));
        next->setProperty("mapTarget", QString("device:%1:%2:next").arg(c.trackId).arg(c.deviceId));
        capRow->addWidget(prev);
        capRow->addWidget(next);
        capRow->addWidget(button("Load…", "Load Capture", "Pick a .nam file: a Neural Amp Modeler capture of an amp or pedal (ToneHunt has thousands). Dropping one anywhere on the device does the same. One undo step.",
                                 [this] {
                                     const QString path = QFileDialog::getOpenFileName(this, "Load Capture", QString(), "Neural Amp Modeler captures (*.nam)");
                                     if (!path.isEmpty()) loadModel(path);
                                 }));
        capRow->addWidget(button("Clear", "Clear Capture", "Forgets the capture: the device plays dry until you load another. One undo step.", [this] { clearModel(); }));
        capRow->addStretch(1);
        cap->addLayout(capRow);
        auto* fileRow = hbox(4);   // the file, and where it sits in the library ("3 / 42")
        m_file = new QLabel(this);
        m_file->setStyleSheet(labelStyle);
        fileRow->addWidget(m_file, 1);
        m_place = new QLabel(this);
        m_place->setStyleSheet(labelStyle);
        InfoView::attach(m_place, "Library Position", "Which capture this is of the ones the browser lists (the User Library folder and your sample folders); ◀ ▶ step through them.");
        fileRow->addWidget(m_place);
        cap->addLayout(fileRow);
        cap->addStretch(1);
        root->addLayout(cap);
        root->addWidget(vline(this));

        auto* mid = vbox(4);
        auto* knobs = hbox(2);
        QWidget* cell = knob(namdev::Input, 28);
        help(cell, namdev::Input, "Gain before the capture, -24 to +24 dB: how hard the amp is driven, like your guitar's volume. More for more distortion, less to clean up.");
        knobs->addWidget(cell);
        cell = knob(namdev::Output, 28);
        help(cell, namdev::Output, "Gain after the capture, Normalize and the IR: the listening level, without changing the drive.");
        knobs->addWidget(cell);
        cell = knob(namdev::Gate, 28);
        help(cell, namdev::Gate, "A noise gate before the capture: it closes when the input falls under this threshold (6 dB of hysteresis, a 20 ms release). All the way down: off.");
        knobs->addWidget(cell);
        cell = knob(namdev::Mix, 28);
        help(cell, namdev::Mix, "How much of the processed sound you hear against the dry input.");
        knobs->addWidget(cell);
        mid->addLayout(knobs);
        auto* normRow = hbox(4);
        QWidget* norm = toggle(namdev::Normalize, "Normalize", 66);
        InfoView::attach(norm, "Normalize", QString("Brings the capture to a standard loudness (%1 dB, from the loudness in its metadata), so different captures sit at the same level. Does nothing for a capture without loudness data.").arg(namdev::kNormalizeTargetDb));
        normRow->addWidget(norm);
        m_norm = new QLabel(this);
        m_norm->setStyleSheet(labelStyle);
        normRow->addWidget(m_norm, 1);
        mid->addLayout(normRow);
        auto* qualRow = hbox(4);
        QWidget* quality = choices(namdev::Quality, {"Full", "Light"}, {}, 96);
        InfoView::attach(quality, "Quality", "Which size of the capture runs. The newest captures hold two networks trained together: Full (the best) and Light (a smaller one, a fraction of the CPU). "
                                             "A capture with one size ignores it.");
        qualRow->addWidget(quality);
        m_sizes = new QLabel(this);
        m_sizes->setStyleSheet(labelStyle);
        qualRow->addWidget(m_sizes, 1);
        mid->addLayout(qualRow);
        mid->addStretch(1);
        root->addLayout(mid);
        root->addWidget(vline(this));

        auto* meter = new LevelMeter(this, 46, 150);
        animate(meter);
        root->addWidget(meter, 0, Qt::AlignTop);
        root->addWidget(vline(this));

        auto* cab = vbox(3);
        cab->addWidget(section("Cabinet IR"));
        cab->addWidget(new IrView(this, 172, 70));
        auto* irRow = hbox(4);
        addIrSteps(this, irRow);
        irRow->addWidget(button("Load IR…", "Load IR", "Pick a cabinet impulse response (a WAV file) to play after the capture. Dropping a WAV or a browser IR on the cabinet does the same. One undo step.",
                                [this] {
                                    const QString path = QFileDialog::getOpenFileName(this, "Load Impulse Response", QString(), "Audio files (" + samples::nameFilters().join(' ') + ")");
                                    if (!path.isEmpty()) loadIr(path);
                                }));
        irRow->addWidget(button("Clear", "Clear IR", "Removes the cabinet IR: the capture plays on its own. One undo step.", [this] { clearIr(); }));
        m_irPlaceLabel = new QLabel(this);
        m_irPlaceLabel->setStyleSheet(labelStyle);
        InfoView::attach(m_irPlaceLabel, "Library Position", "Which impulse response this is of the ones the browser lists under Impulse Responses; ◀ ▶ step through them.");
        irRow->addWidget(m_irPlaceLabel, 1);
        cab->addLayout(irRow);
        auto* irKnobs = hbox(4);
        QWidget* irOn = toggle(namdev::Ir, "IR", 36);
        InfoView::attach(irOn, "IR", "Switches the cabinet IR in and out (to compare, or for a capture that already has its cabinet).");
        irKnobs->addWidget(irOn);
        cell = knob(namdev::IrMix, 22);
        help(cell, namdev::IrMix, "How much of the cabinet you hear against the capture on its own.");
        irKnobs->addWidget(cell);
        irKnobs->addStretch(1);
        cab->addLayout(irKnobs);
        cab->addStretch(1);
        root->addLayout(cab);
        setFixedWidth(720);
        setAcceptDrops(true);
        refresh();
    }
    void refresh() override {
        DeviceBody::refresh();
        const Device* d = device();
        if (!d) return;
        m_file->setText(d->modelPath.isEmpty() ? QString("No capture") : m_file->fontMetrics().elidedText(QFileInfo(d->modelPath).fileName(), Qt::ElideMiddle, 170));
        if (d->modelPath != m_placePath) {   // the library is scanned only when the capture changes
            m_placePath = d->modelPath;
            m_placeText.clear();
            if (!d->modelPath.isEmpty()) {
                const QStringList library = import::captureLibrary();
                const int at = import::captureIndex(d->modelPath, library);
                m_placeText = at > 0 ? QString("%1 / %2").arg(at).arg(library.size()) : QString("not in the library");
            }
        }
        m_place->setText(m_placeText);
        m_irPlaceLabel->setText(m_irPlace.update(d));
        m_norm->setText(!d->model ? QString() : d->model->hasLoudness ? QString("loudness %1 dB").arg(d->model->loudness, 0, 'f', 1) : QString("no loudness data"));
        m_sizes->setText(!d->model ? QString() : d->model->variants.empty() ? QString("one size")
                         : QString("%1 / %2 ch").arg(d->model->channelsSummary).arg(d->model->variants.front().model->channelsSummary));
    }
    // The next / previous capture in the library (one undo step).
    void stepModel(int dir) {
        const DeviceContext& c = ctx();
        QString why;
        bool ok = false;
        {
            EditScope edit(c.project, dir > 0 ? "Next Capture" : "Previous Capture");
            ok = import::stepCapture(*c.project, c.track, c.deviceId, dir, &why);
        }
        if (!ok) InfoView::notify(dir > 0 ? "Next Capture" : "Previous Capture", why);
    }
    // The capture from a file (one undo step).
    void loadModel(const QString& path) {
        QString why;
        const nam::ModelPtr m = nam::load(path, &why);
        if (!m) { QMessageBox::warning(this, "Load Capture", QFileInfo(path).fileName() + ": " + why); return; }
        const DeviceContext& c = ctx();
        {
            EditScope edit(c.project, "Load Capture");
            Device* d = findDevice(c.project, c.track, c.deviceId);
            if (!d) return;
            d->modelPath = QFileInfo(path).absoluteFilePath();
            d->model = m;
        }
        emit c.project->structureChanged();
        emit c.project->deviceChanged();
    }
    void clearModel() {
        const DeviceContext& c = ctx();
        {
            EditScope edit(c.project, "Clear Capture");
            Device* d = findDevice(c.project, c.track, c.deviceId);
            if (!d || d->modelPath.isEmpty()) return;
            d->modelPath.clear();
            d->model.reset();
        }
        emit c.project->structureChanged();
        emit c.project->deviceChanged();
    }
    // The cabinet IR from a file (or a built-in one), switched in (one undo step).
    void loadIr(const QString& path) {
        const DeviceContext& c = ctx();
        QString why;
        bool ok = false;
        { EditScope edit(c.project, "Load IR"); ok = import::loadIr(*c.project, c.track, c.deviceId, path, &why); }
        if (!ok) QMessageBox::warning(this, "Load IR", why);
    }
    void clearIr() {
        const DeviceContext& c = ctx();
        {
            EditScope edit(c.project, "Clear IR");
            Device* d = findDevice(c.project, c.track, c.deviceId);
            if (!d || d->samplePath.isEmpty()) return;
            d->samplePath.clear();
            d->sample.reset();
        }
        emit c.project->structureChanged();
        emit c.project->deviceChanged();
    }
protected:
    void dragEnterEvent(QDragEnterEvent* e) override {
        if (!Browser::modelFilesIn(e->mimeData()).isEmpty() || !Browser::irFilesIn(e->mimeData()).isEmpty() || !Browser::audioFilesIn(e->mimeData()).isEmpty()) e->acceptProposedAction(); else e->ignore();
    }
    void dropEvent(QDropEvent* e) override {
        const QStringList models = Browser::modelFilesIn(e->mimeData());
        const QStringList files = Browser::irFilesIn(e->mimeData()) + Browser::audioFilesIn(e->mimeData());
        if (models.isEmpty() && files.isEmpty()) { e->ignore(); return; }
        e->acceptProposedAction();
        if (!models.isEmpty()) loadModel(models.first());
        else loadIr(files.first());
    }
private:
    QLabel* m_file = nullptr;
    QLabel* m_place = nullptr;   // "3 / 42": where the capture sits in the library
    QString m_placePath, m_placeText;
    QLabel* m_norm = nullptr;
    QLabel* m_sizes = nullptr;
    QLabel* m_irPlaceLabel = nullptr;
    IrPlace m_irPlace;
};

void CaptureView::dropEvent(QDropEvent* e) {
    const QStringList models = Browser::modelFilesIn(e->mimeData());
    if (models.isEmpty()) { e->ignore(); return; }
    e->acceptProposedAction();
    if (auto* b = dynamic_cast<NamBody*>(m_body)) b->loadModel(models.first());
}

// ============================================================================ Cabinet
//
// The IR (its waveform, name and place in the library), ◀ ▶ Load… Clear, the IR switch and Mix,
// Gain, Low Cut / High Cut with their response, and the in / out meter. Drop an IR or a WAV anywhere on it.
class CabinetBody : public DeviceBody {
public:
    CabinetBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        const QString labelStyle = QString("color:%1;font-size:9px;background:transparent;border:0;").arg(col::label.name());
        auto* root = rootRow(this);
        auto* cab = vbox(3);
        cab->addWidget(section("Impulse Response"));
        cab->addWidget(new IrView(this, 200, 84));
        auto* irRow = hbox(4);
        addIrSteps(this, irRow);
        irRow->addWidget(smallButton(this, "Load…", "Load IR", "Pick an impulse response (an audio file). Dropping a WAV or a browser IR on the device does the same. One undo step.", [this] {
            const QString path = QFileDialog::getOpenFileName(this, "Load Impulse Response", QString(), "Audio files (" + samples::nameFilters().join(' ') + ")");
            if (!path.isEmpty()) loadIr(path);
        }));
        irRow->addWidget(smallButton(this, "Clear", "Clear IR", "Removes the impulse response: the device passes the sound through. One undo step.", [this] { clearIr(); }));
        irRow->addStretch(1);
        cab->addLayout(irRow);
        m_place = new QLabel(this);
        m_place->setStyleSheet(labelStyle);
        InfoView::attach(m_place, "Library Position", "Which impulse response this is of the ones the browser lists under Impulse Responses; ◀ ▶ step through them.");
        cab->addWidget(m_place);
        cab->addStretch(1);
        root->addLayout(cab);
        root->addWidget(vline(this));

        auto* knobs = vbox(4);
        auto* row1 = hbox(2);
        QWidget* on = toggle(cabinet::Ir, "IR", 36);
        InfoView::attach(on, "IR", "Switches the impulse response in and out, to compare.");
        row1->addWidget(on, 0, Qt::AlignVCenter);
        row1->addWidget(knob(cabinet::Mix, 26));
        row1->addWidget(knob(cabinet::Gain, 26));
        knobs->addLayout(row1);
        auto* row2 = hbox(2);
        row2->addWidget(knob(cabinet::LowCut, 26));
        row2->addWidget(knob(cabinet::HighCut, 26));
        knobs->addLayout(row2);
        knobs->addStretch(1);
        root->addLayout(knobs);
        root->addWidget(vline(this));
        auto* meter = new LevelMeter(this, 46, 150);
        animate(meter);
        root->addWidget(meter, 0, Qt::AlignTop);
        setFixedWidth(430);
        setAcceptDrops(true);
        refresh();
    }
    void refresh() override {
        DeviceBody::refresh();
        m_place->setText(m_irPlace.update(device()));
    }
    void loadIr(const QString& path) {
        const DeviceContext& c = ctx();
        QString why;
        bool ok = false;
        { EditScope edit(c.project, "Load IR"); ok = import::loadIr(*c.project, c.track, c.deviceId, path, &why); }
        if (!ok) QMessageBox::warning(this, "Load IR", why);
    }
    void clearIr() {
        const DeviceContext& c = ctx();
        {
            EditScope edit(c.project, "Clear IR");
            Device* d = findDevice(c.project, c.track, c.deviceId);
            if (!d || d->samplePath.isEmpty()) return;
            d->samplePath.clear();
            d->sample.reset();
        }
        emit c.project->structureChanged();
        emit c.project->deviceChanged();
    }
protected:
    void dragEnterEvent(QDragEnterEvent* e) override {
        if (!Browser::irFilesIn(e->mimeData()).isEmpty() || !Browser::audioFilesIn(e->mimeData()).isEmpty()) e->acceptProposedAction(); else e->ignore();
    }
    void dropEvent(QDropEvent* e) override {
        const QStringList files = Browser::irFilesIn(e->mimeData()) + Browser::audioFilesIn(e->mimeData());
        if (files.isEmpty()) { e->ignore(); return; }
        e->acceptProposedAction();
        loadIr(files.first());
    }
private:
    QLabel* m_place = nullptr;
    IrPlace m_irPlace;
};

void IrView::dropEvent(QDropEvent* e) {
    const QStringList files = Browser::irFilesIn(e->mimeData()) + Browser::audioFilesIn(e->mimeData());
    if (files.isEmpty()) { e->ignore(); return; }
    e->acceptProposedAction();
    if (auto* b = dynamic_cast<NamBody*>(m_body)) b->loadIr(files.first());
    else if (auto* cb = dynamic_cast<CabinetBody*>(m_body)) cb->loadIr(files.first());
}

// ============================================================================ pedals
//
// Each pedal is a device like the others: its knobs and mode buttons on the left, then its
// graphs. A drive pedal shows its clipper (what the pedal makes of an input, from quiet to full
// scale, at the current gain) and its tone (the filters' response); the phaser its notches
// sweeping, the chorus its sweep, the wah its rocker and the peak it moves, Twang its springs.

// The clipper: one cycle of a quiet note (a guitar at about -12 dBFS, faint) and what the
// pedal's clipper makes of it at this gain: how far it squares up, which side it favours.
class TransferCurve : public Display {
public:
    TransferCurve(DeviceBody* b, int w, int h) : Display(b, w, h) {
        InfoView::attach(this, "Clipping", "One cycle of a quiet note (faint) and what the pedal turns it into at this gain: rounded is soft, "
                                           "flat-topped is clipped, lopsided means even harmonics. The number is the gain on a small signal.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const T t = m_body->ctx().type;
        const std::vector<float> pos = m_body->positions();
        p.setPen(QPen(col::grid, 1));
        p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
        for (int k = 1; k < 4; ++k) p.drawLine(QPointF(r.left() + r.width() * k / 4, r.top()), QPointF(r.left() + r.width() * k / 4, r.bottom()));
        const double quiet = 0.25, half = r.height() / 2 - 3;
        QPainterPath clean, hot;
        for (int i = 0; i <= int(r.width()); ++i) {
            const double s = quiet * std::sin(2 * M_PI * i / r.width());
            const double y = std::clamp(double(audio::pedalTransfer(t, pos.data(), float(s))), -1.0, 1.0);
            const QPointF a(r.left() + i, r.center().y() - s * half), b(r.left() + i, r.center().y() - y * half);
            if (i == 0) { clean.moveTo(a); hot.moveTo(b); } else { clean.lineTo(a); hot.lineTo(b); }
        }
        p.setPen(QPen(col::gridStrong, 1));
        p.drawPath(clean);
        QPainterPath fill = hot;
        fill.lineTo(r.right(), r.center().y()); fill.lineTo(r.left(), r.center().y()); fill.closeSubpath();
        p.fillPath(fill, col::curveFill);
        p.setPen(QPen(col::curve, 1.6));
        p.drawPath(hot);
        const float g = audio::pedalTransfer(t, pos.data(), 0.01f) / 0.01f;   // the small-signal gain
        drawCaption(p, r, g >= 20 ? QString("×%1").arg(int(std::lround(g))) : QString("×%1").arg(g, 0, 'f', 1), Qt::AlignTop | Qt::AlignLeft);
    }
};

// The filters: a response curve, ±24 dB.
class ToneCurve : public Display {
public:
    ToneCurve(DeviceBody* b, int w, int h, const QString& help) : Display(b, w, h) { InfoView::attach(this, "Tone", help); }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        drawFreqGrid(p, r, 24);
        const std::vector<float> pos = m_body->positions();
        const T t = m_body->ctx().type;
        QPainterPath path;
        for (int x = 0; x <= int(r.width()); x += 2) {
            const double dbv = audio::pedalResponseDb(t, pos.data(), float(xToFreq(r.left() + x, r)), kSr);
            const QPointF pt(r.left() + x, r.center().y() - std::clamp(dbv, -24.0, 24.0) / 24.0 * (r.height() / 2 - 2));
            if (x == 0) path.moveTo(pt); else path.lineTo(pt);
        }
        QPainterPath fill = path;
        fill.lineTo(r.right(), r.center().y()); fill.lineTo(r.left(), r.center().y()); fill.closeSubpath();
        p.fillPath(fill, col::curveFill);
        p.setPen(QPen(col::curve, 1.5));
        p.drawPath(path);
    }
};

// The phaser's notches where the sweep is right now (meter[2] is the stages' coefficient), moving.
class PhaserCurve : public Display {
public:
    PhaserCurve(DeviceBody* b, int w, int h) : Display(b, w, h) {
        InfoView::attach(this, "Notches", "The four stages' notches, where the sweep has them right now. Speed moves them, Depth sets how far, Script softens the dips.");
    }
    void tick() override { update(); }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        drawFreqGrid(p, r, 24);
        const float coef = m_body->meter(2);
        QPainterPath path;
        for (int x = 0; x <= int(r.width()); x += 2) {
            const double dbv = audio::phaserResponseDb(coef, float(xToFreq(r.left() + x, r)), kSr);
            const QPointF pt(r.left() + x, r.center().y() - std::clamp(dbv, -24.0, 24.0) / 24.0 * (r.height() / 2 - 2));
            if (x == 0) path.moveTo(pt); else path.lineTo(pt);
        }
        p.setPen(QPen(col::curve, 1.5));
        p.drawPath(path);
    }
};

// Twang: three springs in the tank, shaking with what's in them.
class SpringTank : public Display {
public:
    SpringTank(DeviceBody* b, int w, int h) : Display(b, w, h) {
        InfoView::attach(this, "Springs", "The reverb tank: the springs shake with the sound going through them. Dwell drives them harder, Tone darkens them.");
    }
    void tick() override { m_level = std::max(double(m_body->meter(0)), m_level * 0.88); m_t += 0.35; update(); }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        for (int s = 0; s < 3; ++s) {
            const double y0 = r.top() + r.height() * (s + 1) / 4;
            QPainterPath coil;
            const int turns = 30;
            for (int i = 0; i <= turns * 8; ++i) {
                const double x = r.left() + 10 + (r.width() - 20) * i / (turns * 8.0);
                const double wobble = std::sin(m_t * (1.3 + s * 0.4) + x * 0.05) * std::min(1.0, m_level * 6) * 4;
                const double y = y0 + std::sin(i * 2 * M_PI / 8) * 3.2 + wobble;
                if (i == 0) coil.moveTo(x, y); else coil.lineTo(x, y);
            }
            p.setPen(QPen(col::cyan, 1.1));
            p.drawPath(coil);
        }
        p.fillRect(QRectF(r.left() + 3, r.top() + 4, 5, r.height() - 8), col::gridStrong);
        p.fillRect(QRectF(r.right() - 8, r.top() + 4, 5, r.height() - 8), col::gridStrong);
        drawCaption(p, r, QString("Dwell %1").arg(m_body->text(0)), Qt::AlignTop | Qt::AlignLeft);
    }
private:
    double m_level = 0, m_t = 0;
};

// Phase / Chorus pedals: the sweep's shape, where it is, and a light pulsing with it.
class RateLight : public Display {
public:
    RateLight(DeviceBody* b, int w, int h, bool triangle) : Display(b, w, h), m_triangle(triangle) {
        InfoView::attach(this, "Sweep", "The sweep's shape and where it is; the light pulses with it.");
    }
    void tick() override { update(); }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const double ph = m_body->meter(0);
        const double v = m_triangle ? 1 - 2 * std::fabs(2 * ph - 1) : m_body->meter(1);
        p.setPen(QPen(col::grid, 1));
        p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right() - 24, r.center().y()));
        QPainterPath path;
        const double w = r.width() - 30;
        for (int i = 0; i <= int(w); ++i) {
            const double t = double(i) / w;
            const double y = m_triangle ? 1 - 2 * std::fabs(2 * t - 1) : (2 * std::fabs(2 * t - 1) - 1) * 0.8 + 0.2 * std::sin(2 * M_PI * t);
            const QPointF pt(r.left() + 4 + i, r.center().y() - y * (r.height() / 2 - 6));
            if (i == 0) path.moveTo(pt); else path.lineTo(pt);
        }
        p.setPen(QPen(col::cyan, 1.3));
        p.drawPath(path);
        const double x = r.left() + 4 + ph * w;
        p.setPen(QPen(col::handle, 1));
        p.drawLine(QPointF(x, r.top() + 3), QPointF(x, r.bottom() - 3));
        QColor c = col::curve;
        c.setAlphaF(0.2 + 0.8 * (0.5 + 0.5 * v));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(QPointF(r.right() - 11, r.center().y()), 5, 5);
    }
private:
    bool m_triangle;
};

// The wah's rocker, side on: heel down to toe down. Drag it up and down.
class WahRocker : public Display {
public:
    WahRocker(DeviceBody* b, int w, int h) : Display(b, w, h) {
        setCursor(Qt::SizeVerCursor);
        setProperty("mapTarget", b->target(0));
        InfoView::attach(this, "Pedal", "Rock it: drag up for toe down (bright), down for heel down (dark). Map it to an expression pedal in MIDI Map. In Auto your picking moves it.");
    }
    void tick() override { update(); }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const bool autoMode = m_body->pick(1) == 1;
        const double pos = autoMode ? m_body->meter(0) : m_body->pos(0);
        const QRectF base(r.left() + 8, r.bottom() - 12, r.width() - 16, 6);
        p.setPen(Qt::NoPen);
        p.setBrush(col::gridStrong);
        p.drawRoundedRect(base, 2, 2);
        const QPointF axle(r.left() + r.width() * 0.58, base.top() - 10);
        QPainterPath wedge;
        wedge.moveTo(axle + QPointF(-12, 10)); wedge.lineTo(axle + QPointF(12, 10)); wedge.lineTo(axle + QPointF(4, 0)); wedge.lineTo(axle + QPointF(-4, 0)); wedge.closeSubpath();
        p.fillPath(wedge, col::gridStrong);
        const double ang = (-14 + pos * 28) * M_PI / 180;   // + = toe down
        const double back = r.width() * 0.5, front = r.width() * 0.32;
        const QPointF dir(std::cos(ang), std::sin(ang));
        const QPointF heel = axle - dir * back, toe = axle + dir * front;
        const QPointF up(dir.y(), -dir.x());
        QPainterPath treadle;
        treadle.moveTo(heel); treadle.lineTo(toe); treadle.lineTo(toe + up * 8); treadle.lineTo(heel + up * 11); treadle.closeSubpath();
        p.setPen(QPen(col::handle, 1));
        p.setBrush(col::curveFill);
        p.drawPath(treadle);
        p.setPen(QPen(col::curve, 1.2));
        for (int k = 1; k < 7; ++k) {
            const QPointF a = heel + (toe - heel) * (k / 7.0) + up * 2.5, b2 = a + up * 6;
            p.drawLine(a, b2);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(col::curve);
        p.drawEllipse(axle, 3, 3);
        drawCaption(p, r, autoMode ? QString("Auto  %1%").arg(int(pos * 100)) : QString("%1%").arg(int(pos * 100)), Qt::AlignTop | Qt::AlignLeft);
    }
    void mousePressEvent(QMouseEvent* e) override { m_y = e->position().y(); m_start = m_body->pos(0); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (e->buttons() & Qt::LeftButton) m_body->editMany({{0, m_start + (m_y - e->position().y()) / 90.0}}, 0);
    }
private:
    double m_y = 0, m_start = 0;
};

class PedalBody : public DeviceBody {
public:
    PedalBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = rootRow(this);
        constexpr int kH = 124;   // the graphs' height
        auto knobRowOf = [this](std::initializer_list<int> params, int size) {
            auto* row = hbox(0);
            for (int i : params) row->addWidget(knob(i, size));
            row->addStretch(1);
            return row;
        };
        auto column = [this](const QString& title) { auto* l = vbox(3); l->addWidget(section(title)); return l; };
        auto finish = [root](QVBoxLayout* l) { l->addStretch(1); root->addLayout(l); };
        auto graphs = [this, root](QWidget* a, QWidget* b) {
            root->addWidget(vline(this));
            auto* l = hbox(6);
            l->addWidget(a, 0, Qt::AlignTop);
            if (b) l->addWidget(b, 0, Qt::AlignTop);
            l->addStretch(1);
            root->addLayout(l);
        };
        switch (c.type) {
        case T::Overdrive: {
            auto* l = column("Overdrive");
            l->addLayout(knobRowOf({0, 1, 2}, 26));
            l->addWidget(choices(3, {}, {}, 110));
            finish(l);
            auto* tone = new ToneCurve(this, 150, kH, "The overdrive's voice: the lows pass clean, the mids are driven; Tone moves the top end from dark to open.");
            watch(tone);
            auto* clip = new TransferCurve(this, 104, kH);
            watch(clip);
            graphs(clip, tone);
            break;
        }
        case T::Distortion: {
            auto* l = column("Distortion");
            l->addLayout(knobRowOf({0, 1, 2}, 26));
            l->addWidget(choices(3, {}, {}, 140));
            finish(l);
            auto* tone = new ToneCurve(this, 150, kH, "The gain stage leans on the treble and tightens the lows as it goes up; Filter darkens the output.");
            watch(tone);
            auto* clip = new TransferCurve(this, 104, kH);
            watch(clip);
            graphs(clip, tone);
            break;
        }
        case T::MetalDistortion: {
            auto* l = column("Metal Distortion");
            l->addLayout(knobRowOf({0, 5, 6}, 22));
            l->addLayout(knobRowOf({1, 2, 3, 4}, 22));
            finish(l);
            auto* tone = new ToneCurve(this, 150, kH, "Tight cuts the lows before the gain; after it, Low, the sweepable Mid and High shape the sound.");
            watch(tone);
            auto* clip = new TransferCurve(this, 104, kH);
            watch(clip);
            graphs(clip, tone);
            break;
        }
        case T::Chainsaw: {
            auto* l = column("Chainsaw");
            l->addLayout(knobRowOf({0, 1, 2, 3}, 26));
            finish(l);
            auto* tone = new ToneCurve(this, 150, kH, "The famous curve: Low and High on 10 give the buzzing low end and the sawing peak in the upper mids.");
            watch(tone);
            auto* clip = new TransferCurve(this, 104, kH);
            watch(clip);
            graphs(clip, tone);
            break;
        }
        case T::MetalBoost: {
            auto* l = column("Metal Boost");
            l->addLayout(knobRowOf({0, 1, 2, 3}, 26));
            finish(l);
            auto* tone = new ToneCurve(this, 150, kH, "Tight cuts the lows before the amp so palm mutes stay crisp; Tone is a treble shelf.");
            watch(tone);
            auto* clip = new TransferCurve(this, 104, kH);
            watch(clip);
            graphs(clip, tone);
            break;
        }
        case T::Fuzz: {
            auto* l = column("Fuzz");
            l->addLayout(knobRowOf({0, 1, 2, 3}, 26));
            finish(l);
            auto* tone = new ToneCurve(this, 150, kH, "A dark input and output around the two transistors; Tone blends the muff's low and high paths.");
            watch(tone);
            auto* clip = new TransferCurve(this, 104, kH);
            watch(clip);
            graphs(clip, tone);
            break;
        }
        case T::Crunch: {
            auto* l = column("Crunch");
            l->addLayout(knobRowOf({0, 1, 2, 4}, 26));
            l->addWidget(toggle(3, "Bright", 56));
            finish(l);
            auto* tone = new ToneCurve(this, 150, kH, "Tone tilts the sound from warm to cutting, Presence adds bite on top; Bright lifts the treble before the gain.");
            watch(tone);
            auto* clip = new TransferCurve(this, 104, kH);
            watch(clip);
            graphs(clip, tone);
            break;
        }
        case T::Twang: {
            auto* l = column("Twang");
            l->addLayout(knobRowOf({0, 1, 2, 3}, 26));
            finish(l);
            auto* tank = new SpringTank(this, 200, kH);
            animate(tank);
            graphs(tank, nullptr);
            break;
        }
        case T::PhasePedal: {
            auto* l = column("Phase Pedal");
            l->addLayout(knobRowOf({0, 2}, 26));
            l->addWidget(toggle(1, "Script", 56));
            finish(l);
            auto* light = new RateLight(this, 110, kH, false);
            animate(light);
            auto* notches = new PhaserCurve(this, 150, kH);
            animate(notches);
            graphs(light, notches);
            break;
        }
        case T::ChorusPedal: {
            auto* l = column("Chorus Pedal");
            l->addLayout(knobRowOf({0, 1, 3}, 26));
            l->addWidget(toggle(2, "Stereo", 56));
            finish(l);
            auto* light = new RateLight(this, 150, kH, true);
            animate(light);
            graphs(light, nullptr);
            break;
        }
        case T::Wah: {
            auto* l = column("Wah");
            l->addWidget(choices(1, {}, {}, 100));
            l->addWidget(choices(4, {}, {}, 100));
            l->addLayout(knobRowOf({2, 3}, 22));
            finish(l);
            auto* rocker = new WahRocker(this, 110, kH);
            animate(rocker);
            auto* tone = new ToneCurve(this, 150, kH, "The peak the pedal moves: heel down low and soft, toe down high and sharp; Q sharpens it.");
            watch(tone);
            graphs(rocker, tone);
            break;
        }
        default: break;
        }
        root->addStretch(1);
        root->activate();
        setFixedWidth(root->sizeHint().width());
    }
};

} // namespace

DeviceBody* createGuitarBody(const DeviceContext& ctx, QWidget* parent) {
    switch (ctx.type) {
    case T::Tuner:     return new TunerBody(ctx, parent);
    case T::GuitarAmp: return new GuitarAmpBody(ctx, parent);
    case T::Nam:       return new NamBody(ctx, parent);
    case T::Cabinet:   return new CabinetBody(ctx, parent);
    default:           return isGuitarType(ctx.type) ? new PedalBody(ctx, parent) : nullptr;
    }
}

} // namespace sessions::devui
