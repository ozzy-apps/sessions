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
#include <QLineEdit>
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
#include <QScrollBar>
#include <QScreen>
#include <QShortcut>
#include <QUndoStack>
#include <map>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QTimer>
#include <QWidgetAction>
#include <QWheelEvent>
#include <QGridLayout>
#include <array>
#include <cmath>
#include <set>

namespace sessions::devui {
namespace {
namespace mo = monolith;
constexpr auto kSourceMime = "application/x-sessions-osmium-source";
constexpr double kPi = 3.141592653589793;

QColor sourceColor(int s) {
    return s <= mo::SrcEnv4 ? theme::osmiumEnv : mo::curveOfSource(s) >= 0 ? theme::osmiumCurve
         : mo::lfoOfSource(s) >= 0 ? theme::osmiumLfo : theme::accent;
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

// Both panel sizes use the same ordered bank and atomic, identity-preserving load path.
ChoiceBox* patchPicker(DeviceBody* body, const QString& objectName) {
    auto* picker = new ChoiceBox(body); picker->setObjectName(objectName);
    picker->addItems({"Init", "Basalt Bass", "Obsidian Reese", "Glacial Pad", "Pressure Sequence"});
    picker->insertSeparator(picker->count());
    bool wobbleBasses = false;
    for (const auto& patch : osmium::factoryPresets()) {
        const QLatin1String category(patch.category);
        if (!wobbleBasses && (category == QLatin1String("Dubstep") || category == QLatin1String("DnB Bass"))) {
            picker->insertSeparator(picker->count()); wobbleBasses = true;
        }
        picker->addItem(QString::fromUtf8(patch.name).mid(7), QString::fromUtf8(patch.name));
        picker->setItemData(picker->count() - 1, QString::fromUtf8(patch.description), Qt::ToolTipRole);
    }
    picker->setMaxVisibleItems(16); picker->setEditable(true); picker->setInsertPolicy(QComboBox::NoInsert);
    picker->completer()->setFilterMode(Qt::MatchContains); picker->completer()->setCaseSensitivity(Qt::CaseInsensitive);
    picker->setAccessibleName("Osmium preset");
    InfoView::attach(picker, "Preset", QString("Type to search %1 factory presets and the starting patches. Loading replaces the sound in one undo step. ").arg(osmium::factoryPresets().size()) +
                     "Macros: Tone, Warp, Drive, Width, Space, Echo, Body and Air. Automate Curve1 Rate to change a wobble phrase's length.");
    return picker;
}
void applyStartingPatch(Device& d, int i) {
    const QStringList names{"Monolith", "Dub Wobble", "Neuro Reese", "Glass Pad", "Neuro Step"};
    const auto* spec = deviceSpec(names[i]); const auto* defaults = specOf(DeviceType::Osmium);
    for (int p = 0; p < d.params.size(); ++p) {
        d.params[p].value = p < int(spec->params.size()) ? spec->params[size_t(p)].def : defaults->params[size_t(p)].def;
    }
    d.mods.clear(); for (const auto& r : presetMods(names[i])) d.mods.push_back(r);
    if (i == 0) d.mods = {{mo::SrcEnv2, mo::fparam(0, mo::FCutoff), 0.28}};
    for (int e = 0; e < 4; ++e) { d.params[osmium::bend(e, 1)].value = 0.22; d.params[osmium::bend(e, 2)].value = 0.22; }
    d.curves = {{{0, 0}, {0.25, 1, -0.35, 1}, {0.5, 0.15}, {0.75, 0.7, 0.3, 1}, {1, 0}}, {{0, 1, 0.35}, {0.5, 0, -0.35}, {1, 1}}};
}
void syncPatchPicker(DeviceBody* body, QComboBox* picker) {
    if (const auto* d = body->device(); d && (picker->property("deviceName").toString() != d->name || d->name == "Osmium")) {
        picker->setProperty("deviceName", d->name);
        int item = picker->findData(d->name);
        if (item < 0 && d->name == "Osmium") {
            // The original starting patches share a device name. Recognize their actual
            // sound after a panel rebuild or undo instead of losing the navigation position.
            for (int i = 1; i < 5; ++i) {
                Device reference = *d; applyStartingPatch(reference, i);
                bool same = reference.mods == d->mods && reference.curves == d->curves;
                for (int p = 0; same && p < d->params.size(); ++p) same &= reference.params[p].value == d->params[p].value;
                if (same) { item = i; break; }
            }
        }
        const int selected = item >= 0 ? item : 0;
        if (picker->currentIndex() != selected) picker->setCurrentIndex(selected);
    }
}
void loadPatch(DeviceBody* body, QComboBox* picker, int i) {
    if (i < 0 || i >= picker->count()) return;
    const auto* patch = osmium::factoryPreset(picker->itemData(i).toString());
    if (!patch && i >= 5) return;
    bool renamed = false;
    {
        EditScope edit(body->ctx().project, "Load Osmium Patch");
        Device* d = mutableDevice(body); if (!d) return;
        const QString name = patch ? QString::fromUtf8(patch->name) : QString("Osmium");
        renamed = d->name != name; d->name = name;
        if (patch) osmium::applyPreset(*d, *patch);
        else applyStartingPatch(*d, i);
        picker->setProperty("deviceName", d->name);
        for (int p = 0; p < d->params.size(); ++p) body->ctx().project->touchTarget(body->ctx().project->indexOfTrack(body->ctx().trackId), target::device(body->ctx().deviceId, p));
    }
    emit body->ctx().project->deviceChanged(); body->refresh();
    if (renamed) emit body->ctx().project->structureChanged();
}

QPushButton* patchArrow(DeviceBody* body, QComboBox* picker, int direction) {
    auto* arrow = new QPushButton(direction < 0 ? "‹" : "›", body);
    arrow->setObjectName(direction < 0 ? "osmiumPreviousPreset" : "osmiumNextPreset");
    arrow->setFixedSize(28, 28); arrow->setStyleSheet("font-size:18px;padding:0;border-radius:5px;");
    const QString label = direction < 0 ? "Previous preset" : "Next preset";
    arrow->setAccessibleName(label); arrow->setToolTip(label);
    QObject::connect(arrow, &QPushButton::clicked, body, [body, picker, direction] {
        int index = picker->currentIndex();
        do { index = (index + direction + picker->count()) % picker->count(); }
        while (index >= 5 && picker->itemData(index).toString().isEmpty());
        picker->setCurrentIndex(index); loadPatch(body, picker, index);
    });
    return arrow;
}

// Each control exposes its base value, source slot and directly draggable range ring.
class RangeKnob : public QWidget {
public:
    RangeKnob(DeviceBody* b, int param, QString label, int diameter, std::function<int()> source,
              std::function<void(int)> focus, std::function<void(int)> select = {}) : QWidget(b), m_body(b), m_param(param), m_label(std::move(label)),
              m_d(std::max(52, diameter)), m_source(std::move(source)), m_focus(std::move(focus)), m_select(std::move(select)) {
        setFixedSize(m_d + 12, m_d + 48);
        setObjectName(QString("osmiumKnob_%1").arg(param));
        setProperty("mapTarget", b->target(param));
        setAcceptDrops(true); setFocusPolicy(Qt::StrongFocus); setCursor(Qt::SizeVerCursor); setMouseTracking(true);
        setProperty("dialDiameter", m_d);
        setProperty("baseRadius", baseRadius()); setProperty("rangeRadius", rangeRadius());
        InfoView::attach(this, b->name(param), "Drag the centre or inner ring to set the base value. Drag the accent outer ring up/right to increase modulation, down/left to decrease or invert it. "
                         "Click + to assign the selected source; drag a source here to assign it. Shift: fine. Right-click: sources, remove, or enter a value. Delete: reset.");
        b->watch(this);
    }
    ModRoute activeRoute() const {
        ModRoute selected = routeFor(m_body, m_source(), m_param);
        if (selected.source >= 0) return selected;
        if (const Device* d = m_body->device()) for (const auto& r : d->mods) if (r.param == m_param) return r;
        return {-1, m_param, 0};
    }
    QPointF center() const { return {width() / 2.0, 16 + m_d / 2.0}; }
    double baseRadius() const { return m_d / 2.0 - 10; }
    double rangeRadius() const { return m_d / 2.0 - 1; }
    bool onRange(QPointF pt) const {
        const auto delta = pt - center(); const double distance = QLineF(pt, center()).length();
        const double angle = std::atan2(delta.y(), delta.x()) * 180 / kPi;
        return distance >= rangeRadius() - 3 && distance <= rangeRadius() + 5 && !(angle > 45 && angle < 135);
    }
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
        const double base = m_body->pos(m_param), radius = rangeRadius();
        arc(p, 0, 1, baseRadius(), theme::devGridStrong, 3);
        arc(p, 0, base, baseRadius(), theme::textSoft, 3);
        // The full modulation track remains visible, including when no source is assigned.
        arc(p, 0, 1, radius, theme::withAlpha(theme::accent, 55), 4);
        const ModRoute r = activeRoute();
        if (r.source >= 0) {
            const double end = std::clamp(base + r.amount, 0.0, 1.0);
            const double begin = mo::sourceBipolar(r.source) ? std::clamp(base - r.amount, 0.0, 1.0) : base;
            arc(p, std::min(begin, end), std::max(begin, end), radius, theme::accent, 4);
            p.setPen(Qt::NoPen); p.setBrush(theme::accent); p.drawEllipse(point(end, radius), 3, 3);
            p.setPen(QPen(theme::withAlpha(theme::accent, 90), 1));
            p.drawLine(point(base, radius - 2), point(base, radius + 3));
        }
        p.setPen(QPen(hasFocus() ? theme::accent : theme::devGridStrong, 1));
        const double face = baseRadius() - 5;
        p.setBrush(theme::devDisplay); p.drawEllipse(center(), face, face);
        p.setPen(QPen(theme::textBright, 2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(point(base, 3), point(base, face - 3));
        p.setPen(theme::text); p.setFont(pxFont(this, 10));
        p.drawText(QRectF(0, m_d + 17, width(), 14), Qt::AlignCenter, m_body->text(m_param));
        const QRectF slot(4, height() - 16, width() - 8, 14);
        p.setBrush(r.source >= 0 ? theme::withAlpha(theme::accent, 20) : theme::devDisplay);
        p.setPen(Qt::NoPen); p.drawRoundedRect(slot, 5, 5);
        p.setPen(r.source >= 0 ? theme::accent : theme::textFaint); p.setFont(pxFont(this, 9));
        p.drawText(slot, Qt::AlignCenter, r.source < 0 ? "+" : QString("%1 %2%3").arg(mo::sourceName(r.source))
                   .arg(r.amount >= 0 ? "+" : "").arg(int(std::round(r.amount * 100))));
        if (m_drop) { p.setPen(QPen(theme::accent, 2)); p.setBrush(Qt::NoBrush); p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 8, 8); }
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        setFocus(); m_focus(m_param);
        if (e->position().y() >= height() - 17) { if (routeFor(m_body, m_source(), m_param).source < 0) setRoute(m_body, m_source(), m_param, 0.25); return; }
        m_range = onRange(e->position());
        const bool onValue = e->position().y() >= m_d + 17 && e->position().y() < height() - 17;
        if (!m_range && !onValue && QLineF(e->position(), center()).length() > baseRadius() + 3) return;
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
        if (!m_dragging || !(e->buttons() & Qt::LeftButton)) {
            setCursor(onRange(e->position()) ? Qt::SizeAllCursor : Qt::SizeVerCursor); return;
        }
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
        InfoView::attach(this, mo::sourceName(source), "Click to edit this source. Drag it onto a parameter to assign modulation, then drag the knob's outer ring to set the range.");
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

// Keep the complete table catalogue searchable without flattening its musical families.
class TablePicker : public QPushButton {
public:
    TablePicker(DeviceBody* body, int osc) : QPushButton(body), m_body(body), m_osc(osc) {
        setObjectName(QString("osmiumTablePicker_%1").arg(osc));
        setAcceptDrops(true); installEventFilter(body);
        setMinimumHeight(26); setMinimumWidth(120); setCursor(Qt::PointingHandCursor);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_menu = new QMenu(this); m_menu->setObjectName(QString("osmiumTableMenu_%1").arg(osc));
        auto* content = new QWidget(m_menu); auto* layout = new QVBoxLayout(content);
        layout->setContentsMargins(8, 8, 8, 8); layout->setSpacing(6);
        m_search = new QLineEdit(content); m_search->setObjectName(QString("osmiumTableSearch_%1").arg(osc));
        m_search->setPlaceholderText("Search wavetables"); m_search->setClearButtonEnabled(true); layout->addWidget(m_search);
        m_tree = new QTreeWidget(content); m_tree->setObjectName(QString("osmiumTableTree_%1").arg(osc));
        m_tree->setHeaderHidden(true); m_tree->setIndentation(13); m_tree->setIconSize(QSize(56, 20));
        m_tree->setUniformRowHeights(true); m_tree->setMinimumSize(326, 308); m_tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        layout->addWidget(m_tree);
        std::map<QString, QTreeWidgetItem*> families;
        for (int index = 0; index < audio::wavetableCount(); ++index) {
            const QString family = QString::fromUtf8(audio::wavetableCategory(index));
            auto*& group = families[family];
            if (!group) {
                group = new QTreeWidgetItem(m_tree, QStringList{family});
                group->setFlags(group->flags() & ~Qt::ItemIsSelectable); group->setExpanded(true);
                auto font = m_tree->font(); font.setBold(true); group->setFont(0, font); group->setSizeHint(0, QSize(0, 24));
            }
            auto* item = new QTreeWidgetItem(group, QStringList{QString::fromUtf8(audio::wavetable(index).name)});
            item->setData(0, Qt::UserRole, index); item->setSizeHint(0, QSize(0, 26));
            item->setToolTip(0, family + " · " + item->text(0));
        }
        auto* action = new QWidgetAction(m_menu); action->setDefaultWidget(content); m_menu->addAction(action);
        connect(m_search, &QLineEdit::textChanged, this, [this](const QString& text) {
            for (int family = 0; family < m_tree->topLevelItemCount(); ++family) {
                auto* group = m_tree->topLevelItem(family); bool any = false;
                for (int row = 0; row < group->childCount(); ++row) {
                    auto* item = group->child(row);
                    const bool show = text.isEmpty() || item->text(0).contains(text, Qt::CaseInsensitive) || group->text(0).contains(text, Qt::CaseInsensitive);
                    item->setHidden(!show); any |= show;
                }
                group->setHidden(!any); if (!text.isEmpty()) group->setExpanded(true);
            }
        });
        const auto choose = [this](QTreeWidgetItem* item) {
            if (!item || !item->parent()) return;
            const int index = item->data(0, Qt::UserRole).toInt();
            m_body->ctx().project->beginEdit("Select Wavetable");
            if (index < 8) m_body->editMany({{mo::oparam(m_osc, mo::Table), choicePosition(index, 8)}, {osmium::tableExtension(m_osc), 0}}, 27 + m_osc);
            else m_body->edit(osmium::tableExtension(m_osc), choicePosition(index - 7, 41));
            m_body->ctx().project->endEdit(); m_menu->hide(); refresh();
        };
        connect(m_tree, &QTreeWidget::itemClicked, this, [choose](QTreeWidgetItem* item, int) { choose(item); });
        connect(m_tree, &QTreeWidget::itemActivated, this, [choose](QTreeWidgetItem* item, int) { choose(item); });
        connect(m_search, &QLineEdit::returnPressed, this, [this, choose] {
            for (int family = 0; family < m_tree->topLevelItemCount(); ++family) {
                auto* group = m_tree->topLevelItem(family);
                if (group->isHidden()) continue;
                for (int row = 0; row < group->childCount(); ++row) if (!group->child(row)->isHidden()) { choose(group->child(row)); return; }
            }
        });
        connect(this, &QPushButton::clicked, this, [this] {
            m_search->clear(); refreshIcons();
            const int index = current();
            for (int family = 0; family < m_tree->topLevelItemCount(); ++family) {
                auto* group = m_tree->topLevelItem(family);
                for (int row = 0; row < group->childCount(); ++row) if (group->child(row)->data(0, Qt::UserRole).toInt() == index) {
                    m_tree->setCurrentItem(group->child(row)); m_tree->scrollToItem(group->child(row));
                }
            }
            m_menu->popup(mapToGlobal(QPoint(0, height()))); m_search->setFocus();
        });
        InfoView::attach(this, "Wavetable", "Choose a table by family, or type to search. Each thumbnail shows a frame of the actual band-limited table. The display scans its eight frames with Position.");
        refresh();
    }
    int current() const { return osmium::tableIndex(m_body->pos(mo::oparam(m_osc, mo::Table)), m_body->pos(osmium::tableExtension(m_osc))); }
    void refresh() {
        const int index = current();
        setProperty("mapTarget", m_body->target(index >= 8 ? osmium::tableExtension(m_osc) : mo::oparam(m_osc, mo::Table)));
        setText(QString::fromUtf8(audio::wavetable(index).name));
        setToolTip(QString::fromUtf8(audio::wavetableCategory(index)) + " · " + text()); setProperty("tableIndex", index);
    }
protected:
    void paintEvent(QPaintEvent* event) override {
        QPushButton::paintEvent(event); QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(theme::textFaint, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const double x = width() - 11, y = height() / 2.0; QPolygonF arrow;
        arrow << QPointF(x - 3, y - 1) << QPointF(x, y + 2) << QPointF(x + 3, y - 1); p.drawPolyline(arrow);
    }
private:
    void refreshIcons() {
        for (int family = 0; family < m_tree->topLevelItemCount(); ++family) {
            auto* group = m_tree->topLevelItem(family); group->setForeground(0, theme::textFaint);
            for (int row = 0; row < group->childCount(); ++row) {
                auto* item = group->child(row); const auto& table = audio::wavetable(item->data(0, Qt::UserRole).toInt());
                QPixmap pixmap(56, 20); pixmap.fill(Qt::transparent); QPainter painter(&pixmap); painter.setRenderHint(QPainter::Antialiasing);
                QPainterPath path;
                for (int x = 0; x < 56; ++x) {
                    const QPointF point(x, 10 - 8 * audio::wavetableRead(table, 0, 0.45f, float(x) / 56));
                    if (x == 0) path.moveTo(point); else path.lineTo(point);
                }
                painter.setPen(QPen(theme::osmiumLfo, 1.1)); painter.drawPath(path); item->setIcon(0, QIcon(pixmap));
            }
        }
    }
    DeviceBody* m_body; int m_osc; QMenu* m_menu; QLineEdit* m_search; QTreeWidget* m_tree;
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
        p.setPen(Qt::NoPen); p.setBrush(theme::devDisplay); p.drawRoundedRect(QRectF(rect()), 2, 2);
        p.setFont(pxFont(this, 10)); draw(p, QRectF(rect()).adjusted(14, 12, -14, -16));
    }
    void grid(QPainter& p, const QRectF& r, int columns = 16) {
        for (int i = 0; i <= columns; ++i) {
            p.setPen(QPen(theme::withAlpha(theme::devGridStrong, i % 4 == 0 ? 170 : 70), 1));
            const double x = r.left() + r.width() * i / columns; p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        }
        p.setPen(QPen(theme::withAlpha(theme::devGridStrong, 110), 1));
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
    WavePlot(DeviceBody* body, int osc, int height = 172) : Plot(body, height), m_osc(osc), m_compact(height < 100) {
        setObjectName(QString("osmiumWave_%1").arg(osc)); setFocusPolicy(Qt::StrongFocus); setMouseTracking(true);
        setProperty("mapTarget", body->target(mo::oparam(osc, mo::Position))); installEventFilter(body); setAcceptDrops(true);
        setProperty("displayMode", 0);
        if (!m_compact) {
            setMinimumHeight(132); setMaximumHeight(QWIDGETSIZE_MAX); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            setCursor(Qt::SizeHorCursor);
        }
        InfoView::attach(this, "Wavetable display", "Table shows all eight frames and the selected scan position. Wave shows the warped waveform; Harm shows its harmonic content. Drag horizontally to scan Position. Shift: fine. Arrow keys and the wheel also scan; one drag is one undo step.");
    }
    ~WavePlot() override { finish(); }
protected:
    int tableIndex() const { return m_body->ctx().type == DeviceType::Osmium ? osmium::tableIndex(m_body->pos(mo::oparam(m_osc, mo::Table)), m_body->pos(osmium::tableExtension(m_osc))) : m_body->pick(mo::oparam(m_osc, mo::Table)); }
    int warpMode() const { return m_body->ctx().type == DeviceType::Osmium ? osmium::warpMode(m_body->pos(mo::oparam(m_osc, mo::Mode)), m_body->pos(osmium::warpExtension(m_osc))) : m_body->pick(mo::oparam(m_osc, mo::Mode)); }
    float sample(const audio::Wavetable& table, double position, double phase) const {
        float warped = audio::osmiumWarpPhase(m_mode, float(phase), m_intensity);
        if (m_mode == 7 && m_nextTable) {
            const double modPhase = phase * m_pitchRatio;
            const float mod = audio::wavetableRead(*m_nextTable, 0, m_nextPosition, float(modPhase - std::floor(modPhase)));
            warped = float(phase) + mod * 1.5f * m_intensity * m_intensity; warped -= std::floor(warped);
        }
        const float value = audio::wavetableRead(table, 0, float(position), std::clamp(warped, 0.0f, 0.999999f));
        return audio::osmiumFold(value, m_fold);
    }
    QPainterPath waveform(const audio::Wavetable& table, double position, const QRectF& frame) const {
        QPainterPath path; const int points = std::clamp(int(frame.width() * 1.2), 160, 520);
        for (int point = 0; point <= points; ++point) {
            const double phase = double(point) / (points + 1);
            const QPointF at(frame.left() + frame.width() * point / points, frame.center().y() - sample(table, position, phase) * frame.height() * 0.43);
            if (point == 0) path.moveTo(at); else path.lineTo(at);
        }
        return path;
    }
    void paintEvent(QPaintEvent*) override {
        m_mode = warpMode(); m_intensity = float(m_body->pos(mo::oparam(m_osc, mo::Intensity)));
        m_fold = float(m_body->pos(osmium::fold(m_osc)));
        const int next = (m_osc + 1) % mo::kOscs;
        m_nextIndex = osmium::tableIndex(m_body->pos(mo::oparam(next, mo::Table)), m_body->pos(osmium::tableExtension(next)));
        m_nextTable = &audio::wavetable(m_nextIndex); m_nextPosition = float(m_body->pos(mo::oparam(next, mo::Position)));
        m_pitchRatio = std::pow(2.0, (m_body->real(mo::oparam(next, mo::Pitch)) - m_body->real(mo::oparam(m_osc, mo::Pitch))
            + (m_body->real(mo::oparam(next, mo::Fine)) - m_body->real(mo::oparam(m_osc, mo::Fine))) / 100) / 12);
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), theme::devDisplay);
        const QRectF area = m_compact ? QRectF(rect()).adjusted(9, 6, -9, -6) : QRectF(rect()).adjusted(12, 32, -12, -25);
        draw(p, area);
        if (m_compact) return;
        p.setFont(pxFont(this, 9));
        const QStringList modes{"Table", "Wave", "Harm"};
        for (int mode = 0; mode < 3; ++mode) {
            const QRectF button(7 + mode * 41, 6, 38, 17);
            p.setPen(Qt::NoPen); p.setBrush(m_display == mode ? theme::withAlpha(theme::osmiumLfo, 30) : theme::devCell); p.drawRect(button);
            p.setPen(m_display == mode ? theme::osmiumLfo : theme::textFaint); p.drawText(button, Qt::AlignCenter, modes[mode]);
        }
        const double position = m_body->pos(mo::oparam(m_osc, mo::Position));
        p.setPen(theme::textSoft); p.drawText(QRectF(132, 6, width() - 142, 17), Qt::AlignRight | Qt::AlignVCenter,
            m_mode == 7 ? QString("OSC %1 → FM").arg((m_osc + 1) % mo::kOscs + 1) : QString("%1 / 8").arg(1 + position * 7, 0, 'f', 1));
        const QRectF scan(12, height() - 13, width() - 24, 3);
        p.setPen(QPen(theme::devGridStrong, 2)); p.drawLine(scan.topLeft(), scan.topRight());
        for (int frame = 0; frame < 8; ++frame) { const double x = scan.left() + scan.width() * frame / 7; p.setPen(theme::textFaint); p.drawLine(QPointF(x, scan.top() - 2), QPointF(x, scan.top() + 3)); }
        const double x = scan.left() + scan.width() * position;
        p.setPen(QPen(theme::osmiumLfo, 3, Qt::SolidLine, Qt::RoundCap)); p.drawLine(QPointF(scan.left(), scan.top()), QPointF(x, scan.top()));
        p.setPen(QPen(theme::osmiumLfo, 1)); p.setBrush(theme::devDisplay); p.drawEllipse(QPointF(x, scan.top()), 3.5, 3.5);
    }
    void draw(QPainter& p, const QRectF& r) override {
        const auto& table = audio::wavetable(tableIndex()); const double position = m_body->pos(mo::oparam(m_osc, mo::Position));
        p.save(); p.setClipRect(r.adjusted(-1, -1, 1, 1));
        if (m_compact || m_display == 1) {
            grid(p, r, 8);
            p.setPen(QPen(theme::withAlpha(theme::osmiumLfo, 60), 1)); p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
            const auto path = waveform(table, position, r);
            auto fill = path; fill.lineTo(r.bottomRight()); fill.lineTo(r.bottomLeft()); fill.closeSubpath();
            QLinearGradient gradient(r.topLeft(), r.bottomLeft()); gradient.setColorAt(0, theme::withAlpha(theme::osmiumLfo, 45)); gradient.setColorAt(1, theme::withAlpha(theme::osmiumLfo, 3));
            p.setBrush(gradient); p.setPen(Qt::NoPen); p.drawPath(fill);
            p.setBrush(Qt::NoBrush); p.setPen(QPen(theme::osmiumLfo, 1.7)); p.drawPath(path);
        } else if (m_display == 0) {
            const double depthX = r.width() * 0.14, depthY = r.height() * 0.35;
            const auto frameRect = [&](double depth) { return QRectF(r.left() + depthX * depth, r.top() + depthY * (1 - depth), r.width() - depthX, r.height() - depthY); };
            p.setPen(QPen(theme::withAlpha(theme::devGridStrong, 110), 1));
            for (int line = 0; line <= 4; ++line) {
                const auto front = frameRect(0), back = frameRect(1); const double t = line / 4.0;
                p.drawLine(QPointF(front.left() + front.width() * t, front.center().y()), QPointF(back.left() + back.width() * t, back.center().y()));
            }
            for (int frame = 7; frame >= 0; --frame) {
                const auto area = frameRect(frame / 7.0);
                p.setPen(QPen(theme::withAlpha(theme::osmiumLfo, frame == 0 ? 90 : 40 + (7 - frame) * 5), 1));
                p.setBrush(Qt::NoBrush); p.drawPath(waveform(table, frame / 7.0, area));
            }
            const auto active = frameRect(position); const auto path = waveform(table, position, active);
            p.setPen(QPen(theme::withAlpha(theme::devDisplay, 210), 4.5)); p.drawPath(path);
            p.setPen(QPen(theme::osmiumLfo, 2)); p.drawPath(path);
        } else {
            grid(p, r, 8);
            // A small, deterministic DFT of the displayed cycle, cached while parameters stand still.
            const std::array<double, 8> key{double(tableIndex()), position, double(m_mode), m_intensity, m_fold, double(m_nextIndex), m_nextPosition, m_pitchRatio};
            if (key != m_spectrumKey) {
                m_spectrumKey = key; std::array<double, 256> wave{};
                for (int i = 0; i < 256; ++i) wave[size_t(i)] = sample(table, position, i / 256.0);
                for (int harmonic = 1; harmonic <= 64; ++harmonic) {
                    double real = 0, imaginary = 0;
                    for (int i = 0; i < 256; ++i) { const double phase = 2 * kPi * harmonic * i / 256; real += wave[size_t(i)] * std::cos(phase); imaginary -= wave[size_t(i)] * std::sin(phase); }
                    m_harmonics[size_t(harmonic - 1)] = std::clamp((20 * std::log10(std::max(0.00001, std::hypot(real, imaginary) / 128)) + 60) / 60, 0.0, 1.0);
                }
            }
            const double bar = r.width() / 64;
            for (int harmonic = 0; harmonic < 64; ++harmonic) {
                const double height = r.height() * m_harmonics[size_t(harmonic)];
                p.fillRect(QRectF(r.left() + harmonic * bar, r.bottom() - height, std::max(1.0, bar - 1), height), theme::withAlpha(theme::osmiumLfo, harmonic < 8 ? 210 : 145));
            }
        }
        p.restore();
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (m_compact || e->button() != Qt::LeftButton) return;
        if (e->position().y() < 29) {
            const int mode = int((e->position().x() - 7) / 41);
            if (e->position().x() >= 7 && mode >= 0 && mode < 3 && e->position().x() < 130) { m_display = mode; setProperty("displayMode", mode); update(); }
            return;
        }
        setFocus(); m_press = e->position(); m_initial = m_body->pos(mo::oparam(m_osc, mo::Position)); m_drag = true;
        m_body->ctx().project->beginEdit("Scan Wavetable");
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (m_drag && (e->buttons() & Qt::LeftButton)) m_body->edit(mo::oparam(m_osc, mo::Position), m_initial + (e->position().x() - m_press.x()) / std::max(1, width() - 24) * (e->modifiers() & Qt::ShiftModifier ? 0.1 : 1));
    }
    void mouseReleaseEvent(QMouseEvent*) override { finish(); }
    void wheelEvent(QWheelEvent* e) override {
        if (m_compact) { e->ignore(); return; }
        m_body->edit(mo::oparam(m_osc, mo::Position), m_body->pos(mo::oparam(m_osc, mo::Position)) + e->angleDelta().y() / 120.0 * (e->modifiers() & Qt::ShiftModifier ? 0.001 : 0.01)); e->accept();
    }
    void keyPressEvent(QKeyEvent* e) override {
        if (e->key() == Qt::Key_Left || e->key() == Qt::Key_Right) m_body->edit(mo::oparam(m_osc, mo::Position), m_body->pos(mo::oparam(m_osc, mo::Position)) + (e->key() == Qt::Key_Right ? 1 : -1) * (e->modifiers() & Qt::ShiftModifier ? 0.001 : 0.01));
        else Plot::keyPressEvent(e);
    }
private:
    void finish() { if (m_drag) { m_drag = false; m_body->ctx().project->endEdit(); } }
    int m_osc, m_display = 0; bool m_compact, m_drag = false; QPointF m_press; double m_initial = 0;
    int m_mode = 0, m_nextIndex = 0; float m_intensity = 0, m_fold = 0, m_nextPosition = 0; double m_pitchRatio = 1;
    const audio::Wavetable* m_nextTable = nullptr;
    std::array<double, 8> m_spectrumKey{-1, -1, -1, -1, -1, -1, -1, -1}; std::array<double, 64> m_harmonics{};
};

class FilterPlot : public Plot {
public:
    FilterPlot(DeviceBody* b, int f) : Plot(b, 172), m_filter(f) {
        setObjectName(QString("osmiumFilter_%1").arg(f));
        setProperty("mapTarget", b->target(mo::fparam(f, mo::FCutoff))); installEventFilter(b); setAcceptDrops(true);
        setMinimumHeight(132); setMaximumHeight(QWIDGETSIZE_MAX); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setCursor(Qt::SizeAllCursor);
        InfoView::attach(this, "Filter", "Drag horizontally for cutoff and vertically for resonance. The accent range around Cutoff controls how far modulation opens the filter.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        drawFreqGrid(p, r, 24);
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
        curve(p, r, path, theme::accent);
        const double x = freqToX(cutoff, r);
        const double db = std::clamp(double(dsp::gainToDb(std::max(1e-5f, magnitude(cutoff)))), -24.0, 24.0);
        const QPointF handle(x, r.center().y() - db / 24 * r.height() / 2);
        p.setPen(QPen(theme::withAlpha(theme::accent, 85), 1, Qt::DashLine)); p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.setPen(QPen(theme::accent, 1.5)); p.setBrush(theme::devDisplay); p.drawEllipse(handle, 4, 4);
        p.setFont(pxFont(this, 9)); p.setPen(theme::textSoft);
        p.drawText(r.adjusted(4, 3, -4, -3), Qt::AlignLeft | Qt::AlignTop, m_body->text(mo::fparam(m_filter, mo::FCutoff)));
        p.drawText(r.adjusted(4, 3, -4, -3), Qt::AlignRight | Qt::AlignTop, QString("Q %1").arg(q, 0, 'f', 2));
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

class UnisonPlot : public Plot {
public:
    UnisonPlot(DeviceBody* body) : Plot(body, 150) {
        setObjectName("osmiumUnisonField"); setMinimumHeight(132); setMaximumHeight(QWIDGETSIZE_MAX);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding); setCursor(Qt::SizeAllCursor);
        InfoView::attach(this, "Unison field", "Each oscillator row shows its unison copies: horizontal position is stereo balance; vertical displacement is detune. Dot size follows oscillator level. Drag horizontally for Spread and vertically for Detune. Shift: fine. Pitch and fine tuning are listed at the left.");
    }
    ~UnisonPlot() override { finish(); }
protected:
    void draw(QPainter& p, const QRectF& bounds) override {
        const QRectF r = bounds.adjusted(105, 12, -8, -5);
        const int count = std::clamp(int(m_body->real(mo::Unison)), 1, 4);
        const double spread = m_body->pos(mo::Spread), detune = m_body->real(mo::Detune);
        const double offsets[4][4] = {{0, 0, 0, 0}, {-1, 1, 0, 0}, {-1, 0, 1, 0}, {-1, -0.33, 0.33, 1}};
        p.setFont(pxFont(this, 9)); p.setPen(theme::textFaint);
        p.drawText(QRectF(r.left(), bounds.top() - 3, 30, 14), Qt::AlignLeft, "L");
        p.drawText(QRectF(r.center().x() - 15, bounds.top() - 3, 30, 14), Qt::AlignCenter, "C");
        p.drawText(QRectF(r.right() - 30, bounds.top() - 3, 30, 14), Qt::AlignRight, "R");
        p.setPen(QPen(theme::devGridStrong, 1, Qt::DashLine)); p.drawLine(QPointF(r.center().x(), r.top()), QPointF(r.center().x(), r.bottom()));
        const QColor colors[] = {theme::osmiumLfo, theme::accent, theme::osmiumCurve};
        for (int osc = 0; osc < 3; ++osc) {
            const double y = r.top() + r.height() * (osc + 0.5) / 3;
            const double level = m_body->pos(mo::oparam(osc, mo::Level)); const QColor color = theme::withAlpha(colors[osc], level > 0.001 ? 230 : 65);
            p.setPen(QPen(theme::devGridStrong, 1)); p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
            p.setPen(theme::textSoft); p.drawText(QRectF(bounds.left(), y - 17, 98, 14), Qt::AlignLeft, QString("OSC %1").arg(osc + 1));
            p.setPen(theme::textFaint); p.drawText(QRectF(bounds.left(), y - 1, 98, 14), Qt::AlignLeft,
                QString("%1 · %2").arg(m_body->text(mo::oparam(osc, mo::Pitch)), m_body->text(mo::oparam(osc, mo::Fine))));
            for (int copy = 0; copy < count; ++copy) {
                const double offset = offsets[count - 1][copy];
                const QPointF point(r.center().x() + offset * spread * (r.width() / 2 - 12), y - offset * detune / 50 * std::min(22.0, r.height() / 9));
                p.setPen(QPen(theme::withAlpha(color, 80), 1)); p.drawLine(QPointF(r.center().x(), y), point);
                p.setPen(QPen(color, 1.5)); p.setBrush(theme::withAlpha(color, 35)); p.drawEllipse(point, 4 + level * 4, 4 + level * 4);
            }
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        m_press = e->position(); m_spread = m_body->pos(mo::Spread); m_detune = m_body->pos(mo::Detune);
        m_body->ctx().project->beginEdit("Shape Unison Field"); m_drag = true;
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!m_drag || !(e->buttons() & Qt::LeftButton)) return;
        const double scale = e->modifiers() & Qt::ShiftModifier ? 0.1 : 1;
        m_body->editMany({{mo::Spread, m_spread + (e->position().x() - m_press.x()) / 250 * scale}, {mo::Detune, m_detune + (m_press.y() - e->position().y()) / 150 * scale}}, 28);
    }
    void mouseReleaseEvent(QMouseEvent*) override { finish(); }
private:
    void finish() { if (m_drag) { m_drag = false; m_body->ctx().project->endEdit(); } }
    bool m_drag = false; QPointF m_press; double m_spread = 0, m_detune = 0;
};

// These displays describe the corresponding settings; the EQ uses the processor's exact shelves.
class FxPlot : public Plot {
public:
    FxPlot(DeviceBody* body, int section) : Plot(body, 150), m_section(section) {
        setObjectName(QString("osmiumFxPlot_%1").arg(section)); setMinimumHeight(100); setMaximumHeight(QWIDGETSIZE_MAX);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        const QStringList help{
            "The two traces show the left and right chorus delay modulation, in milliseconds, over one second. Rate changes their speed; Depth changes the delay excursion.",
            "Each alternating stereo tap is one synced delay repeat. Height follows Mix and the processor's 0.95-scaled Feedback; Tone darkens successive repeats.",
            "The envelope estimates the four comb lines' decay from Room Size and Mix. It describes the tail shape rather than measuring the current audio.",
            "The actual 120 Hz low shelf and 6 kHz high shelf used by the processor, plotted from 20 Hz to 20 kHz. Drag horizontally for Low EQ, vertically for High EQ."
        };
        InfoView::attach(this, QStringList{"Chorus modulation", "Ping-pong repeats", "Reverb decay", "EQ response"}[section], help[section]);
        if (section == 3) setCursor(Qt::SizeAllCursor);
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        if (m_section == 3) {
            drawFreqGrid(p, r, 24); dsp::Biquad low, high;
            low.lowShelf(120, float(m_body->real(mo::fx(mo::EqLow))), 48000); high.highShelf(6000, float(m_body->real(mo::fx(mo::EqHigh))), 48000);
            QPainterPath path;
            for (int x = 0; x <= int(r.width()); ++x) {
                const float f = float(xToFreq(r.left() + x, r));
                const double db = dsp::gainToDb(std::max(1e-5f, low.magnitude(f, 48000) * high.magnitude(f, 48000)));
                const QPointF point(r.left() + x, r.center().y() - db / 24 * r.height() * 0.5);
                if (x == 0) path.moveTo(point); else path.lineTo(point);
            }
            curve(p, r, path, theme::accent); return;
        }
        grid(p, r, 8); p.setFont(pxFont(this, 9));
        if (m_section == 0) {
            const double rate = m_body->real(mo::fx(mo::ChorusRate)), depth = m_body->pos(mo::fx(mo::ChorusDepth));
            for (int channel = 0; channel < 2; ++channel) {
                QPainterPath path;
                for (int x = 0; x <= int(r.width()); ++x) {
                    const double ms = 7 + 6 * depth * (0.5 + 0.5 * std::sin(2 * kPi * (rate * x / r.width() + channel * 0.5)));
                    const QPointF point(r.left() + x, r.bottom() - (ms - 5) / 10 * r.height());
                    if (x == 0) path.moveTo(point); else path.lineTo(point);
                }
                p.setPen(QPen(channel ? theme::accent : theme::osmiumLfo, 1.7)); p.drawPath(path);
            }
            p.setPen(theme::textFaint); p.drawText(r.adjusted(3, 2, -3, -2), Qt::AlignLeft | Qt::AlignTop, "15 ms");
            p.drawText(r.adjusted(3, 2, -3, -2), Qt::AlignRight | Qt::AlignBottom, "1 s");
        } else if (m_section == 1) {
            const double feedback = m_body->pos(mo::fx(mo::DelayFeedback)) * 0.95, mix = m_body->pos(mo::fx(mo::DelayMix));
            const double time = m_body->real(mo::fx(mo::DelayTime)), tone = m_body->pos(mo::fx(mo::DelayTone));
            const double span = std::max(4.0, time * 5), spacing = r.width() * time / span;
            for (int tap = 0; tap * time < span; ++tap) {
                const double x = r.left() + (tap + 0.5) * spacing;
                const double level = std::pow(feedback, tap) * mix, height = r.height() * level * 0.8;
                const QColor color = theme::withAlpha(tap % 2 ? theme::accent : theme::osmiumLfo, int(std::max(40.0, 220 - tap * (20 - 14 * tone))));
                p.setPen(QPen(color, std::clamp(spacing * 0.25, 3.0, 9.0), Qt::SolidLine, Qt::RoundCap)); p.drawLine(QPointF(x, r.bottom() - 2), QPointF(x, r.bottom() - std::max(2.0, height)));
                if (spacing >= 18) { p.setPen(theme::textFaint); p.drawText(QRectF(x - 10, r.top() + 2, 20, 13), Qt::AlignCenter, tap % 2 ? "R" : "L"); }
            }
            p.setPen(theme::textFaint); p.drawText(r.adjusted(3, 2, -3, -2), Qt::AlignRight | Qt::AlignBottom, QString("%1 beats").arg(span, 0, 'g', 2));
        } else {
            const double size = m_body->pos(mo::fx(mo::ReverbSize)), mix = m_body->pos(mo::fx(mo::ReverbMix));
            const double feedback = 0.7 + 0.22 * size, period = 0.0379 * (0.6 + 1.2 * size);
            QPainterPath path; path.moveTo(r.bottomLeft());
            for (int x = 0; x <= int(r.width()); ++x) {
                const double time = 3.0 * x / r.width(), amplitude = mix * std::pow(feedback, time / period);
                path.lineTo(r.left() + x, r.bottom() - r.height() * amplitude * (0.72 + 0.18 * std::sin(x * 1.73) + 0.1 * std::cos(x * 3.13)));
            }
            curve(p, r, path, theme::osmiumCurve);
            p.setPen(theme::textFaint); p.drawText(r.adjusted(3, 2, -3, -2), Qt::AlignRight | Qt::AlignBottom, "3 s");
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (m_section != 3 || e->button() != Qt::LeftButton) return;
        m_press = e->position(); m_low = m_body->pos(mo::fx(mo::EqLow)); m_high = m_body->pos(mo::fx(mo::EqHigh));
        m_body->ctx().project->beginEdit("Shape Oscillator EQ"); m_drag = true;
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (m_drag && (e->buttons() & Qt::LeftButton)) m_body->editMany({{mo::fx(mo::EqLow), m_low + (e->position().x() - m_press.x()) / 180}, {mo::fx(mo::EqHigh), m_high + (m_press.y() - e->position().y()) / 180}}, 29);
    }
    void mouseReleaseEvent(QMouseEvent*) override { if (m_drag) { m_drag = false; m_body->ctx().project->endEdit(); } }
private:
    int m_section; bool m_drag = false; QPointF m_press; double m_low = 0, m_high = 0;
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
            p.setPen(QPen(theme::osmiumEnv, 1)); p.setBrush(theme::devDisplay);
            QPolygonF diamond; diamond << mid + QPointF(0, -4) << mid + QPointF(4, 0) << mid + QPointF(0, 4) << mid + QPointF(-4, 0); p.drawPolygon(diamond);
        }
        for (int k = 1; k <= 3; ++k) { p.setPen(QPen(theme::osmiumEnv, 2)); p.setBrush(theme::devDisplay); p.drawEllipse(n[k], 4, 4); }
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

// Numeric node edits share the same gesture lifetime as the graph.
class CurveValue : public DragValue {
public:
    CurveValue(Project* project, QWidget* parent) : DragValue(parent), m_project(project) {}
    ~CurveValue() override { finish(); }
protected:
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && isEnabled()) { m_project->beginEdit("Edit Curve Node"); m_gesture = true; }
        DragValue::mousePressEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent* e) override { DragValue::mouseReleaseEvent(e); finish(); }
private:
    void finish() { if (m_gesture) { m_gesture = false; m_project->endEdit(); } }
    Project* m_project; bool m_gesture = false;
};

class CurvePlot : public Plot {
public:
    CurvePlot(DeviceBody* b, int c) : Plot(b, 200), m_curve(c) {
        setMinimumHeight(180); setMaximumHeight(QWIDGETSIZE_MAX); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setObjectName(QString("osmiumCurve_%1").arg(c)); setFocusPolicy(Qt::StrongFocus); setMouseTracking(true);
        InfoView::attach(this, "Curve editor", "Double-click to add or remove a node. Drag nodes or segment diamonds; Alt bypasses snap. "
                         "Shift-click or drag a box to select several nodes. Ctrl-drag locks an axis. Arrows nudge, Shift makes fine nudges; Tab selects the next node. "
                         "Delete removes interior nodes. Draw follows the X grid, up to 256 intervals. Zoom reveals dense details.");
    }
    ~CurvePlot() override { finish(); }
    std::function<void()> selectionChanged;
    Points points() const { const auto* d = m_body->device(); return d && m_curve < d->curves.size() ? d->curves[m_curve] : Points{}; }
    int selected() const { return m_selected >= 0 && m_selected < points().size() && m_selection.count(m_selected) ? m_selected : -1; }
    int segment() const { return m_segment >= 0 && m_segment + 1 < points().size() ? m_segment : -1; }
    int selectionCount() const { int n = 0; for (int k : m_selection) if (k < points().size()) ++n; return n; }
    void fill(const QString& label, Points p) {
        if (p.size() > osmium::kCurvePoints) p.resize(osmium::kCurvePoints);
        editCurve(m_body, m_curve, label, [p](Points& pts) { pts = p; });
        m_selection.clear(); m_selected = -1; m_segment = 0; notify();
    }
    void preset(int shape) {
        Points p;
        if (shape == 0) p = {{0, 0}, {0.5, 1}, {1, 0}};
        if (shape == 1) p = {{0, 0.5, 0, 1}, {0.25, 1, 0, 1}, {0.75, 0, 0, 1}, {1, 0.5}};
        if (shape == 2) p = {{0, 0, -0.4}, {1, 1}};
        if (shape == 3) p = {{0, 1, -0.35}, {1, 0}};
        if (shape == 4) p = {{0, 1, 0, 2}, {0.25, 0.25, 0, 2}, {0.5, 0.75, 0, 2}, {0.75, 0.4, 0, 2}, {1, 1}};
        if (shape == 5) for (int i = 0; i <= 8; ++i) p.push_back({i / 8.0, QRandomGenerator::global()->bounded(1.0), 0, 2});
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
            if (std::any_of(old.begin(), old.end(), [](const CurvePoint& q) { return q.shape == 2; })) {
                p.clear(); for (int i = 0; i < osmium::kCurvePoints; ++i) { const double x = double(i) / (osmium::kCurvePoints - 1); p.push_back({x, modcurve::value(old.constData(), int(old.size()), 1 - x)}); }
            }
        }
        fill(what == 0 ? "Invert Curve" : "Reverse Curve", p);
    }
    void setGrid(int steps) { m_grid = steps; update(); }
    void setYGrid(int steps) { m_yGrid = steps; update(); }
    void setDraw(bool draw) { m_draw = draw; setCursor(draw ? Qt::CrossCursor : Qt::ArrowCursor); }
    void setView(int zoom, int scroll) { m_zoom = zoom; m_start = double(scroll) / (1000 * zoom); update(); }
    void setShape(int shape) {
        const int s = segment();
        editCurve(m_body, m_curve, "Segment Shape", [=](Points& p) { if (s >= 0) p[s].shape = shape; }); notify();
    }
    void setCoordinate(bool time, double value) {
        const int k = selected(); if (k < 0 || selectionCount() != 1) return;
        editCurve(m_body, m_curve, "Edit Curve Node", [=](Points& p) {
            if (time) { if (k > 0 && k + 1 < p.size()) p[k].x = std::clamp(value, p[k - 1].x + gap(p, k), p[k + 1].x - gap(p, k)); }
            else p[k].y = std::clamp(value, 0.0, 1.0);
        }); notify();
    }
    void setBend(double value) {
        const int s = segment();
        editCurve(m_body, m_curve, "Bend Curve", [=](Points& p) { if (s >= 0) { p[s].bend = value; if (p[s].shape == 2) p[s].shape = 0; } }); notify();
    }
    static Points& clipboard() { static Points p; return p; }
protected:
    static double gap(const Points& p, int k) { return std::min(0.00001, std::max(0.0, p[k + 1].x - p[k - 1].x) * 0.25); }
    void notify() { update(); if (selectionChanged) selectionChanged(); }
    void finish() { if (m_gesture) { m_gesture = false; m_body->ctx().project->endEdit(); } }
    QPointF at(const CurvePoint& pt) const { const auto r = graph(); return {r.left() + (pt.x - m_start) * m_zoom * r.width(), r.bottom() - pt.y * r.height()}; }
    double timeAt(double px) const { const auto r = graph(); return std::clamp(m_start + (px - r.left()) / (m_zoom * r.width()), 0.0, 1.0); }
    double levelAt(double py) const { const auto r = graph(); return std::clamp((r.bottom() - py) / r.height(), 0.0, 1.0); }
    double snapX(double x, Qt::KeyboardModifiers mods) const { return m_grid > 0 && !(mods & Qt::AltModifier) ? std::round(x * m_grid) / m_grid : x; }
    double snapY(double y, Qt::KeyboardModifiers mods) const { return m_yGrid > 0 && !(mods & Qt::AltModifier) ? std::round(y * m_yGrid) / m_yGrid : y; }
    QPointF midpoint(const Points& p, int i) const { return at({(p[i].x + p[i + 1].x) * 0.5, modcurve::segment(p[i], p[i + 1], 0.5)}); }
    int pointAt(QPointF pt) const {
        const auto p = points(); int best = -1; double dist = 10;
        for (int i = 0; i < p.size(); ++i) { const auto q = at(p[i]); if (!graph().adjusted(-5, -5, 5, 5).contains(q)) continue;
            const double d = QLineF(pt, q).length(); if (d < dist) { best = i; dist = d; } }
        return best;
    }
    int segmentAt(QPointF pt) const {
        const auto p = points(); const double x = timeAt(pt.x());
        for (int i = 0; i + 1 < p.size(); ++i) if (x >= p[i].x && x <= p[i + 1].x) return i;
        return -1;
    }
    void draw(QPainter& painter, const QRectF& r) override {
        const int columns = m_grid > 0 ? m_grid : 32, rows = m_yGrid > 0 ? m_yGrid : 8;
        for (int i = 0; i <= columns; ++i) {
            const double x = at({double(i) / columns, 0}).x(); if (x < r.left() || x > r.right()) continue;
            const int stride = std::max(1, int(std::ceil(5.0 * columns / (r.width() * m_zoom))));
            if (i % stride != 0 && i % std::max(1, columns / 4) != 0) continue;
            painter.setPen(QPen(theme::withAlpha(theme::devGridStrong, i % std::max(1, columns / 4) == 0 ? 190 : 65), 1)); painter.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        }
        for (int i = 0; i <= rows; ++i) { const double y = r.bottom() - r.height() * i / rows;
            painter.setPen(QPen(theme::withAlpha(theme::devGridStrong, i % std::max(1, rows / 4) == 0 ? 150 : 65), 1)); painter.drawLine(QPointF(r.left(), y), QPointF(r.right(), y)); }
        const auto p = points(); if (p.isEmpty()) return;
        QPainterPath path;
        for (int i = 0; i <= int(r.width()); ++i) { const double x = timeAt(r.left() + i); const auto pt = at({x, modcurve::value(p.constData(), int(p.size()), x)});
            if (i == 0) path.moveTo(pt); else path.lineTo(pt); }
        painter.save(); painter.setClipRect(r.adjusted(-6, -6, 6, 6)); curve(painter, r, path, theme::osmiumCurve);
        for (int i = 0; i + 1 < p.size(); ++i) {
            if ((p[i + 1].x - p[i].x) * r.width() * m_zoom < 28 && i != m_segment) continue;
            const auto mid = midpoint(p, i);
            painter.setPen(QPen(theme::withAlpha(theme::osmiumCurve, i == m_segment ? 255 : 130), 1)); painter.setBrush(theme::devDisplay);
            QPolygonF diamond; diamond << mid + QPointF(0, -5) << mid + QPointF(5, 0) << mid + QPointF(0, 5) << mid + QPointF(-5, 0); painter.drawPolygon(diamond);
        }
        for (int i = 0; i < p.size(); ++i) {
            const bool selected = m_selection.count(i); const double radius = selected ? 5 : p.size() > 65 ? 2.5 : 4;
            painter.setBrush(selected ? theme::osmiumCurve : theme::devDisplay); painter.setPen(QPen(theme::osmiumCurve, selected ? 2 : 1.4)); painter.drawEllipse(at(p[i]), radius, radius);
        }
        const double phase = m_body->scope(14 + m_curve); const double x = at({phase, 0}).x();
        painter.setPen(QPen(theme::withAlpha(theme::textBright, 100), 1)); painter.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        if (m_box) { painter.setPen(QPen(theme::accent, 1)); painter.setBrush(theme::withAlpha(theme::accent, 25)); painter.drawRect(QRectF(m_press, m_boxEnd).normalized()); }
        painter.restore(); painter.setPen(theme::textFaint);
        for (int i = 0; i <= 4; ++i) { const double t = m_start + double(i) / (4 * m_zoom);
            painter.drawText(QRectF(r.left() + r.width() * i / 4 - 22, r.bottom() + 2, 44, 13), Qt::AlignCenter, QString::number(t * 100, 'f', m_zoom > 1 ? 1 : 0) + "%"); }
    }
    void paintAt(QPointF pt, Qt::KeyboardModifiers mods) {
        const int steps = m_grid > 0 ? m_grid : 256;
        const int index = std::clamp(int(std::round(timeAt(pt.x()) * steps)), 0, steps);
        const double y = snapY(levelAt(pt.y()), mods);
        editCurve(m_body, m_curve, "Draw Curve", [=](Points& p) {
            if (p.size() != steps + 1 || !uniform(p, steps)) { const auto old = p; p.clear(); for (int i = 0; i <= steps; ++i) p.push_back({double(i) / steps, modcurve::value(old.constData(), int(old.size()), double(i) / steps)}); }
            if (m_lastPaint >= 0 && index != m_lastPaint) for (int i = std::min(index, m_lastPaint); i <= std::max(index, m_lastPaint); ++i) {
                const double t = double(i - m_lastPaint) / (index - m_lastPaint); p[i].y = m_lastY + (y - m_lastY) * t; p[i].bend = 0; p[i].shape = 0;
            }
            p[index].y = y; p[index].bend = 0; p[index].shape = 0;
        });
        m_lastPaint = index; m_lastY = y; notify();
    }
    static bool uniform(const Points& p, int steps) { for (int i = 0; i < p.size(); ++i) if (std::abs(p[i].x - double(i) / steps) > 1e-8) return false; return true; }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        setFocus(); m_press = e->position(); const int hit = pointAt(m_press); m_segment = segmentAt(m_press); m_bending = false;
        const auto p = points();
        if (m_draw) { m_selection.clear(); m_selected = -1; }
        else if (hit >= 0) {
            if (e->modifiers() & Qt::ShiftModifier) { if (m_selection.count(hit)) m_selection.erase(hit); else m_selection.insert(hit); }
            else if (!m_selection.count(hit)) { m_selection.clear(); m_selection.insert(hit); }
            m_selected = m_selection.count(hit) ? hit : m_selection.empty() ? -1 : *m_selection.begin();
            m_segment = std::min(hit, int(p.size()) - 2);
        } else {
            if (m_segment >= 0 && m_segment + 1 < p.size()) {
                m_bending = QLineF(m_press, midpoint(p, m_segment)).length() < 12 || (e->modifiers() & Qt::AltModifier);
                m_initial = p[m_segment].bend;
            }
            if (!m_bending) { m_box = true; m_boxEnd = m_press; m_beforeBox = (e->modifiers() & Qt::ShiftModifier) ? m_selection : std::set<int>{}; }
        }
        if (!m_box && (m_draw || m_bending || (hit >= 0 && m_selection.count(hit)))) { m_body->ctx().project->beginEdit("Edit Curve"); m_gesture = true; m_startPoints = p; }
        if (m_draw) { m_lastPaint = -1; paintAt(m_press, e->modifiers()); }
        notify();
    }
    void moveSelection(const Points& original, double dx, double dy) {
        double minX = -1, maxX = 1, minY = -1, maxY = 1;
        for (int k : m_selection) if (k >= 0 && k < original.size()) {
            minY = std::max(minY, -original[k].y); maxY = std::min(maxY, 1 - original[k].y);
            if (k > 0 && k + 1 < original.size()) {
                if (k == 1 || !m_selection.count(k - 1)) minX = std::max(minX, original[k - 1].x + gap(original, k) - original[k].x);
                if (k + 2 == original.size() || !m_selection.count(k + 1)) maxX = std::min(maxX, original[k + 1].x - gap(original, k) - original[k].x);
            }
        }
        dx = std::clamp(dx, minX, maxX); dy = std::clamp(dy, minY, maxY);
        editCurve(m_body, m_curve, "Move Curve Nodes", [=](Points& p) { if (p.size() != original.size()) return;
            for (int k : m_selection) if (k >= 0 && k < p.size()) { if (k > 0 && k + 1 < p.size()) p[k].x = original[k].x + dx; p[k].y = original[k].y + dy; }
        }); notify();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton)) return;
        if (m_box) {
            m_boxEnd = e->position(); const auto bounds = QRectF(m_press, m_boxEnd).normalized(); const auto p = points(); m_selection = m_beforeBox;
            for (int k = 0; k < p.size(); ++k) if (bounds.contains(at(p[k]))) m_selection.insert(k);
            m_selected = m_selection.empty() ? -1 : *m_selection.begin(); notify(); return;
        }
        if (!m_gesture) return;
        if (m_draw) { paintAt(e->position(), e->modifiers()); return; }
        if (m_bending) { const auto p = points(); if (m_segment < 0 || m_segment + 1 >= p.size()) return;
            setBend(std::clamp(m_initial + (e->position().y() - m_press.y()) / 90 * (p[m_segment + 1].y >= p[m_segment].y ? 1 : -1), -1.0, 1.0)); return; }
        const int k = selected(); if (k < 0 || k >= m_startPoints.size()) return;
        const auto r = graph();
        const double targetX = m_startPoints[k].x + (e->position().x() - m_press.x()) / (r.width() * m_zoom);
        const double targetY = m_startPoints[k].y - (e->position().y() - m_press.y()) / r.height();
        double dx = snapX(targetX, e->modifiers()) - m_startPoints[k].x;
        double dy = snapY(targetY, e->modifiers()) - m_startPoints[k].y;
        if (e->modifiers() & Qt::ControlModifier) { if (std::abs(e->position().x() - m_press.x()) > std::abs(e->position().y() - m_press.y())) dy = 0; else dx = 0; }
        moveSelection(m_startPoints, dx, dy);
    }
    void mouseReleaseEvent(QMouseEvent*) override { finish(); m_box = false; notify(); }
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton || m_draw) return;
        finish(); m_box = false;
        const int k = pointAt(e->position());
        if (k >= 0) { m_selected = k; m_selection = {k}; removePoint(); return; }
        const double x = snapX(timeAt(e->position().x()), e->modifiers()), y = snapY(levelAt(e->position().y()), e->modifiers());
        int added = -1;
        editCurve(m_body, m_curve, "Add Curve Point", [&](Points& p) {
            if (p.size() >= osmium::kCurvePoints) return;
            for (const auto& pt : p) if (std::abs(pt.x - x) < 0.00001) return;
            auto it = std::upper_bound(p.begin(), p.end(), x, [](double x, const CurvePoint& q) { return x < q.x; });
            added = int(it - p.begin()); p.insert(it, {x, y});
        });
        if (added >= 0) { m_selected = added; m_selection = {added}; m_segment = added; } notify();
    }
    void removePoint() {
        editCurve(m_body, m_curve, "Delete Curve Nodes", [this](Points& p) { for (auto it = m_selection.rbegin(); it != m_selection.rend(); ++it) if (*it > 0 && *it + 1 < p.size()) p.removeAt(*it); });
        m_selection.clear(); m_selected = -1; notify();
    }
    void keyPressEvent(QKeyEvent* e) override {
        if (e->matches(QKeySequence::SelectAll)) { m_selection.clear(); for (int i = 0; i < points().size(); ++i) m_selection.insert(i); m_selected = 0; notify(); return; }
        if (e->key() == Qt::Key_Escape) { m_selection.clear(); m_selected = -1; notify(); return; }
        if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) { removePoint(); return; }
        if (e->key() == Qt::Key_Left || e->key() == Qt::Key_Right || e->key() == Qt::Key_Up || e->key() == Qt::Key_Down) {
            const bool fine = e->modifiers() & Qt::ShiftModifier;
            const double dx = fine ? 1.0 / 4096 : 1.0 / (m_grid > 0 ? m_grid : 256), dy = fine ? 0.001 : m_yGrid > 0 ? 1.0 / m_yGrid : 0.01;
            if (!m_selection.empty()) moveSelection(points(), e->key() == Qt::Key_Right ? dx : e->key() == Qt::Key_Left ? -dx : 0, e->key() == Qt::Key_Up ? dy : e->key() == Qt::Key_Down ? -dy : 0);
            return;
        }
        Plot::keyPressEvent(e);
    }
    bool focusNextPrevChild(bool next) override {
        const int n = int(points().size()); if (n == 0 || selected() < 0) return Plot::focusNextPrevChild(next);
        m_selected = (m_selected + (next ? 1 : n - 1) + n) % n; m_selection = {m_selected}; m_segment = std::min(m_selected, n - 2); notify(); return true;
    }
    void contextMenuEvent(QContextMenuEvent* e) override {
        m_segment = segmentAt(e->pos()); const int k = pointAt(e->pos()); if (k >= 0 && !m_selection.count(k)) { m_selected = k; m_selection = {k}; } notify();
        QMenu menu(this);
        for (int shape = 0; shape < 3; ++shape) menu.addAction(QStringList{"Curve", "Smooth", "Hold"}[shape], this, [this, shape] { setShape(shape); });
        menu.addAction("Straighten segment", this, [this] { setBend(0); });
        menu.addSeparator(); menu.addAction("Delete nodes", this, [this] { removePoint(); })->setEnabled(selectionCount() > 0);
        menu.exec(e->globalPos());
    }
private:
    int m_curve, m_selected = -1, m_segment = 0, m_grid = 32, m_yGrid = 0, m_lastPaint = -1, m_zoom = 1;
    bool m_bending = false, m_gesture = false, m_draw = false, m_box = false;
    std::set<int> m_selection, m_beforeBox; Points m_startPoints;
    QPointF m_press, m_boxEnd; double m_initial = 0, m_lastY = 0, m_start = 0;
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
            p.fillRect(QRectF(r.left() + i * w + 2, y > 0 ? r.center().y() - y : r.center().y(), w - 4, std::max(1.0, std::abs(y))), i < count ? theme::accent : theme::devGridStrong);
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
        InfoView::attach(this, "Modulation range", "The accent interval is this source's reachable range on the selected parameter. The white line is the base value. Bipolar sources extend both sides of the base.");
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        const int param = m_param(); const auto route = routeFor(m_body, m_source(), param); const double base = m_body->pos(param);
        const double a = std::clamp(base + route.amount, 0.0, 1.0), b = mo::sourceBipolar(m_source()) ? std::clamp(base - route.amount, 0.0, 1.0) : base;
        const double lo = std::min(a, b), hi = std::max(a, b), y = r.top() + 6;
        p.setPen(QPen(theme::devGridStrong, 5, Qt::SolidLine, Qt::RoundCap)); p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        p.setPen(QPen(theme::accent, 5, Qt::SolidLine, Qt::RoundCap)); p.drawLine(QPointF(r.left() + r.width() * lo, y), QPointF(r.left() + r.width() * hi, y));
        p.setPen(QPen(theme::textBright, 2)); p.drawLine(QPointF(r.left() + base * r.width(), y - 6), QPointF(r.left() + base * r.width(), y + 6));
        p.setFont(pxFont(this, 10)); p.setPen(theme::textSoft);
        p.drawText(QRectF(r.left(), y + 12, r.width() / 2, 16), Qt::AlignLeft, paramText(m_body->ctx().type, param, lo));
        p.drawText(QRectF(r.center().x(), y + 12, r.width() / 2, 16), Qt::AlignRight, paramText(m_body->ctx().type, param, hi));
    }
private: std::function<int()> m_source, m_param;
};

// Give each oscillator a useful drawing area; wrap complete cards before controls get cramped.
class SoundPage : public QWidget {
public:
    SoundPage(std::vector<QWidget*> cells, QWidget* parent) : QWidget(parent), m_cells(std::move(cells)) {
        setObjectName("osmiumSoundPage");
        m_grid = new QGridLayout(this); m_grid->setContentsMargins(0, 0, 0, 0); m_grid->setSpacing(7);
        m_grid->setSizeConstraint(QLayout::SetNoConstraint); setMinimumWidth(620); reflow(2);
    }
protected:
    void resizeEvent(QResizeEvent* e) override {
        QWidget::resizeEvent(e); const int columns = width() >= 1420 ? 5 : width() >= 930 ? 3 : 2;
        if (columns != m_columns) reflow(columns);
    }
private:
    void reflow(int columns) {
        m_columns = columns; setProperty("columns", columns);
        while (auto* item = m_grid->takeAt(0)) delete item;
        for (int c = 0; c < 6; ++c) m_grid->setColumnStretch(c, 0);
        for (int r = 0; r < 3; ++r) m_grid->setRowStretch(r, 0);
        if (columns == 5) {
            for (int c = 0; c < 5; ++c) { m_grid->setColumnStretch(c, c < 3 ? 4 : 3); m_grid->addWidget(m_cells[size_t(c)], 0, c); }
            m_grid->setRowStretch(0, 1);
        } else if (columns == 3) {
            for (int c = 0; c < 6; ++c) m_grid->setColumnStretch(c, 1);
            for (int o = 0; o < 3; ++o) m_grid->addWidget(m_cells[size_t(o)], 0, o * 2, 1, 2);
            for (int f = 0; f < 2; ++f) m_grid->addWidget(m_cells[size_t(f + 3)], 1, f * 3, 1, 3);
            m_grid->setRowStretch(0, 1); m_grid->setRowStretch(1, 1);
        } else {
            m_grid->setColumnStretch(0, 1); m_grid->setColumnStretch(1, 1);
            m_grid->addWidget(m_cells[0], 0, 0); m_grid->addWidget(m_cells[1], 0, 1);
            m_grid->addWidget(m_cells[2], 1, 0, 1, 2);
            m_grid->addWidget(m_cells[3], 2, 0); m_grid->addWidget(m_cells[4], 2, 1);
            for (int r = 0; r < 3; ++r) m_grid->setRowStretch(r, 1);
        }
        setMinimumHeight(m_grid->minimumSize().height()); m_grid->activate();
    }
    std::vector<QWidget*> m_cells; QGridLayout* m_grid; int m_columns = 0;
};

// Use the pane's available width before adding another row of voice controls.
class VoiceGrid : public QWidget {
public:
    VoiceGrid(std::vector<QWidget*> controls, QWidget* parent) : QWidget(parent), m_controls(std::move(controls)) {
        m_grid = new QGridLayout(this); m_grid->setContentsMargins(4, 6, 4, 4); m_grid->setSpacing(8);
        m_grid->setSizeConstraint(QLayout::SetNoConstraint); setMinimumWidth(188); reflow(7);
    }
protected:
    void resizeEvent(QResizeEvent* e) override {
        QWidget::resizeEvent(e); const int columns = std::clamp((width() - 8) / 89, 2, 14); if (columns != m_columns) reflow(columns);
    }
private:
    void reflow(int columns) {
        m_columns = columns;
        while (auto* item = m_grid->takeAt(0)) delete item;
        for (int r = 0; r < m_grid->rowCount(); ++r) m_grid->setRowStretch(r, 0);
        for (int c = 0; c < 14; ++c) m_grid->setColumnStretch(c, c < columns ? 1 : 0);
        for (int i = 0; i < int(m_controls.size()); ++i) m_grid->addWidget(m_controls[size_t(i)], i / columns, i % columns, Qt::AlignTop | Qt::AlignHCenter);
        m_grid->setRowStretch((int(m_controls.size()) + columns - 1) / columns, 1);
        setMinimumHeight(m_grid->minimumSize().height()); m_grid->activate();
    }
    std::vector<QWidget*> m_controls; QGridLayout* m_grid; int m_columns = 0;
};

class VoiceSections : public QWidget {
public:
    VoiceSections(std::vector<QWidget*> cards, QWidget* parent) : QWidget(parent), m_cards(std::move(cards)) {
        m_grid = new QGridLayout(this); m_grid->setContentsMargins(0, 0, 0, 0); m_grid->setSpacing(7);
        m_grid->setSizeConstraint(QLayout::SetNoConstraint); setMinimumWidth(620); reflow(2);
    }
protected:
    void resizeEvent(QResizeEvent* e) override { QWidget::resizeEvent(e); const int columns = width() >= 1100 ? 3 : 2; if (columns != m_columns) reflow(columns); }
private:
    void reflow(int columns) {
        m_columns = columns;
        while (auto* item = m_grid->takeAt(0)) delete item;
        for (int column = 0; column < 3; ++column) m_grid->setColumnStretch(column, column < columns ? 1 : 0);
        for (int card = 0; card < int(m_cards.size()); ++card) {
            const bool lastFull = columns == 2 && card + 1 == int(m_cards.size()) && card % 2 == 0;
            m_grid->addWidget(m_cards[size_t(card)], card / columns, card % columns, 1, lastFull ? 2 : 1);
        }
        setMinimumHeight(m_grid->minimumSize().height()); m_grid->activate();
    }
    std::vector<QWidget*> m_cards; QGridLayout* m_grid; int m_columns = 0;
};

class EffectGrid : public QWidget {
public:
    EffectGrid(std::vector<QWidget*> cells, QWidget* parent) : QWidget(parent), m_cells(std::move(cells)) {
        setObjectName("osmiumEffectGrid"); m_grid = new QGridLayout(this);
        m_grid->setContentsMargins(0, 0, 0, 0); m_grid->setSpacing(7); m_grid->setSizeConstraint(QLayout::SetNoConstraint);
        setMinimumWidth(620); reflow(2);
    }
protected:
    void resizeEvent(QResizeEvent* e) override { QWidget::resizeEvent(e); const int columns = width() >= 1320 ? 4 : 2; if (columns != m_columns) reflow(columns); }
private:
    void reflow(int columns) {
        m_columns = columns;
        while (auto* item = m_grid->takeAt(0)) delete item;
        for (int column = 0; column < 4; ++column) m_grid->setColumnStretch(column, column < columns ? 1 : 0);
        for (int row = 0; row < 2; ++row) m_grid->setRowStretch(row, row < 4 / columns ? 1 : 0);
        for (int cell = 0; cell < 4; ++cell) m_grid->addWidget(m_cells[size_t(cell)], cell / columns, cell % columns);
        setMinimumHeight(m_grid->minimumSize().height()); m_grid->activate();
    }
    std::vector<QWidget*> m_cells; QGridLayout* m_grid; int m_columns = 0;
};

class OsmiumEditor : public DeviceBody {
public:
    OsmiumEditor(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        setObjectName("osmiumEditor"); setMinimumSize(1060, 620); setMaximumHeight(QWIDGETSIZE_MAX);
        auto* root = new QVBoxLayout(this); root->setContentsMargins(8, 6, 8, 6); root->setSpacing(5);
        auto* header = hbox(12);
        auto* title = new QLabel("OSMIUM", this); title->setObjectName("osmiumTitle"); header->addWidget(title);
        header->addSpacing(22);
        m_patches = patchPicker(this, "osmiumPatch"); m_patches->setFixedWidth(258);
        syncPatchPicker(this, m_patches);
        connect(m_patches, &QComboBox::activated, this, [this](int i) { loadPatch(this, m_patches, i); }); header->addWidget(m_patches);
        header->addStretch();
        auto* voices = new QLabel(this); header->addWidget(voices);
        m_refreshers.push_back([this, voices] { voices->setText(QString("%1 / 8 voices").arg(int(meter(0)))); });
        auto* audition = button("Audition C2", "Play a short C2 through this track's instrument and effects.", [this] {
            if (ctx().transport) ctx().transport->previewNote(ctx().project->indexOfTrack(ctx().trackId), 48, 0.8);
        });
        header->addWidget(audition); root->addLayout(header);

        std::vector<QWidget*> soundCells;
        for (int o = 0; o < 3; ++o) soundCells.push_back(oscillator(o));
        for (int f = 0; f < 2; ++f) soundCells.push_back(filter(f));
        auto* soundPage = new SoundPage(std::move(soundCells), this);

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
        for (int i = 0; i < 4; ++i) {
            auto* b = button(QStringList{"Sound", "Modulators", "Voice && mixer", "Effects"}[i], "Open this editing page.", [this, i] { setPage(i); });
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
        m_pages->addWidget(scrollPage(soundPage));
        m_pages->addWidget(m_modulators);
        m_pages->addWidget(voice()); m_pages->addWidget(effects());
        pane->addWidget(m_pages); workspace->addWidget(panel, 1);

        auto* inspector = card(); inspector->setObjectName("osmiumRangeInspector"); inspector->setFixedWidth(216); auto* ins = new QVBoxLayout(inspector); ins->setContentsMargins(10, 10, 10, 10); ins->setSpacing(6);
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
            col->addWidget(dial(mo::macro(k), QString::number(k + 1), 84)); cell->setLayout(col); macros->addWidget(cell, 1, Qt::AlignHCenter);
        }
        macros->addStretch(); macros->addWidget(dial(mo::Volume, "Output", 64)); root->addWidget(macroPanel);
        applyStyle(); connect(theme::notifier(), &theme::Notifier::changed, this, [this] { applyStyle(); });
        selectSource(mo::SrcEnv2, false); setPage(0); refresh();
        connect(c.project, &Project::restored, this, [this] { m_ctx.track = ctx().project->indexOfTrack(ctx().trackId); refresh(); });
        auto* timer = new QTimer(this); timer->setInterval(33);
        connect(timer, &QTimer::timeout, this, [this] {
            if (!isVisible()) return;
            m_ctx.track = ctx().project->indexOfTrack(ctx().trackId);
            if (!device()) { hide(); return; }
            if (ctx().transport) follow(ctx().transport->beat());
            refresh();
        }); timer->start();
    }
    void refresh() override {
        DeviceBody::refresh();
        if (!m_destination) return;
        syncPatchPicker(this, m_patches);
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
            if (revealPage) setPage(1);
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
    QFrame* card() { auto* f = new QFrame(this); f->setObjectName("osmiumCard"); f->setProperty("osmiumCard", true); return f; }
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
        auto* box = card(); box->setObjectName(QString("osmiumOscillator_%1").arg(o)); box->setProperty("osmiumCard", true);
        auto* l = new QVBoxLayout(box); l->setContentsMargins(9, 8, 9, 8); l->setSpacing(5);
        auto* top = hbox(7); auto* title = new QLabel(QString("OSC %1").arg(o + 1), box); title->setObjectName("osmiumCaption"); top->addWidget(title);
        m_used.insert(mo::oparam(o, mo::Table)); m_used.insert(osmium::tableExtension(o));
        auto* tables = new TablePicker(this, o); top->addWidget(tables, 1); m_refreshers.push_back([tables] { tables->refresh(); }); l->addLayout(top);
        l->addWidget(new WavePlot(this, o), 1);
        auto* knobs = hbox(3); knobs->addStretch();
        knobs->addWidget(dial(mo::oparam(o, mo::Position), "Position", 64));
        knobs->addWidget(dial(mo::oparam(o, mo::Intensity), "Warp", 52));
        knobs->addWidget(dial(osmium::fold(o), "Fold", 52));
        knobs->addWidget(dial(mo::oparam(o, mo::Level), "Level", 52)); knobs->addStretch(); l->addLayout(knobs);
        auto* bottom = hbox(5);
        m_used.insert(mo::oparam(o, mo::Mode)); m_used.insert(osmium::warpExtension(o));
        auto* mode = new ChoiceBox(this); mode->setObjectName(QString("osmiumWarpMode_%1").arg(o));
        mode->setAcceptDrops(true); mode->installEventFilter(this);
        mode->addItems({"Bend", "Formant", "Sync", "Phase", "Fold", "Asym", "Steps", "Cross FM"}); mode->setMinimumWidth(82); bottom->addWidget(mode, 1);
        connect(mode, &QComboBox::activated, this, [this, o](int index) {
            ctx().project->beginEdit("Select Oscillator Warp");
            if (index < 3) editMany({{mo::oparam(o, mo::Mode), choicePosition(index, 3)}, {osmium::warpExtension(o), 0}}, 30 + o);
            else edit(osmium::warpExtension(o), choicePosition(index - 2, 6));
            ctx().project->endEdit();
        });
        m_refreshers.push_back([this, o, mode] {
            const int index = osmium::warpMode(pos(mo::oparam(o, mo::Mode)), pos(osmium::warpExtension(o)));
            mode->setCurrentIndex(index); mode->setProperty("mapTarget", target(index >= 3 ? osmium::warpExtension(o) : mo::oparam(o, mo::Mode)));
        });
        InfoView::attach(mode, "Oscillator warp", "Bend, Formant and Sync reshape phase. Phase adds phase distortion; Fold, Asym and Steps reshape the cycle. Cross FM takes its phase modulation from the next oscillator, independently of that oscillator's level. Warp sets the depth; Fold adds a separate wavefold stage.");
        auto addField = [this, bottom, box](int param, const QString& label, int width) {
            bottom->addWidget(new QLabel(label, box)); m_used.insert(param);
            auto* value = field(param, width); value->setAcceptDrops(true); value->installEventFilter(this); bottom->addWidget(value);
        };
        addField(mo::oparam(o, mo::Pitch), "Pitch", 42); addField(mo::oparam(o, mo::Fine), "Fine", 44);
        l->addLayout(bottom); return box;
    }
    QWidget* filter(int f) {
        auto* box = card(); box->setObjectName(QString("osmiumFilterCard_%1").arg(f)); box->setProperty("osmiumCard", true);
        auto* l = new QVBoxLayout(box); l->setContentsMargins(9, 8, 9, 8); l->setSpacing(5);
        auto* top = hbox(6); auto* title = new QLabel(QString("FILTER %1").arg(f + 1), box); title->setObjectName("osmiumCaption"); top->addWidget(title);
        top->addWidget(choice(mo::fparam(f, mo::FType)), 1); l->addLayout(top);
        l->addWidget(new FilterPlot(this, f), 1);
        auto* knobs = hbox(2); knobs->addStretch();
        knobs->addWidget(dial(mo::fparam(f, mo::FCutoff), "Cutoff", 64)); knobs->addWidget(dial(mo::fparam(f, mo::FReso), "Resonance", 52));
        knobs->addWidget(dial(f == 0 ? mo::FDrive : mo::FMix, f == 0 ? "Drive" : "Mix", 52)); knobs->addStretch(); l->addLayout(knobs);
        auto* footer = hbox(5);
        const int footerParam = f == 0 ? mo::FRouting : mo::Feedback;
        footer->addWidget(new QLabel(f == 0 ? "Routing" : "Feedback", box));
        auto* value = field(footerParam, 50); value->setAcceptDrops(true); value->installEventFilter(this); footer->addWidget(value); m_used.insert(footerParam);
        footer->addStretch(); l->addLayout(footer); return box;
    }
    void expandPlot(Plot* plot, int height) {
        plot->setMinimumHeight(height); plot->setMaximumHeight(QWIDGETSIZE_MAX); plot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    QWidget* envelope(int e) {
        auto* w = new QWidget(this); auto* l = vbox(4); w->setLayout(l); auto* row = hbox(6);
        row->addWidget(labelledChoice(mo::eloop(e), "Loop", 66));
        const int params[] = {mo::A, mo::D, mo::S, mo::R}; const QStringList names{"Attack", "Decay", "Sustain", "Release"};
        for (int k = 0; k < 4; ++k) row->addWidget(dial(mo::eparam(e, mo::EnvKnob(params[k])), names[k], 40));
        for (int s = 0; s < 3; ++s) row->addWidget(dial(osmium::bend(e, s), QStringList{"A bend", "D bend", "R bend"}[s], 36));
        row->addWidget(dial(mo::ecurve(e), "Level curve", 36)); row->addStretch();
        auto* plot = new EnvelopePlot(this, e); expandPlot(plot, 166); l->addWidget(plot, 1); l->addLayout(row); return w;
    }
    QWidget* lfo(int n) {
        auto* w = new QWidget(this); auto* l = vbox(5); w->setLayout(l); auto* plot = new LfoPlot(this, n); expandPlot(plot, 166); l->addWidget(plot, 1); auto* row = hbox(12);
        row->addWidget(labelledChoice(mo::lparam(n, mo::LRate), "Rate", 90)); row->addWidget(labelledChoice(mo::lparam(n, mo::LShape), "Shape", 104));
        row->addWidget(dial(mo::lparam(n, mo::LHz), "Free Hz", 32)); row->addWidget(dial(mo::lfade(n), "Fade in", 32));
        row->addWidget(labelledChoice(mo::lretrig(n), "Retrigger", 76)); row->addWidget(labelledChoice(mo::lmode(n), "Mode", 104)); row->addStretch(); l->addLayout(row); return w;
    }
    QWidget* curvePage(int c) {
        auto* w = new QWidget(this); auto* l = vbox(5); w->setLayout(l);
        auto* plot = new CurvePlot(this, c); auto* toolbar = hbox(4);
        auto* shapes = new ChoiceBox(w); shapes->setObjectName(QString("osmiumCurveShape_%1").arg(c)); shapes->addItems({"Triangle", "Sine", "Rise", "Fall", "Steps", "Random"}); shapes->setCurrentIndex(-1); shapes->setPlaceholderText("Shape"); shapes->setFixedWidth(86);
        InfoView::attach(shapes, "Curve shape", "Replace the curve with an editable shape. Every segment can then be bent, smoothed or held.");
        connect(shapes, &QComboBox::activated, this, [plot, shapes](int i) { plot->preset(i); shapes->setCurrentIndex(-1); }); toolbar->addWidget(shapes);
        auto* draw = button("Draw", "Paint at the X grid resolution. Free uses 256 intervals. One stroke is one undo step.", [] {});
        draw->setObjectName(QString("osmiumCurveDraw_%1").arg(c)); draw->setCheckable(true); connect(draw, &QPushButton::toggled, plot, [plot](bool on) { plot->setDraw(on); }); toolbar->addWidget(draw);
        auto* snap = new ChoiceBox(w); snap->setObjectName(QString("osmiumCurveGrid_%1").arg(c)); snap->setFixedWidth(87);
        for (int n : {4, 8, 16, 32, 64, 128, 256}) snap->addItem(QString("X: 1/%1").arg(n), n);
        snap->addItem("X: Free", 0); snap->setCurrentIndex(3);
        connect(snap, &QComboBox::activated, plot, [plot, snap](int i) { plot->setGrid(snap->itemData(i).toInt()); });
        InfoView::attach(snap, "Time grid", "Time snap and Draw resolution, up to 256 intervals. Alt bypasses node snapping."); toolbar->addWidget(snap);
        auto* ySnap = new ChoiceBox(w); ySnap->setObjectName(QString("osmiumCurveYGrid_%1").arg(c)); ySnap->setFixedWidth(81); ySnap->addItem("Y: Free", 0);
        for (int n : {4, 8, 16, 32, 64}) ySnap->addItem(QString("Y: 1/%1").arg(n), n);
        connect(ySnap, &QComboBox::activated, plot, [plot, ySnap](int i) { plot->setYGrid(ySnap->itemData(i).toInt()); });
        InfoView::attach(ySnap, "Level grid", "Optional level snap for nodes and drawing. Alt bypasses it."); toolbar->addWidget(ySnap);
        auto* zoom = new ChoiceBox(w); zoom->setObjectName(QString("osmiumCurveZoom_%1").arg(c)); zoom->setFixedWidth(66);
        for (int n : {1, 2, 4, 8}) zoom->addItem(QString("%1×").arg(n), n);
        InfoView::attach(zoom, "Zoom", "Magnify time, then use the scrollbar to reach the rest of the curve. Saved timing stays unchanged."); toolbar->addWidget(zoom);
        toolbar->addWidget(button("Invert", "Flip the curve vertically.", [plot] { plot->transform(0); })); toolbar->addWidget(button("Reverse", "Reverse the curve in time.", [plot] { plot->transform(1); }));
        toolbar->addWidget(button("Copy", "Copy the curve including shapes and bends.", [plot] { CurvePlot::clipboard() = plot->points(); }));
        toolbar->addWidget(button("Paste", "Paste the copied curve as one undo step.", [plot] { if (!CurvePlot::clipboard().isEmpty()) plot->fill("Paste Curve", CurvePlot::clipboard()); })); toolbar->addStretch();
        l->addLayout(toolbar); l->addWidget(plot, 1);
        auto* scroll = new QScrollBar(Qt::Horizontal, w); scroll->setObjectName(QString("osmiumCurveScroll_%1").arg(c)); scroll->setRange(0, 0); scroll->setPageStep(1000); scroll->setFixedHeight(12); l->addWidget(scroll);
        connect(zoom, &QComboBox::activated, plot, [plot, zoom, scroll](int i) { const int n = zoom->itemData(i).toInt(); scroll->setRange(0, (n - 1) * 1000); plot->setView(n, scroll->value()); });
        connect(scroll, &QScrollBar::valueChanged, plot, [plot, zoom](int value) { plot->setView(zoom->currentData().toInt(), value); });

        auto* nodes = hbox(5); auto* count = new QLabel(w); count->setObjectName(QString("osmiumCurveSelection_%1").arg(c)); count->setMinimumWidth(100); nodes->addWidget(count);
        const auto field = [&](const QString& id, const QString& label, double min, double max) {
            nodes->addWidget(new QLabel(label, w)); auto* value = new CurveValue(ctx().project, w); value->setObjectName(QString("osmiumCurve%1_%2").arg(id).arg(c));
            value->setRange(min, max); value->setFixedSize(62, 24); value->setEditScale(100);
            value->setFormatter([](double v) { return QString::number(v * 100, 'f', 2) + "%"; }); nodes->addWidget(value); return value;
        };
        auto* x = field("X", "X", 0, 1); auto* y = field("Y", "Y", 0, 1); auto* bend = field("Bend", "Bend", -1, 1);
        InfoView::attach(x, "Node time", "Exact time as a percentage of the curve. Drag or double-click to type. Endpoints keep their time.");
        InfoView::attach(y, "Node level", "Exact node level from 0 to 100%. Drag or double-click to type.");
        InfoView::attach(bend, "Segment bend", "Outgoing segment bend from -100 to +100%. Zero straightens the segment.");
        connect(x, &DragValue::valueEdited, plot, [plot](double v) { plot->setCoordinate(true, v); });
        connect(y, &DragValue::valueEdited, plot, [plot](double v) { plot->setCoordinate(false, v); });
        connect(bend, &DragValue::valueEdited, plot, [plot](double v) { plot->setBend(v); });
        auto* segment = new ChoiceBox(w); segment->setObjectName(QString("osmiumCurveSegment_%1").arg(c)); segment->addItems({"Curve", "Smooth", "Hold"}); segment->setFixedWidth(86);
        InfoView::attach(segment, "Segment shape", "Interpolation of the selected outgoing segment. Smooth eases both ends; Hold makes a step.");
        connect(segment, &QComboBox::activated, plot, [plot](int i) { plot->setShape(i); }); nodes->addWidget(segment);
        auto* remove = button("Delete", "Delete selected interior nodes. Endpoints are retained.", [plot] { QKeyEvent e(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier); QApplication::sendEvent(plot, &e); }); nodes->addWidget(remove); nodes->addStretch();
        auto sync = [plot, count, x, y, bend, segment, remove] {
            const auto p = plot->points(); const int k = plot->selected(), s = plot->segment(), n = plot->selectionCount();
            count->setText(n > 1 ? QString("%1 selected / %2").arg(n).arg(p.size()) : k >= 0 ? QString("Node %1 / %2").arg(k + 1).arg(p.size()) : QString("%1 / 257 nodes").arg(p.size()));
            x->setEnabled(n == 1 && k > 0 && k + 1 < p.size()); y->setEnabled(n == 1 && k >= 0); bend->setEnabled(s >= 0); segment->setEnabled(s >= 0); remove->setEnabled(n > 0);
            if (k >= 0) { if (!x->editing()) x->setValue(p[k].x, false); if (!y->editing()) y->setValue(p[k].y, false); }
            if (s >= 0) { if (!bend->editing()) bend->setValue(p[s].bend, false); segment->setCurrentIndex(p[s].shape); }
        };
        plot->selectionChanged = sync; m_refreshers.push_back(sync); sync(); l->addLayout(nodes);
        auto* row = hbox(12); row->addWidget(labelledChoice(mo::cparam(c, mo::CRate), "Length", 100)); row->addWidget(labelledChoice(mo::cparam(c, mo::CMode), "Mode", 100));
        row->addWidget(labelledChoice(mo::cparam(c, mo::CRetrig), "Retrigger", 80)); row->addWidget(dial(mo::cparam(c, mo::CSmooth), "Smoothing", 64)); row->addStretch(); l->addLayout(row); return w;
    }
    QWidget* stepper(int s) {
        auto* w = new QWidget(this); auto* l = vbox(5); w->setLayout(l); auto* plot = new StepPlot(this, s); expandPlot(plot, 138); l->addWidget(plot, 1); auto* row = hbox(10);
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
        std::vector<QWidget*> cards;
        const QStringList titles{"CHORUS", "PING-PONG DELAY", "REVERB", "EQ"};
        const std::vector<std::vector<mo::FxKnob>> params{
            {mo::ChorusRate, mo::ChorusDepth, mo::ChorusMix},
            {mo::DelayTime, mo::DelayFeedback, mo::DelayTone, mo::DelayMix},
            {mo::ReverbSize, mo::ReverbMix}, {mo::EqLow, mo::EqHigh}
        };
        const std::vector<QStringList> labels{{"Rate", "Depth", "Mix"}, {"Time", "Feedback", "Tone", "Mix"}, {"Size", "Mix"}, {"Low", "High"}};
        for (int section = 0; section < 4; ++section) {
            auto* box = card(); box->setProperty("osmiumCard", true); box->setObjectName(QString("osmiumFxCard_%1").arg(section));
            auto* layout = new QVBoxLayout(box); layout->setContentsMargins(9, 8, 9, 8); layout->setSpacing(6);
            auto* title = new QLabel(titles[section], box); title->setObjectName("osmiumCaption"); layout->addWidget(title);
            layout->addWidget(new FxPlot(this, section), 1);
            auto* controls = hbox(4); controls->addStretch();
            for (int k = 0; k < int(params[size_t(section)].size()); ++k) {
                const int param = mo::fx(params[size_t(section)][size_t(k)]); const auto kind = specOf(ctx().type)->params[size_t(param)].kind;
                controls->addWidget(kind == ParamKind::Choice ? labelledChoice(param, labels[size_t(section)][k], 74) : dial(param, labels[size_t(section)][k], 52), 0, Qt::AlignTop);
            }
            controls->addStretch(); layout->addLayout(controls); cards.push_back(box);
        }
        return scrollPage(new EffectGrid(std::move(cards), this));
    }
    QWidget* voice() {
        auto* scroll = new QScrollArea(this); scroll->setObjectName("osmiumVoiceScroll"); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
        auto* content = new QWidget(scroll); auto* layout = new QVBoxLayout(content); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(7);
        std::vector<QWidget*> groups;
        const auto addGroup = [this, &groups](const QString& title, const std::vector<int>& params) {
            std::vector<QWidget*> controls;
            for (int param : params) {
                if (param < 0 || param >= count() || m_used.count(param)) continue;
                const auto& spec = specOf(ctx().type)->params[size_t(param)];
                controls.push_back(spec.kind == ParamKind::Choice || spec.kind == ParamKind::Toggle ? labelledChoice(param, spec.name, 78) : dial(param, spec.name, 52));
            }
            if (controls.empty()) return;
            auto* box = card(); box->setProperty("osmiumCard", true); auto* group = new QVBoxLayout(box); group->setContentsMargins(9, 6, 9, 3); group->setSpacing(1);
            auto* caption = new QLabel(title, box); caption->setObjectName("osmiumCaption"); group->addWidget(caption);
            group->addWidget(new VoiceGrid(std::move(controls), box)); groups.push_back(box);
        };
        addGroup("VOICE", {mo::Glide, mo::VoiceMode, mo::Unison, mo::Detune, mo::Spread, mo::VelAmp, mo::KeyTrack, mo::BendRange, mo::OscRestart});
        addGroup("SOURCES & INSERT", {mo::ModPitch, mo::ModOscWave, mo::ModPhase, mo::ModRing, mo::NoiseLevel, mo::NoiseColor, mo::InsertType, mo::InsertAmount});
        addGroup("SUB & OSCILLATOR MIXER", {mo::SubLevel, mo::SubOct, mo::SubWave, mo::oparam(0, mo::FilterMix), mo::oparam(1, mo::FilterMix), mo::oparam(2, mo::FilterMix)});
        std::vector<int> remaining;
        for (int param = 0; param < count(); ++param) {
            if (m_used.count(param) || param == mo::Volume || (param >= mo::kFxBase && param < mo::kFxBase + mo::kFx)) continue;
            bool macro = false; for (int k = 0; k < 8; ++k) macro |= param == mo::macro(k);
            if (!macro) remaining.push_back(param);
        }
        addGroup("PERFORMANCE", remaining);
        layout->addWidget(new VoiceSections(std::move(groups), content));
        layout->addWidget(new UnisonPlot(this), 1); scroll->setWidget(content); return scroll;
    }
    QWidget* scrollPage(QWidget* w) {
        auto* scroll = new QScrollArea(this); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(w); return scroll;
    }
    void addSourcePage(int source, QWidget* w) { auto* page = scrollPage(w); m_sourcePages[source] = page; m_modulators->addWidget(page); }
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
    void applyStyle() {
        setStyleSheet(QString(
            "QWidget#osmiumEditor{background:%1;color:%5;}"
            "QFrame[osmiumCard=\"true\"]{background:%2;border:1px solid %4;border-radius:2px;}"
            "QLabel{background:transparent;color:%5;font-size:10px;border:0;}"
            "QLabel#osmiumTitle{font-size:13px;font-weight:700;color:%5;}"
            "QLabel#osmiumCaption{font-size:9px;font-weight:600;color:%7;letter-spacing:1px;}"
            "QPushButton{background:%3;border:1px solid %4;border-radius:2px;color:%5;padding:3px 8px;font-size:10px;}"
            "QPushButton:hover{border-color:%6;} QPushButton:checked{background:%6;color:%8;border-color:%6;}"
            "QComboBox{background:%3;color:%5;border:1px solid %4;border-radius:2px;min-height:24px;padding:0 20px 0 7px;font-size:10px;}"
            "QComboBox::drop-down{width:16px;border:0;} QComboBox::down-arrow{image:none;}"
            "QComboBox QAbstractItemView{background:%2;color:%5;selection-background-color:%4;}"
            "QPushButton#osmiumTablePicker_0,QPushButton#osmiumTablePicker_1,QPushButton#osmiumTablePicker_2{text-align:left;padding-left:7px;padding-right:20px;}"
            "QTreeWidget{background:%3;color:%5;border:0;outline:0;font-size:11px;}"
            "QTreeWidget::item{padding:2px;} QTreeWidget::item:selected{background:%4;color:%5;}"
            "QLineEdit{background:%3;color:%5;border:1px solid %4;border-radius:2px;padding:4px;font-size:11px;}"
            "QScrollArea{background:transparent;border:0;} QScrollArea>QWidget>QWidget{background:transparent;}"
        ).arg(theme::panel.name(), theme::devCell.name(), theme::devDisplay.name(), theme::devSeparator.name(),
              theme::text.name(), theme::accent.name(), theme::textFaint.name(), theme::devButtonOnText.name()));
        if (m_modulators) selectSource(m_source, false);
    }
    int m_source = mo::SrcEnv2, m_focus = mo::fparam(0, mo::FCutoff);
    std::set<int> m_used; std::map<int, QWidget*> m_sourcePages;
    std::vector<std::pair<int, SourceButton*>> m_sources; std::vector<QPushButton*> m_pagesButtons;
    QStackedWidget* m_pages = nullptr; QStackedWidget* m_modulators = nullptr; QWidget* m_performance = nullptr;
    QComboBox* m_other = nullptr; QComboBox* m_patches = nullptr;
    QLabel* m_sourceTitle = nullptr; QLabel* m_destination = nullptr; QLabel* m_rangeLabel = nullptr;
    DragValue* m_amount = nullptr; QVBoxLayout* m_routes = nullptr; QString m_routesSignature;
};

class OsmiumBody : public DeviceBody {
public:
    OsmiumBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        setFixedWidth(502);
        setObjectName("osmiumCompact");
        auto* outer = new QVBoxLayout(this); outer->setContentsMargins(10, 6, 10, 6); outer->setSpacing(6);
        auto* header = hbox(4);
        auto* title = new QLabel("OSMIUM", this); title->setStyleSheet(QString("color:%1;font-size:13px;font-weight:700;").arg(theme::text.name())); header->addWidget(title);
        header->addSpacing(6);
        m_patches = patchPicker(this, "osmiumCompactPatch"); m_patches->setMinimumWidth(200);
        header->addWidget(m_patches, 1); header->addWidget(patchArrow(this, m_patches, -1)); header->addWidget(patchArrow(this, m_patches, 1));
        connect(m_patches, &QComboBox::activated, this, [this](int i) { loadPatch(this, m_patches, i); });
        outer->addLayout(header);
        auto* root = hbox(10); auto* left = vbox(6);
        left->addWidget(new WavePlot(this, 0, 74), 1);
        auto* open = new QPushButton("Expand", this); open->setObjectName("osmiumOpenEditor"); open->setFixedHeight(28);
        InfoView::attach(open, "Open Osmium", "Expand the instrument inside the device pane: accent modulation ranges on the knobs, bendable envelopes, LFOs, curves, steppers, macros and effects.");
        connect(open, &QPushButton::clicked, this, [this] { emit editorRequested(); });
        left->addWidget(open); left->addStretch(); root->addLayout(left, 1);
        auto* controls = hbox(2);
        for (const int p : std::vector<int>{mo::oparam(0, mo::Position), mo::fparam(0, mo::FCutoff), mo::FDrive, mo::Volume}) {
            auto* k = new RangeKnob(this, p, p == mo::FDrive ? "Drive" : p == mo::Volume ? "Output" : p == mo::fparam(0, mo::FCutoff) ? "Cutoff" : "Position", 44,
                                    [] { return mo::SrcEnv2; }, [](int) {}); controls->addWidget(k, 0, Qt::AlignVCenter);
        }
        root->addLayout(controls); outer->addLayout(root, 1);
        refresh();
    }
    void refresh() override { DeviceBody::refresh(); syncPatchPicker(this, m_patches); }
    DeviceBody* createEditor(QWidget* parent) override { return new OsmiumEditor(ctx(), parent); }
private:
    QComboBox* m_patches = nullptr;
};

} // namespace
DeviceBody* createOsmiumBody(const DeviceContext& ctx, QWidget* parent) { return new OsmiumBody(ctx, parent); }
} // namespace sessions::devui
