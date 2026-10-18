#include "DeviceView.h"
#include "Browser.h"
#include "DeviceDrag.h"
#include <QPainter>
#include <QFileInfo>
#include <QDir>
#include <functional>
#include "InfoView.h"
#include "core/Model.h"
#include "core/Presets.h"
#include "core/Theme.h"
#include "core/DeviceSpecs.h"
#include "core/Transport.h"
#include "devices/DeviceBody.h"
#include <QApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QLabel>
#include <QToolButton>
#include <QDrag>
#include <QDragEnterEvent>
#include <QMouseEvent>
#include <QMimeData>
#include <QFrame>
#include <QMenu>
#include <QPointer>
#include <QInputDialog>
#include <QMessageBox>

namespace sessions {

namespace {

Device* findDevice(Project* project, int track, int id) {
    Track* t = project->trackAt(track);
    if (!t) return nullptr;
    for (Device& d : t->devices) if (d.id == id) return &d;
    return nullptr;
}

const Device* findDevice(const Project* project, int track, int id) {
    const Track* t = project->trackAt(track);
    if (!t) return nullptr;
    for (const Device& d : t->devices) if (d.id == id) return &d;
    return nullptr;
}

DeviceView* viewOf(const QWidget* w) {
    for (QWidget* p = w ? w->parentWidget() : nullptr; p; p = p->parentWidget())
        if (auto* v = qobject_cast<DeviceView*>(p)) return v;
    return nullptr;
}

// Stylesheets built from the theme when they're applied (and again when View > Theme changes: the Device View rebuilds).
QString panelStyle(bool on) {
    return QString("QFrame#devicePanel{background:%1;border:1px solid %2;border-radius:3px;}").arg((on ? theme::devicePanelOn : theme::devicePanelOff).name(), theme::line.name());
}
QString dropZoneStyle(bool active, bool chain) {
    const QColor edge = active ? theme::accent : chain ? theme::hint : theme::dropEdge, text = active ? theme::accent : theme::disabled;
    return QString("border:1px dashed %1;border-radius:3px;color:%2;%3").arg(edge.name(), text.name(), chain ? "font-size:9px;padding:6px;" : "");
}
const QColor& kTitle = theme::deviceTitle;
const QColor& kTitleSelected = theme::deviceTitleSel;
constexpr int kFoldedWidth = 24;

Project::DeviceBlocks& clipboard() { static Project::DeviceBlocks c; return c; }   // Ctrl+C in the Device View

// A device's title bar: click selects, drag moves (Ctrl: copies), double-click folds,
// right-click opens the panel's menu. Standing on end when the device is folded.
class DeviceTitle : public QWidget {
public:
    DeviceTitle(DevicePanel* panel, Project* project, int track, int id, const QString& name, bool vertical)
        : QWidget(panel), m_panel(panel), m_project(project), m_track(track), m_id(id), m_name(name), m_vertical(vertical) {
        setAttribute(Qt::WA_OpaquePaintEvent);
        InfoView::attach(this, name, "Click selects it (Ctrl+click: several, Shift+click: the ones in between). Drag it to move it along the chain, "
                                     "into a rack, onto another track or the empty space for a new track; hold Ctrl to copy. Double-click folds it; "
                                     "right-click for Group, Save as Preset and more.");
    }
    void setSelected(bool on) {
        m_selected = on;
        for (QLabel* l : findChildren<QLabel*>()) l->setStyleSheet(QString("font-weight:600;background:transparent;border:0;color:%1;").arg((on ? theme::selectionText : theme::text).name()));
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), m_selected ? kTitleSelected : kTitle);
        if (!m_vertical) return;
        p.setPen(m_selected ? theme::selectionText : theme::text);
        QFont f = font(); f.setBold(true); p.setFont(f);
        const int fh = p.fontMetrics().height();
        p.translate(width() / 2.0, 24);   // under the activator, reading downwards
        p.rotate(90);
        p.drawText(QRect(0, -fh / 2, height() - 30, fh), Qt::AlignLeft | Qt::AlignVCenter,
                   p.fontMetrics().elidedText(m_name, Qt::ElideRight, height() - 30));
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) { QWidget::mousePressEvent(e); return; }
        m_press = e->pos();
        m_armed = true;
        m_collapse = false;
        DeviceView* v = viewOf(this);
        if (!v) return;
        v->setFocus(Qt::MouseFocusReason);
        const bool more = e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
        if (!more && m_selected && v->selection().size() > 1) m_collapse = true;   // a drag takes them all; a click just this one
        else v->clickDevice(m_id, e->modifiers());
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!m_armed || !(e->buttons() & Qt::LeftButton)) return;
        if ((e->pos() - m_press).manhattanLength() < QApplication::startDragDistance()) return;
        m_armed = m_collapse = false;
        if (DeviceView* v = viewOf(this)) v->dragDevice(m_id);   // the view keeps this title until the drop is done
    }
    void mouseReleaseEvent(QMouseEvent*) override {
        const bool collapse = m_collapse;
        m_armed = m_collapse = false;
        if (collapse) if (DeviceView* v = viewOf(this)) v->clickDevice(m_id, Qt::NoModifier);
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        const Device* d = findDevice(m_project, m_track, m_id);
        if (d) m_project->setDeviceFolded(m_track, m_id, !d->folded);   // rebuilds the view (later)
    }
    void contextMenuEvent(QContextMenuEvent* e) override { m_panel->showMenu(e->globalPos()); }

private:
    DevicePanel* m_panel;
    Project* m_project;
    int m_track, m_id;
    QString m_name;
    bool m_vertical, m_selected = false, m_armed = false, m_collapse = false;
    QPoint m_press;
};

} // namespace

// A device in the chain: the title bar (activator, name, delete) over the device's own
// body (devui::createBody). Everything edits the device by id rather than by index, so a
// panel stays valid while the chain around it changes.
DevicePanel::DevicePanel(Project* project, Transport* transport, int track, const Device& d, QWidget* parent,
                         std::function<void(const QString&, int)> onSample)
    : QFrame(parent), m_project(project), m_track(track), m_id(d.id), m_type(deviceType(d.name)), m_folded(d.folded), m_onSample(std::move(onSample)) {
    const int id = d.id;
    setObjectName("devicePanel");
    setStyleSheet(panelStyle(d.enabled));
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(1, 1, 1, 1);
    lay->setSpacing(0);

    auto* title = new DeviceTitle(this, project, track, id, d.name, m_folded);
    m_title = title;
    auto* act = new QToolButton(title);
    act->setCheckable(true);
    act->setChecked(d.enabled);
    act->setFixedSize(11, 11);
    act->setFocusPolicy(Qt::NoFocus);
    act->setStyleSheet(QString("QToolButton{border-radius:5px;border:0;background:%1;padding:0;min-height:0;}QToolButton:checked{background:%2;}").arg(theme::buttonOff.name(), theme::mute.name()));
    InfoView::attach(act, "Device Activator", "Turns the device on or off. An instrument switched off goes silent; an effect switched off is bypassed.");
    const int trackId = project->trackAt(track) ? project->trackAt(track)->id : 0;
    act->setProperty("mapTarget", QString("device:%1:%2:on").arg(trackId).arg(id));
    if (m_folded) {
        // folded: a slim strip with the activator on top and the name standing on end
        auto* tl = new QVBoxLayout(title);
        tl->setContentsMargins(0, 6, 0, 6);
        tl->addWidget(act, 0, Qt::AlignHCenter);
        tl->addStretch(1);
        lay->addWidget(title, 1);
        setFixedSize(kFoldedWidth, devui::kBodyHeight + 22);
    } else {
        title->setFixedHeight(20);
        auto* tl = new QHBoxLayout(title);
        tl->setContentsMargins(6, 0, 6, 0);
        tl->setSpacing(6);
        tl->addWidget(act);
        auto* name = new QLabel(d.name, title);
        name->setStyleSheet("font-weight:600;background:transparent;border:0;");
        name->setAttribute(Qt::WA_TransparentForMouseEvents);   // clicks and drags are the title's
        tl->addWidget(name, 1);
        auto* del = new QToolButton(title);
        del->setText("×");
        del->setFixedSize(14, 14);
        del->setFocusPolicy(Qt::NoFocus);
        del->setStyleSheet(QString("QToolButton{border:0;background:transparent;color:%1;padding:0;min-height:0;font-size:12px;}QToolButton:hover{color:%2;}").arg(theme::dim.name(), theme::textBright.name()));
        InfoView::attach(del, "Delete Device", "Removes it from the chain.");
        tl->addWidget(del);
        lay->addWidget(title);
        QObject::connect(del, &QToolButton::clicked, this, [project, track, id] { project->deleteDevice(track, id); });

        devui::DeviceContext ctx;
        ctx.project = project; ctx.transport = transport; ctx.track = track; ctx.trackId = trackId; ctx.deviceId = id; ctx.type = m_type;
        m_body = devui::createBody(ctx, this);
        m_body->setEnabled(true);
        lay->addWidget(m_body);
        setFixedSize(m_body->width() + 2, devui::kBodyHeight + 22);
        m_body->installEventFilter(this);   // a body that changes width (the Monolith's drawers) takes the panel with it
        if (m_type == DeviceType::Sampler) setAcceptDrops(true);   // drop a sample on it
        if (const DeviceSpec* spec = specOf(m_type); spec && *spec->help)
            InfoView::attach(name, d.name, QString::fromUtf8(spec->help));
    }

    QObject::connect(act, &QToolButton::toggled, this, [this, project, track, id](bool on) {
        EditScope edit(project, on ? "Device On" : "Device Off");
        Device* dev = findDevice(project, track, id);   // after the snapshot, so the change isn't in it
        if (!dev) return;
        dev->enabled = on;
        setStyleSheet(panelStyle(on));
        emit project->deviceChanged();
    });
}

DeviceView* DevicePanel::view() const { return viewOf(this); }

void DevicePanel::setSelected(bool on) {
    if (on == m_selected) return;
    m_selected = on;
    static_cast<DeviceTitle*>(m_title)->setSelected(on);
}

// Right-click on the title. The menu acts on the selection (right-clicking a device that isn't
// selected selects it). Its commands run after the menu has closed: they may rebuild the view,
// and this panel with it.
void DevicePanel::showMenu(const QPoint& globalPos) {
    QPointer<DeviceView> v = view();
    if (v && !v->selection().contains(m_id)) v->setSelection({m_id});
    const QList<int> ids = v ? v->selection() : QList<int>{m_id};
    const Device* d = findDevice(m_project, m_track, m_id);
    if (!d) return;
    bool groupable = true;
    const Track* owner = m_project->trackAt(m_track);
    for (int id : ids) {
        const Device* s = findDevice(m_project, m_track, id);
        const bool padsFull = s && owner && holdsChains(*s) && std::any_of(owner->devices.begin(), owner->devices.end(), [s](const Device& o) { return o.rack == s->id; });
        groupable &= s && !isRack(*s) && s->rack == 0 && !padsFull && deviceType(s->name) != DeviceType::Macros;
    }
    const bool rack = isRack(*d);
    const bool folded = d->folded;
    QMenu menu;   // no parent: this panel may be gone when it returns
    QAction* group = menu.addAction(ids.size() > 1 ? "Group into Rack\tCtrl+G" : d->kind == DeviceKind::Instrument ? "Group into Instrument Rack\tCtrl+G"
                                    : d->kind == DeviceKind::MidiEffect ? "Group into MIDI Effect Rack\tCtrl+G" : "Group into Audio Effect Rack\tCtrl+G");
    group->setEnabled(groupable);
    QAction* ungroup = nullptr, * chain = nullptr;
    if (rack) {
        ungroup = menu.addAction("Ungroup Rack\tCtrl+Shift+G");
        ungroup->setEnabled(d->chains.size() <= 1 && ids.size() == 1);
        chain = menu.addAction("Add Chain");
        chain->setEnabled(d->chains.size() < racks::kChains && ids.size() == 1);
    }
    QAction* fold = menu.addAction(d->folded ? "Unfold" : "Fold");
    menu.addSeparator();
    QAction* save = menu.addAction(ids.size() > 1 ? "Save Devices as Preset…" : "Save as Preset…");
    menu.addSeparator();
    QAction* duplicate = menu.addAction("Duplicate\tCtrl+D");
    QAction* copy = menu.addAction("Copy\tCtrl+C");
    QAction* paste = menu.addAction("Paste\tCtrl+V");
    paste->setEnabled(v && v->canPaste());
    menu.addSeparator();
    QAction* del = menu.addAction("Delete\tDel");
    QAction* chosen = menu.exec(globalPos);
    if (!chosen) return;
    Project* project = m_project;
    const int track = m_track, id = m_id;
    // from here on, don't touch this panel
    if (chosen == group) { if (v) v->groupSelection(); else project->groupIntoRack(track, ids); }
    else if (chosen == ungroup) project->ungroupRack(track, id);
    else if (chosen == chain) { const int c = project->addChain(track, id); if (c >= 0) devui::selectChain(id, c); }
    else if (chosen == fold) for (int s : ids) project->setDeviceFolded(track, s, !folded);
    else if (chosen == save && v) v->askToSavePreset(ids);
    else if (chosen == duplicate && v) v->duplicateSelection();
    else if (chosen == copy && v) v->copySelection();
    else if (chosen == paste && v) v->paste();
    else if (chosen == del) { if (v) v->deleteSelection(); else project->deleteDevices(track, ids); }
}

namespace {

// The drop zone at the end of a rack's chain: a device dropped here goes into the shown chain
// (or makes the first chain).
class ChainDrop : public QLabel {
public:
    ChainDrop(Project* project, int track, int rackId, int chain, const QString& text, QWidget* parent)
        : QLabel(text, parent), m_project(project), m_track(track), m_rack(rackId), m_chainIndex(chain) {
        setAlignment(Qt::AlignCenter);
        setWordWrap(true);
        setFixedSize(110, devui::kBodyHeight + 22);
        setAcceptDrops(true);
        idle();
        InfoView::attach(this, "Chain Drop Zone", "Drag a device here, from the browser or the chain, to put it at the end of this chain of the rack. Hold Ctrl to copy one from the chain.");
    }
protected:
    void idle() { setStyleSheet(dropZoneStyle(false, true)); }
    bool take(QDragMoveEvent* e) {
        const devicedrag::Payload pl = devicedrag::parse(e->mimeData());
        if (!pl.valid() || !devicedrag::accepts(*m_project, pl, m_track, {0, m_rack, m_chainIndex})) { e->ignore(); idle(); return false; }
        if (pl.source == devicedrag::Payload::Devices) { e->setDropAction(devicedrag::copying(e) ? Qt::CopyAction : Qt::MoveAction); e->accept(); }
        else e->acceptProposedAction();
        setStyleSheet(dropZoneStyle(true, true));
        return true;
    }
    void dragEnterEvent(QDragEnterEvent* e) override { take(e); }
    void dragMoveEvent(QDragMoveEvent* e) override { take(e); }
    void dragLeaveEvent(QDragLeaveEvent*) override { idle(); }
    void dropEvent(QDropEvent* e) override {
        idle();
        const devicedrag::Payload pl = devicedrag::parse(e->mimeData());
        if (!pl.valid()) { e->ignore(); return; }
        const Track* t = m_project->trackAt(m_track);
        int before = 0;
        if (t) for (const Device& d : t->devices) if (d.id == m_rack) before = int(d.chains.size());
        const bool copy = devicedrag::copying(e);
        const devicedrag::Result r = devicedrag::drop(*m_project, pl, m_track, {0, m_rack, m_chainIndex}, copy);
        InfoView::notify(r.title, r.text);
        if (!r.ok) return;
        e->setDropAction(pl.source == devicedrag::Payload::Devices && !copy ? Qt::MoveAction : Qt::CopyAction);
        e->accept();
        devui::selectChain(m_rack, m_chainIndex >= 0 ? m_chainIndex : before);
    }
private:
    Project* m_project;
    int m_track, m_rack, m_chainIndex;
};

} // namespace

RackPanel::RackPanel(Project* project, Transport* transport, int track, const Device& rack, QWidget* parent,
                     std::function<void(const QString&, int)> onSample)
    : QFrame(parent), m_id(rack.id) {
    setObjectName("rackPanel");
    setStyleSheet(QString("QFrame#rackPanel{background:%1;border:1px solid %2;border-radius:4px;}").arg(theme::rackPanel.name(), theme::rackEdge.name()));
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(1, 1, 4, 1);
    lay->setSpacing(4);
    m_own = new DevicePanel(project, transport, track, rack, this, onSample);
    lay->addWidget(m_own, 0, Qt::AlignTop);
    if (rack.folded) return;   // just the rack's strip
    const Track* t = project->trackAt(track);
    const DeviceType type = deviceType(rack.name);
    const bool drum = type == DeviceType::DrumRack;   // its chains are its pads: the selected pad's
    const int chains = drum ? drumrack::kPads : int(rack.chains.size());
    const int chain = drum ? devui::selectedDrumPad(rack.id) : std::min(devui::selectedChain(rack.id), std::max(0, chains - 1));
    m_chain = chains > 0 ? chain : -1;
    bool hasInstrument = false;
    if (t && chains > 0)
        for (const Device& m : t->devices)
            if (m.rack == rack.id && m.chain == chain) {
                auto* panel = new DevicePanel(project, transport, track, m, this, onSample);
                lay->addWidget(panel, 0, Qt::AlignTop);
                m_members << panel;
                hasInstrument |= m.kind == DeviceKind::Instrument;
            }
    const bool takesInstrument = type == DeviceType::InstrumentRack || drum;
    const QString what = type == DeviceType::MidiEffectRack ? "MIDI effects" : takesInstrument && !hasInstrument ? "an instrument or effects" : "effects";
    QString text;
    if (drum) text = QString("Drop %1 onto pad %2").arg(what, devui::drumPadLabel(rack, chain));
    else if (chains == 0) text = type == DeviceType::InstrumentRack ? "Drop an instrument here to make the first chain"
                               : type == DeviceType::MidiEffectRack ? "Drop a MIDI effect here to make the first chain" : "Drop an effect here to make the first chain";
    else text = QString("Drop %1 into %2").arg(what, rack.chains.value(chain));
    m_drop = new ChainDrop(project, track, rack.id, m_chain, text, this);
    lay->addWidget(m_drop, 0, Qt::AlignTop);
}

bool DevicePanel::eventFilter(QObject* o, QEvent* e) {
    if (o == m_body && e->type() == QEvent::Resize && m_body && !m_folded && width() != m_body->width() + 2) setFixedWidth(m_body->width() + 2);
    return QFrame::eventFilter(o, e);
}

void DevicePanel::dragEnterEvent(QDragEnterEvent* e) {
    if (!Browser::audioFilesIn(e->mimeData()).isEmpty()) e->acceptProposedAction(); else e->ignore();
}

void DevicePanel::dropEvent(QDropEvent* e) {
    const QStringList files = Browser::audioFilesIn(e->mimeData());
    if (files.isEmpty()) { e->ignore(); return; }
    e->acceptProposedAction();
    if (m_onSample) m_onSample(files.first(), m_id);
}


DeviceView::DeviceView(Project* project, Transport* transport, QWidget* parent)
    : QWidget(parent), m_project(project), m_transport(transport) {
    setAcceptDrops(true);
    setFocusPolicy(Qt::ClickFocus);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_host = new QWidget(m_scroll);
    m_host->setObjectName("panel");
    m_chain = new QHBoxLayout(m_host);
    m_chain->setContentsMargins(6, 6, 6, 6);
    m_chain->setSpacing(4);
    m_chain->setAlignment(Qt::AlignLeft);
    m_scroll->setWidget(m_host);
    lay->addWidget(m_scroll);
    m_line = new QWidget(m_host);
    m_line->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_line->setAutoFillBackground(true);
    QPalette pal = m_line->palette();
    pal.setColor(QPalette::Window, theme::accent);
    m_line->setPalette(pal);
    m_line->hide();
    InfoView::attach(this, "Device View", "The selected track's devices, left to right: MIDI effects → instrument → audio effects. Drag devices here from the browser, "
                                          "or drag a device's title to move it (Ctrl: copy). Click titles to select; Delete, Ctrl+D, Ctrl+G act on them.");
    connect(project, &Project::structureChanged, this, &DeviceView::rebuild);
    connect(theme::notifier(), &theme::Notifier::changed, this, [this] {   // View > Theme: the panels' stylesheets are built on the way in
        QPalette pal = m_line->palette();
        pal.setColor(QPalette::Window, theme::accent);
        m_line->setPalette(pal);
        rebuild();
    });
    connect(project, &Project::selectionChanged, this, &DeviceView::rebuild);
    connect(transport, &Transport::tick, this, &DeviceView::followAutomation);
    connect(devui::rackSignals(), &devui::RackSignals::chainSelected, this, &DeviceView::rebuild, Qt::QueuedConnection);
    m_meters.setInterval(33);
    connect(&m_meters, &QTimer::timeout, this, &DeviceView::tickPanels);
    m_meters.start();
    m_autoScroll.setInterval(30);
    connect(&m_autoScroll, &QTimer::timeout, this, [this] {
        QScrollBar* bar = m_scroll->horizontalScrollBar();
        bar->setValue(bar->value() + m_scrollStep);
    });
    rebuild();
}

QList<QPair<int, QWidget*>> DeviceView::blockWidgets() const {
    QList<QPair<int, QWidget*>> out;
    for (int i = 0; i < m_chain->count(); ++i) {
        QWidget* w = m_chain->itemAt(i)->widget();
        if (!w || !w->isVisibleTo(m_host)) continue;
        if (auto* r = qobject_cast<RackPanel*>(w)) out << qMakePair(r->rackId(), w);
        else if (auto* d = qobject_cast<DevicePanel*>(w)) out << qMakePair(d->deviceId(), w);
    }
    return out;
}

QList<DevicePanel*> DeviceView::panels() const {
    QList<DevicePanel*> out;
    for (DevicePanel* p : m_host->findChildren<DevicePanel*>())
        if (p->isVisibleTo(m_host)) out << p;   // not the ones a rebuild is about to delete
    return out;
}

int DeviceView::trackIndex() const { return m_project->selectedTrack; }

void DeviceView::followAutomation() {
    if (!isVisible()) return;
    for (DevicePanel* p : panels()) if (p->body()) p->body()->follow(m_transport->beat());
}

void DeviceView::tickPanels() {
    if (!isVisible()) return;
    for (DevicePanel* p : panels()) if (p->body()) p->body()->tick();
}

void DeviceView::rebuild() {
    if (m_panelDrag) { m_rebuildPending = true; return; }   // the dragged title must live until the drop is done
    // deleteLater: rebuild can run from inside a panel's own button handler.
    while (QLayoutItem* it = m_chain->takeAt(0)) {
        if (QWidget* w = it->widget()) { w->hide(); w->deleteLater(); }
        delete it;
    }
    m_dropZone = nullptr;
    hideLine();
    m_host->setMinimumSize(0, 0);
    const Track* t = m_project->trackAt(m_project->selectedTrack);
    if (!t || t->id != m_trackId) { m_selected.clear(); m_trackId = t ? t->id : 0; }   // a selection belongs to its track
    if (!t) return;
    auto onSample = [this](const QString& path, int id) { emit sampleDropped(path, id); };
    for (int i = 0; i < t->devices.size(); i = deviceBlockEnd(t->devices, i)) {   // a rack and its chains are one block
        const Device& d = t->devices[i];
        if (holdsChains(d)) m_chain->addWidget(new RackPanel(m_project, m_transport, m_project->selectedTrack, d, m_host, onSample), 0, Qt::AlignTop);
        else m_chain->addWidget(new DevicePanel(m_project, m_transport, m_project->selectedTrack, d, m_host, onSample), 0, Qt::AlignTop);
    }

    bool hasInstrument = false;
    for (const Device& d : t->devices) hasInstrument |= d.kind == DeviceKind::Instrument;
    QString hint = m_project->isReturn(m_project->selectedTrack) || t->type != TrackType::Midi ? "Drop Audio Effects Here"
                 : hasInstrument ? "Drop Audio Effects Here" : "Drop an Instrument or a Sample Here";
    m_dropZone = new QLabel(hint, m_host);
    m_dropZone->setAlignment(Qt::AlignCenter);
    m_dropZone->setFixedSize(130, devui::kBodyHeight + 22);
    m_dropZone->setStyleSheet(dropZoneStyle(false, false));
    InfoView::attach(m_dropZone, "Drop Zone", "Drag devices here from the browser, or double-click them there.");
    m_chain->addWidget(m_dropZone, 0, Qt::AlignTop);
    // The scroll area sizes the host to its viewport; pin the host to the chain's own size so
    // a long chain scrolls instead of squeezing the panels over each other.
    for (int i = 0; i < m_chain->count(); ++i) if (QWidget* w = m_chain->itemAt(i)->widget()) w->show();   // count them now, not after the event loop
    m_chain->activate();
    m_host->setMinimumSize(m_chain->minimumSize());
    showSelection();
}

// ---- selection ------------------------------------------------------------

QList<int> DeviceView::selection() const {
    QList<int> out;
    const Track* t = m_project->trackAt(trackIndex());
    if (!t || t->id != m_trackId) return out;
    for (const Device& d : t->devices) if (m_selected.contains(d.id)) out << d.id;   // in chain order
    return out;
}

void DeviceView::setSelection(const QList<int>& ids) {
    m_selected = ids;
    if (!ids.isEmpty()) m_anchor = ids.last();
    showSelection();
}

void DeviceView::showSelection() {
    for (DevicePanel* p : panels()) p->setSelected(m_selected.contains(p->deviceId()));
}

void DeviceView::clickDevice(int id, Qt::KeyboardModifiers mods) {
    const Track* t = m_project->trackAt(trackIndex());
    if (!t) return;
    if (mods & Qt::ControlModifier) {
        if (m_selected.contains(id)) m_selected.removeAll(id); else m_selected << id;
        m_anchor = id;
    } else if ((mods & Qt::ShiftModifier) && m_anchor && m_anchor != id) {
        // the devices between the two, on the clicked one's level (the track, or the same chain)
        const Device* clicked = findDevice(m_project, trackIndex(), id);
        int a = -1, b = -1;
        for (int i = 0; i < t->devices.size(); ++i) {
            if (t->devices[i].id == m_anchor) a = i;
            if (t->devices[i].id == id) b = i;
        }
        if (a < 0 || !clicked) { m_selected = {id}; m_anchor = id; }
        else {
            m_selected.clear();
            for (int i = std::min(a, b); i <= std::max(a, b); ++i) {
                const Device& d = t->devices[i];
                if (d.rack == clicked->rack && (d.rack == 0 || d.chain == clicked->chain)) m_selected << d.id;
            }
        }
    } else {
        m_selected = {id};
        m_anchor = id;
    }
    showSelection();
}

void DeviceView::mousePressEvent(QMouseEvent* e) {
    setFocus(Qt::MouseFocusReason);
    if (e->button() == Qt::LeftButton && !(e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) { m_selected.clear(); showSelection(); }
    QWidget::mousePressEvent(e);
}

// Right after the last selected device, on its level: where Duplicate and Paste put things.
static Project::DevicePlace afterLast(const Project* p, int track, const QList<int>& ids) {
    Project::DevicePlace place;   // natural
    const Track* t = p->trackAt(track);
    if (!t || ids.isEmpty()) return place;
    int last = -1;
    for (int i = 0; i < t->devices.size(); ++i) if (ids.contains(t->devices[i].id)) last = i;
    if (last < 0) return place;
    const Device& d = t->devices[last];
    if (d.rack == 0) {
        const int next = deviceBlockEnd(t->devices, last);
        place.beforeId = next < t->devices.size() ? t->devices[next].id : 0;
        return place;
    }
    place.rackId = d.rack;
    place.chain = d.chain;
    place.beforeId = 0;
    if (last + 1 < t->devices.size() && t->devices[last + 1].rack == d.rack && t->devices[last + 1].chain == d.chain) place.beforeId = t->devices[last + 1].id;
    return place;
}

void DeviceView::deleteSelection() {
    const QList<int> ids = selection();
    if (ids.isEmpty()) return;
    m_selected.clear();
    m_project->deleteDevices(trackIndex(), ids);
}

void DeviceView::duplicateSelection() {
    const QList<int> ids = selection();
    if (ids.isEmpty()) return;
    QList<int> placed;
    QString why;
    if (m_project->moveDevices(trackIndex(), ids, trackIndex(), afterLast(m_project, trackIndex(), ids), true, &placed, &why)) setSelection(placed);
    else InfoView::notify("Can't duplicate that", why);
}

void DeviceView::copySelection() {
    const QList<int> ids = selection();
    if (ids.isEmpty()) return;
    clipboard() = m_project->deviceBlocks(trackIndex(), ids);
    InfoView::notify(ids.size() == 1 ? "Copied " + clipboard().first().first().name : QString("Copied %1 devices").arg(clipboard().size()),
                     "Select a track (and a device to paste after) and press Ctrl+V in the Device View.");
}

bool DeviceView::canPaste() const { return !clipboard().isEmpty(); }

void DeviceView::paste() {
    if (clipboard().isEmpty() || !m_project->trackAt(trackIndex())) return;
    const Project::DeviceBlocks blocks = m_project->freshCopies(clipboard());
    const QString label = blocks.size() == 1 ? "Paste " + blocks.first().first().name : QString("Paste Devices");
    Project::DevicePlace place = afterLast(m_project, trackIndex(), selection());
    QList<int> placed;
    QString why;
    bool ok = m_project->insertDevices(trackIndex(), blocks, place, label, &placed, &why);
    if (!ok && place.rackId) ok = m_project->insertDevices(trackIndex(), blocks, {}, label, &placed, &why);   // not in a chain: where they go on the track
    if (ok) setSelection(placed);
    else InfoView::notify("Can't paste here", why);
}

int DeviceView::groupSelection() {
    const QList<int> ids = selection();
    const int rack = ids.isEmpty() ? 0 : m_project->groupIntoRack(trackIndex(), ids);
    if (rack) setSelection({rack});
    else InfoView::notify("Group into Rack", "Select devices on the track itself first (click their titles; Ctrl+click for more). MIDI effects, Macros and racks don't go into racks.");
    return rack;
}

bool DeviceView::ungroupSelection() {
    for (int id : selection()) {
        const Device* d = findDevice(m_project, trackIndex(), id);
        if (!d || !isRack(*d)) continue;
        if (m_project->ungroupRack(trackIndex(), id)) return true;
        InfoView::notify("Ungroup Rack", "Only a rack with one chain can be ungrouped: delete the others, or move their devices out first.");
        return false;
    }
    InfoView::notify("Ungroup Rack", "Select a rack first (click its title).");
    return false;
}

bool DeviceView::savePreset(const QList<int>& ids, const QString& name, QString* error) {
    const Project::DeviceBlocks blocks = m_project->deviceBlocks(trackIndex(), ids);
    if (blocks.isEmpty() || name.trimmed().isEmpty()) { if (error) *error = "Nothing to save."; return false; }
    QString err;
    if (!presets::save(presets::pathFor(name), name, blocks, &err)) {
        if (error) *error = err;
        InfoView::notify("Can't save the preset", err);
        return false;
    }
    InfoView::notify("Saved " + name.trimmed(), "It's in the browser under User Library: drag it onto any track, or double-click it.");
    return true;
}

void DeviceView::askToSavePreset(const QList<int>& ids) {
    const Project::DeviceBlocks blocks = m_project->deviceBlocks(trackIndex(), ids);
    if (blocks.isEmpty()) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, "Save as Preset", blocks.size() > 1 ? QString("A name for these %1 devices:").arg(blocks.size()) : QString("A name for it:"),
                                               QLineEdit::Normal, blocks.first().first().name, &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    if (QFileInfo::exists(presets::pathFor(name))
        && QMessageBox::question(this, "Save as Preset", "There's a preset called \"" + name + "\" already. Replace it?") != QMessageBox::Yes) return;
    QString error;
    if (!savePreset(ids, name, &error)) QMessageBox::warning(this, "Save as Preset", "Couldn't save it:\n\n" + error);
}

// ---- drag and drop --------------------------------------------------------

void DeviceView::dragDevice(int id) {
    const Track* t = m_project->trackAt(trackIndex());
    if (!t) return;
    if (!m_selected.contains(id)) { m_selected = {id}; m_anchor = id; showSelection(); }
    const QList<int> ids = selection();
    auto* drag = new QDrag(this);   // owned by the view, which outlives the drop
    drag->setMimeData(devicedrag::mimeFor(t->id, ids));
    for (DevicePanel* p : panels())
        if (p->deviceId() == ids.value(0)) {
            QPixmap pm = p->titleBar()->grab(QRect(0, 0, std::min(p->titleBar()->width(), 220), std::min(p->titleBar()->height(), 20)));
            if (ids.size() > 1) {
                QPainter painter(&pm);
                painter.fillRect(QRect(pm.width() - 30, 0, 30, pm.height()), theme::accent);
                painter.setPen(Qt::black);
                painter.drawText(QRect(pm.width() - 30, 0, 30, pm.height()), Qt::AlignCenter, QString("+%1").arg(ids.size() - 1));
            }
            drag->setPixmap(pm);
            drag->setHotSpot(QPoint(16, pm.height() / 2));
        }
    m_panelDrag = true;
    drag->exec(Qt::MoveAction | Qt::CopyAction, Qt::MoveAction);
    m_panelDrag = false;
    hideLine();
    m_autoScroll.stop();
    if (m_rebuildPending) { m_rebuildPending = false; QTimer::singleShot(0, this, &DeviceView::rebuild); }
}

Project::DevicePlace DeviceView::placeAt(const QPoint& pos, QRect* line) const {
    Project::DevicePlace place;
    place.beforeId = 0;   // the end
    const QPoint hp = m_host->mapFrom(this, pos);
    const int h = devui::kBodyHeight + 22;
    auto mark = [line, h](int x, int top) { if (line) *line = QRect(x, top, 3, h); };
    QWidget* last = nullptr;
    for (int i = 0; i < m_chain->count(); ++i) {
        QWidget* w = m_chain->itemAt(i)->widget();
        if (!w || w == m_dropZone || w->isHidden()) continue;
        const QRect g = w->geometry();
        if (auto* rp = qobject_cast<RackPanel*>(w)) {
            const QRect own(rp->ownPanel()->mapTo(m_host, QPoint(0, 0)), rp->ownPanel()->size());
            if (rp->chainDrop() && hp.x() > own.right() && hp.x() <= g.right()) {   // next to the rack: into the chain it shows
                place.rackId = rp->rackId();
                place.chain = rp->chain();
                int x = rp->chainDrop()->mapTo(m_host, QPoint(0, 0)).x() - 3;
                for (DevicePanel* m : rp->members()) {
                    const QRect mg(m->mapTo(m_host, QPoint(0, 0)), m->size());
                    if (hp.x() < mg.center().x()) { place.beforeId = m->deviceId(); x = mg.left() - 3; break; }
                }
                mark(x, own.top());
                return place;
            }
            if (hp.x() < own.center().x()) { place.beforeId = rp->rackId(); mark(g.left() - 3, g.top()); return place; }
        } else if (auto* dp = qobject_cast<DevicePanel*>(w)) {
            if (hp.x() < g.center().x()) { place.beforeId = dp->deviceId(); mark(g.left() - 3, g.top()); return place; }
        }
        last = w;
    }
    mark(last ? last->geometry().right() + 1 : 3, m_chain->contentsMargins().top());
    return place;
}

void DeviceView::hideLine() { if (m_line) m_line->hide(); }

void DeviceView::dragEnterEvent(QDragEnterEvent* e) {
    if (!Browser::audioFilesIn(e->mimeData()).isEmpty()) {
        e->acceptProposedAction();
        if (m_dropZone) m_dropZone->setStyleSheet(dropZoneStyle(true, false));
        return;
    }
    if (!devicedrag::parse(e->mimeData()).valid()) { e->ignore(); return; }
    dragMoveEvent(e);
}

void DeviceView::dragMoveEvent(QDragMoveEvent* e) {
    if (!Browser::audioFilesIn(e->mimeData()).isEmpty()) { e->acceptProposedAction(); return; }
    const devicedrag::Payload pl = devicedrag::parse(e->mimeData());
    if (!pl.valid()) { e->ignore(); return; }
    const QPoint pos = e->position().toPoint();
    m_scrollStep = pos.x() < 28 ? -18 : pos.x() > width() - 28 ? 18 : 0;   // near an edge: scroll the chain
    if (m_scrollStep && !m_autoScroll.isActive()) m_autoScroll.start();
    if (!m_scrollStep) m_autoScroll.stop();
    QRect line;
    const Project::DevicePlace place = placeAt(pos, &line);
    QString why;
    if (!devicedrag::accepts(*m_project, pl, trackIndex(), place, &why)) {
        hideLine();
        e->ignore();
        return;
    }
    m_line->setGeometry(line);
    m_line->show();
    m_line->raise();
    if (pl.source == devicedrag::Payload::Devices) { e->setDropAction(devicedrag::copying(e) ? Qt::CopyAction : Qt::MoveAction); e->accept(); }
    else e->acceptProposedAction();
}

void DeviceView::dragLeaveEvent(QDragLeaveEvent*) {
    if (m_dropZone) m_dropZone->setStyleSheet(dropZoneStyle(false, false));
    hideLine();
    m_autoScroll.stop();
}

void DeviceView::dropEvent(QDropEvent* e) {
    dragLeaveEvent(nullptr);
    if (const QStringList files = Browser::audioFilesIn(e->mimeData()); !files.isEmpty()) {
        e->acceptProposedAction();
        emit sampleDropped(files.first(), -1);
        return;
    }
    const devicedrag::Payload pl = devicedrag::parse(e->mimeData());
    if (!pl.valid()) return;
    const bool copy = devicedrag::copying(e);
    const devicedrag::Result r = devicedrag::drop(*m_project, pl, trackIndex(), placeAt(e->position().toPoint()), copy);
    InfoView::notify(r.title, r.text);
    if (!r.ok) return;
    e->setDropAction(pl.source == devicedrag::Payload::Devices && !copy ? Qt::MoveAction : Qt::CopyAction);
    e->accept();
    setSelection(r.ids);
}

} // namespace sessions
