// The MIDI effects' panels. Parameter indices follow core/DeviceSpecs.cpp.

#include "Bodies.h"
#include "audio/Devices.h"
#include "core/Scales.h"
#include "ui/InfoView.h"
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <cmath>

namespace sessions::devui {

namespace {

using T = DeviceType;

// ============================================================================ Arpeggiator

// What the arpeggiator plays from a held C major chord: the steps in order, across the
// octaves, the one playing now lit.
class ArpDisplay : public Display {
public:
    ArpDisplay(DeviceBody* b, int w, int h) : Display(b, w, h) {
        InfoView::attach(this, "Pattern", "What the arpeggiator plays from a held C major chord, step by step. The lit step is the one playing.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const int style = m_body->pick(3), octaves = int(m_body->real(2));
        std::vector<int> seq;
        for (int o = 0; o < octaves; ++o) for (int n : {0, 4, 7}) seq.push_back(n + 12 * o);
        if (style == 4) for (int o = 0; o < octaves; ++o) std::swap(seq[size_t(o * 3 + 1)], seq[size_t(o * 3 + 2)]);   // as played: C, G, E
        const int len = int(seq.size());
        const int period = style == 2 ? std::max(1, 2 * len - 2) : style == 3 ? 16 : len;
        std::vector<int> order;
        for (int s = 0; s < period; ++s) {
            int idx = s % len;
            if (style == 1) idx = len - 1 - s % len;
            else if (style == 2) idx = s < len ? s : period - s;
            else if (style == 3) idx = int((s * 7 + 3) % len);
            order.push_back(seq[size_t(idx)]);
        }
        const int top = 12 * octaves;
        const double cw = r.width() / period, rh = (r.height() - 10) / (top + 1);
        for (int s = 0; s <= period; ++s) {
            p.setPen(QPen(s % 4 ? col::grid : col::gridStrong, 1));
            const double x = std::round(r.left() + s * cw) + 0.5;
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        }
        const bool running = m_body->meter(1) > 0;
        const int now = running ? int(m_body->meter(0)) % period : -1;
        const double gate = std::min(1.0, m_body->real(1));
        for (int s = 0; s < period; ++s) {
            const double y = r.bottom() - 4 - (order[size_t(s)] + 1) * rh;
            p.fillRect(QRectF(r.left() + s * cw + 1, y, std::max(2.0, cw * gate - 2), std::max(2.0, rh)), s == now ? col::curve : col::cyan);
        }
        drawCaption(p, r, m_body->text(3) + "  " + m_body->text(0), Qt::AlignTop | Qt::AlignRight);
    }
};

class ArpBody : public DeviceBody {
public:
    ArpBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = rootRow(this);
        auto* a = vbox(4);
        a->addWidget(choices(3, {"Up", "Down", "U/D", "Rnd", "Play"}, {}, 196));
        auto* disp = new ArpDisplay(this, 196, 126);
        animate(disp);
        a->addWidget(disp);
        a->addStretch(1);
        root->addLayout(a);
        root->addWidget(vline(this));
        auto* b = vbox(4);
        b->addWidget(section("Rate"));
        b->addWidget(chooser(0, 64));
        b->addWidget(section("Steps"));
        b->addWidget(choices(2, {"1", "2", "3", "4"}, {}, 64));
        b->addWidget(knob(1, 24));
        b->addStretch(1);
        root->addLayout(b);
        setFixedWidth(300);
    }
};

// ============================================================================ Chord

// A shift: a vertical slider from -12 to +12 semitones, drawn from the middle.
class ShiftSlider : public QWidget {
public:
    ShiftSlider(DeviceBody* body, int i, int number) : QWidget(body), m_body(body), m_i(i), m_number(number) {
        setFixedSize(34, 132);
        setCursor(Qt::SizeVerCursor);
        setProperty("mapTarget", body->target(i));
        InfoView::attach(this, QString("Shift %1").arg(number), "Adds a note this many semitones from each note played (0: none). Drag up or down; double-click for 0.");
        body->watch(this);
    }
protected:
    QRectF track() const { return QRectF(10, 14, width() - 20, height() - 30); }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QFont f = font(); f.setPointSizeF(6.5); p.setFont(f);
        p.setPen(col::label);
        p.drawText(QRectF(0, 0, width(), 12), Qt::AlignCenter, QString("Shift %1").arg(m_number));
        const QRectF t = track();
        p.fillRect(t, col::display);
        const double mid = t.center().y();
        p.setPen(QPen(col::gridStrong, 1));
        p.drawLine(QPointF(t.left(), mid), QPointF(t.right(), mid));
        const int v = int(m_body->real(m_i));
        const double y = mid - v / 12.0 * (t.height() / 2);
        if (v != 0) p.fillRect(QRectF(t.left() + 2, std::min(y, mid), t.width() - 4, std::fabs(y - mid)), col::curve);
        p.setPen(v ? col::value : col::gridText);
        p.drawText(QRectF(0, height() - 14, width(), 14), Qt::AlignCenter, v ? m_body->text(m_i) : QString("–"));
    }
    void mousePressEvent(QMouseEvent* e) override { m_y = e->position().y(); m_start = m_body->real(m_i); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton)) return;
        const double v = std::clamp(std::round(m_start + (m_y - e->position().y()) / 5.0), -12.0, 12.0);
        m_body->edit(m_i, m_body->posFor(m_i, v));
    }
    void mouseDoubleClickEvent(QMouseEvent*) override { m_body->edit(m_i, 0.5); }
private:
    DeviceBody* m_body; int m_i, m_number;
    double m_y = 0, m_start = 0;
};

// Two octaves of keys: C3 played (outlined), the chord's added notes lit.
class ChordKeys : public Display {
public:
    ChordKeys(DeviceBody* b, int w, int h) : Display(b, w, h) {
        InfoView::attach(this, "Chord", "The chord you get when you play C3: C3 outlined, the added notes lit.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const int base = 48;   // C2 .. B4: shifts of ±12 around C3
        std::vector<int> on;
        for (int i : {0, 1, 2, 4, 5}) { const int s = int(m_body->real(i)); if (s) on.push_back(60 + s); }
        auto isBlack = [](int n) { const int pc = n % 12; return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; };
        int whites = 0;
        for (int n = base; n < base + 36; ++n) whites += isBlack(n) ? 0 : 1;
        const double ww = r.width() / whites;
        std::vector<std::pair<int, QRectF>> keys;
        int wi = 0;
        for (int n = base; n < base + 36; ++n) {
            if (isBlack(n)) continue;
            keys.push_back({n, QRectF(r.left() + wi * ww, r.top(), ww, r.height())});
            ++wi;
        }
        auto litColor = [&](int n) { return std::find(on.begin(), on.end(), n) != on.end(); };
        for (const auto& [n, kr] : keys) {
            p.setPen(QPen(theme::devPadEdge, 1));
            p.setBrush(litColor(n) ? col::curve : theme::devKeyWhite);
            p.drawRect(kr.adjusted(0.5, 0.5, -0.5, -0.5));
            if (n == 60) { p.setPen(QPen(col::cyan, 2)); p.setBrush(Qt::NoBrush); p.drawRect(kr.adjusted(2, 2, -2, -2)); }
        }
        wi = 0;
        for (int n = base; n < base + 36; ++n) {
            if (!isBlack(n)) { ++wi; continue; }
            const QRectF kr(r.left() + wi * ww - ww * 0.3, r.top(), ww * 0.6, r.height() * 0.6);
            p.setPen(QPen(theme::devPadEdge, 1));
            p.setBrush(litColor(n) ? col::curve : theme::devKeyBlack);
            p.drawRect(kr);
        }
    }
};

class ChordBody : public DeviceBody {
public:
    ChordBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(8, 6, 8, 4);
        root->setSpacing(4);
        root->addWidget(new ChordKeys(this, 300, 36));
        auto* row = hbox(2);
        int n = 1;
        for (int i : {0, 1, 2, 4, 5}) row->addWidget(new ShiftSlider(this, i, n++));
        row->addWidget(vline(this));
        auto* v = vbox(0);
        v->addWidget(knob(3, 26));
        v->addStretch(1);
        row->addLayout(v);
        row->addStretch(1);
        root->addLayout(row);
        root->addStretch(1);
        setFixedWidth(316);
    }
};

// ============================================================================ Scale

// Where each of the twelve notes goes: input across, output up. The last note played is lit.
class ScaleGrid : public Display {
public:
    ScaleGrid(DeviceBody* b, int w, int h) : Display(b, w, h) {
        InfoView::attach(this, "Scale Map", "Each note you play (across) and the note it becomes (up). The scale's notes stay; the others move to the nearest one.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const std::vector<float> pos = m_body->positions();
        const double cw = (r.width() - 14) / 12, ch = (r.height() - 12) / 12;
        const double left = r.left() + 14, bottom = r.bottom() - 12;
        const int root = m_body->pick(0), scale = m_body->pick(1);
        for (int k = 0; k < 12; ++k) {
            const bool inS = inScale(k, root, scale);
            p.fillRect(QRectF(left, bottom - (k + 1) * ch, r.width() - 14, ch), inS ? theme::withAlpha(theme::devHighlight, 14) : QColor(0, 0, 0, 0));
            p.setPen(inS ? col::label : col::gridText);
            p.drawText(QRectF(r.left(), bottom - (k + 1) * ch, 13, ch), Qt::AlignCenter, noteName(k));
            p.drawText(QRectF(left + k * cw, bottom, cw, 12), Qt::AlignCenter, QString(noteName(k)).left(2));
        }
        const int lastIn = int(m_body->meter(0)), lastOut = int(m_body->meter(1));
        const bool played = lastIn > 0 || lastOut > 0;
        for (int k = 0; k < 12; ++k) {
            const int out = audio::scaleMap(pos.data(), 60 + k) - 60;
            const int pc = ((out % 12) + 12) % 12;
            const QPointF c(left + (k + 0.5) * cw, bottom - (pc + 0.5) * ch);
            const bool lit = played && (lastIn % 12) == k;
            p.setPen(Qt::NoPen);
            p.setBrush(lit ? col::curve : (out == k ? col::cyan : theme::devDot));
            p.drawEllipse(c, std::min(cw, ch) * 0.32, std::min(cw, ch) * 0.32);
        }
    }
};

class ScaleBody : public DeviceBody {
public:
    ScaleBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = rootRow(this);
        auto* a = vbox(4);
        a->addWidget(section("Root"));
        a->addWidget(chooser(0, 90));
        a->addWidget(section("Scale"));
        a->addWidget(chooser(1, 110));
        a->addWidget(knob(2, 26));
        a->addStretch(1);
        root->addLayout(a);
        root->addWidget(vline(this));
        auto* grid = new ScaleGrid(this, 170, 172);
        animate(grid);
        root->addWidget(grid, 0, Qt::AlignTop);
        setFixedWidth(318);
    }
};

// ============================================================================ Velocity

// The curve from how hard you play (across) to what comes out (up): Out Hi / Low squeeze
// it, Drive bends it, Random spreads it (shaded). The dot is the last note.
class VelocityDisplay : public Display {
public:
    VelocityDisplay(DeviceBody* b, int w, int h) : Display(b, w, h) {
        setCursor(Qt::SizeVerCursor);
        InfoView::attach(this, "Velocity Curve", "How hard you play (across) against what comes out (up). Drag up or down to bend the curve (Drive). The dot is the last note.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        for (int k = 1; k < 4; ++k) {
            p.setPen(QPen(col::grid, 1));
            p.drawLine(QPointF(r.left() + r.width() * k / 4, r.top()), QPointF(r.left() + r.width() * k / 4, r.bottom()));
            p.drawLine(QPointF(r.left(), r.top() + r.height() * k / 4), QPointF(r.right(), r.top() + r.height() * k / 4));
        }
        const std::vector<float> pos = m_body->positions();
        const double rnd = m_body->pos(3) * 0.5;
        auto at = [&](double in, double out) { return QPointF(r.left() + in * r.width(), r.bottom() - std::clamp(out, 0.0, 1.0) * r.height()); };
        QPainterPath path, band;
        for (int i = 0; i <= 64; ++i) {
            const double in = i / 64.0, out = audio::velocityMap(pos.data(), float(in));
            if (i == 0) path.moveTo(at(in, out)); else path.lineTo(at(in, out));
        }
        if (rnd > 0.001) {
            for (int i = 0; i <= 64; ++i) { const double in = i / 64.0; const QPointF q = at(in, audio::velocityMap(pos.data(), float(in)) + rnd); if (i == 0) band.moveTo(q); else band.lineTo(q); }
            for (int i = 64; i >= 0; --i) { const double in = i / 64.0; band.lineTo(at(in, audio::velocityMap(pos.data(), float(in)) - rnd)); }
            band.closeSubpath();
            p.fillPath(band, col::cyanFill);
        }
        p.setPen(QPen(col::curve, 1.7));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        const float in = m_body->meter(0), out = m_body->meter(1);
        if (in > 0) {
            p.setPen(Qt::NoPen);
            p.setBrush(col::cyan);
            p.drawEllipse(at(in, out), 3.5, 3.5);
        }
    }
    void mousePressEvent(QMouseEvent* e) override { m_y = e->position().y(); m_start = m_body->pos(2); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (e->buttons() & Qt::LeftButton) m_body->editMany({{2, m_start + (m_y - e->position().y()) / 160.0}}, 2);
    }
private:
    double m_y = 0, m_start = 0;
};

class VelocityBody : public DeviceBody {
public:
    VelocityBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = rootRow(this);
        auto* disp = new VelocityDisplay(this, 170, 170);
        animate(disp);
        root->addWidget(disp, 0, Qt::AlignTop);
        root->addWidget(vline(this));
        auto* g = new QGridLayout;
        g->setContentsMargins(0, 0, 0, 0);
        g->setSpacing(2);
        g->addWidget(knob(2, 24, {}, true), 0, 0);
        g->addWidget(knob(3, 24), 0, 1);
        g->addWidget(knob(0, 24), 1, 0);
        g->addWidget(knob(1, 24), 1, 1);
        auto* col = vbox(0);
        col->addLayout(g);
        col->addStretch(1);
        root->addLayout(col);
        setFixedWidth(298);
    }
};

} // namespace

DeviceBody* createMidiBody(const DeviceContext& ctx, QWidget* parent) {
    switch (ctx.type) {
    case T::Arpeggiator: return new ArpBody(ctx, parent);
    case T::Chord:       return new ChordBody(ctx, parent);
    case T::Scale:       return new ScaleBody(ctx, parent);
    case T::Velocity:    return new VelocityBody(ctx, parent);
    default:             return nullptr;
    }
}

} // namespace sessions::devui
