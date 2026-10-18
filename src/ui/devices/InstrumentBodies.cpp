// The instruments' panels. Parameter indices follow core/DeviceSpecs.cpp.

#include "Bodies.h"
#include "audio/Dsp.h"
#include "audio/Wavetables.h"
#include "audio/Devices.h"
#include "core/Model.h"
#include "core/Scales.h"
#include "core/Transport.h"
#include "core/Import.h"
#include "ui/Browser.h"
#include "ui/DeviceDrag.h"
#include "ui/InfoView.h"
#include "widgets/DragValue.h"
#include "widgets/Knob.h"
#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QContextMenuEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMenu>
#include <QMimeData>
#include <QStackedWidget>
#include <QPropertyAnimation>
#include <QGridLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QRandomGenerator>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <cmath>

namespace sessions::devui {

namespace {

using T = DeviceType;

QLabel* opHeader(QWidget* parent, const QString& text, const QColor& color) {
    auto* l = new QLabel(text, parent);
    l->setAlignment(Qt::AlignCenter);
    l->setFixedHeight(14);
    l->setStyleSheet(QString("background:%1;color:#161616;font-size:9px;font-weight:700;border-radius:2px;").arg(color.name()));
    return l;
}

QHBoxLayout* knobRow(DeviceBody* b, std::initializer_list<int> params, int size, std::initializer_list<const char*> labels = {}) {
    auto* row = hbox(0);
    auto lab = labels.begin();
    for (int i : params) {
        row->addWidget(b->knob(i, size, lab != labels.end() ? QString(*lab) : QString()));
        if (lab != labels.end()) ++lab;
    }
    row->addStretch(1);
    return row;
}

// ============================================================================ Analog

class AnalogBody : public DeviceBody {
public:
    AnalogBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = rootRow(this);
        // oscillators
        auto* osc = vbox(4);
        osc->addWidget(section("Oscillators"));
        for (int i : {4, 5}) {
            auto* r = hbox(4);
            r->addWidget(section(i == 4 ? "1" : "2"));
            r->addWidget(choices(i, {}, waveIcon, 100));
            r->addStretch(1);
            osc->addLayout(r);
        }
        auto* oct = hbox(4);
        oct->addWidget(section("Oct 2"));
        oct->addWidget(choices(6, {}, {}, 72));
        oct->addStretch(1);
        osc->addLayout(oct);
        osc->addLayout(knobRow(this, {3, 7, 8}, 22));
        osc->addStretch(1);
        root->addLayout(osc);
        root->addWidget(vline(this));
        // filter
        auto* flt = vbox(4);
        auto* fh = hbox(4);
        fh->addWidget(section("Filter"));
        fh->addStretch(1);
        fh->addWidget(choices(9, {}, filterIcon, 66));
        flt->addLayout(fh);
        flt->addWidget(new FilterDisplay(this, 176, 74, 0, 1, 9));
        flt->addLayout(knobRow(this, {0, 1, 2, 10}, 22, {"Cutoff", "Reso", "Env", "Key"}));
        flt->addStretch(1);
        root->addLayout(flt);
        root->addWidget(vline(this));
        // envelopes
        auto* env = vbox(2);
        env->addWidget(new AdsrDisplay(this, 150, 40, 11, 12, 13, 14, col::cyan, "Filter"));
        env->addLayout(knobRow(this, {11, 12, 13, 14}, 18, {"A", "D", "S", "R"}));
        env->addWidget(new AdsrDisplay(this, 150, 40, 15, 16, 17, 18, col::curve, "Amp"));
        env->addLayout(knobRow(this, {15, 16, 17, 18}, 18, {"A", "D", "S", "R"}));
        env->addStretch(1);
        root->addLayout(env);
        root->addWidget(vline(this));
        auto* glob = vbox(2);
        glob->addWidget(knob(19, 24));
        glob->addWidget(knob(20, 26));
        glob->addStretch(1);
        root->addLayout(glob);
        setFixedWidth(604);
    }
};

// ============================================================================ FM

// The algorithm as boxes: modulators above what they modulate, heard operators on the
// bottom row. Click for the next one, or use the mouse wheel.
class AlgoDisplay : public Display {
public:
    AlgoDisplay(DeviceBody* b, int w, int h) : Display(b, w, h) {
        setCursor(Qt::PointingHandCursor);
        setProperty("mapTarget", b->target(4));
        InfoView::attach(this, "Algorithm", "Which operators modulate which. Operators on the bottom row are heard; the loop marks the one that feeds back on itself. "
                                            "Click for the next arrangement, or use the mouse wheel.");
    }
protected:
    struct Op { int op; double col, row; };
    static void layout(int alg, std::vector<Op>& ops, std::vector<std::pair<int, int>>& links, int& fb) {
        switch (alg) {
        case 0: ops = {{0, 1, 0}, {1, 1, 1}}; links = {{1, 0}}; fb = 1; break;
        case 1: ops = {{0, 1, 0}, {1, 1, 1}, {2, 1, 2}}; links = {{2, 1}, {1, 0}}; fb = 2; break;
        case 2: ops = {{0, 1.5, 0}, {1, 1.5, 1}, {2, 1.5, 2}, {3, 1.5, 3}}; links = {{3, 2}, {2, 1}, {1, 0}}; fb = 3; break;
        case 3: ops = {{0, 1.5, 0}, {1, 1, 1}, {2, 2, 1}}; links = {{1, 0}, {2, 0}}; fb = 2; break;
        case 4: ops = {{0, 1.5, 0}, {1, 0.5, 1}, {2, 1.5, 1}, {3, 2.5, 1}}; links = {{1, 0}, {2, 0}, {3, 0}}; fb = 3; break;
        case 5: ops = {{0, 0.75, 0}, {1, 0.75, 1}, {2, 2.25, 0}, {3, 2.25, 1}}; links = {{1, 0}, {3, 2}}; fb = 3; break;
        default: ops = {{0, 0, 0}, {1, 1, 0}, {2, 2, 0}, {3, 3, 0}}; links = {}; fb = 0; break;
        }
    }
    void draw(QPainter& p, const QRectF& r) override {
        static const QColor colors[4] = {QColor("#ffa63c"), QColor("#ffd166"), QColor("#6ad0ff"), QColor("#b58cff")};
        static const int levelParam[4] = {-1, 1, 10, 13};
        const int alg = m_body->pick(4);
        std::vector<Op> ops; std::vector<std::pair<int, int>> links; int fb = 0;
        layout(alg, ops, links, fb);
        double rows = 1;
        for (const Op& o : ops) rows = std::max(rows, o.row + 1);
        const double cw = (r.width() - 8) / 4, ch = (r.height() - 34) / std::max(2.0, rows);
        const double bw = std::min(cw - 3, 22.0), bh = std::min(ch - 6, 18.0);
        auto center = [&](const Op& o) { return QPointF(r.left() + 4 + (o.col + 0.5) * cw, r.bottom() - 14 - (o.row + 0.5) * ch); };
        auto find = [&](int op) { for (const Op& o : ops) if (o.op == op) return o; return ops[0]; };
        p.setPen(QPen(col::label, 1.2));
        for (const auto& [from, to] : links) p.drawLine(center(find(from)), center(find(to)));
        const double outY = r.bottom() - 6;
        for (const Op& o : ops) if (o.row == 0) p.drawLine(center(o), QPointF(center(o).x(), outY));
        double minX = 1e9, maxX = -1e9;
        for (const Op& o : ops) if (o.row == 0) { minX = std::min(minX, center(o).x()); maxX = std::max(maxX, center(o).x()); }
        p.drawLine(QPointF(minX, outY), QPointF(maxX, outY));
        int unused = 0;
        for (int op = 0; op < 4; ++op) {   // operators this algorithm leaves out, dimmed in the corner
            bool used = false;
            for (const Op& o : ops) used |= o.op == op;
            if (used) continue;
            const QRectF box(r.right() - 4 - (unused + 1) * (bw * 0.7 + 3), r.top() + 14, bw * 0.7, bh * 0.7);
            p.setPen(QPen(col::gridStrong, 1));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(box, 2, 2);
            p.setPen(col::gridText);
            p.drawText(box, Qt::AlignCenter, QString(QChar('A' + op)));
            ++unused;
        }
        for (const Op& o : ops) {
            const QPointF c = center(o);
            const QRectF box(c.x() - bw / 2, c.y() - bh / 2, bw, bh);
            const double level = levelParam[o.op] < 0 ? 1.0 : m_body->pos(levelParam[o.op]);
            QColor fill = colors[o.op];
            if (level < 0.02) fill = theme::devPad;
            p.setPen(QPen(col::displayEdge, 1));
            p.setBrush(fill);
            p.drawRoundedRect(box, 2, 2);
            p.setPen(theme::ink);
            QFont f = p.font(); f.setBold(true); f.setPointSizeF(7); p.setFont(f);
            p.drawText(box, Qt::AlignCenter, QString(QChar('A' + o.op)));
            f.setBold(false); f.setPointSizeF(6.5); p.setFont(f);
            if (o.op == fb && m_body->pos(15) > 0.01) {
                p.setPen(QPen(col::curve, 1.1));
                p.setBrush(Qt::NoBrush);
                p.drawArc(QRectF(box.right() - 2, box.top() - 6, 10, 10), -90 * 16, 270 * 16);
            }
        }
        drawCaption(p, r, m_body->text(4), Qt::AlignTop | Qt::AlignHCenter);
    }
    void mousePressEvent(QMouseEvent*) override { step(1); }
    void wheelEvent(QWheelEvent* e) override { step(e->angleDelta().y() > 0 ? -1 : 1); e->accept(); }
private:
    void step(int d) { m_body->editMany({{4, choicePosition((m_body->pick(4) + d + 7) % 7, 7)}}, 4); }
};

class FmBody : public DeviceBody {
public:
    FmBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        static const QColor colors[4] = {QColor("#ffa63c"), QColor("#ffd166"), QColor("#6ad0ff"), QColor("#b58cff")};
        auto* root = rootRow(this, 5);
        auto* algo = vbox(3);
        algo->addWidget(new AlgoDisplay(this, 104, 120));
        algo->addLayout(knobRow(this, {15}, 22));
        algo->addStretch(1);
        root->addLayout(algo);
        root->addWidget(vline(this));
        // A: the operator that's always heard, with the amp envelope
        auto* a = vbox(3);
        auto* ah = hbox(4);
        ah->addWidget(opHeader(this, "A", colors[0]));
        ah->addWidget(chooser(16, 50));
        a->addLayout(ah);
        a->addWidget(new AdsrDisplay(this, 144, 44, 2, 3, 5, 6, colors[0]));
        a->addLayout(knobRow(this, {2, 3, 5, 6}, 18, {"A", "D", "S", "R"}));
        a->addStretch(1);
        root->addLayout(a);
        // B, C, D: ratio, level, decay (B also sustain: B, C and D share it)
        const int params[3][4] = {{0, 1, 7, 8}, {9, 10, 11, -1}, {12, 13, 14, -1}};
        for (int k = 0; k < 3; ++k) {
            root->addWidget(vline(this));
            auto* col = vbox(2);
            col->addWidget(opHeader(this, QString(QChar('B' + k)), colors[k + 1]));
            col->addWidget(chooser(params[k][0], 54));
            col->addWidget(knob(params[k][1], 24, "Level"));
            auto* r = hbox(0);
            r->addWidget(knob(params[k][2], 18, "Decay"));
            if (params[k][3] >= 0) r->addWidget(knob(params[k][3], 18, "Sus"));
            r->addStretch(1);
            col->addLayout(r);
            col->addStretch(1);
            root->addLayout(col);
        }
        root->addWidget(vline(this));
        auto* glob = vbox(2);
        glob->addWidget(knob(18, 22, "Vel > Mod"));
        glob->addWidget(knob(17, 26));
        glob->addStretch(1);
        root->addLayout(glob);
        setFixedWidth(560);
    }
};

// ============================================================================ Wavetable

// The waveform playing now, large, with its neighbours faint behind it; underneath, every
// frame of the table in a row with a marker at Position. Drag across to scan.
class WavetableDisplay : public Display {
public:
    WavetableDisplay(DeviceBody* b, int w, int h) : Display(b, w, h) {
        setCursor(Qt::SizeHorCursor);
        InfoView::attach(this, "Wavetable", "The waveform playing now, and the whole table in a row underneath. Drag left and right to move through it (Position).");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const audio::Wavetable& t = audio::wavetable(m_body->pick(4));
        const double pos = m_body->pos(0);
        const QRectF main(r.left() + 4, r.top() + 12, r.width() - 8, r.height() - 40);
        const QRectF strip(r.left() + 4, r.bottom() - 24, r.width() - 8, 20);
        auto wavePath = [&](double position, const QRectF& box, int points) {
            QPainterPath path;
            for (int i = 0; i <= points; ++i) {
                const float ph = float(i) / float(points) * 0.9999f;
                const float v = audio::wavetableRead(t, 0, float(position), ph);
                const QPointF pt(box.left() + box.width() * i / points, box.center().y() - v * box.height() / 2 * 0.92);
                if (i == 0) path.moveTo(pt); else path.lineTo(pt);
            }
            return path;
        };
        p.setPen(QPen(col::grid, 1));
        p.drawLine(QPointF(main.left(), main.center().y()), QPointF(main.right(), main.center().y()));
        for (double ghost : {pos - 0.14, pos + 0.14}) {
            if (ghost < 0 || ghost > 1) continue;
            p.setPen(QPen(theme::withAlpha(col::cyan, 55), 1));
            p.drawPath(wavePath(ghost, main, 128));
        }
        QPainterPath cur = wavePath(pos, main, 160);
        QPainterPath fill = cur;
        fill.lineTo(main.right(), main.center().y());
        fill.lineTo(main.left(), main.center().y());
        fill.closeSubpath();
        p.fillPath(fill, col::cyanFill);
        p.setPen(QPen(col::cyan, 1.8));
        p.drawPath(cur);
        // the table in a row
        const double fw = strip.width() / audio::kWtFrames;
        for (int f = 0; f < audio::kWtFrames; ++f) {
            const QRectF cell(strip.left() + f * fw + 1, strip.top(), fw - 2, strip.height());
            p.fillRect(cell, theme::devCell);
            p.setPen(QPen(theme::withAlpha(col::cyan, 140), 1));
            p.drawPath(wavePath(double(f) / (audio::kWtFrames - 1), cell.adjusted(2, 2, -2, -2), 32));
        }
        const double mx = strip.left() + fw / 2 + pos * (strip.width() - fw);
        p.setPen(QPen(col::curve, 2));
        p.drawLine(QPointF(mx, strip.top() - 3), QPointF(mx, strip.bottom() + 1));
        drawCaption(p, r, QString("%1   %2").arg(QString::fromUtf8(t.name)).arg(m_body->text(0)), Qt::AlignTop | Qt::AlignLeft);
    }
    void mousePressEvent(QMouseEvent* e) override { m_x = e->position().x(); m_start = m_body->pos(0); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (e->buttons() & Qt::LeftButton) m_body->editMany({{0, m_start + (e->position().x() - m_x) / 180.0}}, 0);
    }
private:
    double m_x = 0, m_start = 0;
};

class WavetableBody : public DeviceBody {
public:
    WavetableBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        audio::wavetable(0);   // build the tables now, not on the audio thread
        auto* root = rootRow(this);
        auto* osc = vbox(4);
        auto* top = hbox(4);
        top->addWidget(chooser(4, 84));
        top->addStretch(1);
        top->addWidget(section("Unison"));
        top->addWidget(choices(12, {}, {}, 54));
        osc->addLayout(top);
        osc->addWidget(new WavetableDisplay(this, 236, 104));
        osc->addLayout(knobRow(this, {0, 2, 11}, 22));
        osc->addStretch(1);
        root->addLayout(osc);
        root->addWidget(vline(this));
        auto* flt = vbox(4);
        auto* fh = hbox(4);
        fh->addWidget(section("Filter"));
        fh->addStretch(1);
        fh->addWidget(choices(6, {}, filterIcon, 66));
        flt->addLayout(fh);
        flt->addWidget(new FilterDisplay(this, 150, 74, 1, 5, 6));
        auto* fk = hbox(0);
        fk->addWidget(knob(1, 22));
        fk->addWidget(knob(5, 22));
        fk->addWidget(knob(10, 22, "Env", true));
        fk->addStretch(1);
        flt->addLayout(fk);
        flt->addStretch(1);
        root->addLayout(flt);
        root->addWidget(vline(this));
        auto* amp = vbox(3);
        amp->addWidget(section("Amp"));
        amp->addWidget(new AdsrDisplay(this, 148, 62, 3, 7, 8, 9));
        amp->addLayout(knobRow(this, {3, 7, 8, 9}, 18, {"A", "D", "S", "R"}));
        amp->addStretch(1);
        root->addLayout(amp);
        root->addWidget(vline(this));
        auto* glob = vbox(2);
        glob->addWidget(knob(13, 26));
        glob->addStretch(1);
        root->addLayout(glob);
        setFixedWidth(640);
    }
};

// ============================================================================ Sampler

// The sample's waveform with its start and end (drag them) and the loop between them.
class SampleDisplay : public Display {
public:
    SampleDisplay(DeviceBody* b, int w, int h) : Display(b, w, h) {
        setCursor(Qt::SizeHorCursor);
        InfoView::attach(this, "Sample", "Drop an audio file here. Drag the start and end markers; with Loop on, the stretch between them repeats while the key is held.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const Device* d = m_body->device();
        const SamplePtr s = d ? d->sample : nullptr;
        if (!s || s->frames() < 2) {
            p.setPen(d && !d->samplePath.isEmpty() ? theme::record : col::gridText);
            p.drawText(r, Qt::AlignCenter, d && !d->samplePath.isEmpty() ? "Missing: " + QFileInfo(d->samplePath).fileName() : "Drop a sample here");
            return;
        }
        const double dur = s->seconds();
        const double mid = r.center().y() + 4, half = r.height() / 2 - 10;
        for (int x = 0; x < int(r.width()); ++x) {
            float lo, hi;
            s->range(dur * x / r.width(), dur * (x + 1) / r.width(), lo, hi);
            p.fillRect(QRectF(r.left() + x, mid - hi * half, 1, std::max(1.0, double(hi - lo) * half)), col::cyan);
        }
        const double xs = r.left() + m_body->pos(4) * r.width(), xe = r.left() + m_body->pos(5) * r.width();
        p.fillRect(QRectF(r.left(), r.top(), std::max(0.0, xs - r.left()), r.height()), QColor(0, 0, 0, 150));
        p.fillRect(QRectF(xe, r.top(), std::max(0.0, r.right() - xe), r.height()), QColor(0, 0, 0, 150));
        if (m_body->on(6)) p.fillRect(QRectF(xs, r.top() + 12, xe - xs, 3), col::curve);
        for (double x : {xs, xe}) {
            p.setPen(QPen(col::handle, 1));
            p.drawLine(QPointF(x, r.top() + 10), QPointF(x, r.bottom()));
            QPainterPath flag;
            const bool start = x == xs;
            flag.moveTo(x, r.top() + 10); flag.lineTo(x + (start ? 7 : -7), r.top() + 14); flag.lineTo(x, r.top() + 18); flag.closeSubpath();
            p.fillPath(flag, col::handle);
        }
        p.setPen(col::value);
        p.drawText(r.adjusted(4, 1, -4, -2), Qt::AlignTop | Qt::AlignLeft, p.fontMetrics().elidedText(QFileInfo(d->samplePath).fileName(), Qt::ElideMiddle, int(r.width()) - 60));
        p.setPen(col::gridText);
        p.drawText(r.adjusted(4, 1, -4, -2), Qt::AlignTop | Qt::AlignRight, QString("%1 s").arg(dur, 0, 'f', 2));
    }
    void mousePressEvent(QMouseEvent* e) override {
        const QRectF r = inner();
        const double xs = r.left() + m_body->pos(4) * r.width(), xe = r.left() + m_body->pos(5) * r.width();
        m_drag = std::fabs(e->position().x() - xs) <= std::fabs(e->position().x() - xe) ? 4 : 5;
        mouseMoveEvent(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton)) return;
        const QRectF r = inner();
        double v = (e->position().x() - r.left()) / r.width();
        if (m_drag == 4) v = std::min(v, m_body->pos(5) - 0.002);
        else v = std::max(v, m_body->pos(4) + 0.002);
        m_body->editMany({{m_drag, v}}, m_drag);
    }
private:
    int m_drag = 4;
};

class SamplerBody : public DeviceBody {
public:
    SamplerBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = rootRow(this);
        auto* smp = vbox(3);
        smp->addWidget(new SampleDisplay(this, 290, 112));
        auto* r = hbox(0);
        r->addWidget(knob(4, 20));
        r->addWidget(knob(5, 20));
        r->addWidget(toggle(6, "Loop", 38), 0, Qt::AlignVCenter);
        r->addSpacing(6);
        r->addWidget(knob(11, 20));
        r->addWidget(knob(0, 20));
        r->addStretch(1);
        smp->addLayout(r);
        smp->addStretch(1);
        root->addLayout(smp);
        root->addWidget(vline(this));
        auto* flt = vbox(3);
        flt->addWidget(section("Filter"));
        flt->addWidget(new FilterDisplay(this, 120, 68, 7, 8, -1, 0));
        flt->addLayout(knobRow(this, {7, 8}, 22));
        flt->addStretch(1);
        root->addLayout(flt);
        root->addWidget(vline(this));
        auto* amp = vbox(3);
        amp->addWidget(section("Amp"));
        amp->addWidget(new AdsrDisplay(this, 148, 62, 1, 9, 10, 2));
        amp->addLayout(knobRow(this, {1, 9, 10, 2}, 18, {"A", "D", "S", "R"}));
        amp->addStretch(1);
        root->addLayout(amp);
        root->addWidget(vline(this));
        auto* glob = vbox(2);
        glob->addWidget(knob(12, 22, "Vel"));
        glob->addWidget(knob(3, 26));
        glob->addStretch(1);
        root->addLayout(glob);
        setFixedWidth(674);
    }
};

// ============================================================================ Organ

// One drawbar: pulled down (louder) from its slot, numbered 1 to 8, with its colour.
class Drawbar : public QWidget {
public:
    Drawbar(DeviceBody* body, int i, QColor color) : QWidget(body), m_body(body), m_i(i), m_color(color) {
        setFixedSize(20, 156);
        setCursor(Qt::SizeVerCursor);
        setProperty("mapTarget", body->target(i));
        InfoView::attach(this, "Drawbar " + body->name(i), "Pull it down for more of this footage (0 to 8). Drag, or click where it should stop.");
        body->watch(this);
    }
protected:
    QRectF slot() const { return QRectF(3, 14, width() - 6, height() - 16); }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF s = slot();
        p.setPen(theme::neutral);
        QFont f = font(); f.setPointSizeF(6); p.setFont(f);
        p.drawText(QRectF(0, 0, width(), 12), Qt::AlignCenter, m_body->name(m_i).remove('\''));
        p.fillRect(s, theme::meterBg);
        const int n = int(std::lround(m_body->real(m_i)));
        const double step = (s.height() - 14) / 8.0;
        const double len = n * step;
        const QRectF bar(s.left() + 2, s.top(), s.width() - 4, len);
        p.fillRect(bar, QColor("#d9d2c3"));
        p.setPen(QColor("#4a4a4a"));
        for (int k = 1; k <= n; ++k) {
            const double y = s.top() + k * step;
            p.drawLine(QPointF(bar.left(), y), QPointF(bar.right(), y));
            p.drawText(QRectF(bar.left(), y - step, bar.width(), step), Qt::AlignCenter, QString::number(k));
        }
        const QRectF knob(s.left(), s.top() + len, s.width(), 14);
        p.setPen(QPen(QColor("#0a0a0a"), 1));
        p.setBrush(m_color);
        p.drawRoundedRect(knob, 2, 2);
    }
    void mousePressEvent(QMouseEvent* e) override { mouseMoveEvent(e); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton)) return;
        const QRectF s = slot();
        const double step = (s.height() - 14) / 8.0;
        const int n = std::clamp(int(std::lround((e->position().y() - s.top() - 7) / step)), 0, 8);
        m_body->edit(m_i, n / 8.0);
    }
private:
    DeviceBody* m_body; int m_i; QColor m_color;
};

// The rotary speaker's horn seen from above, turning at the speed the processor reports.
class RotaryDisplay : public Display {
public:
    RotaryDisplay(DeviceBody* b, int w, int h) : Display(b, w, h) {
        InfoView::attach(this, "Rotary", "The rotary speaker's horn, turning Slow or Fast (it takes a moment to speed up and slow down).");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const QPointF c = r.center();
        const double rad = std::min(r.width(), r.height()) / 2 - 6;
        p.setPen(QPen(col::gridStrong, 1));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, rad, rad);
        if (m_body->pick(14) == 0) { drawCaption(p, r, "Off", Qt::AlignCenter); return; }
        const double a = m_body->meter(1) * 2 * M_PI;
        const QPointF tip(c.x() + std::cos(a) * rad, c.y() + std::sin(a) * rad);
        const QPointF back(c.x() - std::cos(a) * rad * 0.7, c.y() - std::sin(a) * rad * 0.7);
        p.setPen(QPen(col::curve, 3, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(back, tip);
        p.setPen(Qt::NoPen);
        p.setBrush(col::curve);
        p.drawEllipse(tip, 4, 4);
    }
};

class OrganBody : public DeviceBody {
public:
    OrganBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        static const QColor brown("#7a4a2a"), white("#ece6da"), black("#1e1e1e");
        const QColor colors[9] = {brown, brown, white, white, black, white, black, black, white};
        auto* root = rootRow(this);
        auto* bars = hbox(2);
        for (int i = 0; i < 9; ++i) bars->addWidget(new Drawbar(this, i, colors[i]));
        auto* barsCol = vbox(2);
        barsCol->addLayout(bars);
        barsCol->addStretch(1);
        root->addLayout(barsCol);
        root->addWidget(vline(this));
        auto* perc = vbox(4);
        perc->addWidget(section("Percussion"));
        perc->addWidget(toggle(9, "On", 64));
        perc->addWidget(choices(10, {}, {}, 64));
        perc->addWidget(choices(11, {}, {}, 64));
        perc->addWidget(knob(12, 22));
        perc->addStretch(1);
        root->addLayout(perc);
        root->addWidget(vline(this));
        auto* vib = vbox(4);
        vib->addWidget(section("Vibrato / Chorus"));
        vib->addWidget(choices(13, {"–", "V1", "V2", "V3", "C1", "C2", "C3"}, {}, 150));
        vib->addSpacing(4);
        vib->addWidget(section("Rotary"));
        auto* rot = hbox(6);
        auto* disp = new RotaryDisplay(this, 64, 64);
        animate(disp);
        rot->addWidget(disp);
        auto* rc = vbox(4);
        rc->addWidget(choices(14, {}, {}, 80));
        rc->addWidget(knob(15, 22));
        rc->addStretch(1);
        rot->addLayout(rc);
        rot->addStretch(1);
        vib->addLayout(rot);
        vib->addStretch(1);
        root->addLayout(vib);
        root->addWidget(vline(this));
        auto* glob = vbox(2);
        glob->addWidget(knob(16, 26));
        glob->addStretch(1);
        root->addLayout(glob);
        setFixedWidth(530);
    }
};

// ============================================================================ Bass

// The filter sweep of one note: the cutoff jumps up by Env Mod and falls back over Decay.
// The dashed line is an accented note. Drag across for Decay, up and down for Env Mod.
class SweepDisplay : public Display {
public:
    SweepDisplay(DeviceBody* b, int w, int h) : Display(b, w, h) {
        setCursor(Qt::SizeAllCursor);
        InfoView::attach(this, "Filter Sweep", "Where the filter goes during a note: up by Env Mod, back down over Decay; dashed: a hard-played (accented) note. "
                                                "Drag left and right for Decay, up and down for Env Mod.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const double secs = 0.5;
        for (int k = 1; k < 4; ++k) {
            p.setPen(QPen(col::grid, 1));
            const double x = std::round(r.left() + r.width() * k / 4) + 0.5;
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        }
        const double cutoff = m_body->real(1), envMod = m_body->pos(3), decay = m_body->real(4), accent = m_body->pos(5);
        auto y = [&](double f) { return r.bottom() - 4 - std::log(std::clamp(f, 30.0, 12000.0) / 30.0) / std::log(12000.0 / 30.0) * (r.height() - 18); };
        for (int pass = 0; pass < 2; ++pass) {
            const double acc = pass ? accent : 0.0;
            const double d = decay * (acc > 0 ? 0.6 : 1.0);
            QPainterPath path;
            for (int i = 0; i <= int(r.width()); ++i) {
                const double t = secs * i / r.width();
                const double env = std::exp(-t * 4.6 / d);
                const double f = cutoff * std::exp2(envMod * 4.5 * env * (1 + acc));
                const QPointF pt(r.left() + i, y(f));
                if (i == 0) path.moveTo(pt); else path.lineTo(pt);
            }
            p.setPen(QPen(pass ? col::cyan : col::curve, 1.5, pass ? Qt::DashLine : Qt::SolidLine));
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
        }
        p.setPen(QPen(col::gridStrong, 1));
        p.drawLine(QPointF(r.left(), y(cutoff)), QPointF(r.right(), y(cutoff)));
        drawCaption(p, r, m_body->text(1) + "   decay " + m_body->text(4), Qt::AlignTop | Qt::AlignRight);
    }
    void mousePressEvent(QMouseEvent* e) override { m_from = e->position(); m_decay = m_body->pos(4); m_env = m_body->pos(3); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton)) return;
        const QPointF d = e->position() - m_from;
        m_body->editMany({{4, m_decay + d.x() / 200.0}, {3, m_env - d.y() / 120.0}}, 0);
    }
private:
    QPointF m_from;
    double m_decay = 0, m_env = 0;
};

class BassBody : public DeviceBody {
public:
    BassBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(8, 6, 8, 4);
        root->setSpacing(4);
        auto* top = hbox(6);
        top->addWidget(new SweepDisplay(this, 300, 96));
        auto* side = vbox(4);
        side->addWidget(section("Wave"));
        side->addWidget(choices(0, {}, waveIcon, 60));
        side->addWidget(knob(7, 24));
        side->addStretch(1);
        top->addLayout(side);
        top->addStretch(1);
        root->addLayout(top);
        root->addLayout(knobRow(this, {1, 2, 3, 4, 5, 6, 8, 9}, 24));
        root->addStretch(1);
        setFixedWidth(392);
    }
};

// ============================================================================ Electric

// How long a note rings across the keyboard (low keys longer); the bell partial's flash.
class DecayDisplay : public Display {
public:
    DecayDisplay(DeviceBody* b, int w, int h) : Display(b, w, h) {
        setCursor(Qt::SizeVerCursor);
        InfoView::attach(this, "Decay", "How long a held note rings, across the keyboard: low notes ring longest. Drag up or down to change it.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const double decay = m_body->real(2);
        const double maxT = 12;
        for (int oct = 1; oct < 7; ++oct) {
            p.setPen(QPen(col::grid, 1));
            const double x = std::round(r.left() + r.width() * oct / 7) + 0.5;
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        }
        QPainterPath path;
        for (int i = 0; i <= int(r.width()); ++i) {
            const double pitch = 24 + 84.0 * i / r.width();
            const double t = decay * std::exp2(-(pitch - 60) / 24.0);
            const QPointF pt(r.left() + i, r.bottom() - 4 - std::min(1.0, t / maxT) * (r.height() - 14));
            if (i == 0) path.moveTo(pt); else path.lineTo(pt);
        }
        QPainterPath fill = path;
        fill.lineTo(r.right(), r.bottom()); fill.lineTo(r.left(), r.bottom()); fill.closeSubpath();
        p.fillPath(fill, col::curveFill);
        p.setPen(QPen(col::curve, 1.5));
        p.drawPath(path);
        p.setPen(col::gridText);
        p.drawText(QPointF(r.left() + 3, r.bottom() - 3), "C1");
        p.drawText(QPointF(r.right() - 16, r.bottom() - 3), "C8");
        drawCaption(p, r, "C3 rings " + m_body->text(2), Qt::AlignTop | Qt::AlignRight);
    }
    void mousePressEvent(QMouseEvent* e) override { m_y = e->position().y(); m_start = m_body->pos(2); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (e->buttons() & Qt::LeftButton) m_body->editMany({{2, m_start + (m_y - e->position().y()) / 120.0}}, 2);
    }
private:
    double m_y = 0, m_start = 0;
};

// The amp's tremolo: two lamps, left and right, pulsing with it while notes sound (dark when
// nothing plays, glowing out over a moment as the last note dies).
class TremoloLamps : public Display {
public:
    TremoloLamps(DeviceBody* b, int w, int h) : Display(b, w, h) {
        InfoView::attach(this, "Tremolo", "The amp's stereo tremolo, swinging the sound between left and right. The lamps light while notes sound.");
    }
    void tick() override {
        const bool sounding = m_body->meter(0) > 0.5f;   // voices sounding (PolySynth)
        m_glow = sounding ? std::min(1.0, m_glow + 0.25) : std::max(0.0, m_glow - 0.08);
        update();
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const double depth = m_body->pos(5);
        const double s = std::sin(2 * M_PI * m_body->meter(1));
        const double gl = 1 - depth * (0.5 + 0.5 * s), gr = 1 - depth * (0.5 - 0.5 * s);
        for (int side = 0; side < 2; ++side) {
            const double g = side ? gr : gl;
            QColor c = col::curve;
            c.setAlphaF(0.12 + 0.88 * g * m_glow);
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawEllipse(QPointF(r.left() + r.width() * (side ? 0.72 : 0.28), r.center().y()), 6, 6);
        }
    }
private:
    double m_glow = 0;
};

class ElectricBody : public DeviceBody {
public:
    ElectricBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = rootRow(this);
        auto* d = vbox(3);
        d->addWidget(new DecayDisplay(this, 170, 120));
        d->addStretch(1);
        root->addLayout(d);
        root->addWidget(vline(this));
        auto* mallet = vbox(2);
        mallet->addWidget(section("Mallet"));
        mallet->addWidget(knob(1, 26));
        mallet->addWidget(knob(0, 26));
        mallet->addStretch(1);
        root->addLayout(mallet);
        root->addWidget(vline(this));
        auto* damper = vbox(2);
        damper->addWidget(section("Damper"));
        damper->addWidget(knob(2, 26));
        damper->addWidget(knob(3, 26));
        damper->addStretch(1);
        root->addLayout(damper);
        root->addWidget(vline(this));
        auto* trem = vbox(2);
        trem->addWidget(section("Tremolo"));
        auto* lamps = new TremoloLamps(this, 46, 22);   // animate() below; lamps glow only while notes sound
        animate(lamps);
        trem->addWidget(lamps, 0, Qt::AlignHCenter);
        trem->addWidget(knob(4, 22, "Rate"));
        trem->addWidget(knob(5, 22, "Depth"));
        trem->addStretch(1);
        root->addLayout(trem);
        root->addWidget(vline(this));
        auto* glob = vbox(2);
        glob->addWidget(knob(6, 22));
        glob->addWidget(knob(7, 26));
        glob->addStretch(1);
        root->addLayout(glob);
        setFixedWidth(436);
    }
};

// ============================================================================ Drum kits

// Sixteen pads, C1 to D#2, like a drum rack: click one to hear it; a pad lights when it plays.
class PadGrid : public Display {
public:
    PadGrid(DeviceBody* b, int w, int h) : Display(b, w, h) {
        setCursor(Qt::PointingHandCursor);
        InfoView::attach(this, "Pads", "The kit's sounds on their notes (C1 kick, D1 snare, F#1 closed hat, …). Click a pad to hear it; pads light up as they play.");
    }
    void tick() override {
        const float hits = m_body->meter(1);
        if (hits != m_hits) { m_hits = hits; m_lit = int(m_body->meter(0)); m_glow = 1.0; }
        else m_glow = std::max(0.0, m_glow - 0.12);
        if (m_pressed >= 0 || m_glow > 0) update();
    }
protected:
    static QString padName(int pitch) {
        switch (pitch) {
        case 36: return "Kick";   case 37: return "Rim";     case 38: return "Snare";  case 39: return "Clap";
        case 40: return "Snare 2"; case 41: return "Tom Lo"; case 42: return "Hat";    case 43: return "Tom Lo 2";
        case 44: return "Pedal";  case 45: return "Tom Mid"; case 46: return "Open Hat"; case 47: return "Tom Mid 2";
        case 48: return "Tom Hi"; case 49: return "Crash";   case 50: return "Tom Hi 2"; case 51: return "Ride";
        default: return {};
        }
    }
    QRectF padRect(int k, const QRectF& r) const {   // k 0..15 from C1, bottom-left first
        const double w = r.width() / 4, h = r.height() / 4;
        return QRectF(r.left() + (k % 4) * w, r.bottom() - (k / 4 + 1) * h, w, h).adjusted(1.5, 1.5, -1.5, -1.5);
    }
    void draw(QPainter& p, const QRectF& r) override {
        for (int k = 0; k < 16; ++k) {
            const int pitch = 36 + k;
            const QRectF pr = padRect(k, r);
            QColor fill = theme::devPad;
            if (pitch == m_lit && m_glow > 0) fill = QColor::fromRgbF(0.23 + 0.77 * m_glow, 0.23 + 0.42 * m_glow, 0.23 + 0.0 * m_glow);
            if (k == m_pressed) fill = col::curve;
            p.setPen(QPen(theme::devPadEdge, 1));
            p.setBrush(fill);
            p.drawRoundedRect(pr, 2, 2);
            p.setPen(k == m_pressed ? col::buttonOnText : col::value);
            p.drawText(pr.adjusted(3, 2, -3, -2), Qt::AlignTop | Qt::AlignLeft, padName(pitch));
            p.setPen(k == m_pressed ? col::buttonOnText : col::gridText);
            p.drawText(pr.adjusted(3, 2, -3, -2), Qt::AlignBottom | Qt::AlignRight, QString("%1%2").arg(noteName(pitch)).arg(pitch / 12 - 2));
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        const QRectF r = inner();
        for (int k = 0; k < 16; ++k) {
            if (!padRect(k, r).contains(e->position())) continue;
            m_pressed = k;
            const DeviceContext& c = m_body->ctx();
            if (c.transport) c.transport->previewNote(c.track, 36 + k, 0.85);
            update();
            return;
        }
    }
    void mouseReleaseEvent(QMouseEvent*) override { m_pressed = -1; update(); }
private:
    float m_hits = 0;
    int m_lit = -1, m_pressed = -1;
    double m_glow = 0;
};

class DrumBody : public DeviceBody {
public:
    DrumBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = rootRow(this);
        auto* pads = new PadGrid(this, 252, 176);
        animate(pads);
        root->addWidget(pads, 0, Qt::AlignTop);
        root->addWidget(vline(this));
        auto* g = new QGridLayout;
        g->setContentsMargins(0, 0, 0, 0);
        g->setSpacing(2);
        g->addWidget(knob(0, 24), 0, 0);
        g->addWidget(knob(1, 24), 0, 1);
        g->addWidget(knob(2, 24), 1, 0);
        g->addWidget(knob(3, 24, {}, true), 1, 1);
        g->addWidget(knob(4, 26), 2, 0);
        auto* kc = vbox(0);
        kc->addLayout(g);
        kc->addStretch(1);
        root->addLayout(kc);
        setFixedWidth(380);
    }
};

// ============================================================================ Drum Rack

// The rack's sixteen pads, C1 bottom-left to D#2 top-right, each showing its
// sound. Click a pad to select it (its controls appear beside) and hear it; drop samples on
// a pad to load them (several fill the pads after it); right-click to clear one.
class RackPads : public Display {
public:
    RackPads(DeviceBody* b, int w, int h, std::function<void()> selected) : Display(b, w, h), m_selected(std::move(selected)) {
        setObjectName("drumPads");
        setCursor(Qt::PointingHandCursor);
        setAcceptDrops(true);
        InfoView::attach(this, "Pads",
            "Each pad plays a sample on its note (C1 to D#2). Click a pad to select and hear it. Drop samples from the browser "
            "or your file manager onto a pad; several fill the pads after it. Drop an instrument on a pad and it plays that pad instead "
            "(at C3); drop effects on a pad and they process just that pad. The selected pad's devices show next to the Drum Rack. "
            "Right-click a pad to clear its sample or its devices.");
    }
    void tick() override {
        const float hits = m_body->meter(1);
        if (hits != m_hits) { m_hits = hits; m_lit = int(m_body->meter(0)) - drumrack::kFirstNote; m_glow = 1.0; }
        else m_glow = std::max(0.0, m_glow - 0.12);
        if (m_glow > 0 || m_pressed >= 0) update();
    }
    int padAt(const QPointF& pt) const {
        const QRectF r = inner();
        for (int k = 0; k < drumrack::kPads; ++k) if (padRect(k, r).contains(pt)) return k;
        return -1;
    }
    QRectF padRect(int k, const QRectF& r) const {   // bottom-left first
        const double w = r.width() / 4, h = r.height() / 4;
        return QRectF(r.left() + (k % 4) * w, r.bottom() - (k / 4 + 1) * h, w, h).adjusted(1.5, 1.5, -1.5, -1.5);
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const Device* d = m_body->device();
        const int sel = selectedDrumPad(m_body->ctx().deviceId);
        QFont small = font(); small.setPointSizeF(7);
        p.setFont(small);
        const DeviceContext& c = m_body->ctx();
        for (int k = 0; k < drumrack::kPads; ++k) {
            DrumPad pad = d && k < d->pads.size() ? d->pads[k] : DrumPad{};
            const QList<const Device*> chain = padDevices(c.project, c.track, c.deviceId, k);
            const Device* inst = nullptr;
            for (const Device* m : chain) if (m->kind == DeviceKind::Instrument) inst = m;
            if (inst) { pad.name = inst->name; pad.samplePath = "-"; pad.sample = nullptr; }   // it plays the instrument
            const bool empty = pad.samplePath.isEmpty();
            const QRectF pr = padRect(k, r);
            QColor fill = empty ? theme::devPadEmpty : theme::devPad;
            if (k == m_lit && m_glow > 0 && !empty) fill = QColor::fromRgbF(0.23 + 0.77 * m_glow, 0.23 + 0.42 * m_glow, 0.23);
            if (k == m_pressed || k == m_dropAt) fill = col::curve;
            p.setPen(QPen(k == sel ? col::curve : theme::devPadEdge, k == sel ? 1.5 : 1));
            p.setBrush(fill);
            p.drawRoundedRect(pr, 2, 2);
            const bool dark = k == m_pressed || k == m_dropAt;
            const QRectF tr = pr.adjusted(3, 2, -3, -2);
            if (!empty) {
                p.setPen(dark ? col::buttonOnText : inst ? col::cyan : pad.sample ? col::value : theme::record);
                p.drawText(tr, Qt::AlignTop | Qt::AlignLeft | Qt::TextWordWrap,
                           p.fontMetrics().elidedText(pad.name, Qt::ElideRight, int(tr.width()) * 2 - 8));
            }
            if (chain.size() > (inst ? 1 : 0)) {   // effects on the pad: a dot
                p.setPen(Qt::NoPen);
                p.setBrush(dark ? col::buttonOnText : col::cyan);
                p.drawEllipse(QPointF(pr.left() + 5, pr.bottom() - 5), 2, 2);
            }
            p.setPen(dark ? col::buttonOnText : col::gridText);
            const int note = drumrack::kFirstNote + k;
            p.drawText(tr, Qt::AlignBottom | Qt::AlignRight, QString("%1%2").arg(noteName(note)).arg(note / 12 - 2));
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        const int k = padAt(e->position());
        if (k < 0 || e->button() != Qt::LeftButton) return;
        m_pressed = k;
        selectDrumPad(m_body->ctx().deviceId, k);
        if (m_selected) m_selected();
        const DeviceContext& c = m_body->ctx();
        if (c.transport) c.transport->previewNote(c.track, drumrack::kFirstNote + k, 0.85);
        update();
    }
    void mouseReleaseEvent(QMouseEvent*) override { m_pressed = -1; update(); }
    void contextMenuEvent(QContextMenuEvent* e) override {
        const int k = padAt(e->pos());
        const Device* d = m_body->device();
        if (k < 0 || !d) return;
        const DeviceContext c = m_body->ctx();
        QMenu menu(this);
        QAction* clear = menu.addAction("Clear Sample");
        clear->setEnabled(k < d->pads.size() && !d->pads[k].samplePath.isEmpty());
        QAction* devices = menu.addAction("Remove Pad Devices");
        devices->setEnabled(!padDevices(c.project, c.track, c.deviceId, k).isEmpty());
        QAction* chosen = menu.exec(e->globalPos());
        if (chosen == devices) { c.project->deleteChain(c.track, c.deviceId, k); return; }
        if (chosen != clear) return;
        EditScope edit(c.project, "Clear Pad");
        import::clearDrumPad(*c.project, c.project->indexOfTrack(c.trackId), c.deviceId, k);
    }
    // Samples load onto a pad (and the ones after it); devices (browser items, presets, panels
    // dragged by their title) go into the pad's chain, when the Drum Rack is on the track itself.
    bool takes(const QMimeData* mime, int pad, QString* why = nullptr) const {
        if (pad < 0) return false;
        if (!Browser::audioFilesIn(mime).isEmpty()) return true;
        const devicedrag::Payload pl = devicedrag::parse(mime);
        const DeviceContext& c = m_body->ctx();
        const Device* d = m_body->device();
        if (!pl.valid() || !d) return false;
        if (!holdsChains(*d)) { if (why) *why = "A Drum Rack inside a rack's chain plays samples only: move it onto the track to put devices on its pads."; return false; }
        return devicedrag::accepts(*c.project, pl, c.track, {0, c.deviceId, pad}, why);
    }
    void dragEnterEvent(QDragEnterEvent* e) override {
        if (Browser::audioFilesIn(e->mimeData()).isEmpty() && !devicedrag::parse(e->mimeData()).valid()) { e->ignore(); return; }
        e->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent* e) override {
        m_dropAt = padAt(e->position());
        if (takes(e->mimeData(), m_dropAt)) {
            const devicedrag::Payload pl = devicedrag::parse(e->mimeData());
            if (pl.source == devicedrag::Payload::Devices) { e->setDropAction(devicedrag::copying(e) ? Qt::CopyAction : Qt::MoveAction); e->accept(); }
            else e->acceptProposedAction();
        } else {
            m_dropAt = -1;
            e->ignore();
        }
        update();
    }
    void dragLeaveEvent(QDragLeaveEvent*) override { m_dropAt = -1; update(); }
    void dropEvent(QDropEvent* e) override {
        const QStringList files = Browser::audioFilesIn(e->mimeData());
        const int first = padAt(e->position());
        m_dropAt = -1;
        update();
        if (first < 0) { e->ignore(); return; }
        const DeviceContext c = m_body->ctx();
        if (files.isEmpty()) {   // a device onto the pad
            QString why;
            if (!takes(e->mimeData(), first, &why)) { e->ignore(); if (!why.isEmpty()) InfoView::notify("Can't put that on the pad", why); return; }
            const devicedrag::Payload pl = devicedrag::parse(e->mimeData());
            const bool copy = devicedrag::copying(e);
            const devicedrag::Result r = devicedrag::drop(*c.project, pl, c.track, {0, c.deviceId, first}, copy);
            InfoView::notify(r.title, r.ok ? QString("On pad %1.").arg(drumrack::kFirstNote + first) : r.text);
            if (!r.ok) { e->ignore(); return; }
            e->setDropAction(pl.source == devicedrag::Payload::Devices && !copy ? Qt::MoveAction : Qt::CopyAction);
            e->accept();
            selectDrumPad(c.deviceId, first);   // the panel is rebuilt: touch nothing after
            return;
        }
        e->acceptProposedAction();
        selectDrumPad(c.deviceId, first);
        loadPads(c, first, files.mid(0, drumrack::kPads - first));
    }
public:
    // Loads files onto pads from `first` on, in the background; one undo step.
    static void loadPads(const DeviceContext& c, int first, const QStringList& files) {
        Project* project = c.project;
        const int trackId = c.trackId, deviceId = c.deviceId;
        QGuiApplication::setOverrideCursor(Qt::BusyCursor);
        samples::loadAsync(files, project, [project, trackId, deviceId, first, files](QVector<SamplePtr> loaded, QStringList) {
            QGuiApplication::restoreOverrideCursor();
            const int track = project->indexOfTrack(trackId);
            EditScope edit(project, files.size() > 1 ? "Load Samples" : "Load Sample");
            int pad = first;
            for (int i = 0; i < files.size() && pad < drumrack::kPads; ++i)
                if (loaded.value(i) && import::toDrumPad(*project, track, deviceId, pad, files[i])) ++pad;
        });
    }
private:
    std::function<void()> m_selected;
    float m_hits = 0;
    int m_lit = -1, m_pressed = -1, m_dropAt = -1;
    double m_glow = 0;
};

// The selected pad's sample.
class PadWave : public Display {
public:
    PadWave(DeviceBody* b, int w, int h) : Display(b, w, h) {
        InfoView::attach(this, "Pad Sample", "The selected pad's sample. Drop a sample on the pad to replace it.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const Device* d = m_body->device();
        const DeviceContext& c = m_body->ctx();
        const int k = selectedDrumPad(c.deviceId);
        const DrumPad pad = d && k < d->pads.size() ? d->pads[k] : DrumPad{};
        QFont small = font(); small.setPointSizeF(7.5);
        p.setFont(small);
        const int note = drumrack::kFirstNote + k;
        const QString title = QString("%1%2").arg(noteName(note)).arg(note / 12 - 2);
        if (const Device* inst = padInstrument(c.project, c.track, c.deviceId, k)) {   // the pad plays an instrument
            const int play = std::clamp(drumrack::kPlayNote + int(std::lround(m_body->real(drumrack::param(k, drumrack::Transpose)))), 0, 127);
            p.setPen(col::cyan);
            p.drawText(r.adjusted(6, 4, -6, -4), Qt::AlignCenter | Qt::TextWordWrap,
                       QString("%1 plays %2\nat %3%4").arg(title, inst->name).arg(noteName(play)).arg(play / 12 - 2));
            return;
        }
        if (!pad.sample || pad.sample->frames() < 2) {
            p.setPen(pad.samplePath.isEmpty() ? col::gridText : theme::record);
            p.drawText(r, Qt::AlignCenter, pad.samplePath.isEmpty() ? title + ": empty pad\nDrop a sample on it"
                                                                    : "Missing: " + QFileInfo(pad.samplePath).fileName());
            return;
        }
        const SamplePtr s = pad.sample;
        const double dur = s->seconds();
        const double mid = r.center().y() + 5, half = r.height() / 2 - 10;
        for (int x = 0; x < int(r.width()); ++x) {
            float lo, hi;
            s->range(dur * x / r.width(), dur * (x + 1) / r.width(), lo, hi);
            p.fillRect(QRectF(r.left() + x, mid - hi * half, 1, std::max(1.0, double(hi - lo) * half)), col::cyan);
        }
        p.setPen(col::value);
        p.drawText(r.adjusted(4, 1, -4, -2), Qt::AlignTop | Qt::AlignLeft,
                   p.fontMetrics().elidedText(title + "  " + pad.name, Qt::ElideRight, int(r.width()) - 50));
        p.setPen(col::gridText);
        p.drawText(r.adjusted(4, 1, -4, -2), Qt::AlignTop | Qt::AlignRight, QString("%1 s").arg(dur, 0, 'f', 2));
    }
};

class DrumRackBody : public DeviceBody {
public:
    DrumRackBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = rootRow(this);
        m_pages = new QStackedWidget(this);
        auto* pads = new RackPads(this, 248, 176, [this] { showSelected(); });
        animate(pads);
        root->addWidget(pads, 0, Qt::AlignTop);
        root->addWidget(vline(this));
        auto* sel = vbox(4);
        m_wave = new PadWave(this, 214, 78);
        watch(m_wave);
        sel->addWidget(m_wave);
        // one page of controls per pad; the selected pad's shows
        for (int k = 0; k < drumrack::kPads; ++k) {
            auto* page = new QWidget(m_pages);
            auto* row = hbox(0);
            row->addWidget(knob(drumrack::param(k, drumrack::Vol), 22, "Volume"));
            row->addWidget(knob(drumrack::param(k, drumrack::Pan), 22, "Pan", true));
            row->addWidget(knob(drumrack::param(k, drumrack::Transpose), 22, "Transp", true));
            row->addWidget(knob(drumrack::param(k, drumrack::Decay), 22, "Decay"));
            auto* choke = vbox(2);
            auto* chokeLabel = new QLabel("Choke", page);
            chokeLabel->setStyleSheet(QString("color:%1;font-size:9px;background:transparent;border:0;").arg(col::label.name()));
            choke->addWidget(chokeLabel);
            choke->addWidget(chooser(drumrack::param(k, drumrack::Choke), 44));
            choke->addStretch(1);
            row->addLayout(choke);
            row->addStretch(1);
            page->setLayout(row);
            m_pages->addWidget(page);
        }
        sel->addWidget(m_pages);
        sel->addStretch(1);
        root->addLayout(sel);
        root->addWidget(vline(this));
        auto* glob = vbox(2);
        glob->addWidget(knob(1, 22, "Vel"));
        glob->addWidget(knob(0, 26));
        glob->addStretch(1);
        root->addLayout(glob);
        showSelected();
        setFixedWidth(600);
    }
    void showSelected() {
        m_pages->setCurrentIndex(selectedDrumPad(ctx().deviceId));
        m_wave->update();
        update();
    }
private:
    QStackedWidget* m_pages;
    PadWave* m_wave;
};

// ============================================================================ Monolith

namespace mono = monolith;


// One oscillator's cycle: its table at its Position, warped as the oscillator warps it.
// Drag up or down for the position.
class MonoWaveDisplay : public Display {
public:
    MonoWaveDisplay(DeviceBody* b, int w, int h, int osc) : Display(b, w, h), m_osc(osc) {
        setCursor(Qt::SizeVerCursor);
        InfoView::attach(this, QString("Osc %1 Wave").arg(osc + 1),
                         "One cycle of this oscillator: its table at its Position, bent, squeezed (Formant) or synced by Intensity. Drag up or down to move through the table.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const int table = m_body->pick(mono::oparam(m_osc, mono::Table)), mode = m_body->pick(mono::oparam(m_osc, mono::Mode));
        const float pos = float(m_body->pos(mono::oparam(m_osc, mono::Position))), inten = float(m_body->pos(mono::oparam(m_osc, mono::Intensity)));
        const audio::Wavetable& t = audio::wavetable(table);
        p.setPen(QPen(col::grid, 1));
        p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
        QPainterPath path;
        for (int x = 0; x <= int(r.width()); ++x) {
            const float ph = float(x) / float(r.width()) * 0.9999f;
            const float v = audio::wavetableRead(t, 0, pos, audio::monolithWarpPhase(mode, ph, inten));
            const QPointF pt(r.left() + x, r.center().y() - v * (r.height() / 2 - 3));
            if (x == 0) path.moveTo(pt); else path.lineTo(pt);
        }
        const bool silent = m_body->pos(mono::oparam(m_osc, mono::Level)) < 0.0005;
        p.setPen(QPen(silent ? col::gridText : col::curve, 1.4));
        p.drawPath(path);
        drawCaption(p, r, QString("%1 · %2").arg(t.name, m_body->text(mono::oparam(m_osc, mono::Mode))), Qt::AlignTop | Qt::AlignLeft);
    }
    void mousePressEvent(QMouseEvent* e) override { m_y = e->position().y(); m_start = m_body->pos(mono::oparam(m_osc, mono::Position)); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton)) return;
        m_body->editMany({{mono::oparam(m_osc, mono::Position), m_start + (m_y - e->position().y()) / 120.0}}, m_osc);
    }
private:
    int m_osc;
    double m_y = 0, m_start = 0;
};

// One filter's response with a handle at cutoff / resonance; drag it.
class MonoFilterDisplay : public Display {
public:
    MonoFilterDisplay(DeviceBody* b, int w, int h, int filter) : Display(b, w, h), m_f(filter) {
        setCursor(Qt::SizeAllCursor);
        InfoView::attach(this, QString("Filter %1").arg(filter + 1),
                         "The filter's response: drag the dot sideways for the cutoff, up and down for the resonance. Comb and Scream respond to Reso in their own way.");
    }
protected:
    static constexpr double kRange = 24;
    void draw(QPainter& p, const QRectF& r) override {
        drawFreqGrid(p, r, kRange);
        const int type = m_body->pick(mono::fparam(m_f, mono::FType));
        const float cutoff = float(m_body->real(mono::fparam(m_f, mono::FCutoff))), reso = float(m_body->pos(mono::fparam(m_f, mono::FReso)));
        const float q = audio::monolithFilterQ(type, reso);
        auto mag = [&](float f) {
            using dsp::Svf;
            switch (type) {
            case 0:  return Svf::magnitude(0, f, cutoff, q);
            case 1:  return Svf::magnitude(0, f, cutoff, q) * Svf::magnitude(0, f, cutoff, 0.707f);
            case 2:  return Svf::magnitude(1, f, cutoff, q);
            case 3:  return Svf::magnitude(2, f, cutoff, q) * 2.0f;
            case 4:  return Svf::magnitude(3, f, cutoff, q);
            case 5:  { const float w = 2 * dsp::kPi * f / cutoff; return 0.7f / std::sqrt(std::max(0.0025f, 1 - 2 * q * std::cos(w) + q * q)); }
            default: return Svf::magnitude(0, f, cutoff, q) * Svf::magnitude(0, f, cutoff, 0.707f) * (1 + 0.5f * reso);
            }
        };
        QPainterPath path;
        for (int x = 0; x <= int(r.width()); x += 2) {
            const double db = std::clamp(double(dsp::gainToDb(std::max(1e-5f, mag(float(xToFreq(r.left() + x, r)))))), -kRange * 1.4, kRange);
            const QPointF pt(r.left() + x, r.center().y() - db / kRange * r.height() / 2);
            if (x == 0) path.moveTo(pt); else path.lineTo(pt);
        }
        QPainterPath fill = path;
        fill.lineTo(r.right(), r.bottom()); fill.lineTo(r.left(), r.bottom()); fill.closeSubpath();
        p.fillPath(fill, col::curveFill);
        p.setPen(QPen(col::curve, 1.6));
        p.drawPath(path);
        drawHandle(p, QPointF(freqToX(cutoff, r), r.bottom() - 4 - reso * (r.height() - 8)), col::handle);
        drawCaption(p, r, m_body->text(mono::fparam(m_f, mono::FType)), Qt::AlignTop | Qt::AlignLeft);
    }
    void mousePressEvent(QMouseEvent* e) override { mouseMoveEvent(e); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton)) return;
        const QRectF r = inner();
        m_body->editMany({{mono::fparam(m_f, mono::FCutoff), m_body->posFor(mono::fparam(m_f, mono::FCutoff), xToFreq(e->position().x(), r))},
                          {mono::fparam(m_f, mono::FReso), std::clamp((r.bottom() - 4 - e->position().y()) / (r.height() - 8), 0.0, 1.0)}}, 3 + m_f);
    }
private:
    int m_f;
};

// One of the Monolith's drawn curves (0.21): its points along one length of the curve, a line
// where it is now. Click to add a point, drag a point to move it (x on a 1/16 grid; Alt: free),
// double-click a point to delete it, right-click for shapes (ramps, steps, random) and Clear.
class MonoCurveDisplay : public Display {
public:
    using Points = QVector<CurvePoint>;
    MonoCurveDisplay(DeviceBody* b, int w, int h, int curve, std::function<Points()> get,
                     std::function<void(const QString&, std::function<void(Points&)>)> edit, std::function<void(bool)> dragEdit)
        : Display(b, w, h), m_c(curve), m_get(std::move(get)), m_edit(std::move(edit)), m_dragEdit(std::move(dragEdit)) {
        setMouseTracking(true);
        InfoView::attach(this, QString("Curve %1").arg(curve + 1),
                         "Draw a shape for a knob to follow: click to add a point, drag points (Alt: off the 1/16 grid), double-click one to remove it, "
                         "right-click for ramps, steps and random shapes. Drag the curve's chip onto any knob (or right-click the knob → Modulate by → Curve) and set how much in the Mod drawer. "
                         "Rate is the curve's length in the song, Loop or One-shot, Retrig starts it with every note, Smooth rounds the corners.");
    }
    void fill(const QString& label, const Points& pts) { m_edit(label, [pts](Points& p) { p = pts; }); }
    void randomize(int steps, bool hold) {
        Points pts;
        for (int i = 0; i < steps; ++i) {
            const double y = QRandomGenerator::global()->bounded(1.0);
            pts.push_back({double(i) / steps, y});
            if (hold) pts.push_back({double(i + 1) / steps - 0.002, y});
        }
        fill(hold ? "Random Steps" : "Random Curve", pts);
    }
protected:
    QPointF at(const CurvePoint& pt, const QRectF& r) const { return QPointF(r.left() + pt.x * r.width(), r.bottom() - pt.y * r.height()); }
    CurvePoint from(const QPointF& p, const QRectF& r, bool free) const {
        double x = std::clamp((p.x() - r.left()) / r.width(), 0.0, 1.0), y = std::clamp((r.bottom() - p.y()) / r.height(), 0.0, 1.0);
        if (!free) x = std::round(x * 16) / 16;
        return {x, y};
    }
    void draw(QPainter& p, const QRectF& r) override {
        p.setPen(QPen(col::grid, 1));
        for (int i = 1; i < 16; ++i) {
            const double x = r.left() + r.width() * i / 16;
            p.setPen(QPen(i % 4 ? theme::withAlpha(col::grid, 90) : col::grid, 1));
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        }
        p.setPen(QPen(col::grid, 1));
        p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
        const Points pts = m_get();
        if (!pts.isEmpty()) {
            QPainterPath path;
            path.moveTo(r.left(), at(pts.first(), r).y());
            for (const CurvePoint& pt : pts) path.lineTo(at(pt, r));
            path.lineTo(r.right(), at(pts.last(), r).y());
            QPainterPath area = path;
            area.lineTo(r.right(), r.bottom()); area.lineTo(r.left(), r.bottom()); area.closeSubpath();
            p.setPen(Qt::NoPen); p.setBrush(theme::withAlpha(col::curve, 40)); p.drawPath(area);
            p.setPen(QPen(col::curve, 1.5)); p.setBrush(Qt::NoBrush); p.drawPath(path);
            for (int k = 0; k < pts.size(); ++k) drawHandle(p, at(pts[k], r), k == m_hover || k == m_drag ? col::handle : col::curve);
        } else {
            drawCaption(p, r, "Click to draw", Qt::AlignCenter);
        }
        const double ph = m_body->scope(14 + m_c);
        if (ph > 0 && ph < 1) {
            p.setPen(QPen(theme::withAlpha(col::handle, 150), 1));
            const double x = r.left() + ph * r.width();
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        }
        drawCaption(p, r, QString("Curve %1").arg(m_c + 1), Qt::AlignTop | Qt::AlignLeft);
    }
    int pointAt(const QPointF& p) const {
        const QRectF r = inner();
        const Points pts = m_get();
        int best = -1;
        double bestD = 7;
        for (int k = 0; k < pts.size(); ++k) { const double d = QLineF(p, at(pts[k], r)).length(); if (d < bestD) { bestD = d; best = k; } }
        return best;
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        const bool free = e->modifiers() & (Qt::AltModifier | Qt::MetaModifier);
        m_drag = pointAt(e->position());
        m_dragEdit(true);
        if (m_drag < 0) {   // a new point where you clicked
            const CurvePoint pt = from(e->position(), inner(), free);
            int index = -1;
            m_edit("Add Curve Point", [pt, &index](Points& p) {
                if (p.size() >= mono::kCurvePoints) return;
                auto it = std::upper_bound(p.begin(), p.end(), pt.x, [](double x, const CurvePoint& q) { return x < q.x; });
                index = int(it - p.begin());
                p.insert(index, pt);
            });
            m_drag = index;
        }
        update();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (m_drag < 0 || !(e->buttons() & Qt::LeftButton)) {
            const int h = pointAt(e->position());
            if (h != m_hover) { m_hover = h; update(); }
            return;
        }
        const bool free = e->modifiers() & (Qt::AltModifier | Qt::MetaModifier);
        CurvePoint pt = from(e->position(), inner(), free);
        const int k = m_drag;
        m_edit("Move Curve Point", [pt, k](Points& p) mutable {
            if (k < 0 || k >= p.size()) return;
            if (k > 0) pt.x = std::max(pt.x, p[k - 1].x);
            if (k + 1 < p.size()) pt.x = std::min(pt.x, p[k + 1].x);
            p[k] = pt;
        });
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        if (m_drag >= 0) m_dragEdit(false);
        m_drag = -1;
        update();
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        const int k = pointAt(e->position());
        if (k < 0) return;
        m_edit("Delete Curve Point", [k](Points& p) { if (k < p.size()) p.removeAt(k); });
    }
    void contextMenuEvent(QContextMenuEvent* e) override {
        QMenu menu(this);
        menu.addAction("Ramp Up", this, [this] { fill("Ramp Up", {{0, 0}, {1, 1}}); });
        menu.addAction("Ramp Down", this, [this] { fill("Ramp Down", {{0, 1}, {1, 0}}); });
        menu.addAction("Triangle", this, [this] { fill("Triangle", {{0, 0}, {0.5, 1}, {1, 0}}); });
        menu.addAction("Four Steps", this, [this] { fill("Four Steps", {{0, 1}, {0.248, 1}, {0.25, 0.25}, {0.498, 0.25}, {0.5, 0.75}, {0.748, 0.75}, {0.75, 0.5}, {1, 0.5}}); });
        menu.addSeparator();
        menu.addAction("Random 8 Steps", this, [this] { randomize(8, true); });
        menu.addAction("Random 16 Steps", this, [this] { randomize(16, true); });
        menu.addAction("Random 8 Points", this, [this] { randomize(8, false); });
        menu.addSeparator();
        menu.addAction("Clear", this, [this] { fill("Clear Curve", {}); });
        menu.exec(e->globalPos());
    }
private:
    int m_c;
    std::function<Points()> m_get;
    std::function<void(const QString&, std::function<void(Points&)>)> m_edit;
    std::function<void(bool)> m_dragEdit;
    int m_drag = -1, m_hover = -1;
};

// One LFO's shape, one cycle across, with its value now as a line.
class MonoLfoDisplay : public Display {
public:
    MonoLfoDisplay(DeviceBody* b, int w, int h, int lfo) : Display(b, w, h), m_l(lfo) {
        InfoView::attach(this, QString("LFO %1").arg(lfo + 1), "The LFO's shape, one cycle across; the line is its value now. Synced rates follow the song, Free runs at the Hz knob.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        p.setPen(QPen(col::grid, 1));
        p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
        const int shape = m_body->pick(mono::lparam(m_l, mono::LShape));
        QPainterPath path;
        for (int i = 0; i <= int(r.width()); ++i) {
            const double ph = double(i) / r.width() * 0.9999;
            const double v = shape == 5 ? dsp::lfoShape(5, std::floor(ph * 4) + 3.0 + m_l) : dsp::lfoShape(shape, ph);
            const QPointF pt(r.left() + i, r.center().y() - v * (r.height() / 2 - 4));
            if (i == 0) path.moveTo(pt); else path.lineTo(pt);
        }
        p.setPen(QPen(col::cyan, 1.4));
        p.drawPath(path);
        const double v = std::clamp(double(m_body->meter(1 + m_l)), -1.0, 1.0);
        p.setPen(QPen(col::handle, 1));
        const double y = r.center().y() - v * (r.height() / 2 - 4);
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        drawCaption(p, r, m_body->text(mono::lparam(m_l, mono::LRate)), Qt::AlignTop | Qt::AlignLeft);
    }
private:
    int m_l;
};

// A stepper's sixteen steps as bars from the centre line (-100 % to +100 %); drag across them
// to draw the pattern. Steps past Steps are dimmed; the one playing is lit.
class StepDisplay : public Display {
public:
    StepDisplay(DeviceBody* b, int w, int h, int stepper) : Display(b, w, h), m_s(stepper) {
        setCursor(Qt::CrossCursor);
        InfoView::attach(this, QString("Step %1").arg(stepper + 1),
                         "The step sequencer's pattern: each bar is a step's value, -100 % to +100 %. Drag across the bars to draw. Steps sets how many play; Rate how fast; Glide slides between them.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const int count = std::clamp(int(std::lround(m_body->real(mono::sparam(m_s, mono::SCount)))), 1, mono::kSteps);
        const double w = r.width() / mono::kSteps, mid = r.center().y(), half = r.height() / 2 - 2;
        int playing = -1;
        if (Transport* t = m_body->ctx().transport) {
            const double beats = std::max(0.0625, m_body->real(mono::sparam(m_s, mono::SRate)));
            const long idx = long(std::floor(t->beat() / beats));
            playing = int(((idx % count) + count) % count);
        }
        p.setPen(QPen(col::grid, 1));
        p.drawLine(QPointF(r.left(), mid), QPointF(r.right(), mid));
        for (int i = 0; i < mono::kSteps; ++i) {
            const double v = m_body->pos(mono::step(m_s, i)) * 2 - 1;
            const double x = r.left() + i * w;
            if (i == playing && m_body->ctx().transport && m_body->ctx().transport->playing()) p.fillRect(QRectF(x, r.top(), w, r.height()), theme::devHighlight);
            QColor c = i < count ? col::curve : col::gridStrong;
            p.fillRect(QRectF(x + 1, v >= 0 ? mid - v * half : mid, w - 2, std::max(1.0, std::fabs(v) * half)), c);
        }
    }
    void mousePressEvent(QMouseEvent* e) override { mouseMoveEvent(e); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton)) return;
        const QRectF r = inner();
        const int i = std::clamp(int((e->position().x() - r.left()) / (r.width() / mono::kSteps)), 0, mono::kSteps - 1);
        const double v = std::clamp(1 - (e->position().y() - r.top()) / r.height(), 0.0, 1.0);
        m_body->editMany({{mono::step(m_s, i), v}}, 5 + m_s);
    }
private:
    int m_s;
};

// A modulation source you drag onto a knob.
class SourceChip : public QWidget {
public:
    SourceChip(QWidget* parent, int source) : QWidget(parent), m_source(source) {
        setFixedSize(54, 16);
        setCursor(Qt::OpenHandCursor);
        InfoView::attach(this, QString("%1 (source)").arg(mono::sourceName(source)),
                         "Drag it onto any knob of the Monolith and that knob follows this source (then set how much in the Mod page's list). Right-click a knob for the same without dragging.");
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(QPen(mono::sourceBipolar(m_source) ? col::cyan : col::curve, 1));
        p.setBrush(theme::devChip);
        p.drawRoundedRect(r, 3, 3);
        QFont f = font(); f.setPointSizeF(7.5); p.setFont(f);
        p.setPen(col::value);
        p.drawText(r, Qt::AlignCenter, mono::sourceName(m_source));
    }
    void mousePressEvent(QMouseEvent* e) override { if (e->button() == Qt::LeftButton) m_press = e->pos(); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton) || (e->pos() - m_press).manhattanLength() < QApplication::startDragDistance()) return;
        auto* drag = new QDrag(this);
        auto* mime = new QMimeData;
        mime->setData(kModSourceMime, QByteArray::number(m_source));
        drag->setMimeData(mime);
        QPixmap pix(size());
        pix.fill(Qt::transparent);
        render(&pix);
        drag->setPixmap(pix);
        drag->exec(Qt::CopyAction);
    }
private:
    int m_source;
    QPoint m_press;
};

// The Monolith's panel (0.20), after the way Massive lays itself out: a main panel with the
// knobs you reach for most, big (the oscillators' Position and Level, the filters' Cutoff, the
// Volume), and six drawers that slide out to its right on the buttons between them: Env (the
// four envelopes, with a dot showing where each is while a note plays), LFO, Step, Mod (the
// sources and the routes), Voice and FX. One drawer at a time; the choice is remembered per
// device for the session.
QHash<int, int>& monoDrawers() { static QHash<int, int> d; return d; }   // device id -> the open drawer (-1: none)

// An envelope with where it is right now: a dot on the curve (the processor's scope).
class MonoEnvDisplay : public AdsrDisplay {
public:
    MonoEnvDisplay(DeviceBody* b, int w, int h, int env)
        : AdsrDisplay(b, w, h, mono::eparam(env, mono::A), mono::eparam(env, mono::D), mono::eparam(env, mono::S), mono::eparam(env, mono::R),
                      env == 0 ? col::curve : col::cyan, env == 0 ? "Env 1 · Amp" : QString("Env %1").arg(env + 1)), m_env(env) {
        InfoView::attach(this, env == 0 ? "Env 1 (Amp)" : QString("Env %1").arg(env + 1),
                         "Drag the handles: the attack's peak, the decay's end (and the sustain level), the release's end. "
                         "While a note plays the dot shows where the envelope is, so you can shape it as you hear it. Drag the envelope's chip onto a knob to modulate it.");
    }
protected:
    void drawLive(QPainter& p, const QRectF& r) override {
        const float level = m_body->scope(m_env);
        const int stage = int(m_body->scope(4 + m_env));
        if (stage <= 0 || level <= 0.002f) return;
        const QPointF p0 = point(0, r), p1 = point(1, r), p2 = point(2, r), p3 = point(3, r);
        const double left = r.left() + 4, bottom = r.bottom() - 4, top = r.top() + 8, h = bottom - top;
        const double sus = std::max(0.02, m_body->pos(sustainParam()));
        double x;
        switch (stage) {
        case 1:  x = left + (p0.x() - left) * level; break;
        case 2:  { const double t = std::clamp((1 - level) / std::max(0.02, 1 - sus), 0.0, 1.0); x = p0.x() + (p1.x() - p0.x()) * t; break; }
        case 3:  x = p2.x(); break;
        default: { const double t = std::clamp(1 - level / sus, 0.0, 1.0); x = p2.x() + (p3.x() - p2.x()) * t; break; }
        }
        const double y = bottom - level * h;
        p.setPen(QPen(theme::withAlpha(col::handle, 90), 1));
        p.drawLine(QPointF(x, top), QPointF(x, bottom));
        p.setPen(Qt::NoPen);
        p.setBrush(col::handle);
        p.drawEllipse(QPointF(x, y), 3.5, 3.5);
    }
private:
    int m_env;
};

// The FX chain as four blocks (EQ, Chorus, Delay, Reverb), lit when they do something.
class MonoFxDisplay : public Display {
public:
    MonoFxDisplay(DeviceBody* b, int w, int h) : Display(b, w, h) {
        InfoView::attach(this, "FX", "After the voices: an EQ (low and high shelves), a Dimension chorus, a synced ping-pong delay and a reverb, in that order. A lit block is in use.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const char* names[4] = {"EQ", "Chorus", "Delay", "Reverb"};
        const bool on[4] = {std::fabs(m_body->real(mono::fx(mono::EqLow))) > 0.05 || std::fabs(m_body->real(mono::fx(mono::EqHigh))) > 0.05,
                            m_body->pos(mono::fx(mono::ChorusMix)) > 0.001, m_body->pos(mono::fx(mono::DelayMix)) > 0.001, m_body->pos(mono::fx(mono::ReverbMix)) > 0.001};
        const QString values[4] = {QString("%1 / %2").arg(m_body->text(mono::fx(mono::EqLow)), m_body->text(mono::fx(mono::EqHigh))),
                                   m_body->text(mono::fx(mono::ChorusMix)), m_body->text(mono::fx(mono::DelayTime)), m_body->text(mono::fx(mono::ReverbMix))};
        const double w = (r.width() - 10) / 4, gap = 10;
        QFont f = p.font(); f.setPointSizeF(7); p.setFont(f);
        for (int k = 0; k < 4; ++k) {
            const QRectF box(r.left() + 4 + k * w, r.top() + 5, w - gap, r.height() - 10);
            p.setPen(QPen(on[k] ? col::curve : col::gridStrong, 1));
            p.setBrush(on[k] ? theme::withAlpha(col::curve, 60) : theme::devCell);
            p.drawRoundedRect(box, 3, 3);
            p.setPen(on[k] ? col::value : col::gridText);
            p.drawText(box.adjusted(0, 2, 0, 0), Qt::AlignTop | Qt::AlignHCenter, names[k]);
            p.setPen(col::gridText);
            p.drawText(box.adjusted(0, 0, 0, -2), Qt::AlignBottom | Qt::AlignHCenter, p.fontMetrics().elidedText(values[k], Qt::ElideRight, int(box.width()) - 4));
            if (k < 3) { p.setPen(col::gridText); p.drawText(QRectF(box.right(), r.top(), gap, r.height()), Qt::AlignCenter, "›"); }
        }
    }
};

class MonolithBody : public DeviceBody {
public:
    MonolithBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        audio::wavetable(0);   // build the tables now, not on the audio thread
        auto* root = rootRow(this, 4);
        m_main = buildMain();
        root->addWidget(m_main, 0, Qt::AlignTop);
        root->addWidget(vline(this));
        // the drawer buttons
        auto* col = vbox(2);
        const char* names[kDrawers] = {"Env", "LFO", "Step", "Mod", "Voice", "FX", "Curve"};
        const char* helps[kDrawers] = {"The four envelopes, with a dot showing where each is while a note plays: shape them as you hear them. Curve bends each one, Loop makes it repeat while the note holds.",
                                       "The four LFOs: rate (synced or free), shape, Fade, Retrig, and Loop or One-shot (an extra envelope).",
                                       "The two step sequencers: draw the steps, set how many and how fast they go, Glide between them, Retrig.",
                                       "Modulation: the sources to drag onto any knob, and every route with its amount.",
                                       "The voice: Poly / Mono / Legato, unison and its spread, glide, pitch bend range, velocity, key tracking, Restart, the modulation oscillator's wave.",
                                       "The FX after the voices: an EQ, a Dimension chorus, a synced ping-pong delay and a reverb.",
                                       "Two curves you draw (or roll at random) for knobs to follow over a bar or more: the phrase tool. Drag a curve's chip onto a knob."};
        for (int k = 0; k < kDrawers; ++k) {
            auto* b = new QPushButton(names[k], this);
            b->setObjectName("monoDrawer");
            b->setCheckable(true);
            b->setFixedSize(46, 19);
            b->setFocusPolicy(Qt::NoFocus);
            b->setStyleSheet(QString("QPushButton#monoDrawer{background:%1;color:%2;border:0;border-radius:2px;font-size:9px;}"
                                     "QPushButton#monoDrawer:checked{background:%3;color:%4;}").arg(col::button.name(), col::buttonText.name(), col::buttonOn.name(), col::buttonOnText.name()));
            InfoView::attach(b, QString("%1 drawer").arg(names[k]), QString(helps[k]) + " Click to slide it out beside the main panel; click again to close it.");
            connect(b, &QPushButton::clicked, this, [this, k] { toggleDrawer(k); });
            m_drawerButtons.push_back(b);
            col->addWidget(b);
        }
        col->addStretch(1);
        root->addLayout(col);
        // the drawer itself: a stack of pages whose width slides between 0 and the page's
        m_drawerHost = new QWidget(this);
        auto* dh = new QHBoxLayout(m_drawerHost);
        dh->setContentsMargins(0, 0, 0, 0);
        dh->setSpacing(4);
        dh->addWidget(vline(m_drawerHost));
        m_drawers = new QStackedWidget(m_drawerHost);
        m_drawers->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        dh->addWidget(m_drawers, 1);
        m_drawers->addWidget(buildEnv());
        m_drawers->addWidget(buildLfo());
        m_drawers->addWidget(buildStep());
        m_drawers->addWidget(buildMod());
        m_drawers->addWidget(buildVoice());
        m_drawers->addWidget(buildFx());
        m_drawers->addWidget(buildCurve());
        root->addWidget(m_drawerHost, 0, Qt::AlignTop);
        m_anim = new QPropertyAnimation(m_drawerHost, "maximumWidth", this);
        m_anim->setDuration(170);
        m_anim->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) { m_drawerHost->setMinimumWidth(v.toInt()); updateWidth(); });
        for (Knob* k : findChildren<Knob*>()) { k->setAcceptDrops(true); k->installEventFilter(this); }
        m_mainWidth = m_main->sizeHint().width();
        setDrawer(monoDrawers().value(c.deviceId, -1), false);
        rebuildRoutes();
    }

    static constexpr int kDrawers = 7;
    int  drawer() const { return m_open; }
    int  drawerWidth() const { return m_drawerHost->maximumWidth(); }
    void setDrawer(int k, bool animate = true) {
        m_open = k < 0 || k >= kDrawers ? -1 : k;
        monoDrawers().insert(ctx().deviceId, m_open);
        for (int i = 0; i < kDrawers; ++i) m_drawerButtons[size_t(i)]->setChecked(i == m_open);
        int target = 0;
        if (m_open >= 0) { m_drawers->setCurrentIndex(m_open); target = m_drawers->widget(m_open)->sizeHint().width() + 1 + 4; }
        m_anim->stop();
        if (animate && isVisible()) {
            m_anim->setStartValue(m_drawerHost->maximumWidth());
            m_anim->setEndValue(target);
            m_anim->start();
        } else {
            m_drawerHost->setMaximumWidth(target);
            m_drawerHost->setMinimumWidth(target);
            updateWidth();
        }
    }
    void toggleDrawer(int k) { setDrawer(m_open == k ? -1 : k); }
    bool modulated(int i) const override {
        const Device* d = device();
        if (d) for (const ModRoute& r : d->mods) if (r.param == i) return true;
        return false;
    }
    void refresh() override {
        DeviceBody::refresh();
        rebuildRoutes();
    }

    // ---- the routes (Device::mods), each edit one undo step
    void editMods(const QString& label, const std::function<void(QVector<ModRoute>&)>& fn, quintptr mergeKey = 0) {
        {
            EditScope edit(ctx().project, label, mergeKey);
            Device* d = findDevice(ctx().project, ctx().track, ctx().deviceId);   // after the snapshot
            if (!d) return;
            fn(d->mods);
        }
        emit ctx().project->deviceChanged();
    }
    void addRoute(int source, int param, double amount) {
        if (source < 0 || source >= mono::kSources || param < 0 || param >= count()) return;
        editMods(QString("Modulate %1 by %2").arg(name(param), mono::sourceName(source)), [=](QVector<ModRoute>& mods) {
            for (ModRoute& r : mods) if (r.source == source && r.param == param) { r.amount = amount; return; }
            if (mods.size() < mono::kMaxRoutes) mods.push_back({source, param, amount});
        });
    }
    // ---- the drawn curves (Device::curves), each edit one undo step; a drag is one step (beginEdit .. endEdit around its moves)
    void editCurve(int c, const QString& label, const std::function<void(QVector<CurvePoint>&)>& fn) {
        {
            EditScope edit(ctx().project, label);
            Device* d = findDevice(ctx().project, ctx().track, ctx().deviceId);   // after the snapshot
            if (!d) return;
            if (d->curves.size() < mono::kCurves) d->curves.resize(mono::kCurves);
            QVector<CurvePoint>& pts = d->curves[c];
            fn(pts);
            for (CurvePoint& pt : pts) { pt.x = std::clamp(pt.x, 0.0, 1.0); pt.y = std::clamp(pt.y, 0.0, 1.0); }
            std::stable_sort(pts.begin(), pts.end(), [](const CurvePoint& a, const CurvePoint& b) { return a.x < b.x; });
            while (pts.size() > mono::kCurvePoints) pts.removeLast();
        }
        emit ctx().project->deviceChanged();
    }
    void removeRoute(int index) {
        editMods("Remove Modulation", [index](QVector<ModRoute>& mods) { if (index >= 0 && index < mods.size()) mods.removeAt(index); });
    }
    void setRouteAmount(int index, double amount) {
        editMods("Modulation Amount", [index, amount](QVector<ModRoute>& mods) { if (index >= 0 && index < mods.size()) mods[index].amount = std::clamp(amount, -1.0, 1.0); },
                 deviceMergeKey(ctx().deviceId, 160 + (index & 31)));
    }

protected:
    void updateWidth() { setFixedWidth(8 + m_mainWidth + 4 + 1 + 4 + 46 + 4 + m_drawerHost->maximumWidth() + 8); }

    // Knobs take a dropped source; a right-click offers the sources and the knob's routes.
    bool eventFilter(QObject* o, QEvent* ev) override {
        auto* knob = qobject_cast<Knob*>(o);
        if (!knob) return DeviceBody::eventFilter(o, ev);
        const int param = knob->property("mapTarget").toString().section(':', 3, 3).toInt();
        if (ev->type() == QEvent::DragEnter || ev->type() == QEvent::DragMove) {
            auto* de = static_cast<QDragMoveEvent*>(ev);
            if (de->mimeData()->hasFormat(kModSourceMime)) { de->acceptProposedAction(); return true; }
            return false;
        }
        if (ev->type() == QEvent::Drop) {
            auto* de = static_cast<QDropEvent*>(ev);
            if (!de->mimeData()->hasFormat(kModSourceMime)) return false;
            addRoute(de->mimeData()->data(kModSourceMime).toInt(), param, 0.3);
            de->acceptProposedAction();
            setDrawer(3);
            return true;
        }
        if (ev->type() == QEvent::ContextMenu) {
            QMenu menu(this);
            QMenu* by = menu.addMenu("Modulate by");
            for (int s = 0; s < mono::kSources; ++s) {
                QAction* a = by->addAction(mono::sourceName(s));
                connect(a, &QAction::triggered, this, [this, s, param] { addRoute(s, param, 0.3); setDrawer(3); });
            }
            const Device* d = device();
            QList<int> here;
            if (d) for (int k = 0; k < d->mods.size(); ++k) if (d->mods[k].param == param) here << k;
            if (!here.isEmpty()) {
                menu.addSeparator();
                for (int k : here) {
                    QAction* a = menu.addAction(QString("Remove %1 (%2 %)").arg(mono::sourceName(d->mods[k].source)).arg(int(std::lround(d->mods[k].amount * 100))));
                    connect(a, &QAction::triggered, this, [this, k] { removeRoute(k); });
                }
            }
            menu.exec(static_cast<QContextMenuEvent*>(ev)->globalPos());
            return true;
        }
        return DeviceBody::eventFilter(o, ev);
    }

private:
    // ---- the main panel
    // One oscillator: table, its cycle, warp mode, then Position and Level big, Pitch, and the small ones.
    QWidget* oscColumn(int o) {
        auto* w = new QWidget(this);
        auto* l = vbox(2);
        auto* top = hbox(3);
        top->addWidget(section(QString("Osc %1").arg(o + 1)));
        top->addWidget(chooser(mono::oparam(o, mono::Table), 66));
        l->addLayout(top);
        auto* disp = new MonoWaveDisplay(this, 136, 42, o);
        watch(disp);
        l->addWidget(disp);
        l->addWidget(choices(mono::oparam(o, mono::Mode), {}, {}, 136, 14));
        auto* big = hbox(0);
        big->addWidget(knob(mono::oparam(o, mono::Position), 30, "Pos"));
        big->addWidget(knob(mono::oparam(o, mono::Level), 30, "Level"));
        big->addWidget(knob(mono::oparam(o, mono::Pitch), 24, "Pitch", true));
        big->addStretch(1);
        l->addLayout(big);
        auto* r2 = hbox(0);
        r2->addWidget(knob(mono::oparam(o, mono::Intensity), 14, "Intens"));
        r2->addWidget(knob(mono::oparam(o, mono::Fine), 14, "Fine", true));
        r2->addWidget(knob(mono::oparam(o, mono::FilterMix), 14, "F1/F2"));
        r2->addStretch(1);
        l->addLayout(r2);
        l->addStretch(1);
        w->setLayout(l);
        return w;
    }
    // The sub oscillator, the modulation oscillator, noise and feedback: a narrow column.
    QWidget* subColumn() {
        auto* w = new QWidget(this);
        auto* l = vbox(2);
        l->addWidget(section("Sub"));
        auto* sub = hbox(2);
        sub->addWidget(knob(mono::SubLevel, 22, "Sub"));
        auto* subOpts = vbox(2);
        QWidget* oct = choices(mono::SubOct, {}, {}, 50, 14);
        InfoView::attach(oct, "Sub Oct", "The sub oscillator an octave or two under oscillator 1's note.");
        QWidget* wave = chooser(mono::SubWave, 50);
        InfoView::attach(wave, "Sub Wave", "The sub's wave: a sine (pure weight), a square (growl under the growl), a triangle (between).");
        subOpts->addWidget(oct);
        subOpts->addWidget(wave);
        sub->addLayout(subOpts);
        sub->addStretch(1);
        l->addLayout(sub);
        l->addWidget(section("Mod Osc"));
        auto* m1 = hbox(0);
        m1->addWidget(knob(mono::ModPitch, 14, "Pitch", true));
        m1->addWidget(knob(mono::ModPhase, 14, "Phase"));
        m1->addWidget(knob(mono::ModRing, 14, "Ring"));
        m1->addStretch(1);
        l->addLayout(m1);
        l->addWidget(section("Noise · Feedb."));
        l->addLayout(knobRow(this, {mono::NoiseLevel, mono::NoiseColor, mono::Feedback}, 14, {"Noise", "Color", "Feedb."}));
        l->addStretch(1);
        w->setLayout(l);
        return w;
    }
    // One filter: type, its response, Cutoff big, Reso; filter 1 the drive and the Par/Ser routing,
    // filter 2 the key tracking, F Mix and the insert after it.
    QWidget* filterColumn(int f) {
        auto* w = new QWidget(this);
        auto* l = vbox(2);
        auto* top = hbox(3);
        top->addWidget(section(QString("Filter %1").arg(f + 1)));
        top->addWidget(chooser(mono::fparam(f, mono::FType), 72));
        l->addLayout(top);
        auto* disp = new MonoFilterDisplay(this, 142, 52, f);
        watch(disp);
        l->addWidget(disp);
        auto* r = hbox(0);
        r->addWidget(knob(mono::fparam(f, mono::FCutoff), 30, "Cutoff"));
        r->addWidget(knob(mono::fparam(f, mono::FReso), 22, "Reso"));
        if (f == 0) {
            QWidget* drive = knob(mono::FDrive, 16, "Drive");
            if (auto* k = drive->findChild<Knob*>()) InfoView::attach(k, "F Drive", "Drives the sound into both filters (a tanh before them): thicker, with harmonics for the filters to work on.");
            r->addWidget(drive);
            r->addWidget(knob(mono::FRouting, 16, "Par/Ser"));
        } else { r->addWidget(knob(mono::KeyTrack, 16, "Key")); r->addWidget(knob(mono::FMix, 16, "F Mix")); }
        r->addStretch(1);
        l->addLayout(r);
        auto* ins = hbox(3);
        if (f == 1) {
            ins->addWidget(section("Insert"));
            ins->addWidget(chooser(mono::InsertType, 56));
            ins->addWidget(knob(mono::InsertAmount, 14, "Amount"));
        }
        ins->addStretch(1);
        l->addLayout(ins);
        l->addStretch(1);
        w->setLayout(l);
        return w;
    }
    QWidget* buildMain() {
        auto* page = new QWidget(this);
        auto* row = hbox(4);
        row->setContentsMargins(0, 0, 0, 0);
        for (int o = 0; o < mono::kOscs; ++o) { row->addWidget(oscColumn(o), 0, Qt::AlignTop); row->addWidget(vline(page)); }
        row->addWidget(subColumn(), 0, Qt::AlignTop);
        row->addWidget(vline(page));
        for (int f = 0; f < mono::kFilters; ++f) { row->addWidget(filterColumn(f), 0, Qt::AlignTop); row->addWidget(vline(page)); }
        auto* master = vbox(2);
        master->addWidget(section("Master"));
        auto* vol = hbox(0);
        vol->addWidget(knob(mono::Volume, 36));
        vol->addStretch(1);
        master->addLayout(vol);
        master->addWidget(section("Macros"));
        auto* mg = new QGridLayout;
        mg->setContentsMargins(0, 0, 0, 0);
        mg->setSpacing(2);
        for (int m = 0; m < mono::kMacrosAll; ++m) mg->addWidget(knob(mono::macro(m), 18), m / 4, m % 4);   // eight, two rows of four (0.21)
        master->addLayout(mg);
        master->addStretch(1);
        row->addLayout(master);
        page->setLayout(row);
        return page;
    }

    // ---- the drawers
    QWidget* buildEnv() {
        auto* page = new QWidget(m_drawers);
        auto* grid = new QGridLayout(page);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(2);
        for (int e = 0; e < mono::kEnvs; ++e) {
            auto* disp = new MonoEnvDisplay(this, 176, 46, e);
            animate(disp);
            auto* cell = vbox(1);
            cell->addWidget(disp);
            auto* knobs = knobRow(this, {mono::eparam(e, mono::A), mono::eparam(e, mono::D), mono::eparam(e, mono::S), mono::eparam(e, mono::R)}, 14, {"A", "D", "S", "R"});
            QWidget* curve = knob(mono::ecurve(e), 14, "Curve", true);
            if (auto* k = curve->findChild<Knob*>()) InfoView::attach(k, name(mono::ecurve(e)), "Bends the envelope: to the right it starts slow and ends quick (exponential), to the left quick then slow (logarithmic), in the middle straight.");
            knobs->insertWidget(knobs->count() - 1, curve);
            QWidget* loop = toggle(mono::eloop(e), "Loop", 34);
            InfoView::attach(loop, name(mono::eloop(e)), "On: while the note holds, the envelope runs its attack and decay again and again (a rhythmic pulse of any shape). Off: it holds its sustain.");
            knobs->insertWidget(knobs->count() - 1, loop, 0, Qt::AlignVCenter);
            knobs->insertWidget(knobs->count() - 1, new SourceChip(this, mono::SrcEnv1 + e), 0, Qt::AlignVCenter);   // before the stretch
            cell->addLayout(knobs);
            grid->addLayout(cell, e / 2, e % 2);
        }
        return page;
    }
    QWidget* buildLfo() {
        auto* page = new QWidget(m_drawers);
        auto* row = hbox(8);
        row->setContentsMargins(0, 0, 0, 0);
        for (int l = 0; l < mono::kLfosAll; ++l) {   // four since 0.21: LFO 3 and 4 in the appended block
            auto* c = vbox(2);
            auto* top = hbox(3);
            top->addWidget(section(QString("LFO %1").arg(l + 1)));
            top->addWidget(chooser(mono::lparam(l, mono::LRate), 60));
            top->addWidget(new SourceChip(this, l < mono::kLfos ? mono::SrcLfo1 + l : mono::SrcLfo3 + (l - mono::kLfos)));
            c->addLayout(top);
            auto* disp = new MonoLfoDisplay(this, 150, 40, l);
            animate(disp);
            c->addWidget(disp);
            c->addWidget(choices(mono::lparam(l, mono::LShape), {}, lfoIcon, 150, 14));
            QWidget* mode = choices(mono::lmode(l), {}, {}, 150, 14);
            InfoView::attach(mode, name(mono::lmode(l)), "Loop: the LFO runs round and round. One-shot: it runs one cycle from each note's start and holds its end, an extra envelope with any shape.");
            c->addWidget(mode);
            auto* r = hbox(0);
            r->addWidget(knob(mono::lparam(l, mono::LHz), 14, "Free Hz"));
            QWidget* fade = knob(mono::lfade(l), 14, "Fade");
            if (auto* k = fade->findChild<Knob*>()) InfoView::attach(k, name(mono::lfade(l)), "Ramps the LFO in over this time from each note's start (vibrato that comes in late). All the way down: at once.");
            r->addWidget(fade);
            QWidget* retrig = toggle(mono::lretrig(l), "Retrig", 40);
            InfoView::attach(retrig, name(mono::lretrig(l)), "On: every note restarts this LFO from its own start, so each note gets the same movement. Off: one LFO shared by all the notes, in time with the song (or free).");
            r->addWidget(retrig, 0, Qt::AlignVCenter);
            r->addStretch(1);
            c->addLayout(r);
            c->addStretch(1);
            row->addLayout(c);
        }
        page->setLayout(row);
        return page;
    }
    QWidget* buildStep() {
        auto* page = new QWidget(m_drawers);
        auto* row = hbox(8);
        row->setContentsMargins(0, 0, 0, 0);
        for (int s = 0; s < mono::kSteppers; ++s) {
            auto* c = vbox(2);
            auto* top = hbox(3);
            top->addWidget(section(QString("Step %1").arg(s + 1)));
            top->addWidget(chooser(mono::sparam(s, mono::SRate), 56));
            top->addWidget(new SourceChip(this, mono::SrcStep1 + s));
            c->addLayout(top);
            auto* disp = new StepDisplay(this, 170, 70, s);
            animate(disp);
            c->addWidget(disp);
            auto* r = hbox(0);
            r->addWidget(knob(mono::sparam(s, mono::SCount), 14, "Steps"));
            r->addWidget(knob(mono::sparam(s, mono::SGlide), 14, "Glide"));
            QWidget* retrig = toggle(mono::sretrig(s), "Retrig", 40);
            InfoView::attach(retrig, name(mono::sretrig(s)), "On: every note starts the sequence from step 1. Off: the sequence runs with the song's beat, the same for every note.");
            r->addWidget(retrig, 0, Qt::AlignVCenter);
            r->addStretch(1);
            c->addLayout(r);
            // every step has a (hidden) knob too, so it can be automated and mapped like any knob
            auto* hidden = new QWidget(page);
            auto* hl = hbox(0);
            for (int i = 0; i < mono::kSteps; ++i) hl->addWidget(knob(mono::step(s, i), 10));
            hidden->setLayout(hl);
            hidden->setFixedHeight(0);
            hidden->setVisible(false);
            c->addWidget(hidden);
            c->addStretch(1);
            row->addLayout(c);
        }
        page->setLayout(row);
        return page;
    }
    QWidget* buildMod() {
        auto* page = new QWidget(m_drawers);
        auto* row = hbox(6);
        row->setContentsMargins(0, 0, 0, 0);
        auto* chips = vbox(2);
        chips->addWidget(section("Sources: drag onto a knob"));
        auto* grid = new QGridLayout;
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(2);
        for (int s = 0; s < mono::kSources; ++s) grid->addWidget(new SourceChip(this, s), s / 4, s % 4);   // four a row: 26 sources in the device's height
        chips->addLayout(grid);
        auto* add = new QPushButton("Add…", this);
        add->setFocusPolicy(Qt::NoFocus);
        add->setFixedSize(60, 18);
        InfoView::attach(add, "Add Route", "A route without dragging: pick the source, then the knob it turns.");
        connect(add, &QPushButton::clicked, this, [this] { addMenu(); });
        chips->addWidget(add, 0, Qt::AlignLeft);
        chips->addStretch(1);
        row->addLayout(chips);
        row->addWidget(vline(page));
        auto* listCol = vbox(2);
        listCol->addWidget(section("Routes"));
        m_routeArea = new QScrollArea(page);
        m_routeArea->setWidgetResizable(true);
        m_routeArea->setFrameShape(QFrame::NoFrame);
        m_routeArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_routeArea->setFixedSize(226, 158);
        m_routeArea->setStyleSheet("background:transparent;");
        m_routeList = new QWidget(m_routeArea);
        m_routeLayout = new QVBoxLayout(m_routeList);
        m_routeLayout->setContentsMargins(0, 0, 0, 0);
        m_routeLayout->setSpacing(1);
        m_routeArea->setWidget(m_routeList);
        listCol->addWidget(m_routeArea);
        row->addLayout(listCol);
        page->setLayout(row);
        return page;
    }
    QWidget* buildCurve() {
        auto* page = new QWidget(m_drawers);
        auto* row = hbox(8);
        row->setContentsMargins(0, 0, 0, 0);
        for (int c = 0; c < mono::kCurves; ++c) {
            auto* col = vbox(2);
            auto* top = hbox(3);
            top->addWidget(section(QString("Curve %1").arg(c + 1)));
            top->addWidget(chooser(mono::cparam(c, mono::CRate), 60));
            top->addWidget(new SourceChip(this, mono::SrcCurve1 + c));
            col->addLayout(top);
            auto* disp = new MonoCurveDisplay(this, 236, 74, c,
                [this, c] { const Device* d = device(); return d && c < d->curves.size() ? d->curves[c] : QVector<CurvePoint>(); },
                [this, c](const QString& label, std::function<void(QVector<CurvePoint>&)> fn) { editCurve(c, label, fn); },
                [this](bool on) { if (on) ctx().project->beginEdit("Edit Curve"); else ctx().project->endEdit(); });
            disp->setObjectName("monoCurve");
            animate(disp);
            col->addWidget(disp);
            auto* r = hbox(3);
            QWidget* mode = choices(mono::cparam(c, mono::CMode), {}, {}, 84, 14);
            InfoView::attach(mode, name(mono::cparam(c, mono::CMode)), "Loop: the curve goes round and round. One-shot: once from its start, then it holds its end.");
            r->addWidget(mode);
            QWidget* retrig = toggle(mono::cparam(c, mono::CRetrig), "Retrig", 40);
            InfoView::attach(retrig, name(mono::cparam(c, mono::CRetrig)), "On: every note starts the curve from its beginning. Off: the curve runs with the song, bar after bar.");
            r->addWidget(retrig, 0, Qt::AlignVCenter);
            r->addWidget(knob(mono::cparam(c, mono::CSmooth), 14, "Smooth"));
            auto* rnd = new QPushButton("Random", this);
            rnd->setObjectName("monoCurveRandom");
            rnd->setFocusPolicy(Qt::NoFocus);
            rnd->setFixedSize(50, 16);
            InfoView::attach(rnd, "Random", "Rolls eight random steps into the curve: a phrase to start from, every point still yours to drag.");
            connect(rnd, &QPushButton::clicked, this, [disp] { disp->randomize(8, true); });
            r->addWidget(rnd, 0, Qt::AlignVCenter);
            r->addStretch(1);
            col->addLayout(r);
            col->addStretch(1);
            row->addLayout(col);
            if (c + 1 < mono::kCurves) row->addWidget(vline(page));
        }
        page->setLayout(row);
        return page;
    }
    QWidget* buildVoice() {
        auto* page = new QWidget(m_drawers);
        auto* l = vbox(3);
        l->setContentsMargins(0, 0, 0, 0);
        auto* r1 = hbox(6);
        auto* voice = vbox(2);
        voice->addWidget(section("Voice"));
        voice->addWidget(choices(mono::VoiceMode, {}, {}, 120, 14));
        voice->addWidget(section("Unison"));
        voice->addWidget(choices(mono::Unison, {}, {}, 120, 14));
        voice->addStretch(1);
        r1->addLayout(voice);
        auto* modw = vbox(2);
        modw->addWidget(section("Mod Osc Wave"));
        QWidget* mw = choices(mono::ModOscWave, {}, waveIcon, 120, 16);
        InfoView::attach(mw, "Mod Wave", "The modulation oscillator's wave for the phase and ring modulation: a sine is smooth, a saw or a square tears.");
        modw->addWidget(mw);
        QWidget* restart = toggle(mono::OscRestart, "Restart", 60);
        InfoView::attach(restart, "Restart", "On: every oscillator starts its cycle at 0 with each new note, so every hit sounds the same (punchier basses). Off: free phases, a looser, wider sound.");
        modw->addWidget(restart, 0, Qt::AlignLeft);
        modw->addStretch(1);
        r1->addLayout(modw);
        l->addLayout(r1);
        auto* knobs = hbox(0);
        knobs->addWidget(knob(mono::Detune, 22));
        knobs->addWidget(knob(mono::Spread, 22));
        knobs->addWidget(knob(mono::Glide, 22));
        knobs->addWidget(knob(mono::BendRange, 22, "Bend"));
        knobs->addWidget(knob(mono::VelAmp, 22, "Vel>Amp"));
        knobs->addWidget(knob(mono::KeyTrack, 22, "Key>Cut"));
        knobs->addStretch(1);
        l->addLayout(knobs);
        l->addStretch(1);
        page->setLayout(l);
        return page;
    }
    QWidget* buildFx() {
        auto* page = new QWidget(m_drawers);
        auto* l = vbox(3);
        l->setContentsMargins(0, 0, 0, 0);
        auto* disp = new MonoFxDisplay(this, 380, 36);
        watch(disp);
        l->addWidget(disp);
        auto* row = hbox(6);
        auto part = [&](const QString& title, std::function<void(QVBoxLayout*)> fill) {
            auto* c = vbox(2);
            c->addWidget(section(title));
            fill(c);
            c->addStretch(1);
            row->addLayout(c);
            row->addWidget(vline(page));
        };
        part("EQ", [&](QVBoxLayout* c) { c->addLayout(knobRow(this, {mono::fx(mono::EqLow), mono::fx(mono::EqHigh)}, 20, {"Low", "High"})); });
        part("Chorus", [&](QVBoxLayout* c) { c->addLayout(knobRow(this, {mono::fx(mono::ChorusRate), mono::fx(mono::ChorusDepth), mono::fx(mono::ChorusMix)}, 20, {"Rate", "Depth", "Mix"})); });
        part("Delay", [&](QVBoxLayout* c) {
            c->addWidget(chooser(mono::fx(mono::DelayTime), 70));
            c->addLayout(knobRow(this, {mono::fx(mono::DelayFeedback), mono::fx(mono::DelayTone), mono::fx(mono::DelayMix)}, 20, {"Feedb.", "Tone", "Mix"}));
        });
        part("Reverb", [&](QVBoxLayout* c) { c->addLayout(knobRow(this, {mono::fx(mono::ReverbSize), mono::fx(mono::ReverbMix)}, 20, {"Size", "Mix"})); });
        row->addStretch(1);
        l->addLayout(row);
        l->addStretch(1);
        page->setLayout(l);
        return page;
    }

    void addMenu() {
        QMenu menu(this);
        for (int s = 0; s < mono::kSources; ++s) {
            QMenu* src = menu.addMenu(mono::sourceName(s));
            QMenu* section = nullptr;
            QString last;
            for (int i = 0; i < count(); ++i) {
                const QString sec = mono::sectionName(i);
                if (sec != last) { section = src->addMenu(sec); last = sec; }
                QAction* a = section->addAction(name(i));
                connect(a, &QAction::triggered, this, [this, s, i] { addRoute(s, i, 0.3); });
            }
        }
        menu.exec(QCursor::pos());
    }

    void rebuildRoutes() {
        if (!m_routeLayout) return;
        const Device* d = device();
        const QVector<ModRoute> mods = d ? d->mods : QVector<ModRoute>();
        if (mods == m_shownRoutes && !m_routeRows.empty()) { for (auto& [row, k] : m_routeRows) if (k < mods.size()) row->setValue(mods[k].amount, false); return; }
        if (mods == m_shownRoutes && mods.isEmpty() && m_routeLayout->count() > 0) return;
        m_shownRoutes = mods;
        m_routeRows.clear();
        while (QLayoutItem* it = m_routeLayout->takeAt(0)) {
            if (it->widget()) it->widget()->deleteLater();
            delete it;
        }
        if (mods.isEmpty()) {
            auto* hint = new QLabel("No routes yet. Drag a source onto a knob (the chips are here and in the Env, LFO and Step drawers), or right-click a knob.", m_routeList);
            hint->setWordWrap(true);
            hint->setStyleSheet(QString("color:%1;font-size:9px;background:transparent;border:0;").arg(col::gridText.name()));
            m_routeLayout->addWidget(hint);
        }
        for (int k = 0; k < mods.size(); ++k) {
            const ModRoute& r = mods[k];
            auto* rowW = new QWidget(m_routeList);
            auto* row = hbox(3);
            const QString knob = name(r.param);   // knob names already say Osc1 / F1 / Env1 …; plain ones get their section
            const QString where = knob.contains(' ') && knob.section(' ', 0, 0).right(1)[0].isDigit() ? knob : mono::sectionName(r.param) + " " + knob;
            auto* label = new QLabel(QString("%1 → %2").arg(mono::sourceName(r.source), where), rowW);
            label->setStyleSheet(QString("color:%1;font-size:9px;background:transparent;border:0;").arg(col::value.name()));
            label->setFixedWidth(140);
            label->setText(label->fontMetrics().elidedText(label->text(), Qt::ElideMiddle, 138));
            label->setToolTip(QString("%1 → %2").arg(mono::sourceName(r.source), where));
            row->addWidget(label);
            auto* amount = new DragValue(rowW);
            amount->setRange(-1, 1);
            amount->setDefault(0);
            amount->setValue(r.amount, false);
            amount->setDragSensitivity(200);
            amount->setFixedWidth(48);
            amount->setFormatter([](double v) { return QString("%1 %").arg(int(std::lround(v * 100))); });
            InfoView::attach(amount, "Amount", "How far the source turns the knob: a share of the knob's range, up or down. Drag, Shift for fine; double-click to type.");
            connect(amount, &DragValue::valueEdited, this, [this, k](double v) { setRouteAmount(k, v); });
            row->addWidget(amount);
            auto* x = new QToolButton(rowW);
            x->setText("✕");
            x->setFixedSize(16, 16);
            x->setFocusPolicy(Qt::NoFocus);
            InfoView::attach(x, "Remove Route", "Takes this route away.");
            connect(x, &QToolButton::clicked, this, [this, k] { removeRoute(k); });
            row->addWidget(x);
            row->addStretch(1);
            rowW->setLayout(row);
            m_routeLayout->addWidget(rowW);
            m_routeRows.push_back({amount, k});
        }
        m_routeLayout->addStretch(1);
    }

    QWidget* m_main = nullptr;
    int m_mainWidth = 0, m_open = -1;
    std::vector<QPushButton*> m_drawerButtons;
    QWidget* m_drawerHost = nullptr;
    QStackedWidget* m_drawers = nullptr;
    QPropertyAnimation* m_anim = nullptr;
    QScrollArea* m_routeArea = nullptr;
    QWidget* m_routeList = nullptr;
    QVBoxLayout* m_routeLayout = nullptr;
    QVector<ModRoute> m_shownRoutes;
    std::vector<std::pair<DragValue*, int>> m_routeRows;
};

} // namespace

DeviceBody* createInstrumentBody(const DeviceContext& ctx, QWidget* parent) {
    switch (ctx.type) {
    case T::DrumRack:       return new DrumRackBody(ctx, parent);
    case T::DrumStudio:     return createDrumStudioBody(ctx, parent);
    case T::Monolith:       return new MonolithBody(ctx, parent);
    case T::DrumKit: case T::Drum808: case T::DrumAcoustic: return new DrumBody(ctx, parent);
    case T::AnalogSynth:    return new AnalogBody(ctx, parent);
    case T::FMSynth:        return new FmBody(ctx, parent);
    case T::WavetableSynth: return new WavetableBody(ctx, parent);
    case T::Sampler:        return new SamplerBody(ctx, parent);
    case T::Organ:          return new OrganBody(ctx, parent);
    case T::Bass:           return new BassBody(ctx, parent);
    case T::Electric:       return new ElectricBody(ctx, parent);
    default:                return nullptr;
    }
}

} // namespace sessions::devui
