// The audio effects' panels. Parameter indices follow core/DeviceSpecs.cpp.

#include "Bodies.h"
#include "audio/Devices.h"
#include "audio/Dsp.h"
#include "core/Model.h"
#include "ui/InfoView.h"
#include "widgets/DragValue.h"
#include <QGridLayout>
#include <QMenu>
#include <QScrollArea>
#include <QToolButton>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <cmath>
#include <deque>

namespace sessions::devui {

namespace {

using T = DeviceType;
constexpr float kSr = 48000;

QGridLayout* knobGrid(DeviceBody* b, std::initializer_list<int> params, int columns, int size = 24) {
    auto* g = new QGridLayout;
    g->setContentsMargins(0, 0, 0, 0);
    g->setHorizontalSpacing(2);
    g->setVerticalSpacing(2);
    int k = 0;
    for (int i : params) { g->addWidget(b->knob(i, size), k / columns, k % columns); ++k; }
    return g;
}

double lerp(double a, double b, double t) { return a + (b - a) * t; }

// ============================================================================ Sidechain (Compressor, Gate, Auto Filter)

// Where the key comes from: this device's own input, or another track. Click for the list
// (tracks that would make a loop are greyed out).
class KeySource : public QWidget {
public:
    KeySource(DeviceBody* body, int width) : QWidget(body), m_body(body) {
        setFixedSize(width, 16);
        setCursor(Qt::PointingHandCursor);
        InfoView::attach(this, "Key From", "Which track's audio drives this device's detector: its own input (Internal), or another track, "
                                           "e.g. the kick ducking a bass. Key Tap picks the point in that track. A track that is itself keyed from this one can't be chosen (it would loop).");
    }
    QString label() const {
        const Device* d = m_body->device();
        if (!d || !d->sidechainTrack) return "Internal";
        const int i = m_body->ctx().project->indexOfTrack(d->sidechainTrack);
        return i >= 0 ? m_body->ctx().project->tracks[i].name : QString("Missing");
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const Device* d = m_body->device();
        const bool external = d && d->sidechainTrack;
        const bool missing = external && m_body->ctx().project->indexOfTrack(d->sidechainTrack) < 0;
        p.setPen(QPen(col::displayEdge, 1));
        p.setBrush(theme::inset);
        p.drawRoundedRect(r, 2, 2);
        QFont f = font(); f.setPointSizeF(7.5); p.setFont(f);
        p.setPen(missing ? theme::record : external ? col::cyan : col::value);
        p.drawText(r.adjusted(5, 0, -12, 0), Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(label(), Qt::ElideRight, int(r.width()) - 18));
        QPainterPath tri;
        const QPointF c(r.right() - 7, r.center().y());
        tri.moveTo(c + QPointF(-3, -1.5)); tri.lineTo(c + QPointF(3, -1.5)); tri.lineTo(c + QPointF(0, 2.5)); tri.closeSubpath();
        p.fillPath(tri, col::label);
    }
    void mousePressEvent(QMouseEvent*) override {
        Project* project = m_body->ctx().project;
        const Device* d = m_body->device();
        if (!d) return;
        QMenu menu(this);
        QAction* internal = menu.addAction("Internal");
        internal->setCheckable(true);
        internal->setChecked(!d->sidechainTrack);
        internal->setData(0);
        menu.addSeparator();
        for (int i = 0; i < project->tracks.size(); ++i) {
            const Track& t = project->tracks[i];
            if (t.isGroup() || t.id == m_body->ctx().trackId) continue;
            QString why;
            QAction* a = menu.addAction(QString("%1 %2").arg(i + 1).arg(t.name));
            a->setCheckable(true);
            a->setChecked(d->sidechainTrack == t.id);
            a->setData(t.id);
            if (!project->canKeyFrom(m_body->ctx().track, m_body->ctx().deviceId, t.id, &why)) { a->setEnabled(false); a->setToolTip(why); }
        }
        if (QAction* a = menu.exec(mapToGlobal(QPoint(0, height())))) project->setKeySource(m_body->ctx().track, m_body->ctx().deviceId, a->data().toInt());
        update();
    }
private:
    DeviceBody* m_body;
};

// The key's level: a slim vertical meter, -60 to 0 dB (the processor's meter[2]).
class KeyMeter : public Display {
public:
    KeyMeter(DeviceBody* b, int w, int h) : Display(b, w, h) {
        InfoView::attach(this, "Key Level", "How loud the key is after Key Gain and its filters: what the detector listens to, -60 to 0 dB.");
    }
    void tick() override {
        const float db = m_body->meter(2);
        m_shown = std::max(db, m_shown - 1.5f);
        update();
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const double h = r.height() - 12;
        for (int db = 0; db <= 60; db += 12) {
            const double y = std::round(r.top() + 10 + db / 60.0 * h) + 0.5;
            p.setPen(QPen(col::grid, 1));
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        }
        const double len = std::clamp((double(m_shown) + 60) / 60.0, 0.0, 1.0) * h;
        p.fillRect(QRectF(r.left() + 3, r.bottom() - 2 - len, r.width() - 6, len), col::cyan);
        p.setPen(col::gridText);
        p.drawText(QRectF(r.left(), r.top(), r.width(), 10), Qt::AlignCenter, "Key");
    }
private:
    float m_shown = -120;
};

// The Sidechain column of a Compressor, Gate or Auto Filter: the key's source and tap, its
// gain and filters, Listen, and its level.
QHBoxLayout* sidechainSection(DeviceBody* b, DeviceType type) {
    using namespace sidechain;
    auto* row = hbox(4);
    auto* colBox = vbox(3);
    colBox->addWidget(b->section("Sidechain"));
    auto* src = new KeySource(b, 96);
    b->watch(src);
    colBox->addWidget(src);
    colBox->addWidget(b->chooser(param(type, Tap), 96));
    auto* knobs = hbox(0);
    knobs->addWidget(b->knob(param(type, Gain), 20, "Gain"));
    knobs->addWidget(b->knob(param(type, HP), 20, "HP"));
    knobs->addWidget(b->knob(param(type, LP), 20, "LP"));
    colBox->addLayout(knobs);
    colBox->addWidget(b->toggle(param(type, Listen), "Listen", 60), 0, Qt::AlignLeft);
    colBox->addStretch(1);
    row->addLayout(colBox);
    auto* meter = new KeyMeter(b, 18, 170);
    b->animate(meter);
    row->addWidget(meter, 0, Qt::AlignTop);
    return row;
}

// ============================================================================ EQ

class EqDisplay : public Display {
public:
    EqDisplay(DeviceBody* b, int w, int h) : Display(b, w, h) {
        setCursor(Qt::SizeAllCursor);
        InfoView::attach(this, "EQ Display", "Drag a band's dot: left and right for its frequency, up and down for its gain. "
                                             "The mouse wheel over the middle band sets its width (Q). Drag a cut to the edge to switch it off.");
    }
protected:
    static constexpr double kRange = 18;
    struct Band { int freq, gain; QColor color; QString label; };
    // The outer bands' dots take the theme's neutral; the inner bands keep their own colours.
    static Band band(int k) {
        static const Band b[5] = {{8, -1, QColor(), "1"}, {4, 0, QColor("#ff8a5c"), "2"}, {5, 1, QColor("#ffd166"), "3"},
                                  {7, 2, QColor("#6ad0ff"), "4"}, {9, -1, QColor(), "5"}};
        Band out = b[k];
        if (!out.color.isValid()) out.color = theme::devBandNeutral;
        return out;
    }
    double dbY(double db, const QRectF& r) const { return r.center().y() - db / kRange * r.height() / 2; }
    QPointF bandPoint(int k, const QRectF& r) const {
        const Band b = band(k);
        const double f = m_body->real(b.freq);
        if (k == 0 && m_body->pos(8) <= 0.002) return {r.left() + 6, r.center().y()};
        if (k == 4 && m_body->pos(9) >= 0.998) return {r.right() - 6, r.center().y()};
        return {freqToX(f, r), b.gain >= 0 ? dbY(m_body->real(b.gain), r) : r.center().y()};
    }
    void draw(QPainter& p, const QRectF& r) override {
        drawFreqGrid(p, r, kRange);
        p.setPen(col::gridText);
        p.drawText(QPointF(r.left() + 2, dbY(12, r) + 3), "12");
        p.drawText(QPointF(r.left() + 2, dbY(-12, r) + 3), "-12");
        const std::vector<float> pos = m_body->positions();
        QPainterPath path;
        for (int x = 0; x <= int(r.width()); x += 2) {
            const double db = audio::eqResponseDb(pos.data(), float(xToFreq(r.left() + x, r)), kSr);
            const QPointF pt(r.left() + x, dbY(std::clamp(db, -kRange * 1.4, kRange * 1.4), r));
            if (x == 0) path.moveTo(pt); else path.lineTo(pt);
        }
        QPainterPath fill = path;
        fill.lineTo(r.right(), r.center().y()); fill.lineTo(r.left(), r.center().y()); fill.closeSubpath();
        p.fillPath(fill, col::curveFill);
        p.setPen(QPen(col::curve, 1.6));
        p.drawPath(path);
        for (int k = 0; k < 5; ++k) {
            const bool off = (k == 0 && m_body->pos(8) <= 0.002) || (k == 4 && m_body->pos(9) >= 0.998);
            drawHandle(p, bandPoint(k, r), band(k).color, band(k).label, off);
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        const QRectF r = inner();
        double best = 1e9;
        for (int k = 0; k < 5; ++k) {
            const double d = QLineF(e->position(), bandPoint(k, r)).length();
            if (d < best) { best = d; m_drag = k; }
        }
        mouseMoveEvent(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (m_drag < 0 || !(e->buttons() & Qt::LeftButton)) return;
        const QRectF r = inner();
        const Band b = band(m_drag);
        const double x = e->position().x();
        std::vector<std::pair<int, double>> ch;
        if (m_drag == 0 && x <= r.left() + 4) ch.push_back({8, 0.0});
        else if (m_drag == 4 && x >= r.right() - 4) ch.push_back({9, 1.0});
        else ch.push_back({b.freq, std::clamp(m_body->posFor(b.freq, xToFreq(x, r)), m_drag == 0 ? 0.01 : 0.0, m_drag == 4 ? 0.99 : 1.0)});
        if (b.gain >= 0) {
            const double db = (r.center().y() - e->position().y()) / (r.height() / 2) * kRange;
            ch.push_back({b.gain, m_body->posFor(b.gain, std::clamp(db, -15.0, 15.0))});
        }
        m_body->editMany(ch, m_drag);
    }
    void mouseReleaseEvent(QMouseEvent*) override { m_drag = -1; }
    void wheelEvent(QWheelEvent* e) override {
        const double step = e->angleDelta().y() > 0 ? 0.04 : -0.04;
        m_body->editMany({{6, m_body->pos(6) + step}}, 6);
        e->accept();
    }
private:
    int m_drag = -1;
};

class EqBody : public DeviceBody {
public:
    EqBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(8, 6, 8, 4);
        root->setSpacing(3);
        root->addWidget(new EqDisplay(this, 456, 116));
        auto* row = hbox(0);
        for (int i : {8, 0, 4}) row->addWidget(knob(i, 22, i == 0 ? "Low" : i == 4 ? "Freq" : "Low Cut"));
        row->addWidget(vline(this));
        for (int i : {1, 5, 6}) row->addWidget(knob(i, 22, i == 1 ? "Mid" : i == 5 ? "Freq" : "Q"));
        row->addWidget(vline(this));
        for (int i : {2, 7, 9}) row->addWidget(knob(i, 22, i == 2 ? "High" : i == 7 ? "Freq" : "High Cut"));
        row->addWidget(vline(this));
        row->addWidget(knob(3, 22));
        row->addStretch(1);
        root->addLayout(row);
        setFixedWidth(472);
    }
};

// ============================================================================ Compressor

// The transfer curve: input level (across) to output level (up). The dot shows the input
// level now; drag the knee for the threshold, the top-right handle for the ratio.
class CompDisplay : public Display {
public:
    CompDisplay(DeviceBody* b, int w, int h) : Display(b, w, h) {
        setCursor(Qt::SizeAllCursor);
        InfoView::attach(this, "Compressor Curve", "How loud goes in (across) against how loud comes out (up). "
                                                   "Drag the knee to set the threshold, the right-hand end to set the ratio. The dot is the signal now.");
    }
    void tick() override {
        const float in = m_body->meter(1);
        m_level = std::max(in, m_level - 1.5f);
        update();
    }
protected:
    static constexpr double kLo = -60;
    QPointF at(double inDb, double outDb, const QRectF& r) const {
        return {r.left() + (inDb - kLo) / -kLo * r.width(), r.bottom() - (std::clamp(outDb, kLo, 6.0) - kLo) / -kLo * r.height()};
    }
    double outFor(double in) const {
        const double thr = m_body->real(0), ratio = m_body->real(1), knee = m_body->real(4);
        const double makeup = m_body->on(5) ? -thr * (1 - 1 / ratio) * 0.25 : 0.0;
        return in + dsp::compReduction(float(in - thr), float(ratio), float(knee)) + makeup;
    }
    void draw(QPainter& p, const QRectF& r) override {
        for (int db = -48; db < 0; db += 12) {
            p.setPen(QPen(col::grid, 1));
            const QPointF a = at(db, db, r);
            p.drawLine(QPointF(std::round(a.x()) + 0.5, r.top()), QPointF(std::round(a.x()) + 0.5, r.bottom()));
            p.drawLine(QPointF(r.left(), std::round(a.y()) + 0.5), QPointF(r.right(), std::round(a.y()) + 0.5));
        }
        p.setPen(QPen(col::gridStrong, 1, Qt::DashLine));
        p.drawLine(at(kLo, kLo, r), at(0, 0, r));
        const double thr = m_body->real(0);
        p.setPen(QPen(col::gridStrong, 1));
        const double tx = std::round(at(thr, thr, r).x()) + 0.5;
        p.drawLine(QPointF(tx, r.top()), QPointF(tx, r.bottom()));
        QPainterPath path;
        for (int x = 0; x <= int(r.width()); ++x) {
            const double in = kLo + double(x) / r.width() * -kLo;
            const QPointF pt = at(in, outFor(in), r);
            if (x == 0) path.moveTo(pt); else path.lineTo(pt);
        }
        QPainterPath fill = path;
        fill.lineTo(r.right(), r.bottom()); fill.lineTo(r.left(), r.bottom()); fill.closeSubpath();
        p.fillPath(fill, col::curveFill);
        p.setPen(QPen(col::curve, 1.6));
        p.drawPath(path);
        drawHandle(p, at(thr, outFor(thr), r), col::handle);
        drawHandle(p, at(0, outFor(0), r) - QPointF(5, 0), col::handle);
        if (m_level > kLo) {
            const double in = std::min(0.0, double(m_level));
            p.setPen(Qt::NoPen);
            p.setBrush(col::cyan);
            p.drawEllipse(at(in, outFor(in), r), 3.5, 3.5);
        }
        p.setPen(col::gridText);
        p.drawText(r.adjusted(4, 2, -4, -2), Qt::AlignTop | Qt::AlignLeft, QString("%1 dB  %2").arg(thr, 0, 'f', 1).arg(m_body->text(1)));
    }
    void mousePressEvent(QMouseEvent* e) override {
        const QRectF r = inner();
        const double thr = m_body->real(0);
        const double dKnee = QLineF(e->position(), at(thr, outFor(thr), r)).length();
        const double dTop = QLineF(e->position(), at(0, outFor(0), r)).length();
        m_drag = dTop < dKnee && e->position().x() > r.center().x() ? 1 : 0;
        mouseMoveEvent(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (m_drag < 0 || !(e->buttons() & Qt::LeftButton)) return;
        const QRectF r = inner();
        if (m_drag == 0) {
            const double in = kLo + (e->position().x() - r.left()) / r.width() * -kLo;
            m_body->editMany({{0, m_body->posFor(0, std::clamp(in, -40.0, 0.0))}}, 0);
        } else {
            const double thr = m_body->real(0);
            const double out = kLo + (r.bottom() - e->position().y()) / r.height() * -kLo;
            const double makeup = m_body->on(5) ? -thr * (1 - 1 / m_body->real(1)) * 0.25 : 0.0;
            const double rise = std::max(0.01, out - makeup - thr);
            m_body->editMany({{1, m_body->posFor(1, std::clamp(-thr / rise, 1.0, 20.0))}}, 1);
        }
    }
    void mouseReleaseEvent(QMouseEvent*) override { m_drag = -1; }
private:
    int m_drag = -1;
    float m_level = -100;
};

// A vertical gain-reduction meter: grows down from the top, 0 to -24 dB.
class GrMeter : public Display {
public:
    GrMeter(DeviceBody* b, int w, int h, int meterIndex) : Display(b, w, h), m_meter(meterIndex) {
        InfoView::attach(this, "Gain Reduction", "How many dB the device is taking off right now.");
    }
    void tick() override {
        const float gr = m_body->meter(m_meter);
        m_shown = std::max(gr, m_shown - 0.6f);
        m_peak = gr > m_peak ? gr : std::max(0.0f, m_peak - 0.15f);
        update();
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const double h = r.height() - 12;
        for (int db = 0; db <= 24; db += 6) {
            const double y = std::round(r.top() + 10 + db / 24.0 * h) + 0.5;
            p.setPen(QPen(col::grid, 1));
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        }
        const double len = std::clamp(double(m_shown), 0.0, 24.0) / 24.0 * h;
        p.fillRect(QRectF(r.left() + 3, r.top() + 10, r.width() - 6, len), col::curve);
        const double py = r.top() + 10 + std::clamp(double(m_peak), 0.0, 24.0) / 24.0 * h;
        p.fillRect(QRectF(r.left() + 3, py - 1, r.width() - 6, 2), col::handle);
        p.setPen(col::gridText);
        p.drawText(QRectF(r.left(), r.top(), r.width(), 10), Qt::AlignCenter, "GR");
    }
private:
    int m_meter;
    float m_shown = 0, m_peak = 0;
};

class CompressorBody : public DeviceBody {
public:
    CompressorBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = rootRow(this);
        auto* disp = new CompDisplay(this, 160, 170);
        animate(disp);
        root->addWidget(disp);
        auto* gr = new GrMeter(this, 22, 170, 0);
        animate(gr);
        root->addWidget(gr);
        root->addWidget(vline(this));
        auto* right = vbox(2);
        right->addLayout(knobGrid(this, {0, 1, 4, 2, 3, 6}, 3));
        auto* last = hbox(4);
        last->addWidget(toggle(5, "Makeup", 52), 0, Qt::AlignVCenter);
        last->addWidget(knob(7, 24));
        last->addStretch(1);
        right->addLayout(last);
        right->addStretch(1);
        root->addLayout(right);
        root->addWidget(vline(this));
        root->addLayout(sidechainSection(this, T::Compressor));
        setFixedWidth(510);
    }
};

// ============================================================================ Saturator

class ShaperDisplay : public Display {
public:
    ShaperDisplay(DeviceBody* b, int w, int h) : Display(b, w, h) {
        setCursor(Qt::SizeVerCursor);
        InfoView::attach(this, "Saturator Curve", "The waveshaper: what each input level (across) becomes (up). Drag up or down for more or less drive.");
    }
    void tick() override { m_peak = std::max(m_body->meter(0), m_peak * 0.9f); update(); }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        p.setPen(QPen(col::grid, 1));
        p.drawLine(QPointF(r.center().x(), r.top()), QPointF(r.center().x(), r.bottom()));
        p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
        p.setPen(QPen(col::gridStrong, 1, Qt::DashLine));
        p.drawLine(r.bottomLeft(), r.topRight());
        const float drive = dsp::dbToGain(float(m_body->real(0)));
        const float comp = 1.0f / std::sqrt(std::max(1.0f, drive * 0.5f));
        const int type = m_body->pick(4);
        auto y = [&](double x) { return r.center().y() - std::clamp(double(dsp::saturate(type, float(x) * drive) * comp), -1.2, 1.2) / 1.2 * r.height() / 2; };
        QPainterPath path;
        for (int i = 0; i <= int(r.width()); ++i) {
            const double x = -1 + 2.0 * i / r.width();
            const QPointF pt(r.left() + i, y(x));
            if (i == 0) path.moveTo(pt); else path.lineTo(pt);
        }
        p.setPen(QPen(col::curve, 1.7));
        p.drawPath(path);
        const double in = std::min(1.0, double(m_peak) / drive);
        if (in > 0.01) {
            const double px = r.center().x() + in * r.width() / 2;
            p.setPen(Qt::NoPen);
            p.setBrush(col::cyan);
            p.drawEllipse(QPointF(px, y(in)), 3, 3);
        }
        drawCaption(p, r, m_body->text(0), Qt::AlignTop | Qt::AlignLeft);
    }
    void mousePressEvent(QMouseEvent* e) override { m_y = e->position().y(); m_start = m_body->pos(0); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton)) return;
        m_body->editMany({{0, m_start + (m_y - e->position().y()) / 150.0}}, 0);
    }
private:
    double m_y = 0, m_start = 0;
    float m_peak = 0;
};

class SaturatorBody : public DeviceBody {
public:
    SaturatorBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = rootRow(this);
        auto* left = vbox(4);
        left->addWidget(chooser(4, 150));
        auto* disp = new ShaperDisplay(this, 150, 150);
        animate(disp);
        left->addWidget(disp);
        left->addStretch(1);
        root->addLayout(left);
        root->addWidget(vline(this));
        auto* right = vbox(2);
        right->addLayout(knobGrid(this, {0, 1, 3, 2}, 2, 28));
        right->addStretch(1);
        root->addLayout(right);
        setFixedWidth(290);
    }
};

// ============================================================================ LFO display (Auto Filter, Auto Pan)

// One cycle of the LFO with a cursor where it is now (the processor's phase meter).
class LfoDisplay : public Display {
public:
    LfoDisplay(DeviceBody* b, int w, int h, std::function<int()> shape, int phaseMeter, std::function<double()> amount)
        : Display(b, w, h), m_shape(std::move(shape)), m_meter(phaseMeter), m_amount(std::move(amount)) {
        InfoView::attach(this, "LFO", "The shape the LFO moves in, one cycle across. The line shows where it is now.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        p.setPen(QPen(col::grid, 1));
        p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
        const int shape = m_shape();
        const double amt = 0.25 + 0.75 * m_amount();
        QPainterPath path;
        for (int i = 0; i <= int(r.width()); ++i) {
            const double ph = double(i) / r.width() * 0.9999;
            const double v = shape == 5 ? dsp::lfoShape(5, std::floor(ph * 4) + 3.0) : dsp::lfoShape(shape, ph);
            const QPointF pt(r.left() + i, r.center().y() - v * amt * (r.height() / 2 - 4));
            if (i == 0) path.moveTo(pt); else path.lineTo(pt);
        }
        p.setPen(QPen(col::cyan, 1.4));
        p.drawPath(path);
        const double ph = m_body->meter(m_meter);
        const double x = r.left() + ph * r.width();
        p.setPen(QPen(col::handle, 1));
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
    }
private:
    std::function<int()> m_shape;
    int m_meter;
    std::function<double()> m_amount;
};

// ============================================================================ Auto Filter

class AutoFilterBody : public DeviceBody {
public:
    AutoFilterBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = rootRow(this);
        auto* a = vbox(3);
        a->addWidget(choices(4, {}, filterIcon, 96));
        auto* fd = new FilterDisplay(this, 200, 100, 0, 1, 4, 0, [this] { return meter(0); });
        animate(fd);
        a->addWidget(fd);
        auto* ar = hbox(6);
        ar->addStretch(1);
        ar->addWidget(knob(0, 30));
        ar->addWidget(knob(1, 30));
        ar->addStretch(1);
        a->addLayout(ar);
        a->addStretch(1);
        root->addLayout(a);
        root->addWidget(vline(this));
        auto* b = vbox(3);
        b->addWidget(section("LFO"));
        b->addWidget(choices(6, {}, lfoIcon, 132));
        auto* lfo = new LfoDisplay(this, 132, 52, [this] { return pick(6); }, 1, [this] { return pos(2); });
        animate(lfo);
        b->addWidget(lfo);
        auto* br = hbox(4);
        br->addWidget(knob(2, 26, "Amount"));
        auto* rateCol = vbox(1);
        rateCol->addWidget(section("Rate"));
        rateCol->addWidget(chooser(5, 64));
        rateCol->addStretch(1);
        br->addLayout(rateCol);
        br->addStretch(1);
        b->addLayout(br);
        b->addStretch(1);
        root->addLayout(b);
        root->addWidget(vline(this));
        auto* cc = vbox(2);
        cc->addWidget(knob(7, 26, "Env", true));
        cc->addWidget(knob(3, 26));
        cc->addStretch(1);
        root->addLayout(cc);
        root->addWidget(vline(this));
        root->addLayout(sidechainSection(this, T::AutoFilter));
        setFixedWidth(574);
    }
};

// ============================================================================ Delay

// The echoes over two bars: left repeats above the line, right below, each as loud as
// the feedback leaves it. Drag up or down for more or less feedback.
class TapDisplay : public Display {
public:
    TapDisplay(DeviceBody* b, int w, int h) : Display(b, w, h) {
        setCursor(Qt::SizeVerCursor);
        InfoView::attach(this, "Echoes", "The repeats over two bars: left side above the line, right side below. Drag up or down to change the feedback.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const double beats = 8;
        for (int bt = 1; bt < 8; ++bt) {
            p.setPen(QPen(bt % 4 ? col::grid : col::gridStrong, 1));
            const double x = std::round(r.left() + r.width() * bt / beats) + 0.5;
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        }
        p.setPen(QPen(col::gridStrong, 1));
        p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
        const double tl = m_body->real(0), tr = m_body->on(5) ? tl : m_body->real(6), fb = m_body->real(1);
        const bool ping = m_body->on(4);
        const double half = r.height() / 2 - 6;
        auto tap = [&](double beat, double level, bool left) {
            if (beat > beats || level < 0.02) return;
            const double x = r.left() + beat / beats * r.width();
            const double h = level * half;
            p.fillRect(QRectF(x - 1.5, left ? r.center().y() - h : r.center().y(), 3, h), left ? col::curve : col::cyan);
        };
        p.fillRect(QRectF(r.left() + 1, r.center().y() - half, 3, half * 2), col::gridText);   // the dry note
        if (ping) {
            double t = tl, level = 1;
            for (int k = 0; k < 24 && t <= beats; ++k) {
                const bool left = k % 2 == 0;
                tap(t, level, left);
                t += left ? tr : tl;
                level *= fb;
            }
        } else {
            double level = 1;
            for (int k = 1; k < 24; ++k, level *= fb) { tap(tl * k, level, true); tap(tr * k, level, false); }
        }
        p.setPen(col::gridText);
        p.drawText(r.adjusted(4, 2, -4, -2), Qt::AlignTop | Qt::AlignRight, "Feedback " + m_body->text(1));
    }
    void mousePressEvent(QMouseEvent* e) override { m_y = e->position().y(); m_start = m_body->pos(1); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (e->buttons() & Qt::LeftButton) m_body->editMany({{1, m_start + (m_y - e->position().y()) / 120.0}}, 1);
    }
private:
    double m_y = 0, m_start = 0;
};

class DelayBody : public DeviceBody {
public:
    DelayBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = rootRow(this);
        auto* a = vbox(4);
        auto* times = hbox(4);
        times->addWidget(section("L"));
        times->addWidget(chooser(0, 64));
        times->addWidget(toggle(5, "Link", 34));
        times->addWidget(section("R"));
        times->addWidget(chooser(6, 64));
        times->addStretch(1);
        a->addLayout(times);
        a->addWidget(new TapDisplay(this, 230, 118));
        auto* opts = hbox(4);
        opts->addWidget(toggle(4, "Ping Pong", 70));
        opts->addStretch(1);
        a->addLayout(opts);
        a->addStretch(1);
        root->addLayout(a);
        root->addWidget(vline(this));
        auto* b = vbox(2);
        b->addLayout(knobGrid(this, {1, 2, 7, 3}, 2, 26));
        b->addStretch(1);
        root->addLayout(b);
        setFixedWidth(370);
    }
};

// ============================================================================ Reverb

// The room's response: the dry hit, pre-delay, early reflections, then the tail ringing
// out for as long as Decay and Size make it; its colour darkens with Damping. Drag across
// for Decay, up and down for Size.
class ReverbDisplay : public Display {
public:
    ReverbDisplay(DeviceBody* b, int w, int h) : Display(b, w, h) {
        setCursor(Qt::SizeAllCursor);
        InfoView::attach(this, "Reverb", "The tail over five seconds. Drag left and right for Decay, up and down for Size.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const double secs = 5;
        for (int s = 1; s < 5; ++s) {
            p.setPen(QPen(col::grid, 1));
            const double x = std::round(r.left() + r.width() * s / secs) + 0.5;
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
            p.setPen(col::gridText);
            p.drawText(QPointF(x + 2, r.bottom() - 2), QString("%1s").arg(s));
        }
        const double size = m_body->pos(0), decay = m_body->pos(1), damp = m_body->pos(2);
        const double pre = m_body->real(4) / 1000.0;
        const bool freeze = m_body->on(8);
        // Freeverb's feedback g gives T60 ≈ -3 · loop / log10(g); loops ~ 25..37 ms · size
        const double g = 0.7 + 0.28 * decay, loop = 0.03 * (0.6 + 0.9 * size);
        const double t60 = freeze ? 1e9 : std::min(30.0, -3.0 * loop / std::log10(g));
        const double x0 = r.left() + pre / secs * r.width();
        const double top = r.top() + 10 + (1 - size) * r.height() * 0.35, bottom = r.bottom() - 12;
        p.fillRect(QRectF(r.left() + 1, top - 6, 2, bottom - top + 6), col::gridText);
        for (int k = 0; k < 6; ++k) {   // early reflections
            const double t = pre + 0.008 * (k + 1) * (0.6 + 0.9 * size);
            const double x = r.left() + t / secs * r.width();
            const double h = (bottom - top) * (0.9 - k * 0.1);
            p.fillRect(QRectF(x, bottom - h, 1.5, h), theme::withAlpha(theme::devHighlight, 70));
        }
        QPainterPath tail;   // in dB: a straight fall to -60 dB at T60
        tail.moveTo(x0, bottom);
        for (int i = 0; i <= 60; ++i) {
            const double t = secs * i / 60.0;
            const double level = std::max(0.0, 1.0 - t / t60);
            tail.lineTo(x0 + t / secs * r.width(), bottom - (bottom - top) * level);
        }
        tail.lineTo(r.right() + 2, bottom);
        tail.closeSubpath();
        QLinearGradient grad(r.topLeft(), r.topRight());
        QColor bright = col::cyan, dark = theme::devCyanDark;
        QColor mid(int(lerp(bright.red(), dark.red(), damp)), int(lerp(bright.green(), dark.green(), damp)), int(lerp(bright.blue(), dark.blue(), damp)));
        bright.setAlpha(150); mid.setAlpha(90);
        grad.setColorAt(0, bright);
        grad.setColorAt(1, mid);
        p.fillPath(tail, grad);
        p.setPen(QPen(col::cyan, 1.2));
        p.setBrush(Qt::NoBrush);
        p.drawPath(tail);
        p.setPen(col::gridText);
        p.drawText(r.adjusted(4, 2, -4, -2), Qt::AlignTop | Qt::AlignRight,
                   freeze ? QString("Frozen") : t60 < 10 ? QString("%1 s").arg(t60, 0, 'f', 1) : QString("long"));
    }
    void mousePressEvent(QMouseEvent* e) override { m_from = e->position(); m_decay = m_body->pos(1); m_size = m_body->pos(0); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton)) return;
        const QPointF d = e->position() - m_from;
        m_body->editMany({{1, m_decay + d.x() / 160.0}, {0, m_size - d.y() / 120.0}}, 0);
    }
private:
    QPointF m_from;
    double m_decay = 0, m_size = 0;
};

class ReverbBody : public DeviceBody {
public:
    ReverbBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = rootRow(this);
        auto* a = vbox(4);
        a->addWidget(new ReverbDisplay(this, 180, 150));
        auto* fr = hbox(4);
        fr->addWidget(toggle(8, "Freeze", 54));
        fr->addStretch(1);
        a->addLayout(fr);
        a->addStretch(1);
        root->addLayout(a);
        root->addWidget(vline(this));
        auto* b = vbox(2);
        b->addLayout(knobGrid(this, {0, 1, 2, 3, 4, 5, 6, 7}, 4, 24));
        b->addStretch(1);
        root->addLayout(b);
        setFixedWidth(400);
    }
};

// ============================================================================ Chorus

// The delay times of the moving copies over one LFO cycle, with dots where they are now.
class ChorusDisplay : public Display {
public:
    ChorusDisplay(DeviceBody* b, int w, int h) : Display(b, w, h) {
        setCursor(Qt::SizeVerCursor);
        InfoView::attach(this, "Chorus", "How the copies' delays move over one LFO cycle. Drag up or down for more or less depth.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const int mode = m_body->pick(4);
        const double depth = m_body->pos(1), width = m_body->pos(6);
        std::vector<std::pair<double, QColor>> taps;
        if (mode == 1) taps = {{0, col::curve}, {1.0 / 3, col::handle}, {2.0 / 3, col::cyan}};
        else if (mode == 2) taps = {{0, col::curve}};
        else taps = {{0, col::curve}, {0.5 * width, col::cyan}};
        const double ph = m_body->meter(0);
        const double h = (r.height() / 2 - 8) * (0.2 + 0.8 * depth);
        p.setPen(QPen(col::grid, 1));
        p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
        for (const auto& [off, color] : taps) {
            QPainterPath path;
            for (int i = 0; i <= int(r.width()); ++i) {
                const double v = std::sin(2 * M_PI * (double(i) / r.width() + off));
                const QPointF pt(r.left() + i, r.center().y() - v * h);
                if (i == 0) path.moveTo(pt); else path.lineTo(pt);
            }
            p.setPen(QPen(color, 1.3));
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
            const double v = std::sin(2 * M_PI * (ph + off));
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            p.drawEllipse(QPointF(r.left() + ph * r.width(), r.center().y() - v * h), 3, 3);
        }
        drawCaption(p, r, m_body->text(0), Qt::AlignTop | Qt::AlignRight);
    }
    void mousePressEvent(QMouseEvent* e) override { m_y = e->position().y(); m_start = m_body->pos(1); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (e->buttons() & Qt::LeftButton) m_body->editMany({{1, m_start + (m_y - e->position().y()) / 120.0}}, 1);
    }
private:
    double m_y = 0, m_start = 0;
};

class ChorusBody : public DeviceBody {
public:
    ChorusBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = rootRow(this);
        auto* a = vbox(4);
        a->addWidget(choices(4, {}, {}, 150));
        auto* disp = new ChorusDisplay(this, 150, 128);
        animate(disp);
        a->addWidget(disp);
        a->addStretch(1);
        root->addLayout(a);
        root->addWidget(vline(this));
        auto* b = vbox(2);
        b->addLayout(knobGrid(this, {0, 1, 5, 2, 6, 3}, 3, 24));
        b->addStretch(1);
        root->addLayout(b);
        setFixedWidth(320);
    }
};

// ============================================================================ Phaser / Flanger

// The response now: the phaser's notches or the flanger's comb, moving with the sweep.
class PhaserDisplay : public Display {
public:
    PhaserDisplay(DeviceBody* b, int w, int h) : Display(b, w, h) {
        setCursor(Qt::SizeAllCursor);
        InfoView::attach(this, "Phaser", "The notches the effect cuts, where they are now. Drag left and right to move the centre, up and down for the amount.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        drawFreqGrid(p, r, 0);
        const bool flanger = m_body->pick(4) == 1;
        const double live = m_body->meter(0);
        const double fc = live > 1 ? live : m_body->real(5);
        const int stages = int(m_body->real(6));
        const double wet = m_body->pos(3), fb = m_body->real(2);
        QPainterPath path;
        for (int i = 0; i <= int(r.width()); i += 1) {
            const double f = xToFreq(r.left() + i, r);
            std::complex<double> h;
            if (flanger) {
                const std::complex<double> z = std::polar(1.0, -2 * M_PI * f / fc);
                h = (1 - wet) + wet * z / (1.0 - fb * z);
            } else {
                const double phase = -2 * std::atan(f / fc) * stages;
                const std::complex<double> ap = std::polar(1.0, phase);
                h = (1 - wet) + wet * ap / (1.0 - fb * ap);
            }
            const double db = 20 * std::log10(std::max(1e-4, std::abs(h)));
            const QPointF pt(r.left() + i, r.top() + 10 + (6 - std::clamp(db, -30.0, 6.0)) / 36.0 * (r.height() - 14));
            if (i == 0) path.moveTo(pt); else path.lineTo(pt);
        }
        QPainterPath fill = path;
        fill.lineTo(r.right(), r.bottom()); fill.lineTo(r.left(), r.bottom()); fill.closeSubpath();
        p.fillPath(fill, col::curveFill);
        p.setPen(QPen(col::curve, 1.4));
        p.drawPath(path);
        drawHandle(p, QPointF(freqToX(m_body->real(5), r), r.top() + 6 + (1 - m_body->pos(1)) * (r.height() - 20)), col::handle);
    }
    void mousePressEvent(QMouseEvent* e) override { mouseMoveEvent(e); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton)) return;
        const QRectF r = inner();
        m_body->editMany({{5, m_body->posFor(5, xToFreq(e->position().x(), r))},
                          {1, 1 - (e->position().y() - r.top() - 6) / (r.height() - 20)}}, 5);
    }
};

class PhaserBody : public DeviceBody {
public:
    PhaserBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = rootRow(this);
        auto* a = vbox(4);
        auto* top = hbox(6);
        top->addWidget(choices(4, {}, {}, 100));
        top->addWidget(choices(6, {}, {}, 80));
        top->addStretch(1);
        a->addLayout(top);
        auto* disp = new PhaserDisplay(this, 186, 128);
        animate(disp);
        a->addWidget(disp);
        a->addStretch(1);
        root->addLayout(a);
        root->addWidget(vline(this));
        auto* b = vbox(2);
        b->addLayout(knobGrid(this, {0, 1, 5, 2, 7, 3}, 3, 24));
        b->addStretch(1);
        root->addLayout(b);
        setFixedWidth(356);
    }
};

// ============================================================================ Utility

class UtilityBody : public DeviceBody {
public:
    UtilityBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(8, 6, 8, 6);
        root->setSpacing(4);
        auto* inv = hbox(3);
        inv->addWidget(toggle(4, "Ø L", 32));
        inv->addWidget(toggle(5, "Ø R", 32));
        inv->addStretch(1);
        root->addLayout(inv);
        auto* row = hbox(6);
        auto* c1 = vbox(2);
        c1->addWidget(knob(0, 30));
        c1->addWidget(knob(1, 26, {}, true));
        c1->addStretch(1);
        auto* c2 = vbox(4);
        c2->addWidget(knob(2, 26));
        c2->addWidget(toggle(3, "Mono", 46), 0, Qt::AlignHCenter);
        c2->addWidget(toggle(6, "Mute", 46), 0, Qt::AlignHCenter);
        c2->addStretch(1);
        row->addLayout(c1);
        row->addLayout(c2);
        row->addStretch(1);
        root->addLayout(row);
        root->addStretch(1);
        setFixedWidth(128);
    }
};

// ============================================================================ Auto Pan

// The left (orange) and right (cyan) levels over one cycle; the line is where it is now.
class PanDisplay : public Display {
public:
    PanDisplay(DeviceBody* b, int w, int h) : Display(b, w, h) {
        setCursor(Qt::SizeAllCursor);
        InfoView::attach(this, "Auto Pan", "Left level in orange, right in blue, over one cycle. Drag up or down for the amount, left and right for the phase between the sides.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        static const int shapes[] = {0, 1, 4, 5};
        const int shape = shapes[std::clamp(m_body->pick(2), 0, 3)];
        const double amount = m_body->pos(0), phase = m_body->real(3) / 360.0;
        p.setPen(QPen(col::grid, 1));
        p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
        for (int side = 0; side < 2; ++side) {
            QPainterPath path;
            for (int i = 0; i <= int(r.width()); ++i) {
                const double ph = double(i) / r.width() * 0.9999 + (side ? phase : 0.0);
                const double v = shape == 5 ? dsp::lfoShape(5, std::floor(ph * 4) + 5.0) : dsp::lfoShape(shape, ph);
                const double g = 1 - amount * (0.5 + 0.5 * v);
                const QPointF pt(r.left() + i, r.bottom() - 4 - g * (r.height() - 12));
                if (i == 0) path.moveTo(pt); else path.lineTo(pt);
            }
            p.setPen(QPen(side ? col::cyan : col::curve, 1.4));
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
        }
        const double x = r.left() + m_body->meter(0) * r.width();
        p.setPen(QPen(col::handle, 1));
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
    }
    void mousePressEvent(QMouseEvent* e) override { m_from = e->position(); m_amount = m_body->pos(0); m_phase = m_body->pos(3); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton)) return;
        const QPointF d = e->position() - m_from;
        m_body->editMany({{0, m_amount - d.y() / 120.0}, {3, m_phase + d.x() / 200.0}}, 0);
    }
private:
    QPointF m_from;
    double m_amount = 0, m_phase = 0;
};

class AutoPanBody : public DeviceBody {
public:
    AutoPanBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = rootRow(this);
        auto* a = vbox(4);
        auto* top = hbox(6);
        top->addWidget(choices(2, {}, [](QPainter& p, const QRectF& r, int k, const QColor& c) { static const int s[] = {0, 1, 4, 5}; lfoIcon(p, r, s[k], c); }, 92));
        top->addWidget(chooser(1, 64));
        top->addStretch(1);
        a->addLayout(top);
        auto* disp = new PanDisplay(this, 180, 128);
        animate(disp);
        a->addWidget(disp);
        a->addStretch(1);
        root->addLayout(a);
        root->addWidget(vline(this));
        auto* b = vbox(2);
        b->addWidget(knob(0, 26));
        b->addWidget(knob(3, 26));
        b->addWidget(knob(4, 26));
        b->addStretch(1);
        root->addLayout(b);
        setFixedWidth(266);
    }
};

// ============================================================================ Redux

// A sine through the crusher: held at the lower rate, rounded to the fewer bits.
class ReduxDisplay : public Display {
public:
    ReduxDisplay(DeviceBody* b, int w, int h) : Display(b, w, h) {
        setCursor(Qt::SizeAllCursor);
        InfoView::attach(this, "Redux", "A sine through the crusher. Drag left and right for the rate, up and down for the bits.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const double bits = m_body->real(0), rate = m_body->real(1);
        const double samples = 220;   // 2 cycles of ~436 Hz at 48 kHz across the display
        const double steps = std::pow(2.0, bits - 1);
        auto y = [&](double v) { return r.center().y() - v * (r.height() / 2 - 8); };
        QPainterPath clean, crushed;
        double held = 0, acc = 1;
        for (int i = 0; i <= int(samples); ++i) {
            const double v = std::sin(2 * M_PI * 2 * i / samples);
            const double x = r.left() + r.width() * i / samples;
            if (i == 0) clean.moveTo(x, y(v)); else clean.lineTo(x, y(v));
            acc += std::min(1.0, rate / kSr);
            if (acc >= 1) { acc -= std::floor(acc); held = bits >= 16 ? v : std::round(v * steps) / steps; }
            if (i == 0) crushed.moveTo(x, y(held));
            else { crushed.lineTo(x, crushed.currentPosition().y()); crushed.lineTo(x, y(held)); }
        }
        p.setPen(QPen(col::gridStrong, 1));
        p.drawPath(clean);
        p.setPen(QPen(col::curve, 1.5));
        p.drawPath(crushed);
        drawCaption(p, r, m_body->text(0) + "   " + m_body->text(1), Qt::AlignTop | Qt::AlignLeft);
    }
    void mousePressEvent(QMouseEvent* e) override { m_from = e->position(); m_bits = m_body->pos(0); m_rate = m_body->pos(1); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton)) return;
        const QPointF d = e->position() - m_from;
        m_body->editMany({{1, m_rate + d.x() / 200.0}, {0, m_bits - d.y() / 150.0}}, 0);
    }
private:
    QPointF m_from;
    double m_bits = 0, m_rate = 0;
};

class ReduxBody : public DeviceBody {
public:
    ReduxBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = rootRow(this);
        root->addWidget(new ReduxDisplay(this, 150, 150), 0, Qt::AlignTop);
        root->addWidget(vline(this));
        auto* b = vbox(2);
        b->addLayout(knobGrid(this, {0, 1, 2, 3}, 2, 26));
        b->addStretch(1);
        root->addLayout(b);
        setFixedWidth(282);
    }
};

// ============================================================================ Gate

// The input level over the last few seconds with the threshold (drag it) and the return
// line; shaded where the gate was shut.
class GateDisplay : public Display {
public:
    GateDisplay(DeviceBody* b, int w, int h) : Display(b, w, h) {
        setCursor(Qt::SizeVerCursor);
        InfoView::attach(this, "Gate", "The input level over the last seconds. Drag the line to set the threshold; the dashed line is where it closes again (Return).");
    }
    void tick() override {
        m_history.push_back({m_body->meter(0), m_body->meter(1)});
        while (int(m_history.size()) > width()) m_history.pop_front();
        update();
    }
protected:
    double y(double db, const QRectF& r) const { return r.top() + (std::clamp(-db, 0.0, 70.0) / 70.0) * r.height(); }
    void draw(QPainter& p, const QRectF& r) override {
        const int n = int(m_history.size());
        for (int i = 0; i < n; ++i) {
            const double x = r.right() - (n - 1 - i);
            const auto& [lvl, g] = m_history[size_t(i)];
            if (g < 0.5f) p.fillRect(QRectF(x, r.top(), 1, r.height()), QColor(0, 0, 0, 90));
            const double yy = y(lvl, r);
            p.fillRect(QRectF(x, yy, 1, r.bottom() - yy), g >= 0.5f ? col::cyan : col::gridStrong);
        }
        const double thr = m_body->real(0), ret = thr - m_body->real(5);
        p.setPen(QPen(col::curve, 1.5));
        p.drawLine(QPointF(r.left(), y(thr, r)), QPointF(r.right(), y(thr, r)));
        p.setPen(QPen(col::curve, 1, Qt::DashLine));
        p.drawLine(QPointF(r.left(), y(ret, r)), QPointF(r.right(), y(ret, r)));
        const bool open = m_body->meter(1) >= 0.5f;
        p.setPen(Qt::NoPen);
        p.setBrush(open ? theme::play : theme::devPad);
        p.drawEllipse(QPointF(r.right() - 8, r.top() + 8), 4, 4);
        drawCaption(p, r, QString("%1 dB").arg(thr, 0, 'f', 1), Qt::AlignTop | Qt::AlignLeft);
    }
    void mousePressEvent(QMouseEvent* e) override { mouseMoveEvent(e); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton)) return;
        const QRectF r = inner();
        const double db = -(e->position().y() - r.top()) / r.height() * 70.0;
        m_body->editMany({{0, m_body->posFor(0, db)}}, 0);
    }
private:
    std::deque<std::pair<float, float>> m_history;
};

class GateBody : public DeviceBody {
public:
    GateBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = rootRow(this);
        auto* disp = new GateDisplay(this, 160, 170);
        animate(disp);
        root->addWidget(disp, 0, Qt::AlignTop);
        root->addWidget(vline(this));
        auto* b = vbox(2);
        b->addLayout(knobGrid(this, {0, 5, 4, 1, 2, 3}, 3, 24));
        b->addStretch(1);
        root->addLayout(b);
        root->addWidget(vline(this));
        root->addLayout(sidechainSection(this, T::Gate));
        setFixedWidth(470);
    }
};

// ============================================================================ Limiter

// Gain reduction over the last few seconds, hanging from the top.
class LimiterDisplay : public Display {
public:
    LimiterDisplay(DeviceBody* b, int w, int h) : Display(b, w, h) {
        InfoView::attach(this, "Limiter", "How much it turned the peaks down over the last seconds.");
    }
    void tick() override {
        m_history.push_back(m_body->meter(0));
        while (int(m_history.size()) > width()) m_history.pop_front();
        update();
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        for (int db = 3; db <= 12; db += 3) {
            const double yy = std::round(r.top() + 12 + db / 12.0 * (r.height() - 14)) + 0.5;
            p.setPen(QPen(col::grid, 1));
            p.drawLine(QPointF(r.left(), yy), QPointF(r.right(), yy));
            p.setPen(col::gridText);
            p.drawText(QPointF(r.left() + 2, yy - 2), QString("-%1").arg(db));
        }
        const int n = int(m_history.size());
        for (int i = 0; i < n; ++i) {
            const double h = std::clamp(double(m_history[size_t(i)]), 0.0, 12.0) / 12.0 * (r.height() - 14);
            p.fillRect(QRectF(r.right() - (n - 1 - i), r.top() + 12, 1, h), col::curve);
        }
        drawCaption(p, r, "Ceiling " + m_body->text(1), Qt::AlignTop | Qt::AlignRight);
    }
private:
    std::deque<float> m_history;
};

class LimiterBody : public DeviceBody {
public:
    LimiterBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = rootRow(this);
        auto* disp = new LimiterDisplay(this, 140, 170);
        animate(disp);
        root->addWidget(disp, 0, Qt::AlignTop);
        root->addWidget(vline(this));
        auto* b = vbox(2);
        b->addWidget(knob(0, 28));
        b->addWidget(knob(1, 26));
        b->addWidget(knob(2, 24));
        b->addStretch(1);
        root->addLayout(b);
        setFixedWidth(222);
    }
};

// ============================================================================ Macros

// Eight knobs; right-click one to map it to a knob of another device on the track. The list
// shows every mapping with its range: the macro at 0 sets the knob to Min, at 1 to Max.
class MacrosBody : public DeviceBody {
public:
    MacrosBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = rootRow(this);
        auto* g = new QGridLayout;
        g->setContentsMargins(0, 0, 0, 0);
        g->setHorizontalSpacing(2);
        g->setVerticalSpacing(6);
        for (int k = 0; k < macros::kKnobs; ++k) {
            QWidget* cell = knob(k, 28, macroLabel(k));
            cell->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(cell, &QWidget::customContextMenuRequested, this, [this, k, cell](const QPoint& at) { macroMenu(k, cell->mapToGlobal(at)); });
            g->addWidget(cell, k / 4, k % 4);
        }
        auto* knobs = vbox(4);
        knobs->addLayout(g);
        knobs->addStretch(1);
        root->addLayout(knobs);
        root->addWidget(vline(this));

        auto* right = vbox(3);
        auto* head = hbox(4);
        head->addWidget(section("Mappings"));
        head->addStretch(1);
        auto* add = new QToolButton(this);
        add->setText("Map…");
        add->setFocusPolicy(Qt::NoFocus);
        add->setStyleSheet("QToolButton{font-size:9px;padding:1px 6px;}");
        InfoView::attach(add, "Map a Macro", "Pick a macro, then a knob of another device on this track. Right-clicking a macro does the same.");
        connect(add, &QToolButton::clicked, this, [this, add] {
            QMenu menu(this);
            for (int k = 0; k < macros::kKnobs; ++k) fillMacroTargets(menu.addMenu(macroLabel(k)), k);
            menu.exec(add->mapToGlobal(QPoint(0, add->height())));
        });
        head->addWidget(add);
        right->addLayout(head);
        auto* scroll = new QScrollArea(this);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setFixedSize(312, 150);
        scroll->setStyleSheet("QScrollArea{background:transparent;}");
        auto* list = new QWidget(scroll);
        list->setStyleSheet("background:transparent;");
        auto* rows = vbox(2);
        list->setLayout(rows);
        const Device* d = device();
        if (!d || d->maps.isEmpty()) {
            auto* hint = new QLabel("Right-click a macro (or click Map…) to have it turn knobs of the other devices on this track.", list);
            hint->setWordWrap(true);
            hint->setStyleSheet("color:#6c6c6c;font-size:9px;");
            rows->addWidget(hint);
        } else {
            for (int r = 0; r < d->maps.size(); ++r) rows->addLayout(macroMapRow(list, d->maps[r], r, 176));
        }
        rows->addStretch(1);
        scroll->setWidget(list);
        right->addWidget(scroll);
        right->addStretch(1);
        root->addLayout(right);
        setFixedWidth(560);
    }
};

} // namespace

DeviceBody* createEffectBody(const DeviceContext& ctx, QWidget* parent) {
    switch (ctx.type) {
    case T::Macros:     return new MacrosBody(ctx, parent);
    case T::EQ:         return new EqBody(ctx, parent);
    case T::Compressor: return new CompressorBody(ctx, parent);
    case T::Saturator:  return new SaturatorBody(ctx, parent);
    case T::AutoFilter: return new AutoFilterBody(ctx, parent);
    case T::Delay:      return new DelayBody(ctx, parent);
    case T::Reverb:     return new ReverbBody(ctx, parent);
    case T::Chorus:     return new ChorusBody(ctx, parent);
    case T::Phaser:     return new PhaserBody(ctx, parent);
    case T::Utility:    return new UtilityBody(ctx, parent);
    case T::AutoPan:    return new AutoPanBody(ctx, parent);
    case T::Redux:      return new ReduxBody(ctx, parent);
    case T::Gate:       return new GateBody(ctx, parent);
    case T::Limiter:    return new LimiterBody(ctx, parent);
    default:            return nullptr;
    }
}

} // namespace sessions::devui
