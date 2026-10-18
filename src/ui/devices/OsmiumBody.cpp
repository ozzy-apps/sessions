#include "Bodies.h"
#include "audio/Dsp.h"
#include "audio/Wavetables.h"
#include "audio/Devices.h"
#include "core/ModulationCurve.h"
#include "core/OsmiumPresets.h"
#include "core/Transport.h"
#include "ui/InfoView.h"
#include "widgets/DragValue.h"
#include <QApplication>
#include <QComboBox>
#include <QCompleter>
#include <QDialog>
#include <QDrag>
#include <QDropEvent>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QScreen>
#include <QShortcut>
#include <QUndoStack>
#include <map>
#include <QStackedWidget>
#include <QTimer>
#include <QWheelEvent>
#include <QGridLayout>
#include <cmath>
#include <set>

namespace sessions::devui {
namespace {
namespace mo = monolith;
constexpr auto kSourceMime = "application/x-sessions-osmium-source";
constexpr double kPi = 3.141592653589793;

QColor sourceColor(int s) {
    return s <= mo::SrcEnv4 ? theme::osmiumEnv : mo::curveOfSource(s) >= 0 ? theme::osmiumCurve
         : mo::lfoOfSource(s) >= 0 ? theme::osmiumLfo : theme::osmiumGreen;
}
QFont pxFont(const QWidget* w, int px, bool bold = false) {
    auto f = w->font(); f.setPixelSize(px); f.setBold(bold); return f;
}
Device* mutableDevice(DeviceBody* b) {
    const auto& c = b->ctx();
    return findDevice(c.project, c.project->indexOfTrack(c.trackId), c.deviceId);
}
ModRoute routeFor(const DeviceBody* b, int source, int param) {
    if (const Device* d = b->device()) for (const auto& r : d->mods)
        if (r.source == source && r.param == param) return r;
    return {-1, param, 0};
}
void setRoute(DeviceBody* b, int source, int param, double amount, bool remove = false) {
    if (!b->device() || source < 0 || source >= mo::kSources || param < 0 || param >= b->count()) return;
    {
        EditScope edit(b->ctx().project, remove ? "Remove Modulation" : "Modulation Range",
                       (quintptr(5) << 40) | (quintptr(b->ctx().deviceId) << 13) | (quintptr(source) << 8) | quintptr(param));
        Device* d = mutableDevice(b);
        if (!d) return;
        auto it = std::find_if(d->mods.begin(), d->mods.end(), [=](const ModRoute& r) { return r.source == source && r.param == param; });
        if (it != d->mods.end()) { if (remove) d->mods.erase(it); else it->amount = std::clamp(amount, -1.0, 1.0); }
        else if (!remove && d->mods.size() < mo::kMaxRoutes) d->mods.push_back({source, param, std::clamp(amount, -1.0, 1.0)});
    }
    emit b->ctx().project->deviceChanged();
}
using Points = QVector<CurvePoint>;
void editCurve(DeviceBody* b, int c, const QString& label, const std::function<void(Points&)>& fn) {
    if (!b->device()) return;
    {
        EditScope edit(b->ctx().project, label);
        Device* d = mutableDevice(b);
        if (!d) return;
        d->curves.resize(mo::kCurves);
        fn(d->curves[c]);
    }
    emit b->ctx().project->deviceChanged();
}

class ChoiceBox : public QComboBox {
public:
    using QComboBox::QComboBox;
protected:
    void paintEvent(QPaintEvent* e) override {
        QComboBox::paintEvent(e);
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(theme::textFaint, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPolygonF arrow; const double x = width() - 11, y = height() / 2.0;
        arrow << QPointF(x - 3, y - 1) << QPointF(x, y + 2) << QPointF(x + 3, y - 1); p.drawPolyline(arrow);
    }
};

// Each control exposes its base value, source slot and directly draggable range ring.
class RangeKnob : public QWidget {
public:
    RangeKnob(DeviceBody* b, int param, QString label, int diameter, std::function<int()> source,
              std::function<void(int)> focus, std::function<void(int)> select = {}) : QWidget(b), m_body(b), m_param(param), m_label(std::move(label)),
              m_d(diameter), m_source(std::move(source)), m_focus(std::move(focus)), m_select(std::move(select)) {
        setFixedSize(std::max(64, diameter + 12), diameter + 44);
        setObjectName(QString("osmiumKnob_%1").arg(param));
        setProperty("mapTarget", b->target(param));
        setAcceptDrops(true); setFocusPolicy(Qt::StrongFocus); setCursor(Qt::SizeVerCursor); setMouseTracking(true);
        InfoView::attach(this, b->name(param), "Drag the centre to set the base value. Drag the green outer ring up/right to increase modulation, down/left to decrease or invert it. "
                         "Click + to assign the selected source; drag a source here to assign it. Shift: fine. Right-click: sources, remove, or enter a value. Delete: reset.");
        b->watch(this);
    }
    ModRoute activeRoute() const {
        ModRoute selected = routeFor(m_body, m_source(), m_param);
        if (selected.source >= 0) return selected;
        if (const Device* d = m_body->device()) for (const auto& r : d->mods) if (r.param == m_param) return r;
        return {-1, m_param, 0};
    }
    QPointF center() const { return {width() / 2.0, 15 + m_d / 2.0}; }
    QPointF point(double v, double radius) const {
        const double a = (135 + std::clamp(v, 0.0, 1.0) * 270) * kPi / 180;
        return center() + QPointF(std::cos(a) * radius, std::sin(a) * radius);
    }
    void arc(QPainter& p, double from, double to, double radius, const QColor& color, double thickness) {
        p.setPen(QPen(color, thickness, Qt::SolidLine, Qt::RoundCap)); p.setBrush(Qt::NoBrush);
        const QRectF r(center().x() - radius, center().y() - radius, radius * 2, radius * 2);
        p.drawArc(r, int((-135 - from * 270) * 16), int(-(to - from) * 270 * 16));
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        p.setFont(pxFont(this, 10)); p.setPen(theme::textSoft);
        QString label = m_label;
        if (const auto* device = m_body->device(); device && osmium::factoryPreset(device->name))
            for (int k = 0; k < 8; ++k) if (m_param == mo::macro(k)) label = QString("%1 %2").arg(k + 1).arg(osmium::kMacroNames[k]);
        p.drawText(QRect(0, 0, width(), 14), Qt::AlignCenter, label);
        const double base = m_body->pos(m_param), radius = m_d / 2.0 - 5;
        arc(p, 0, 1, radius - 5, theme::osmiumBorder, 3);
        arc(p, 0, base, radius - 5, theme::textSoft, 3);
        const ModRoute r = activeRoute();
        if (r.source >= 0) {
            const double end = std::clamp(base + r.amount, 0.0, 1.0);
            const double begin = mo::sourceBipolar(r.source) ? std::clamp(base - r.amount, 0.0, 1.0) : base;
            arc(p, std::min(begin, end), std::max(begin, end), radius, theme::osmiumGreen, 4);
            p.setPen(Qt::NoPen); p.setBrush(theme::osmiumGreen); p.drawEllipse(point(end, radius), 3, 3);
            p.setPen(QPen(theme::withAlpha(theme::osmiumGreen, 90), 1));
            p.drawLine(point(base, radius - 2), point(base, radius + 3));
        } else { arc(p, 0, 1, radius, theme::withAlpha(theme::osmiumBorder, 90), 1); }
        p.setPen(QPen(hasFocus() ? theme::osmiumGreen : theme::osmiumBorder, 1));
        p.setBrush(theme::osmiumInset); p.drawEllipse(center(), std::max(4.0, radius - 6), std::max(4.0, radius - 6));
        p.setPen(QPen(theme::textBright, 2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(point(base, 2), point(base, std::max(4.0, radius - 8)));
        p.setPen(theme::text); p.setFont(pxFont(this, 10));
        p.drawText(QRectF(0, m_d + 12, width(), 14), Qt::AlignCenter, m_body->text(m_param));
        const QRectF slot(4, height() - 16, width() - 8, 14);
        p.setBrush(r.source >= 0 ? theme::withAlpha(theme::osmiumGreen, 20) : theme::osmiumInset);
        p.setPen(Qt::NoPen); p.drawRoundedRect(slot, 5, 5);
        p.setPen(r.source >= 0 ? theme::osmiumGreen : theme::textFaint); p.setFont(pxFont(this, 9));
        p.drawText(slot, Qt::AlignCenter, r.source < 0 ? "+" : QString("%1 %2%3").arg(mo::sourceName(r.source))
                   .arg(r.amount >= 0 ? "+" : "").arg(int(std::round(r.amount * 100))));
        if (m_drop) { p.setPen(QPen(theme::osmiumGreen, 2)); p.setBrush(Qt::NoBrush); p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 8, 8); }
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        setFocus(); m_focus(m_param);
        if (e->position().y() >= height() - 17) { if (routeFor(m_body, m_source(), m_param).source < 0) setRoute(m_body, m_source(), m_param, 0.25); return; }
        m_range = QLineF(e->position(), center()).length() >= m_d / 2.0 - 12 && e->position().y() < m_d + 12;
        m_press = e->position(); m_initial = m_body->pos(m_param);
        const auto r = activeRoute();
        m_dragSource = r.source < 0 ? m_source() : r.source;
        if (m_range && m_select) m_select(m_dragSource);
        m_amount = r.source < 0 ? 0 : r.amount;
        m_dragging = true;
        m_body->ctx().project->beginEdit(m_range ? "Modulation Range" : "Change " + m_body->name(m_param));
        if (m_range && r.source < 0) setRoute(m_body, m_dragSource, m_param, 0);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!m_dragging || !(e->buttons() & Qt::LeftButton)) return;
        const double fine = e->modifiers() & Qt::ShiftModifier ? 0.1 : 1;
        if (m_range) setRoute(m_body, m_dragSource, m_param, m_amount + ((e->position().x() - m_press.x()) + (m_press.y() - e->position().y())) / 150 * fine);
        else m_body->edit(m_param, m_initial + (m_press.y() - e->position().y()) / 150 * fine);
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && m_dragging) { m_dragging = false; m_body->ctx().project->endEdit(); }
    }
    void wheelEvent(QWheelEvent* e) override { m_focus(m_param); m_body->edit(m_param, m_body->pos(m_param) + e->angleDelta().y() / 120.0 * (e->modifiers() & Qt::ShiftModifier ? 0.001 : 0.01)); e->accept(); }
    void keyPressEvent(QKeyEvent* e) override {
        if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) m_body->edit(m_param, specOf(m_body->ctx().type)->params[size_t(m_param)].def);
        else if (e->key() == Qt::Key_Up || e->key() == Qt::Key_Right || e->key() == Qt::Key_Down || e->key() == Qt::Key_Left)
            m_body->edit(m_param, m_body->pos(m_param) + ((e->key() == Qt::Key_Up || e->key() == Qt::Key_Right) ? 1 : -1) * (e->modifiers() & Qt::ShiftModifier ? 0.001 : 0.01));
        else QWidget::keyPressEvent(e);
    }
    void contextMenuEvent(QContextMenuEvent* e) override {
        QMenu menu(this); auto* sources = menu.addMenu("Modulate by");
        for (int s = 0; s < mo::kSources; ++s) sources->addAction(mo::sourceName(s), this, [this, s] { setRoute(m_body, s, m_param, 0.25); if (m_select) m_select(s); m_focus(m_param); });
        if (const Device* d = m_body->device()) for (const auto& r : d->mods) if (r.param == m_param) {
            const int s = r.source;
            menu.addAction(QString("Remove %1").arg(mo::sourceName(s)), this, [this, s] { setRoute(m_body, s, m_param, 0, true); });
        }
        menu.addSeparator();
        menu.addAction("Set base value…", this, [this] {
            bool ok = false; double v = QInputDialog::getDouble(this, m_body->name(m_param), "Position (%)", m_body->pos(m_param) * 100, 0, 100, 2, &ok);
            if (ok) m_body->edit(m_param, v / 100);
        });
        menu.addAction("Reset base value", this, [this] { m_body->edit(m_param, specOf(m_body->ctx().type)->params[size_t(m_param)].def); });
        menu.exec(e->globalPos());
    }
    int droppedSource(const QMimeData* mime) const {
        const auto data = mime->data(kSourceMime).split(':');
        bool idOk = false, sourceOk = false;
        const int id = data.value(0).toInt(&idOk), s = data.value(1).toInt(&sourceOk);
        return data.size() == 2 && idOk && sourceOk && id == m_body->ctx().deviceId && s >= 0 && s < mo::kSources ? s : -1;
    }
    void dragEnterEvent(QDragEnterEvent* e) override { m_drop = droppedSource(e->mimeData()) >= 0; if (m_drop) e->acceptProposedAction(); update(); }
    void dragLeaveEvent(QDragLeaveEvent*) override { m_drop = false; update(); }
    void dropEvent(QDropEvent* e) override {
        const int s = droppedSource(e->mimeData()); m_drop = false;
        if (s >= 0) { setRoute(m_body, s, m_param, 0.25); if (m_select) m_select(s); m_focus(m_param); e->acceptProposedAction(); }
        update();
    }
private:
    DeviceBody* m_body; int m_param; QString m_label; int m_d;
    std::function<int()> m_source; std::function<void(int)> m_focus, m_select;
    QPointF m_press; double m_initial = 0, m_amount = 0;
    int m_dragSource = 0; bool m_range = false, m_dragging = false, m_drop = false;
};

class SourceButton : public QPushButton {
public:
    SourceButton(DeviceBody* body, int source, std::function<void()> select) : QPushButton(mo::sourceName(source), body), m_body(body), m_source(source) {
        setCheckable(true); setMinimumHeight(30); setCursor(Qt::OpenHandCursor);
        setObjectName(QString("osmiumSource_%1").arg(source));
        QObject::connect(this, &QPushButton::clicked, this, std::move(select));
        InfoView::attach(this, mo::sourceName(source), "Click to edit this source. Drag it onto a parameter to assign modulation, then drag the knob's green ring to set the range.");
    }
protected:
    void mousePressEvent(QMouseEvent* e) override { m_press = e->pos(); QPushButton::mousePressEvent(e); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton) || (e->pos() - m_press).manhattanLength() < QApplication::startDragDistance()) return;
        setDown(false);
        auto* drag = new QDrag(this); auto* mime = new QMimeData;
        mime->setData(kSourceMime, QByteArray::number(m_body->ctx().deviceId) + ':' + QByteArray::number(m_source));
        drag->setMimeData(mime); drag->setPixmap(grab()); drag->exec(Qt::CopyAction);
    }
private:
    DeviceBody* m_body; int m_source; QPoint m_press;
};

class Plot : public Display {
public:
    Plot(DeviceBody* b, int height) : Display(b, 180, height) {
        setMinimumWidth(100); setMaximumWidth(QWIDGETSIZE_MAX); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        b->watch(this);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen); p.setBrush(theme::osmiumInset); p.drawRoundedRect(QRectF(rect()), 10, 10);
        p.setFont(pxFont(this, 10)); draw(p, QRectF(rect()).adjusted(14, 12, -14, -16));
    }
    void grid(QPainter& p, const QRectF& r, int columns = 16) {
        for (int i = 0; i <= columns; ++i) {
            p.setPen(QPen(theme::withAlpha(theme::osmiumBorder, i % 4 == 0 ? 170 : 70), 1));
            const double x = r.left() + r.width() * i / columns; p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        }
        p.setPen(QPen(theme::withAlpha(theme::osmiumBorder, 110), 1));
        for (int i = 0; i <= 4; ++i) { const double y = r.top() + r.height() * i / 4; p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y)); }
    }
    QRectF graph() const { return QRectF(rect()).adjusted(14, 12, -14, -16); }
    void curve(QPainter& p, const QRectF& r, const QPainterPath& path, const QColor& color) {
        auto area = path; area.lineTo(r.right(), r.bottom()); area.lineTo(r.left(), r.bottom()); area.closeSubpath();
        QLinearGradient gradient(r.topLeft(), r.bottomLeft()); gradient.setColorAt(0, theme::withAlpha(color, 42)); gradient.setColorAt(1, theme::withAlpha(color, 3));
        p.setPen(Qt::NoPen); p.setBrush(gradient); p.drawPath(area);
        p.setBrush(Qt::NoBrush); p.setPen(QPen(color, 2)); p.drawPath(path);
    }
};

class WavePlot : public Plot {
public:
    WavePlot(DeviceBody* b, int osc, int h = 52) : Plot(b, h), m_osc(osc) {
        InfoView::attach(this, "Oscillator waveform", "The current wavetable position and warp. Position scans the table; Warp bends, syncs or squeezes its phase.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const auto& table = audio::wavetable(m_body->pick(mo::oparam(m_osc, mo::Table)));
        const auto pos = m_body->pos(mo::oparam(m_osc, mo::Position));
        const auto intensity = m_body->pos(mo::oparam(m_osc, mo::Intensity));
        const int mode = m_body->pick(mo::oparam(m_osc, mo::Mode));
        p.setPen(QPen(theme::osmiumBorder, 1)); p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
        QPainterPath path;
        for (int x = 0; x <= int(r.width()); ++x) {
            const float phase = audio::monolithWarpPhase(mode, float(x / r.width()) * 0.99999f, float(intensity));
            const float y = audio::wavetableRead(table, 0, float(pos), phase);
            const QPointF point(r.left() + x, r.center().y() - y * r.height() * 0.45);
            if (x == 0) path.moveTo(point); else path.lineTo(point);
        }
        p.setPen(QPen(theme::osmiumLfo, 1.6)); p.drawPath(path);
    }
private: int m_osc;
};

class FilterPlot : public Plot {
public:
    FilterPlot(DeviceBody* b, int f) : Plot(b, 45), m_filter(f) {
        setCursor(Qt::SizeAllCursor);
        InfoView::attach(this, "Filter", "Drag horizontally for cutoff and vertically for resonance. The green range around Cutoff controls how far modulation opens the filter.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const float cutoff = float(m_body->real(mo::fparam(m_filter, mo::FCutoff)));
        const int type = m_body->pick(mo::fparam(m_filter, mo::FType));
        const float reso = float(m_body->pos(mo::fparam(m_filter, mo::FReso)));
        const float q = audio::monolithFilterQ(type, reso);
        const auto magnitude = [=](float f) {
            using dsp::Svf;
            switch (type) {
            case 0: return Svf::magnitude(0, f, cutoff, q);
            case 1: return Svf::magnitude(0, f, cutoff, q) * Svf::magnitude(0, f, cutoff, 0.707f);
            case 2: return Svf::magnitude(1, f, cutoff, q);
            case 3: return Svf::magnitude(2, f, cutoff, q) * 2;
            case 4: return Svf::magnitude(3, f, cutoff, q);
            case 5: { const float w = 2 * dsp::kPi * f / cutoff; return 0.7f / std::sqrt(std::max(0.0025f, 1 - 2 * q * std::cos(w) + q * q)); }
            default: return Svf::magnitude(0, f, cutoff, q) * Svf::magnitude(0, f, cutoff, 0.707f) * (1 + 0.5f * reso);
            }
        };
        QPainterPath path;
        for (int x = 0; x <= int(r.width()); ++x) {
            const double db = std::clamp(double(dsp::gainToDb(std::max(1e-5f, magnitude(float(xToFreq(r.left() + x, r)))))), -24.0, 24.0);
            const QPointF pt(r.left() + x, r.center().y() - db / 24 * r.height() / 2);
            if (x == 0) path.moveTo(pt); else path.lineTo(pt);
        }
        curve(p, r, path, theme::osmiumGreen);
    }
    void mousePressEvent(QMouseEvent* e) override { if (e->button() == Qt::LeftButton) { m_body->ctx().project->beginEdit("Filter"); m_drag = true; mouseMoveEvent(e); } }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!m_drag || !(e->buttons() & Qt::LeftButton)) return;
        const auto r = graph();
        m_body->editMany({{mo::fparam(m_filter, mo::FCutoff), m_body->posFor(mo::fparam(m_filter, mo::FCutoff), xToFreq(e->position().x(), r))},
                          {mo::fparam(m_filter, mo::FReso), (r.bottom() - e->position().y()) / r.height()}}, 20 + m_filter);
    }
    void mouseReleaseEvent(QMouseEvent*) override { if (m_drag) { m_drag = false; m_body->ctx().project->endEdit(); } }
private: int m_filter; bool m_drag = false;
};

class EnvelopePlot : public Plot {
public:
    EnvelopePlot(DeviceBody* b, int env) : Plot(b, 166), m_env(env) {
        setObjectName(QString("osmiumEnvelope_%1").arg(env)); setMouseTracking(true);
        InfoView::attach(this, "Envelope", "Drag the peak for attack time. Drag the decay node for decay and sustain; drag the release node for release time. "
                         "Drag a diamond to bend that individual segment. Double-click a diamond to straighten it. Times use the same logarithmic scale as the knobs.");
    }
protected:
    double warped(double y) const {
        const double c = m_body->real(mo::ecurve(m_env));
        return std::abs(c) < 0.01 ? y : c > 0 ? std::pow(y, 1 + 3 * c) : 1 - std::pow(1 - y, 1 - 3 * c);
    }
    double unwarped(double y) const {
        const double c = m_body->real(mo::ecurve(m_env));
        return std::abs(c) < 0.01 ? y : c > 0 ? std::pow(y, 1 / (1 + 3 * c)) : 1 - std::pow(1 - y, 1 / (1 - 3 * c));
    }
    std::vector<QPointF> nodes() const {
        const auto r = graph();
        const double x1 = r.left() + r.width() * (0.08 + 0.2 * m_body->pos(mo::eparam(m_env, mo::A)));
        const double x2 = x1 + r.width() * (0.08 + 0.2 * m_body->pos(mo::eparam(m_env, mo::D)));
        const double x3 = r.right() - r.width() * (0.08 + 0.2 * m_body->pos(mo::eparam(m_env, mo::R)));
        const double y = r.bottom() - warped(m_body->pos(mo::eparam(m_env, mo::S))) * r.height();
        return {{r.left(), r.bottom()}, {x1, r.top()}, {x2, y}, {x3, y}, {r.right(), r.bottom()}};
    }
    QPointF segmentPoint(int s, double t) const {
        const auto n = nodes(); const int a = s == 2 ? 3 : s, b = a + 1;
        const double y0 = s == 0 ? 0 : s == 1 ? 1 : m_body->pos(mo::eparam(m_env, mo::S));
        const double y1 = s == 0 ? 1 : s == 1 ? m_body->pos(mo::eparam(m_env, mo::S)) : 0;
        const double y = warped(y0 + (y1 - y0) * modcurve::bend(t, m_body->real(osmium::bend(m_env, s))));
        return {n[a].x() + (n[b].x() - n[a].x()) * t, graph().bottom() - y * graph().height()};
    }
    void draw(QPainter& p, const QRectF& r) override {
        grid(p, r); const auto n = nodes();
        QPainterPath path; path.moveTo(n[0]);
        for (int s = 0; s < 3; ++s) {
            if (s == 2) path.lineTo(n[3]);
            for (int k = 1; k <= 64; ++k) path.lineTo(segmentPoint(s, double(k) / 64));
        }
        curve(p, r, path, theme::osmiumEnv);
        p.setPen(QPen(theme::withAlpha(theme::osmiumEnv, 70), 1, Qt::DashLine));
        p.drawLine(QPointF(n[1].x(), r.top()), QPointF(n[1].x(), r.bottom()));
        p.drawLine(QPointF(n[2].x(), r.top()), QPointF(n[2].x(), r.bottom()));
        p.drawLine(QPointF(n[3].x(), r.top()), QPointF(n[3].x(), r.bottom()));
        p.setPen(theme::textFaint); p.setFont(pxFont(this, 9));
        const QString labels[] = {"ATTACK", "DECAY", "SUSTAIN", "RELEASE"};
        for (int k = 0; k < 4; ++k) p.drawText(QRectF(n[k].x(), r.bottom() + 3, n[k + 1].x() - n[k].x(), 12), Qt::AlignCenter, labels[k]);
        for (int s = 0; s < 3; ++s) {
            const auto mid = segmentPoint(s, 0.5);
            p.setPen(QPen(theme::osmiumEnv, 1)); p.setBrush(theme::osmiumInset);
            QPolygonF diamond; diamond << mid + QPointF(0, -4) << mid + QPointF(4, 0) << mid + QPointF(0, 4) << mid + QPointF(-4, 0); p.drawPolygon(diamond);
        }
        for (int k = 1; k <= 3; ++k) { p.setPen(QPen(theme::osmiumEnv, 2)); p.setBrush(theme::osmiumInset); p.drawEllipse(n[k], 4, 4); }
        const double level = m_body->scope(m_env);
        const int stage = int(m_body->scope(4 + m_env));
        if (stage > 0 && level > 0) {
            p.setPen(QPen(theme::withAlpha(theme::osmiumEnv, 80), 1));
            const double y = r.bottom() - warped(level) * r.height(); p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
            p.setPen(theme::osmiumEnv); p.drawText(r.adjusted(6, 3, -6, -3), Qt::AlignRight | Qt::AlignTop, QString::number(int(level * 100)) + "%");
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        m_drag = -1; const auto n = nodes();
        for (int k = 1; k <= 3; ++k) if (QLineF(e->position(), n[k]).length() < 11) m_drag = k;
        for (int s = 0; s < 3; ++s) if (QLineF(e->position(), segmentPoint(s, 0.5)).length() < 10) m_drag = 10 + s;
        if (m_drag < 0) return;
        m_press = e->position();
        if (m_drag >= 10) m_initial = m_body->pos(osmium::bend(m_env, m_drag - 10));
        m_body->ctx().project->beginEdit("Shape Envelope"); m_gesture = true;
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!m_gesture || !(e->buttons() & Qt::LeftButton)) return;
        const auto r = graph(); const auto n = nodes(); const double x = e->position().x();
        if (m_drag >= 10) {
            const int s = m_drag - 10;
            m_body->edit(osmium::bend(m_env, s), m_initial + (e->position().y() - m_press.y()) / 120 * (s == 0 ? 1 : -1));
        } else if (m_drag == 1) m_body->edit(mo::eparam(m_env, mo::A), ((x - r.left()) / r.width() - 0.08) / 0.2);
        else {
            const double y = unwarped(std::clamp((r.bottom() - e->position().y()) / r.height(), 0.0, 1.0));
            m_body->editMany({{mo::eparam(m_env, m_drag == 2 ? mo::D : mo::R),
                               ((m_drag == 2 ? x - n[1].x() : r.right() - x) / r.width() - 0.08) / 0.2}, {mo::eparam(m_env, mo::S), y}}, 25 + m_env);
        }
    }
    void mouseReleaseEvent(QMouseEvent*) override { if (m_gesture) { m_gesture = false; m_body->ctx().project->endEdit(); } }
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        for (int s = 0; s < 3; ++s) if (QLineF(e->position(), segmentPoint(s, 0.5)).length() < 10) m_body->edit(osmium::bend(m_env, s), 0.5);
    }
private: int m_env, m_drag = -1; bool m_gesture = false; QPointF m_press; double m_initial = 0;
};

class CurvePlot : public Plot {
public:
    CurvePlot(DeviceBody* b, int c) : Plot(b, 144), m_curve(c) {
        setObjectName(QString("osmiumCurve_%1").arg(c)); setFocusPolicy(Qt::StrongFocus); setMouseTracking(true);
        InfoView::attach(this, "Curve editor", "Double-click to add a point; drag points to move them. Drag a segment's diamond to bend it. "
                         "Right-click a segment for Curve, Smooth or Hold. Delete removes the selected interior point. Alt bypasses snap. Draw mode paints a 16-step shape.");
    }
    Points points() const { const auto* d = m_body->device(); return d && m_curve < d->curves.size() ? d->curves[m_curve] : Points{}; }
    void fill(const QString& label, const Points& p) { editCurve(m_body, m_curve, label, [p](Points& pts) { pts = p; }); m_selected = -1; }
    void preset(int shape) {
        Points p;
        if (shape == 0) p = {{0, 0}, {0.5, 1}, {1, 0}};
        if (shape == 1) p = {{0, 0.5, 0, 1}, {0.25, 1, 0, 1}, {0.75, 0, 0, 1}, {1, 0.5}};
        if (shape == 2) p = {{0, 0, -0.4}, {1, 1}};
        if (shape == 3) p = {{0, 1, -0.35}, {1, 0}};
        if (shape == 4) p = {{0, 1, 0, 2}, {0.25, 0.25, 0, 2}, {0.5, 0.75, 0, 2}, {0.75, 0.4, 0, 2}, {1, 1}};
        if (shape == 5) { for (int i = 0; i <= 8; ++i) p.push_back({i / 8.0, QRandomGenerator::global()->bounded(1.0), 0, 2}); }
        fill("Curve Shape", p);
    }
    void transform(int what) {
        auto p = points();
        if (what == 0) for (auto& pt : p) pt.y = 1 - pt.y;
        else {
            const auto old = p;
            for (int i = 0; i < p.size(); ++i) {
                const int from = int(p.size()) - 1 - i;
                p[i] = old[from]; p[i].x = 1 - old[from].x;
                if (from > 0) { p[i].bend = -old[from - 1].bend; p[i].shape = old[from - 1].shape; }
            }
            // Reversing a held segment moves its jump to the other end; sample it explicitly.
            if (std::any_of(old.begin(), old.end(), [](const CurvePoint& q) { return q.shape == 2; })) {
                p.clear(); for (int i = 0; i < mo::kCurvePoints; ++i) { const double x = double(i) / (mo::kCurvePoints - 1); p.push_back({x, modcurve::value(old.constData(), int(old.size()), 1 - x)}); }
            }
        }
        fill(what == 0 ? "Invert Curve" : "Reverse Curve", p);
    }
    void setGrid(int steps) { m_grid = steps; update(); }
    void setDraw(bool draw) { m_draw = draw; setCursor(draw ? Qt::CrossCursor : Qt::ArrowCursor); }
    void setShape(int shape) {
        const int s = m_segment;
        editCurve(m_body, m_curve, "Segment Shape", [=](Points& p) { if (s >= 0 && s + 1 < p.size()) p[s].shape = shape; });
    }
    static Points& clipboard() { static Points p; return p; }
protected:
    QPointF at(const CurvePoint& pt) const { const auto r = graph(); return {r.left() + pt.x * r.width(), r.bottom() - pt.y * r.height()}; }
    QPointF midpoint(const Points& p, int i) const {
        return at({(p[i].x + p[i + 1].x) * 0.5, modcurve::segment(p[i], p[i + 1], 0.5)});
    }
    int pointAt(QPointF pt) const {
        const auto p = points(); int best = -1; double dist = 10;
        for (int i = 0; i < p.size(); ++i) { const double d = QLineF(pt, at(p[i])).length(); if (d < dist) { best = i; dist = d; } }
        return best;
    }
    int segmentAt(QPointF pt) const {
        const auto p = points(); const auto r = graph(); const double x = (pt.x() - r.left()) / r.width();
        for (int i = 0; i + 1 < p.size(); ++i) if (x >= p[i].x && x <= p[i + 1].x) return i;
        return -1;
    }
    void draw(QPainter& painter, const QRectF& r) override {
        grid(painter, r, m_grid > 0 ? m_grid : 16); const auto p = points();
        if (p.isEmpty()) return;
        QPainterPath path;
        for (int i = 0; i <= int(r.width()); ++i) {
            const double x = i / r.width(); const auto pt = at({x, modcurve::value(p.constData(), int(p.size()), x)});
            if (i == 0) path.moveTo(pt); else path.lineTo(pt);
        }
        curve(painter, r, path, theme::osmiumCurve);
        for (int i = 0; i + 1 < p.size(); ++i) {
            const auto mid = midpoint(p, i);
            painter.setPen(QPen(theme::withAlpha(theme::osmiumCurve, i == m_segment ? 255 : 120), 1)); painter.setBrush(theme::osmiumInset);
            QPolygonF diamond; diamond << mid + QPointF(0, -4) << mid + QPointF(4, 0) << mid + QPointF(0, 4) << mid + QPointF(-4, 0); painter.drawPolygon(diamond);
        }
        for (int i = 0; i < p.size(); ++i) { painter.setBrush(i == m_selected ? theme::osmiumCurve : theme::osmiumInset); painter.setPen(QPen(theme::osmiumCurve, 2)); painter.drawEllipse(at(p[i]), i == m_selected ? 5 : 4, i == m_selected ? 5 : 4); }
        const double phase = m_body->scope(14 + m_curve);
        if (phase > 0) { painter.setPen(QPen(theme::withAlpha(theme::textBright, 150), 1)); const double x = r.left() + phase * r.width(); painter.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom())); }
        painter.setPen(theme::textFaint); painter.setFont(pxFont(this, 9));
        for (int i = 0; i <= 4; ++i) painter.drawText(QRectF(r.left() + r.width() * i / 4 - 12, r.bottom() + 3, 24, 12), Qt::AlignCenter, i == 0 ? "0" : i == 4 ? "1" : QString::number(i) + "/4");
    }
    void paintAt(const QPointF& pt) {
        const auto r = graph(); const int index = std::clamp(int(std::round((pt.x() - r.left()) / r.width() * 16)), 0, 16);
        const double y = std::clamp((r.bottom() - pt.y()) / r.height(), 0.0, 1.0);
        editCurve(m_body, m_curve, "Draw Curve", [=](Points& p) {
            if (p.size() != 17) { const auto old = p; p.clear(); for (int i = 0; i <= 16; ++i) p.push_back({i / 16.0, modcurve::value(old.constData(), int(old.size()), i / 16.0)}); }
            if (m_lastPaint >= 0 && index != m_lastPaint) for (int i = std::min(index, m_lastPaint); i <= std::max(index, m_lastPaint); ++i) {
                const double t = double(i - m_lastPaint) / (index - m_lastPaint); p[i].y = m_lastY + (y - m_lastY) * t;
            }
            p[index].y = y;
        });
        m_lastPaint = index; m_lastY = y;
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        setFocus(); m_press = e->position(); m_selected = pointAt(e->position()); m_segment = segmentAt(e->position()); m_bending = false;
        const auto p = points();
        if (m_selected < 0 && m_segment >= 0 && m_segment + 1 < p.size()) {
            m_bending = QLineF(e->position(), midpoint(p, m_segment)).length() < 12 || (e->modifiers() & Qt::AltModifier);
            m_initial = p[m_segment].bend;
        }
        if (m_selected < 0 && !m_bending && !m_draw) { update(); return; }
        m_body->ctx().project->beginEdit("Edit Curve"); m_gesture = true;
        if (m_draw) { m_lastPaint = -1; paintAt(e->position()); }
        update();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!m_gesture || !(e->buttons() & Qt::LeftButton)) return;
        if (m_draw) { paintAt(e->position()); return; }
        const auto r = graph(); const int k = m_selected, s = m_segment;
        const double dy = (e->position().y() - m_press.y()) / 90;
        double x = std::clamp((e->position().x() - r.left()) / r.width(), 0.0, 1.0);
        const double y = std::clamp((r.bottom() - e->position().y()) / r.height(), 0.0, 1.0);
        if (m_grid > 0 && !(e->modifiers() & Qt::AltModifier)) x = std::round(x * m_grid) / m_grid;
        editCurve(m_body, m_curve, "Shape Curve", [=](Points& p) mutable {
            if (m_bending && s >= 0 && s + 1 < p.size()) { p[s].bend = std::clamp(m_initial + dy * (p[s + 1].y >= p[s].y ? 1 : -1), -1.0, 1.0); if (p[s].shape == 2) p[s].shape = 0; }
            else if (k >= 0 && k < p.size()) {
                if (k == 0 || k == p.size() - 1) x = p[k].x;
                else {
                    const double gap = std::min(0.001, std::max(0.0, p[k + 1].x - p[k - 1].x) * 0.25);
                    x = std::clamp(x, p[k - 1].x + gap, p[k + 1].x - gap);
                }
                p[k].x = x; p[k].y = y;
            }
        });
    }
    void mouseReleaseEvent(QMouseEvent*) override { if (m_gesture) { m_gesture = false; m_body->ctx().project->endEdit(); } }
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        const int k = pointAt(e->position());
        if (k >= 0) { m_selected = k; removePoint(); return; }
        const auto r = graph(); double x = std::clamp((e->position().x() - r.left()) / r.width(), 0.0, 1.0);
        if (m_grid > 0 && !(e->modifiers() & Qt::AltModifier)) x = std::round(x * m_grid) / m_grid;
        const double y = std::clamp((r.bottom() - e->position().y()) / r.height(), 0.0, 1.0);
        editCurve(m_body, m_curve, "Add Curve Point", [=](Points& p) {
            if (p.size() >= mo::kCurvePoints) return;
            for (const auto& pt : p) if (std::abs(pt.x - x) < 0.001) return;
            auto it = std::upper_bound(p.begin(), p.end(), x, [](double x, const CurvePoint& q) { return x < q.x; });
            p.insert(it, {x, y});
        });
    }
    void removePoint() {
        const int k = m_selected;
        editCurve(m_body, m_curve, "Delete Curve Point", [=](Points& p) { if (k > 0 && k + 1 < p.size()) p.removeAt(k); });
        m_selected = -1;
    }
    void keyPressEvent(QKeyEvent* e) override {
        if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) removePoint(); else Plot::keyPressEvent(e);
    }
    void contextMenuEvent(QContextMenuEvent* e) override {
        m_segment = segmentAt(e->pos()); QMenu menu(this);
        for (int shape = 0; shape < 3; ++shape) menu.addAction(QStringList{"Curve", "Smooth", "Hold"}[shape], this, [this, shape] { setShape(shape); });
        menu.addAction("Straighten segment", this, [this] { const int s = m_segment; editCurve(m_body, m_curve, "Straighten Segment", [=](Points& p) { if (s >= 0 && s < p.size()) p[s].bend = 0; }); });
        menu.addSeparator(); menu.addAction("Delete point", this, [this] { removePoint(); })->setEnabled(m_selected > 0 && m_selected + 1 < points().size());
        menu.exec(e->globalPos());
    }
private:
    int m_curve, m_selected = -1, m_segment = 0, m_grid = 16, m_lastPaint = -1;
    bool m_bending = false, m_gesture = false, m_draw = false;
    QPointF m_press; double m_initial = 0, m_lastY = 0;
};

class LfoPlot : public Plot {
public:
    LfoPlot(DeviceBody* b, int lfo) : Plot(b, 166), m_lfo(lfo) { InfoView::attach(this, "LFO", "The LFO's shape across one cycle. A synced rate follows the transport; Free uses Hz. Retrigger restarts on each note."); }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        grid(p, r); QPainterPath path;
        for (int x = 0; x <= int(r.width()); ++x) {
            const double phase = x / r.width() * 0.99999;
            const int shape = m_body->pick(mo::lparam(m_lfo, mo::LShape));
            const double y = dsp::lfoShape(shape, shape == 5 ? std::floor(phase * 8) + 3 : phase);
            const QPointF pt(r.left() + x, r.center().y() - y * r.height() * 0.46);
            if (x == 0) path.moveTo(pt); else path.lineTo(pt);
        }
        curve(p, r, path, theme::osmiumLfo);
        const float value = m_body->scope(m_lfo < 2 ? 8 + m_lfo : 12 + m_lfo - 2);
        p.setPen(QPen(theme::withAlpha(theme::osmiumLfo, 120), 1)); const double y = r.center().y() - value * r.height() * 0.46;
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }
private: int m_lfo;
};

class StepPlot : public Plot {
public:
    StepPlot(DeviceBody* b, int s) : Plot(b, 138), m_step(s) { setCursor(Qt::CrossCursor); InfoView::attach(this, "Stepper", "Drag across the steps to draw a bipolar pattern. Count limits the active steps; Glide connects them. Each bar is also available as a mappable parameter below."); }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        grid(p, r); const int count = int(m_body->real(mo::sparam(m_step, mo::SCount)));
        const double w = r.width() / 16;
        for (int i = 0; i < 16; ++i) {
            const double y = (m_body->pos(mo::step(m_step, i)) * 2 - 1) * r.height() * 0.5;
            p.fillRect(QRectF(r.left() + i * w + 2, y > 0 ? r.center().y() - y : r.center().y(), w - 4, std::max(1.0, std::abs(y))), i < count ? theme::osmiumGreen : theme::osmiumBorder);
        }
    }
    void mousePressEvent(QMouseEvent* e) override { if (e->button() == Qt::LeftButton) { m_gesture = true; m_body->ctx().project->beginEdit("Draw Steps"); mouseMoveEvent(e); } }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!m_gesture || !(e->buttons() & Qt::LeftButton)) return;
        const auto r = graph(); const int i = std::clamp(int((e->position().x() - r.left()) / r.width() * 16), 0, 15);
        m_body->edit(mo::step(m_step, i), (r.bottom() - e->position().y()) / r.height());
    }
    void mouseReleaseEvent(QMouseEvent*) override { if (m_gesture) { m_gesture = false; m_body->ctx().project->endEdit(); } }
private: int m_step; bool m_gesture = false;
};
class RangePlot : public Plot {
public:
    RangePlot(DeviceBody* b, std::function<int()> source, std::function<int()> param) : Plot(b, 68), m_source(std::move(source)), m_param(std::move(param)) {
        InfoView::attach(this, "Modulation range", "The green interval is this source's reachable range on the selected parameter. The white line is the base value. Bipolar sources extend both sides of the base.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const int param = m_param(); const auto route = routeFor(m_body, m_source(), param); const double base = m_body->pos(param);
        const double a = std::clamp(base + route.amount, 0.0, 1.0), b = mo::sourceBipolar(m_source()) ? std::clamp(base - route.amount, 0.0, 1.0) : base;
        const double lo = std::min(a, b), hi = std::max(a, b), y = r.top() + 6;
        p.setPen(QPen(theme::osmiumBorder, 5, Qt::SolidLine, Qt::RoundCap)); p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        p.setPen(QPen(theme::osmiumGreen, 5, Qt::SolidLine, Qt::RoundCap)); p.drawLine(QPointF(r.left() + r.width() * lo, y), QPointF(r.left() + r.width() * hi, y));
        p.setPen(QPen(theme::textBright, 2)); p.drawLine(QPointF(r.left() + base * r.width(), y - 6), QPointF(r.left() + base * r.width(), y + 6));
        p.setFont(pxFont(this, 10)); p.setPen(theme::textSoft);
        p.drawText(QRectF(r.left(), y + 12, r.width() / 2, 16), Qt::AlignLeft, paramText(m_body->ctx().type, param, lo));
        p.drawText(QRectF(r.center().x(), y + 12, r.width() / 2, 16), Qt::AlignRight, paramText(m_body->ctx().type, param, hi));
    }
private: std::function<int()> m_source, m_param;
};

class OsmiumEditor : public DeviceBody {
public:
    OsmiumEditor(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        setObjectName("osmiumEditor"); setFixedSize(1180, 806);
        auto* root = new QVBoxLayout(this); root->setContentsMargins(16, 14, 16, 12); root->setSpacing(10);
        auto* header = hbox(12);
        auto* title = new QLabel("OSMIUM", this); title->setObjectName("osmiumTitle"); header->addWidget(title);
        header->addSpacing(22);
        m_patches = new ChoiceBox(this); m_patches->setObjectName("osmiumPatch"); m_patches->setFixedWidth(258);
        m_patches->addItems({"Init", "Basalt Bass", "Obsidian Reese", "Glacial Pad", "Pressure Sequence"});
        m_patches->insertSeparator(m_patches->count());
        for (const auto& patch : osmium::factoryPresets()) {
            m_patches->addItem(QString::fromUtf8(patch.name).mid(7), QString::fromUtf8(patch.name));
            m_patches->setItemData(m_patches->count() - 1, QString::fromUtf8(patch.description), Qt::ToolTipRole);
        }
        m_patches->setMaxVisibleItems(16); m_patches->setEditable(true); m_patches->setInsertPolicy(QComboBox::NoInsert);
        m_patches->completer()->setFilterMode(Qt::MatchContains); m_patches->completer()->setCaseSensitivity(Qt::CaseInsensitive);
        if (device()) { const int item = m_patches->findData(device()->name); if (item >= 0) m_patches->setCurrentIndex(item); m_presetName = device()->name; }
        InfoView::attach(m_patches, "Patch", "Type to search 40 Drum & Bass factory presets plus the original starting patches. Loading a patch replaces the parameters, curves and routes in one undo step. "
                         "Factory macros: Tone, Warp, Drive, Width, Space, Echo, Body and Air.");
        connect(m_patches, &QComboBox::activated, this, [this](int i) { loadPatch(i); }); header->addWidget(m_patches);
        header->addStretch();
        auto* voices = new QLabel(this); header->addWidget(voices);
        m_refreshers.push_back([this, voices] { voices->setText(QString("%1 / 8 voices").arg(int(meter(0)))); });
        auto* audition = button("Audition C2", "Play a short C2 through this track's instrument and effects.", [this] {
            if (ctx().transport) ctx().transport->previewNote(ctx().project->indexOfTrack(ctx().trackId), 48, 0.8);
        });
        header->addWidget(audition); root->addLayout(header);

        auto* top = hbox(8);
        for (int o = 0; o < 3; ++o) top->addWidget(oscillator(o), 4);
        for (int f = 0; f < 2; ++f) top->addWidget(filter(f), 3);
        root->addLayout(top);

        auto* sources = hbox(4);
        const std::vector<int> order{mo::SrcEnv1, mo::SrcEnv2, mo::SrcEnv3, mo::SrcEnv4, mo::SrcLfo1, mo::SrcLfo2, mo::SrcLfo3, mo::SrcLfo4,
                                     mo::SrcCurve1, mo::SrcCurve2, mo::SrcStep1, mo::SrcStep2};
        for (const int s : order) {
            auto* b = new SourceButton(this, s, [this, s] { selectSource(s); });
            sources->addWidget(b); m_sources.push_back({s, b});
        }
        m_other = new ChoiceBox(this); m_other->addItem("More sources");
        for (int s = 0; s < mo::kSources; ++s) if (std::find(order.begin(), order.end(), s) == order.end()) m_other->addItem(mo::sourceName(s), s);
        m_other->setFixedWidth(126); sources->addWidget(m_other);
        InfoView::attach(m_other, "Performance sources", "Select a macro, velocity, key, random, wheel, pressure or pitch bend. Click a knob's + slot to assign the selected source.");
        connect(m_other, &QComboBox::activated, this, [this](int i) { if (i > 0) selectSource(m_other->itemData(i).toInt()); });
        root->addLayout(sources);

        auto* workspace = hbox(10);
        auto* panel = card(); auto* pane = new QVBoxLayout(panel); pane->setContentsMargins(12, 10, 12, 10); pane->setSpacing(6);
        auto* nav = hbox(6); m_sourceTitle = new QLabel(this); nav->addWidget(m_sourceTitle); nav->addStretch();
        for (int i = 0; i < 3; ++i) {
            auto* b = button(QStringList{"Modulators", "Voice & mixer", "Effects"}[i], "Open this editing page.", [this, i] { setPage(i); });
            b->setCheckable(true); nav->addWidget(b); m_pagesButtons.push_back(b);
        }
        pane->addLayout(nav);
        m_pages = new QStackedWidget(panel); m_modulators = new QStackedWidget(m_pages);
        for (int e = 0; e < 4; ++e) addSourcePage(mo::SrcEnv1 + e, envelope(e));
        for (int l = 0; l < 4; ++l) addSourcePage(l < 2 ? mo::SrcLfo1 + l : mo::SrcLfo3 + l - 2, lfo(l));
        for (int c = 0; c < 2; ++c) addSourcePage(mo::SrcCurve1 + c, curvePage(c));
        for (int s = 0; s < 2; ++s) addSourcePage(mo::SrcStep1 + s, stepper(s));
        m_performance = new QWidget(m_modulators); auto* perf = new QVBoxLayout(m_performance);
        auto* perfTitle = new QLabel("Performance source", m_performance); perfTitle->setAlignment(Qt::AlignCenter); perf->addWidget(perfTitle);
        auto* assign = button("Assign to selected parameter", "Connect the selected source to the destination shown in the range panel.", [this] { setRoute(this, m_source, m_focus, 0.25); });
        perf->addWidget(assign, 0, Qt::AlignCenter); perf->addStretch(); m_modulators->addWidget(m_performance);
        m_pages->addWidget(m_modulators);
        m_pages->addWidget(voice()); m_pages->addWidget(effects());
        pane->addWidget(m_pages); workspace->addWidget(panel, 1);

        auto* inspector = card(); inspector->setFixedWidth(256); auto* ins = new QVBoxLayout(inspector); ins->setContentsMargins(14, 12, 14, 12); ins->setSpacing(7);
        auto* rangeTitle = new QLabel("MODULATION RANGE", inspector); rangeTitle->setObjectName("osmiumCaption"); ins->addWidget(rangeTitle);
        m_destination = new QLabel(inspector); ins->addWidget(m_destination);
        ins->addWidget(new RangePlot(this, [this] { return m_source; }, [this] { return m_focus; }));
        auto* amountRow = hbox(8); m_rangeLabel = new QLabel(inspector); amountRow->addWidget(m_rangeLabel, 1);
        m_amount = new DragValue(inspector); m_amount->setObjectName("osmiumAmount"); m_amount->setRange(-100, 100); m_amount->setDefault(0); m_amount->setFixedSize(68, 24);
        m_amount->setFormatter([](double v) { return QString("%1%2 %").arg(v >= 0 ? "+" : "").arg(v, 0, 'f', 1); });
        InfoView::attach(m_amount, "Range amount", "Drag to set the modulation amount. Double-click to type an exact percentage (-100 to +100). A negative amount reverses the direction.");
        connect(m_amount, &DragValue::valueEdited, this, [this](double v) { setRoute(this, m_source, m_focus, v / 100); });
        amountRow->addWidget(m_amount); ins->addLayout(amountRow);
        auto* assignRow = hbox(5);
        assignRow->addWidget(button("Assign", "Assign the selected source to this parameter.", [this] { if (routeFor(this, m_source, m_focus).source < 0) setRoute(this, m_source, m_focus, 0.25); }));
        assignRow->addWidget(button("Remove", "Remove this source from this parameter.", [this] { setRoute(this, m_source, m_focus, 0, true); })); ins->addLayout(assignRow);
        auto* destTitle = new QLabel("DESTINATIONS", inspector); destTitle->setObjectName("osmiumCaption"); ins->addWidget(destTitle);
        auto* routeArea = new QScrollArea(inspector); routeArea->setWidgetResizable(true); routeArea->setFrameShape(QFrame::NoFrame); routeArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto* list = new QWidget(routeArea); m_routes = new QVBoxLayout(list); m_routes->setContentsMargins(0, 0, 0, 0); m_routes->setSpacing(3); routeArea->setWidget(list); ins->addWidget(routeArea, 1);
        workspace->addWidget(inspector); root->addLayout(workspace, 1);

        auto* macroPanel = card(); auto* macros = new QHBoxLayout(macroPanel); macros->setContentsMargins(12, 5, 12, 5); macros->setSpacing(8);
        auto* macroTitle = new QLabel("MACROS", macroPanel); macroTitle->setObjectName("osmiumCaption"); macros->addWidget(macroTitle);
        for (int k = 0; k < 8; ++k) {
            auto* cell = new QWidget(macroPanel); auto* col = vbox(0);
            col->addWidget(dial(mo::macro(k), QString::number(k + 1), 38)); cell->setLayout(col); macros->addWidget(cell, 1, Qt::AlignHCenter);
        }
        macros->addStretch(); macros->addWidget(dial(mo::Volume, "Output", 38)); root->addWidget(macroPanel);
        applyStyle(); connect(theme::notifier(), &theme::Notifier::changed, this, [this] { applyStyle(); });
        selectSource(mo::SrcEnv2); refresh();
        connect(c.project, &Project::restored, this, [this] { m_ctx.track = ctx().project->indexOfTrack(ctx().trackId); refresh(); });
        auto* timer = new QTimer(this); timer->setInterval(33);
        connect(timer, &QTimer::timeout, this, [this] {
            if (!isVisible()) return;
            m_ctx.track = ctx().project->indexOfTrack(ctx().trackId);
            if (!device()) { window()->close(); return; }
            if (ctx().transport) follow(ctx().transport->beat());
            refresh();
        }); timer->start();
    }
    void refresh() override {
        DeviceBody::refresh();
        if (!m_destination) return;
        if (device() && device()->name != m_presetName) {
            m_presetName = device()->name;
            const int item = m_patches->findData(m_presetName); m_patches->setCurrentIndex(item >= 0 ? item : 0);
        }
        m_destination->setText(name(m_focus));
        const auto r = routeFor(this, m_source, m_focus);
        m_amount->setValue(r.amount * 100, false);
        m_rangeLabel->setText(QString("%1  %2").arg(mo::sourceName(m_source), mo::sourceBipolar(m_source) ? "±" : "→"));
        rebuildRoutes();
    }
    void selectSource(int s, bool revealPage = true) {
        m_source = s;
        if (m_other) { const int item = m_other->findData(s); m_other->setCurrentIndex(item >= 0 ? item : 0); }
        for (const auto& [source, button] : m_sources) {
            button->setChecked(source == s);
            button->setStyleSheet(QString("QPushButton{color:%1;} QPushButton:checked{background:%2;color:%3;border-color:%2;}")
                .arg(sourceColor(source).name(), sourceColor(source).name(), (sourceColor(source).lightnessF() > 0.55 ? theme::ink : theme::inkLight).name()));
        }
        if (m_modulators) {
            auto it = m_sourcePages.find(s);
            m_modulators->setCurrentWidget(it == m_sourcePages.end() ? m_performance : it->second);
            m_sourceTitle->setText(QString("%1%2").arg(mo::sourceName(s), s == mo::SrcEnv1 ? " · AMP" : ""));
            if (revealPage) setPage(0);
        }
        refresh();
    }
protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        auto* control = qobject_cast<QWidget*>(object);
        if (!control) return DeviceBody::eventFilter(object, event);
        bool ok = false; const int param = control->property("mapTarget").toString().section(':', -1).toInt(&ok);
        if (!ok) return DeviceBody::eventFilter(object, event);
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::FocusIn) { m_focus = param; refresh(); }
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove || event->type() == QEvent::Drop) {
            auto* drop = static_cast<QDropEvent*>(event);
            const auto data = drop->mimeData()->data(kSourceMime).split(':');
            bool idOk = false, sourceOk = false;
            const int device = data.value(0).toInt(&idOk), source = data.value(1).toInt(&sourceOk);
            if (data.size() != 2 || !idOk || !sourceOk || device != ctx().deviceId || source < 0 || source >= mo::kSources) { drop->ignore(); return true; }
            drop->acceptProposedAction();
            if (event->type() == QEvent::Drop) { m_focus = param; setRoute(this, source, param, 0.25); selectSource(source); }
            return true;
        }
        return DeviceBody::eventFilter(object, event);
    }
private:
    QPushButton* button(const QString& text, const QString& help, std::function<void()> fn) {
        auto* b = new QPushButton(text, this); b->setMinimumHeight(26); b->setCursor(Qt::PointingHandCursor);
        InfoView::attach(b, text, help); connect(b, &QPushButton::clicked, this, std::move(fn)); return b;
    }
    QFrame* card() { auto* f = new QFrame(this); f->setObjectName("osmiumCard"); return f; }
    QWidget* dial(int i, const QString& name = {}, int size = 36) {
        m_used.insert(i);
        return new RangeKnob(this, i, name.isEmpty() ? this->name(i) : name, size, [this] { return m_source; }, [this](int p) { m_focus = p; refresh(); }, [this](int s) { selectSource(s, false); });
    }
    QWidget* choice(int i, int width = 0) {
        m_used.insert(i); const auto& p = specOf(ctx().type)->params[size_t(i)];
        auto* c = new ChoiceBox(this); c->setObjectName(QString("osmiumChoice_%1").arg(i)); c->setProperty("mapTarget", target(i));
        c->setAcceptDrops(true); c->installEventFilter(this);
        if (width > 0) c->setFixedWidth(width); else c->setMinimumWidth(70);
        if (p.kind == ParamKind::Toggle) c->addItems({"Off", "On"}); else for (const char* v : p.choices) c->addItem(v);
        c->setCurrentIndex(p.kind == ParamKind::Toggle ? int(on(i)) : pick(i));
        connect(c, &QComboBox::activated, this, [this, i, c](int index) { edit(i, choicePosition(index, c->count())); });
        m_refreshers.push_back([this, i, c, toggle = p.kind == ParamKind::Toggle] { c->setCurrentIndex(toggle ? int(on(i)) : pick(i)); });
        InfoView::attach(c, name(i), "Choose the parameter's value. Changes can be automated or mapped from a MIDI controller."); return c;
    }
    QWidget* labelledChoice(int i, const QString& label, int width = 0) {
        auto* w = new QWidget(this); auto* l = vbox(3); l->addWidget(new QLabel(label, w)); l->addWidget(choice(i, width)); l->addStretch(); w->setLayout(l); return w;
    }
    QWidget* oscillator(int o) {
        auto* box = card(); auto* l = new QVBoxLayout(box); l->setContentsMargins(10, 10, 10, 8); l->setSpacing(6);
        auto* top = hbox(4); auto* name = new QLabel(QString("OSC %1").arg(o + 1), box); name->setObjectName("osmiumCaption"); top->addWidget(name); top->addStretch();
        top->addWidget(choice(mo::oparam(o, mo::Table), 106)); l->addLayout(top);
        l->addWidget(new WavePlot(this, o, 46));
        auto* knobs = hbox(3); knobs->addStretch(); knobs->addWidget(dial(mo::oparam(o, mo::Position), "Position", 52));
        knobs->addWidget(dial(mo::oparam(o, mo::Intensity), "Warp", 44)); knobs->addWidget(dial(mo::oparam(o, mo::Level), "Level", 44)); knobs->addStretch(); l->addLayout(knobs);
        auto* bottom = hbox(4); bottom->addWidget(choice(mo::oparam(o, mo::Mode), 78)); bottom->addWidget(new QLabel("Pitch", box));
        m_used.insert(mo::oparam(o, mo::Pitch)); auto* pitch = field(mo::oparam(o, mo::Pitch), 46); pitch->setAcceptDrops(true); pitch->installEventFilter(this); bottom->addWidget(pitch); l->addLayout(bottom);
        return box;
    }
    QWidget* filter(int f) {
        auto* box = card(); auto* l = new QVBoxLayout(box); l->setContentsMargins(10, 10, 10, 8); l->setSpacing(6);
        auto* title = new QLabel(QString("FILTER %1").arg(f + 1), box); title->setObjectName("osmiumCaption"); l->addWidget(title);
        l->addWidget(choice(mo::fparam(f, mo::FType))); l->addWidget(new FilterPlot(this, f));
        auto* knobs = hbox(0); knobs->addStretch(); knobs->addWidget(dial(mo::fparam(f, mo::FCutoff), "Cutoff", 52)); knobs->addWidget(dial(mo::fparam(f, mo::FReso), "Resonance", 44)); knobs->addStretch(); l->addLayout(knobs); l->addStretch();
        return box;
    }
    QWidget* envelope(int e) {
        auto* w = new QWidget(this); auto* l = vbox(4); w->setLayout(l); auto* row = hbox(6);
        row->addWidget(labelledChoice(mo::eloop(e), "Loop", 66));
        const int params[] = {mo::A, mo::D, mo::S, mo::R}; const QStringList names{"Attack", "Decay", "Sustain", "Release"};
        for (int k = 0; k < 4; ++k) row->addWidget(dial(mo::eparam(e, mo::EnvKnob(params[k])), names[k], 40));
        for (int s = 0; s < 3; ++s) row->addWidget(dial(osmium::bend(e, s), QStringList{"A bend", "D bend", "R bend"}[s], 36));
        row->addWidget(dial(mo::ecurve(e), "Level curve", 36)); row->addStretch();
        l->addWidget(new EnvelopePlot(this, e)); l->addLayout(row); return w;
    }
    QWidget* lfo(int n) {
        auto* w = new QWidget(this); auto* l = vbox(5); w->setLayout(l); l->addWidget(new LfoPlot(this, n)); auto* row = hbox(12);
        row->addWidget(labelledChoice(mo::lparam(n, mo::LRate), "Rate", 90)); row->addWidget(labelledChoice(mo::lparam(n, mo::LShape), "Shape", 104));
        row->addWidget(dial(mo::lparam(n, mo::LHz), "Free Hz", 32)); row->addWidget(dial(mo::lfade(n), "Fade in", 32));
        row->addWidget(labelledChoice(mo::lretrig(n), "Retrigger", 76)); row->addWidget(labelledChoice(mo::lmode(n), "Mode", 104)); row->addStretch(); l->addLayout(row); return w;
    }
    QWidget* curvePage(int c) {
        auto* w = new QWidget(this); auto* l = vbox(5); w->setLayout(l);
        auto* plot = new CurvePlot(this, c); auto* toolbar = hbox(4);
        auto* shapes = new ChoiceBox(w); shapes->setObjectName(QString("osmiumCurveShape_%1").arg(c)); shapes->addItems({"Triangle", "Sine", "Rise", "Fall", "Steps", "Random"}); shapes->setCurrentIndex(-1); shapes->setPlaceholderText("Shape"); shapes->setFixedWidth(90);
        InfoView::attach(shapes, "Curve shape", "Replace the curve with an editable shape. Every segment can then be bent, smoothed or held.");
        connect(shapes, &QComboBox::activated, this, [plot, shapes](int i) { plot->preset(i); shapes->setCurrentIndex(-1); }); toolbar->addWidget(shapes);
        auto* draw = button("Draw", "Paint a 16-step curve by dragging across the graph.", [] {}); draw->setCheckable(true); connect(draw, &QPushButton::toggled, plot, [plot](bool on) { plot->setDraw(on); }); toolbar->addWidget(draw);
        auto* snap = new ChoiceBox(w); snap->setFixedWidth(76); snap->addItems({"1/16", "1/32", "1/8", "Free"});
        connect(snap, &QComboBox::activated, plot, [plot](int i) { plot->setGrid(std::vector<int>{16, 32, 8, 0}[size_t(i)]); });
        InfoView::attach(snap, "Snap", "Set the time grid for moving or adding nodes. Alt temporarily bypasses it."); toolbar->addWidget(snap);
        auto* segment = new ChoiceBox(w); segment->addItems({"Curve", "Smooth", "Hold"}); segment->setCurrentIndex(-1); segment->setPlaceholderText("Segment"); segment->setFixedWidth(86);
        InfoView::attach(segment, "Segment shape", "Click a segment in the graph, then choose its interpolation. Curve has a bend handle, Smooth eases both ends, Hold makes a step.");
        connect(segment, &QComboBox::activated, plot, [plot, segment](int i) { plot->setShape(i); segment->setCurrentIndex(-1); }); toolbar->addWidget(segment);
        toolbar->addWidget(button("Invert", "Flip the curve vertically.", [plot] { plot->transform(0); })); toolbar->addWidget(button("Reverse", "Reverse the curve in time.", [plot] { plot->transform(1); }));
        toolbar->addWidget(button("Copy", "Copy this curve including segment shapes and bends.", [plot] { CurvePlot::clipboard() = plot->points(); }));
        toolbar->addWidget(button("Paste", "Paste the copied curve as one undo step.", [plot] { if (!CurvePlot::clipboard().isEmpty()) plot->fill("Paste Curve", CurvePlot::clipboard()); })); toolbar->addStretch();
        l->addLayout(toolbar); l->addWidget(plot);
        auto* row = hbox(12); row->addWidget(labelledChoice(mo::cparam(c, mo::CRate), "Length", 100)); row->addWidget(labelledChoice(mo::cparam(c, mo::CMode), "Mode", 100));
        row->addWidget(labelledChoice(mo::cparam(c, mo::CRetrig), "Retrigger", 80)); row->addWidget(dial(mo::cparam(c, mo::CSmooth), "Smoothing", 24)); row->addStretch(); l->addLayout(row); return w;
    }
    QWidget* stepper(int s) {
        auto* w = new QWidget(this); auto* l = vbox(5); w->setLayout(l); l->addWidget(new StepPlot(this, s)); auto* row = hbox(10);
        row->addWidget(labelledChoice(mo::sparam(s, mo::SRate), "Rate", 90)); row->addWidget(dial(mo::sparam(s, mo::SCount), "Count", 28));
        row->addWidget(dial(mo::sparam(s, mo::SGlide), "Glide", 28)); row->addWidget(labelledChoice(mo::sretrig(s), "Retrigger", 76)); row->addStretch(); l->addLayout(row);
        auto* values = hbox(3);
        for (int i = 0; i < 16; ++i) {
            const int param = mo::step(s, i); m_used.insert(param);
            auto* col = vbox(1); auto* label = new QLabel(QString::number(i + 1), w); label->setAlignment(Qt::AlignCenter); col->addWidget(label); col->addWidget(field(param, 42)); values->addLayout(col);
        }
        l->addLayout(values);
        return w;
    }
    QWidget* effects() {
        auto* scroll = new QScrollArea(this); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
        auto* w = new QWidget(scroll); auto* grid = new QGridLayout(w); grid->setContentsMargins(4, 8, 4, 4); grid->setSpacing(10);
        const QStringList names{"Chorus rate", "Depth", "Chorus mix", "Delay time", "Feedback", "Tone", "Delay mix", "Room size", "Reverb mix", "Low EQ", "High EQ"};
        for (int k = 0; k < mo::kFx; ++k) {
            const int i = mo::kFxBase + k; const auto kind = specOf(ctx().type)->params[size_t(i)].kind;
            grid->addWidget(kind == ParamKind::Choice ? labelledChoice(i, names[k]) : dial(i, names[k], 38), k / 6, k % 6, Qt::AlignTop | Qt::AlignHCenter);
        }
        grid->setRowStretch(2, 1); scroll->setWidget(w); return scroll;
    }
    QWidget* voice() {
        auto* scroll = new QScrollArea(this); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
        auto* w = new QWidget(scroll); auto* grid = new QGridLayout(w); grid->setContentsMargins(4, 8, 4, 4); grid->setHorizontalSpacing(10); grid->setVerticalSpacing(8);
        int n = 0;
        // FX, macros and output are built below; all other unused parameters belong here.
        for (int i = 0; i < count(); ++i) {
            if (m_used.count(i) || i == mo::Volume || (i >= mo::kFxBase && i < mo::kFxBase + mo::kFx)) continue;
            bool macro = false; for (int k = 0; k < 8; ++k) macro |= i == mo::macro(k); if (macro) continue;
            const auto& p = specOf(ctx().type)->params[size_t(i)];
            QWidget* control = p.kind == ParamKind::Choice || p.kind == ParamKind::Toggle ? labelledChoice(i, p.name, 96) : dial(i, p.name, 32);
            grid->addWidget(control, n / 7, n % 7, Qt::AlignTop | Qt::AlignHCenter); ++n;
        }
        grid->setRowStretch((n + 6) / 7, 1); scroll->setWidget(w); return scroll;
    }
    void addSourcePage(int source, QWidget* w) { m_sourcePages[source] = w; m_modulators->addWidget(w); }
    void setPage(int i) { m_pages->setCurrentIndex(i); for (int k = 0; k < int(m_pagesButtons.size()); ++k) m_pagesButtons[size_t(k)]->setChecked(k == i); }
    void rebuildRoutes() {
        const auto* d = device(); if (!d) return;
        QString signature = QString::number(m_source);
        for (const auto& r : d->mods) if (r.source == m_source) signature += ':' + QString::number(r.param);
        if (m_routesSignature == signature) return; m_routesSignature = signature;
        while (auto* it = m_routes->takeAt(0)) { if (it->widget()) { it->widget()->hide(); it->widget()->deleteLater(); } delete it; }
        int count = 0;
        for (const auto& r : d->mods) if (r.source == m_source) {
            const int param = r.param; auto* b = button(name(param), "Select this destination to inspect and adjust its range.", [this, param] { m_focus = param; refresh(); });
            b->setStyleSheet("text-align:left;padding-left:8px;"); m_routes->addWidget(b); ++count;
        }
        if (count == 0) { auto* label = new QLabel("No assignments", this); label->setObjectName("osmiumCaption"); m_routes->addWidget(label); }
        m_routes->addStretch();
    }
    void loadPatch(int i) {
        if (i < 0 || i >= m_patches->count()) return;
        const auto* patch = osmium::factoryPreset(m_patches->itemData(i).toString());
        const QStringList names{"Monolith", "Dub Wobble", "Neuro Reese", "Glass Pad", "Neuro Step"}; if (!patch && i >= names.size()) return;
        bool renamed = false;
        {
            EditScope edit(ctx().project, "Load Osmium Patch");
            Device* d = mutableDevice(this); if (!d) return;
            const QString name = patch ? QString::fromUtf8(patch->name) : QString("Osmium");
            renamed = d->name != name; d->name = name;
            if (patch) osmium::applyPreset(*d, *patch);
            else {
                const auto* spec = deviceSpec(names[i]); const auto* defaults = specOf(DeviceType::Osmium);
                for (int p = 0; p < d->params.size(); ++p) {
                    d->params[p].value = p < int(spec->params.size()) ? spec->params[size_t(p)].def : defaults->params[size_t(p)].def;
                }
                d->mods.clear(); for (const auto& r : presetMods(names[i])) d->mods.push_back(r);
                if (i == 0) d->mods = {{mo::SrcEnv2, mo::fparam(0, mo::FCutoff), 0.28}};
                for (int e = 0; e < 4; ++e) { d->params[osmium::bend(e, 1)].value = 0.22; d->params[osmium::bend(e, 2)].value = 0.22; }
                d->curves = {{{0, 0}, {0.25, 1, -0.35, 1}, {0.5, 0.15}, {0.75, 0.7, 0.3, 1}, {1, 0}}, {{0, 1, 0.35}, {0.5, 0, -0.35}, {1, 1}}};
            }
            m_presetName = d->name;
            for (int p = 0; p < d->params.size(); ++p) ctx().project->touchTarget(ctx().project->indexOfTrack(ctx().trackId), target::device(ctx().deviceId, p));
        }
        emit ctx().project->deviceChanged(); refresh();
        if (renamed) emit ctx().project->structureChanged();
    }
    void applyStyle() {
        setStyleSheet(QString(
            "QWidget#osmiumEditor{background:%1;color:%5;}"
            "QFrame#osmiumCard{background:%2;border:1px solid %4;border-radius:14px;}"
            "QLabel{background:transparent;color:%5;font-size:10px;border:0;}"
            "QLabel#osmiumTitle{font-size:24px;font-weight:700;color:%6;letter-spacing:5px;}"
            "QLabel#osmiumCaption{font-size:9px;font-weight:600;color:%7;letter-spacing:1px;}"
            "QPushButton{background:%3;border:1px solid %4;border-radius:8px;color:%5;padding:3px 8px;font-size:10px;}"
            "QPushButton:hover{border-color:%6;} QPushButton:checked{background:%6;color:%8;border-color:%6;}"
            "QComboBox{background:%3;color:%5;border:1px solid %4;border-radius:7px;min-height:24px;padding:0 20px 0 7px;font-size:10px;}"
            "QComboBox::drop-down{width:16px;border:0;} QComboBox::down-arrow{image:none;}"
            "QComboBox QAbstractItemView{background:%2;color:%5;selection-background-color:%4;}"
            "QScrollArea{background:transparent;border:0;} QScrollArea>QWidget>QWidget{background:transparent;}"
        ).arg(theme::osmiumBg.name(), theme::osmiumSurface.name(), theme::osmiumInset.name(), theme::osmiumBorder.name(),
              theme::text.name(), theme::osmiumGreen.name(), theme::textFaint.name(), (theme::osmiumGreen.lightnessF() > 0.55 ? theme::ink : theme::inkLight).name()));
        if (m_modulators) selectSource(m_source, false);
    }
    int m_source = mo::SrcEnv2, m_focus = mo::fparam(0, mo::FCutoff);
    std::set<int> m_used; std::map<int, QWidget*> m_sourcePages;
    std::vector<std::pair<int, SourceButton*>> m_sources; std::vector<QPushButton*> m_pagesButtons;
    QStackedWidget* m_pages = nullptr; QStackedWidget* m_modulators = nullptr; QWidget* m_performance = nullptr;
    QComboBox* m_other = nullptr; QComboBox* m_patches = nullptr; QString m_presetName;
    QLabel* m_sourceTitle = nullptr; QLabel* m_destination = nullptr; QLabel* m_rangeLabel = nullptr;
    DragValue* m_amount = nullptr; QVBoxLayout* m_routes = nullptr; QString m_routesSignature;
};

// Rack panels are rebuilt on undo, track selection and theme changes. A floating editor
// follows the device's stable identity and survives those panel rebuilds.
QHash<QString, QPointer<QDialog>>& windows() { static QHash<QString, QPointer<QDialog>> w; return w; }
class OsmiumBody : public DeviceBody {
public:
    OsmiumBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        setFixedWidth(502);
        // A native tool window leaves the Session grid available while shaping a sound.
        m_key = QString::number(quintptr(c.project)) + ':' + QString::number(c.deviceId);
        m_dialog = windows().value(m_key);
        if (!m_dialog) {
            m_dialog = new QDialog(this, Qt::Window); m_dialog->setObjectName("osmiumWindow"); m_dialog->setWindowTitle("Osmium");
            auto* dialogLayout = new QVBoxLayout(m_dialog); dialogLayout->setContentsMargins(0, 0, 0, 0);
            auto* scroll = new QScrollArea(m_dialog); scroll->setWidgetResizable(false); scroll->setFrameShape(QFrame::NoFrame);
            scroll->setWidget(new OsmiumEditor(c, scroll)); dialogLayout->addWidget(scroll);
            m_dialog->resize(1184, 810); m_dialog->setMinimumSize(700, 480);
            windows().insert(m_key, m_dialog);
            QDialog* dialog = m_dialog;
            connect(c.project, &QObject::destroyed, dialog, [dialog] { delete dialog; });
            connect(dialog, &QObject::destroyed, [key = m_key] { windows().remove(key); });
            auto* undo = new QShortcut(QKeySequence::Undo, dialog);
            connect(undo, &QShortcut::activated, c.project, [project = c.project] { if (!project->editing()) project->undoStack()->undo(); });
            auto* redo = new QShortcut(QKeySequence::Redo, dialog);
            redo->setKeys({QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), QKeySequence(Qt::CTRL | Qt::Key_Y)});
            connect(redo, &QShortcut::activated, c.project, [project = c.project] { if (!project->editing()) project->undoStack()->redo(); });
        } else {
            const bool visible = m_dialog->isVisible(); const auto geometry = m_dialog->geometry();
            m_dialog->setParent(this, Qt::Window); m_dialog->setGeometry(geometry); if (visible) m_dialog->show();
        }
        auto* root = rootRow(this, 10); auto* left = vbox(6);
        auto* title = new QLabel("OSMIUM", this); title->setStyleSheet(QString("color:%1;font-size:19px;font-weight:700;letter-spacing:3px;").arg(theme::osmiumGreen.name())); left->addWidget(title);
        left->addWidget(new WavePlot(this, 0, 58));
        auto* open = new QPushButton("Open Editor", this); open->setObjectName("osmiumOpenEditor"); open->setFixedHeight(28);
        InfoView::attach(open, "Open Osmium", "Open the visual instrument editor: green modulation ranges on the knobs, bendable envelopes, LFOs, curves, steppers, macros and effects.");
        connect(open, &QPushButton::clicked, this, [this] {
            if (const auto* screen = QApplication::primaryScreen()) { const auto available = screen->availableGeometry(); m_dialog->resize(std::min(1184, available.width() - 48), std::min(840, available.height() - 64)); }
            m_dialog->show(); m_dialog->raise(); m_dialog->activateWindow();
        }); left->addWidget(open); left->addStretch(); root->addLayout(left, 1);
        auto* controls = hbox(2);
        for (const int p : std::vector<int>{mo::oparam(0, mo::Position), mo::fparam(0, mo::FCutoff), mo::FDrive, mo::Volume}) {
            auto* k = new RangeKnob(this, p, p == mo::FDrive ? "Drive" : p == mo::Volume ? "Output" : p == mo::fparam(0, mo::FCutoff) ? "Cutoff" : "Position", 44,
                                    [] { return mo::SrcEnv2; }, [](int) {}); controls->addWidget(k, 0, Qt::AlignVCenter);
        }
        root->addLayout(controls);
        refresh();
    }
    ~OsmiumBody() override {
        if (!m_dialog || m_dialog->parentWidget() != this) return;
        const bool visible = m_dialog->isVisible(); const auto geometry = m_dialog->geometry();
        m_dialog->setParent(nullptr, Qt::Window); m_dialog->setGeometry(geometry); if (visible) m_dialog->show();
    }
private: QPointer<QDialog> m_dialog; QString m_key;
};

} // namespace
DeviceBody* createOsmiumBody(const DeviceContext& ctx, QWidget* parent) { return new OsmiumBody(ctx, parent); }
} // namespace sessions::devui
