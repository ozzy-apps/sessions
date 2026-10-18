// The racks' panel (Instrument Rack, Audio Effect Rack, MIDI Effect Rack): eight macros, the
// Chain selector and the chain list, with the zone editor beside it when Zones is on. The
// devices of the selected chain are shown next to it by the Device View (RackPanel).

#include "Bodies.h"
#include "audio/Engine.h"
#include "core/Model.h"
#include "core/Scales.h"
#include "core/Transport.h"
#include "ui/Browser.h"
#include "ui/DeviceDrag.h"
#include "ui/InfoView.h"
#include <QDragEnterEvent>
#include <QGridLayout>
#include <QInputDialog>
#include <QLinearGradient>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QToolButton>
#include <cmath>
#include <functional>

namespace sessions::devui {

namespace {

constexpr int kRowHeight = 16, kRowSpacing = 2;
constexpr int kZoneWidth = 220;     // the zone editor's bars
constexpr int kZoneX = 235;         // where they start in a chain row (On, name, S, two fields, meter)

// The zone buttons: lit in the accent colour while on (the Device View rebuilds its panels when the theme changes).
QString checkedStyle() {
    return QString("QToolButton{font-size:9px;padding:0 5px;}QToolButton:checked{background:%1;color:%2;border-radius:2px;}")
        .arg(theme::accent.name(), theme::onAccent.name());
}

QString zoneValueText(int which, int v) {
    if (which == ChainZones::Key) return QString("%1%2").arg(noteName(v)).arg(v / 12 - 2);
    return QString::number(v);
}

// A chain's name in the list: click to show its devices, double-click to rename, drag up or
// down to move the chain, right-click for Rename / Delete Chain.
class ChainName : public QWidget {
public:
    // rowAt: the chain row under a point of the panel (-1: none); onMoveTo: where a drag would put it (-1: hide the mark).
    ChainName(DeviceBody* body, int chain, std::function<int(const QPoint&)> rowAt, std::function<void(int)> onMoveTo)
        : QWidget(body), m_body(body), m_chain(chain), m_rowAt(std::move(rowAt)), m_moveTo(std::move(onMoveTo)) {
        setObjectName("chainName");
        setFixedSize(104, kRowHeight);
        setCursor(Qt::PointingHandCursor);
        InfoView::attach(this, "Chain", "Click to show this chain's devices. Drag it up or down to move the chain. Double-click to rename it; right-click to rename or delete it. "
                                        "Drop a device from the browser on it to add the device to this chain.");
        body->watch(this);
    }
protected:
    QString nameNow() const {
        const Device* d = m_body->device();
        return d ? d->chains.value(m_chain) : QString();
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const bool sel = selectedChain(m_body->ctx().deviceId) == m_chain;
        p.setPen(Qt::NoPen);
        p.setBrush(sel ? theme::devChainSel : theme::devChain);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 2, 2);
        QFont f = font(); f.setPointSizeF(7.5); f.setBold(sel); p.setFont(f);
        p.setPen(sel ? theme::selectionText : col::value);
        p.drawText(rect().adjusted(5, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(nameNow(), Qt::ElideRight, width() - 9));
    }
    void mousePressEvent(QMouseEvent* e) override {   // selects on release: showing another chain rebuilds the panel, which would end a drag
        if (e->button() != Qt::LeftButton) return;
        m_press = e->position().toPoint();
        m_target = -1;
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton) || m_press.x() < 0) return;
        if (m_target < 0 && std::abs(e->position().y() - m_press.y()) < 5) return;
        const int row = m_rowAt(mapTo(m_body, e->position().toPoint()));
        m_target = row < 0 ? m_chain : row;
        m_moveTo(m_target == m_chain ? -1 : m_target);
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        const int target = m_target;
        const bool pressed = m_press.x() >= 0;
        m_press = QPoint(-1, -1);
        m_target = -1;
        m_moveTo(-1);
        const DeviceContext c = m_body->ctx();
        if (target < 0 || target == m_chain) {   // a click: show this chain
            if (pressed && e->button() == Qt::LeftButton) selectChain(c.deviceId, m_chain);
            return;
        }
        if (c.project->moveChain(c.track, c.deviceId, m_chain, target)) selectChain(c.deviceId, target);   // the panel is rebuilt: touch nothing after
    }
    void mouseDoubleClickEvent(QMouseEvent*) override { rename(); }
    void contextMenuEvent(QContextMenuEvent* e) override {
        QMenu menu(this);
        menu.addAction("Rename", this, [this] { rename(); });
        menu.addAction("Delete Chain", this, [this] {
            const DeviceContext& c = m_body->ctx();
            c.project->deleteChain(c.track, c.deviceId, m_chain);
            selectChain(c.deviceId, std::max(0, m_chain - 1));
        });
        menu.exec(e->globalPos());
    }
    void rename() {
        const DeviceContext& c = m_body->ctx();
        bool ok = false;
        const QString n = QInputDialog::getText(this, "Rename Chain", "Name:", QLineEdit::Normal, nameNow(), &ok);
        if (ok) c.project->renameChain(c.track, c.deviceId, m_chain, n);
    }
private:
    DeviceBody* m_body;
    int m_chain;
    std::function<int(const QPoint&)> m_rowAt;
    std::function<void(int)> m_moveTo;
    QPoint m_press{-1, -1};
    int m_target = -1;
};

// A chain's output level, falling back like the mixer's meters.
class ChainMeter : public QWidget {
public:
    explicit ChainMeter(QWidget* parent) : QWidget(parent) { setFixedSize(5, kRowHeight); }
    void push(float peak) {
        const float level = peak > 1e-5f ? std::clamp((20.0f * std::log10(peak) + 60.0f) / 66.0f, 0.0f, 1.0f) : 0.0f;
        m_level = std::max(level, m_level - 0.04f);
        update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), theme::meterBg);
        const int h = int(std::round(m_level * height()));
        p.fillRect(QRect(0, height() - h, width(), h), m_level > 0.94f ? theme::record : theme::play);
    }
private:
    float m_level = 0;
};

// Shared by the zone bars and the ruler: 0 .. 127 across the width.
struct ZoneScale {
    double w;
    double x(double v) const { return v / 128.0 * w; }
    int    value(double px) const { return std::clamp(int(std::floor(px / w * 128.0)), 0, 127); }
};

// One chain's zone (key, velocity or chain select, as the editor's mode says): the range as a
// bar, its fades shading in and out. Drag the bar's lower half at an end to move that end, its
// upper half at a fade's inner edge to set the fade, the middle to move the whole zone; a click
// beside the bar moves the nearer end there. Double-click: back to the default zone.
class ZoneBar : public QWidget {
public:
    ZoneBar(DeviceBody* body, int chain) : QWidget(body), m_body(body), m_chain(chain) {
        setObjectName("zoneBar");
        setFixedSize(kZoneWidth, kRowHeight);
        setCursor(Qt::SizeHorCursor);
        InfoView::attach(this, "Zone",
            "Which notes (Key), how hard they're played (Vel) or which Chain selector positions (Chain) this chain answers to. "
            "Drag an end of the bar (lower half) to move it, drag the upper half at an end to fade the chain in or out there, drag the middle to move the zone. "
            "Double-click for the default zone.");
        body->watch(this);
    }
    int chain() const { return m_chain; }
protected:
    int  mode() const { return zoneMode(m_body->ctx().deviceId); }
    Zone zone() const {
        const Device* d = m_body->device();
        return d && m_chain < d->zones.size() ? d->zones[m_chain][mode()] : ChainZones{}[mode()];
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.fillRect(r, col::display);
        const ZoneScale sc{r.width()};
        const int m = mode();
        p.setPen(QPen(col::grid, 1));
        if (m == ChainZones::Key) for (int c = 0; c < 128; c += 12) p.drawLine(QPointF(r.left() + sc.x(c), r.top()), QPointF(r.left() + sc.x(c), r.bottom()));
        else for (int v = 32; v < 128; v += 32) p.drawLine(QPointF(r.left() + sc.x(v), r.top()), QPointF(r.left() + sc.x(v), r.bottom()));
        const Zone z = zone();
        const double x0 = r.left() + sc.x(z.lo), x1 = r.left() + sc.x(z.hi + 1);
        const double f0 = r.left() + sc.x(z.lo + z.fadeLo), f1 = r.left() + sc.x(z.hi + 1 - z.fadeHi);
        const QRectF bar(x0, r.top() + 2, std::max(2.0, x1 - x0), r.height() - 4);
        p.fillRect(QRectF(f0, bar.top(), std::max(0.0, f1 - f0), bar.height()), col::curve);
        auto fade = [&](double from, double to, bool in) {
            if (to - from < 0.5) return;
            QLinearGradient g(from, 0, to, 0);
            QColor clear = col::curve;
            clear.setAlphaF(0.12);
            g.setColorAt(in ? 0 : 1, clear);
            g.setColorAt(in ? 1 : 0, col::curve);
            p.fillRect(QRectF(from, bar.top(), to - from, bar.height()), g);
        };
        fade(x0, f0, true);
        fade(f1, x1, false);
        p.setPen(QPen(col::handle, 1));
        p.drawRect(bar);
        if (m == ChainZones::Select) {   // where the selector is
            const double sx = r.left() + sc.x(std::lround(m_body->real(racks::ChainSelect)) + 0.5);
            p.setPen(QPen(col::cyan, 1.5));
            p.drawLine(QPointF(sx, r.top()), QPointF(sx, r.bottom()));
        }
        QFont f = font(); f.setPointSizeF(6.5); p.setFont(f);
        const QString text = z.lo == z.hi ? zoneValueText(m, z.lo) : zoneValueText(m, z.lo) + " – " + zoneValueText(m, z.hi);
        const QRectF tr = bar.adjusted(3, 0, -3, 0);
        if (p.fontMetrics().horizontalAdvance(text) < tr.width()) {
            p.setPen(col::buttonOnText);
            p.drawText(tr, Qt::AlignCenter, text);
        } else {
            p.setPen(col::gridText);
            const bool right = x1 + 4 + p.fontMetrics().horizontalAdvance(text) < r.right();
            p.drawText(right ? QRectF(x1 + 3, r.top(), r.right() - x1 - 3, r.height()) : QRectF(r.left() + 2, r.top(), x0 - r.left() - 4, r.height()),
                       Qt::AlignVCenter | (right ? Qt::AlignLeft : Qt::AlignRight), text);
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        const ZoneScale sc{double(width())};
        const Zone z = zone();
        const double x = e->position().x();
        const bool top = e->position().y() < height() / 2.0;
        const double x0 = sc.x(z.lo), x1 = sc.x(z.hi + 1), f0 = sc.x(z.lo + z.fadeLo), f1 = sc.x(z.hi + 1 - z.fadeHi);
        m_start = z;
        m_from = sc.value(x);
        if (top && std::abs(x - f0) <= 5 && std::abs(x - f0) <= std::abs(x - f1)) m_drag = FadeLo;
        else if (top && std::abs(x - f1) <= 5) m_drag = FadeHi;
        else if (std::abs(x - x0) <= 5 && std::abs(x - x0) <= std::abs(x - x1)) m_drag = Lo;
        else if (std::abs(x - x1) <= 5) m_drag = Hi;
        else if (x > x0 && x < x1) m_drag = Move;
        else { m_drag = x < x0 ? Lo : Hi; apply(sc.value(x)); }
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (m_drag == None || !(e->buttons() & Qt::LeftButton)) return;
        apply(ZoneScale{double(width())}.value(e->position().x()));
    }
    void mouseReleaseEvent(QMouseEvent*) override { m_drag = None; }
    void mouseDoubleClickEvent(QMouseEvent*) override {
        m_drag = None;
        set(ChainZones{}[mode()]);
    }
private:
    enum Drag { None, Lo, Hi, FadeLo, FadeHi, Move };
    void apply(int v) {
        Zone z = m_start;
        switch (m_drag) {
        case Lo: z.lo = std::min(v, z.hi); break;
        case Hi: z.hi = std::max(v, z.lo); break;
        case FadeLo: z.fadeLo = std::clamp(v - z.lo, 0, z.hi - z.lo - z.fadeHi); break;
        case FadeHi: z.fadeHi = std::clamp(z.hi - v, 0, z.hi - z.lo - z.fadeLo); break;
        case Move: {
            const int d = std::clamp(v - m_from, -z.lo, 127 - z.hi);
            z.lo += d; z.hi += d;
            break;
        }
        case None: return;
        }
        set(z);
    }
    void set(const Zone& z) {
        const DeviceContext& c = m_body->ctx();
        c.project->setChainZone(c.track, c.deviceId, m_chain, mode(), z, deviceMergeKey(c.deviceId, 232 + m_chain));
    }
    DeviceBody* m_body;
    int m_chain;
    Drag m_drag = None;
    Zone m_start;
    int m_from = 0;
};

// Above the zone bars: the scale (notes, velocities or selector positions); in Chain mode
// the selector itself, which a click or drag here moves.
class ZoneRuler : public QWidget {
public:
    explicit ZoneRuler(DeviceBody* body) : QWidget(body), m_body(body) {
        setObjectName("zoneRuler");
        setFixedSize(kZoneWidth, 12);
        InfoView::attach(this, "Zone Scale", "Notes (Key), velocities (Vel) or Chain selector positions (Chain). In Chain mode the blue line is the selector: click or drag here to move it.");
        body->watch(this);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        const ZoneScale sc{double(width())};
        const int m = zoneMode(m_body->ctx().deviceId);
        QFont f = font(); f.setPointSizeF(6.5); p.setFont(f);
        p.setPen(col::gridText);
        const std::vector<int> marks = m == ChainZones::Key ? std::vector<int>{0, 24, 48, 72, 96, 120} : std::vector<int>{0, 32, 64, 96, 127};
        for (int v : marks) {
            const double x = sc.x(v);
            p.drawLine(QPointF(x + 0.5, height() - 3), QPointF(x + 0.5, height()));
            p.drawText(QRectF(x + 2, 0, 40, height() - 1), Qt::AlignLeft | Qt::AlignVCenter, zoneValueText(m, v));
        }
        if (m == ChainZones::Select) {
            const double sx = sc.x(std::lround(m_body->real(racks::ChainSelect)) + 0.5);
            p.setPen(Qt::NoPen);
            p.setBrush(col::cyan);
            p.drawPolygon(QPolygonF({QPointF(sx - 4, 0), QPointF(sx + 4, 0), QPointF(sx, height() - 1)}));
        }
    }
    void mousePressEvent(QMouseEvent* e) override { move(e); }
    void mouseMoveEvent(QMouseEvent* e) override { if (e->buttons() & Qt::LeftButton) move(e); }
private:
    void move(QMouseEvent* e) {
        if (zoneMode(m_body->ctx().deviceId) != ChainZones::Select) return;
        const int v = ZoneScale{double(width())}.value(e->position().x());
        m_body->edit(racks::ChainSelect, m_body->posFor(racks::ChainSelect, v));
    }
    DeviceBody* m_body;
};

class RackBody : public DeviceBody {
public:
    RackBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        const bool midi = c.type == DeviceType::MidiEffectRack;
        const bool zones = zonesShown(c.deviceId);
        auto* root = rootRow(this, 6);

        // ---- macros, and what they turn
        auto* mac = vbox(2);
        mac->addWidget(section("Macros"));
        auto* g = new QGridLayout;
        g->setContentsMargins(0, 0, 0, 0);
        g->setHorizontalSpacing(0);
        g->setVerticalSpacing(2);
        for (int k = 0; k < racks::kMacros; ++k) {
            QWidget* cell = knob(k, 24, macroLabel(k));
            cell->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(cell, &QWidget::customContextMenuRequested, this, [this, k, cell](const QPoint& at) { macroMenu(k, cell->mapToGlobal(at)); });
            g->addWidget(cell, k / 4, k % 4);
        }
        mac->addLayout(g);
        auto* scroll = new QScrollArea(this);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setFixedSize(186, 52);
        scroll->setStyleSheet("QScrollArea{background:transparent;}");
        auto* list = new QWidget(scroll);
        list->setStyleSheet("background:transparent;");
        auto* rows = vbox(1);
        list->setLayout(rows);
        const Device* d = device();
        if (!d || d->maps.isEmpty()) {
            auto* hint = new QLabel("Right-click a macro to map it to a knob in a chain, or to the Chain selector.", list);
            hint->setWordWrap(true);
            hint->setStyleSheet("color:#6c6c6c;font-size:9px;");
            rows->addWidget(hint);
        } else {
            for (int r = 0; r < d->maps.size(); ++r) rows->addLayout(macroMapRow(list, d->maps[r], r, 74));
        }
        rows->addStretch(1);
        scroll->setWidget(list);
        mac->addWidget(scroll);
        mac->addStretch(1);
        root->addLayout(mac);
        root->addWidget(vline(this));

        // ---- the chain list: Chains, the selector, Zones, + ; then the zone scale and a row per chain
        auto* chains = vbox(kRowSpacing);
        auto* head = hbox(4);
        head->addWidget(section("Chains"));
        head->addStretch(1);
        if (zones) {
            for (int w = 0; w <= ChainZones::Select; ++w) {
                auto* b = new QToolButton(this);
                b->setText(w == ChainZones::Key ? "Key" : w == ChainZones::Velocity ? "Vel" : "Chain");
                b->setCheckable(true);
                b->setChecked(zoneMode(c.deviceId) == w);
                b->setFocusPolicy(Qt::NoFocus);
                b->setStyleSheet(checkedStyle());
                InfoView::attach(b, b->text() + " Zones", w == ChainZones::Key ? "Edit which notes each chain plays (a keyboard split, layers)."
                                                    : w == ChainZones::Velocity ? "Edit how hard a note must be played for each chain (velocity layers)."
                                                    : "Edit which Chain selector positions each chain answers to: move the selector to switch or crossfade chains.");
                connect(b, &QToolButton::clicked, this, [this, w] { setZoneMode(ctx().deviceId, w); emit rackSignals()->chainSelected(ctx().deviceId, selectedChain(ctx().deviceId)); });
                head->addWidget(b);
            }
            head->addSpacing(6);
        }
        auto* selLabel = new QLabel("Chain", this);
        selLabel->setStyleSheet(QString("color:%1;font-size:9px;background:transparent;border:0;").arg(col::label.name()));
        head->addWidget(selLabel);
        QWidget* selector = field(racks::ChainSelect, 30);
        InfoView::attach(selector, "Chain Selector", "Plays the chains whose chain zone holds this value (0 .. 127). Map a macro to it, automate it, "
                                                     "or set Zones ▸ Chain to switch or crossfade between chains.");
        head->addWidget(selector);
        auto* zoneButton = new QToolButton(this);
        zoneButton->setText("Zones");
        zoneButton->setCheckable(true);
        zoneButton->setChecked(zones);
        zoneButton->setFocusPolicy(Qt::NoFocus);
        zoneButton->setStyleSheet(checkedStyle());
        InfoView::attach(zoneButton, "Zones", "Shows the zone editor: which keys, velocities and Chain selector positions each chain answers to.");
        connect(zoneButton, &QToolButton::clicked, this, [this](bool on) { showZones(ctx().deviceId, on); emit rackSignals()->chainSelected(ctx().deviceId, selectedChain(ctx().deviceId)); });
        head->addWidget(zoneButton);
        auto* add = new QToolButton(this);
        add->setText("+");
        add->setFocusPolicy(Qt::NoFocus);
        add->setStyleSheet("QToolButton{font-size:10px;padding:0 6px;}");
        InfoView::attach(add, "Add Chain", "A new, empty chain. Drop devices into it next to the rack.");
        connect(add, &QToolButton::clicked, this, [this] {
            const int chain = ctx().project->addChain(ctx().track, ctx().deviceId);
            if (chain >= 0) selectChain(ctx().deviceId, chain);
        });
        head->addWidget(add);
        chains->addLayout(head);
        const int n = d ? int(d->chains.size()) : 0;
        add->setEnabled(n < racks::kChains);
        if (zones && n > 0) {
            auto* rulerRow = hbox(0);
            rulerRow->addSpacing(kZoneX);
            rulerRow->addWidget(new ZoneRuler(this));
            rulerRow->addStretch(1);
            chains->addLayout(rulerRow);
        }
        // Every chain's controls exist (hidden past the last chain), so each knob can be mapped.
        auto rowAt = [this](const QPoint& pos) { return chainAt(pos, true); };
        auto moveTo = [this](int target) { markMove(target); };
        for (int ch = 0; ch < racks::kChains; ++ch) {
            auto* row = new QWidget(this);
            auto* rl = new QHBoxLayout(row);
            rl->setContentsMargins(0, 0, 0, 0);
            rl->setSpacing(3);
            rl->addWidget(toggle(racks::param(ch, racks::On), " ", 14));
            rl->addWidget(new ChainName(this, ch, rowAt, moveTo));
            rl->addWidget(toggle(racks::param(ch, racks::Solo), "S", 16));
            rl->addWidget(field(racks::param(ch, racks::Volume), 48));   // a MIDI Effect Rack: Transpose
            rl->addWidget(field(racks::param(ch, racks::Pan), 30));      // and Velocity
            if (midi) {
                rl->addSpacing(5);
                m_meters.push_back(nullptr);
            } else {
                auto* meter = new ChainMeter(row);
                rl->addWidget(meter);
                m_meters.push_back(meter);
            }
            if (zones) rl->addWidget(new ZoneBar(this, ch));
            rl->addStretch(1);
            row->setVisible(ch < n);
            m_rows.push_back(row);
            chains->addWidget(row);
        }
        if (n == 0) {
            auto* hint = new QLabel(ctx().type == DeviceType::InstrumentRack ? "No chains yet. Drop an instrument here (or next to the rack) to make one."
                                    : midi ? "No chains yet. Drop a MIDI effect here (or next to the rack) to make one."
                                           : "No chains yet. Drop an effect here (or next to the rack) to make one.", this);
            hint->setWordWrap(true);
            hint->setFixedWidth(230);
            hint->setStyleSheet("color:#6c6c6c;font-size:9px;");
            chains->addWidget(hint);
        }
        chains->addStretch(1);
        root->addLayout(chains);
        m_mark = new QWidget(this);
        m_mark->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_mark->setAutoFillBackground(true);
        QPalette pal = m_mark->palette();
        pal.setColor(QPalette::Window, theme::accent);
        m_mark->setPalette(pal);
        m_mark->hide();
        setAcceptDrops(true);
        connect(rackSignals(), &RackSignals::chainSelected, this, [this](int rack, int) { if (rack == ctx().deviceId) refresh(); });
        setFixedWidth(zones ? 462 + kZoneWidth + 3 : 462);
    }

    void tick() override {
        DeviceBody::tick();
        audio::Engine* e = ctx().transport ? ctx().transport->engine() : nullptr;
        for (int ch = 0; ch < int(m_meters.size()); ++ch)
            if (m_meters[size_t(ch)] && m_rows[size_t(ch)]->isVisible()) m_meters[size_t(ch)]->push(e ? e->takeChainPeak(ctx().deviceId, ch) : 0.0f);
    }

    // The chain row under `pos`, or -1 (anywhere else on the panel: a new chain). `nearest`: the
    // closest row by height (a chain dragged above the first or below the last).
    int chainAt(const QPoint& pos, bool nearest = false) const {
        int last = -1;
        for (int ch = 0; ch < int(m_rows.size()); ++ch) {
            const QWidget* row = m_rows[size_t(ch)];
            if (!row->isVisible()) continue;
            const QRect g(row->mapTo(this, QPoint(0, 0)), row->size());
            if (g.contains(pos) || (nearest && pos.y() >= g.top() - kRowSpacing && pos.y() <= g.bottom() + kRowSpacing)) return ch;
            if (nearest && pos.y() < g.top() && last < 0) return ch;
            last = ch;
        }
        return nearest ? last : -1;
    }

protected:
    // A chain being dragged: a line where it would land (-1: none).
    void markMove(int target) {
        if (target < 0 || target >= int(m_rows.size())) { m_mark->hide(); return; }
        const QWidget* row = m_rows[size_t(target)];
        const QPoint at = row->mapTo(this, QPoint(0, 0));
        m_mark->setGeometry(at.x(), at.y() - 1, 104 + 14 + 3, 2);
        m_mark->show();
        m_mark->raise();
    }
    void take(QDragMoveEvent* e) {
        const devicedrag::Payload pl = devicedrag::parse(e->mimeData());
        if (!pl.valid() || !devicedrag::accepts(*ctx().project, pl, ctx().track, {0, ctx().deviceId, chainAt(e->position().toPoint())})) { e->ignore(); return; }
        if (pl.source == devicedrag::Payload::Devices) { e->setDropAction(devicedrag::copying(e) ? Qt::CopyAction : Qt::MoveAction); e->accept(); }
        else e->acceptProposedAction();
    }
    void dragEnterEvent(QDragEnterEvent* e) override { take(e); }
    void dragMoveEvent(QDragMoveEvent* e) override { take(e); }
    // Dropped on a chain's row: into that chain. Anywhere else: a new chain. Browser items,
    // presets, or devices dragged by their title (moved; Ctrl: copied).
    void dropEvent(QDropEvent* e) override {
        const devicedrag::Payload pl = devicedrag::parse(e->mimeData());
        if (!pl.valid()) { e->ignore(); return; }
        const int chain = chainAt(e->position().toPoint());
        const Device* d = device();
        const int before = d ? int(d->chains.size()) : 0;
        const int rackId = ctx().deviceId;
        const bool copy = devicedrag::copying(e);
        const devicedrag::Result r = devicedrag::drop(*ctx().project, pl, ctx().track, {0, rackId, chain}, copy);
        InfoView::notify(r.title, r.text);
        if (!r.ok) return;
        e->setDropAction(pl.source == devicedrag::Payload::Devices && !copy ? Qt::MoveAction : Qt::CopyAction);
        e->accept();
        selectChain(rackId, chain >= 0 ? chain : before);
    }

private:
    std::vector<QWidget*> m_rows;
    std::vector<ChainMeter*> m_meters;   // null for a MIDI Effect Rack
    QWidget* m_mark = nullptr;
};

} // namespace

DeviceBody* createRackBody(const DeviceContext& ctx, QWidget* parent) { return new RackBody(ctx, parent); }

} // namespace sessions::devui
