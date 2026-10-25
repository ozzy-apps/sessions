#include "MainWindow.h"
#include "SamplePacks.h"
#include "RecoveryDialog.h"
#include "core/Recovery.h"
#include "widgets/DragValue.h"
#include "ControlBar.h"
#include "Browser.h"
#include "SampleEditor.h"
#include "SessionView.h"
#include "ArrangementView.h"
#include "ClipView.h"
#include "PianoRoll.h"
#include "DeviceView.h"
#include "DeviceDrag.h"
#include "core/Presets.h"
#include <QTemporaryDir>
#include "devices/DeviceBody.h"
#include "InfoView.h"
#include "core/Model.h"
#include "core/Transport.h"
#include "core/ControlSurface.h"
#include "core/NektarPads.h"
#include "core/Theme.h"
#include "core/Scales.h"
#include "core/DeviceSpecs.h"
#include "ui/devices/DeviceBody.h"
#include "audio/Nam.h"
#include "audio/Engine.h"
#include "audio/MidiInput.h"
#include "core/ApcKey25.h"
#include "MapOverlay.h"
#include "Preferences.h"
#include "AppIcon.h"
#include "ThemeEditor.h"
#include "OverviewBar.h"
#include "audio/Builtin.h"
#include "core/Grooves.h"
#include "core/Import.h"
#include "core/Sample.h"
#include <QStackedWidget>
#include <QSplitter>
#include <QStyle>
#include <QStylePainter>
#include <QStyleOptionToolButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QApplication>
#include <QScrollArea>
#include <QScrollBar>
#include <QInputDialog>
#include <QDesktopServices>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QUndoStack>
#include <QActionGroup>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QDragEnterEvent>
#include <QUrl>
#include <QMimeData>
#include <QProgressDialog>
#include <QSpinBox>
#include <QEventLoop>
#include <QScopedValueRollback>
#include <QElapsedTimer>

namespace sessions {
namespace {
// A tab standing on end (the Clip / Device switch beside the detail pane, 0.21): the button's
// frame from the stylesheet, its text turned to read bottom-up.
class SideTab : public QToolButton {
public:
    using QToolButton::QToolButton;
    QSize sizeHint() const override { return QSize(18, 60); }
    QSize minimumSizeHint() const override { return QSize(18, 60); }
protected:
    void paintEvent(QPaintEvent*) override {
        QStylePainter p(this);
        QStyleOptionToolButton opt;
        initStyleOption(&opt);
        opt.text.clear();
        opt.icon = QIcon();
        p.drawComplexControl(QStyle::CC_ToolButton, opt);
        p.setRenderHint(QPainter::TextAntialiasing, true);
        p.setPen(isChecked() ? theme::onAccent : theme::text);
        p.translate(width() / 2.0, height() / 2.0);
        p.rotate(-90);
        p.drawText(QRectF(-height() / 2.0, -width() / 2.0, height(), width()), Qt::AlignCenter, text());
    }
};
}
}
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QDialog>
#include <QProcessEnvironment>
#include <QSvgRenderer>
#include <QFile>
#include <QPainter>
#include <cmath>

namespace sessions {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    resize(1280, 800);

    m_project = new Project(this);
    m_project->loadDemo();
    m_engine = new audio::Engine(m_project, this);
    m_transport = new Transport(m_project, m_engine, this);
    m_mapper = new Mapper(m_project, m_transport, this);
    m_midi = new audio::MidiInput(this);
    m_surface = new ControlSurface(m_project, m_transport, m_mapper, this);
    m_surface->pads()->setSender([this, failed = false](const QString& port, const QByteArray& data) mutable {
        const bool sent = m_midi->send(port, data);
        if (!sent && !failed && m_info) m_info->show("Nektar: pad feedback unavailable", m_midi->sendError());
        failed = !sent;
        return sent;
    });
    connect(m_surface->pads(), &NektarPads::trackChosen, this, &MainWindow::selectTrack);
    connect(m_surface->pads(), &NektarPads::clipChosen, this, [this](int t, int s) {
        selectTrack(t); selectClip(m_project->tracks[t].clipSlots.value(s)); showClipView(true);
    });
    connect(m_surface->pads(), &NektarPads::message, this, [this](const QString& title, const QString& body) {
        if (m_info) m_info->show(title, body);
    });
    m_apc = new ApcKey25(m_project, m_transport, m_mapper, this);
    m_apc->setSender([this](const QString& port, const QByteArray& message) {
        const bool sent = m_midi->send(port, message);
        if (!sent && !m_apcLightsFailed && m_info) {   // once, so a missing output says why the pads stay dark
            m_apcLightsFailed = true;
            m_info->show("APC Key 25: pads can't be lit", m_midi->sendError());
        }
        if (sent) m_apcLightsFailed = false;
        return sent;
    });

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_controlBar = new ControlBar(m_project, m_transport, central);
    root->addWidget(m_controlBar);

    auto* vsplit = m_vsplit = new QSplitter(Qt::Vertical, central);
    vsplit->setChildrenCollapsible(false);

    // ---- middle: browser | main views, with a divider between them
    auto* middle = m_hsplit = new QSplitter(Qt::Horizontal, vsplit);
    middle->setObjectName("browserSplit");
    middle->setChildrenCollapsible(false);
    m_browser = new Browser(middle);
    middle->addWidget(m_browser);

    m_mainStack = new QStackedWidget(middle);
    m_session = new SessionView(m_project, m_transport, m_mainStack);
    auto* sessionScroll = new QScrollArea(m_mainStack);
    sessionScroll->setWidgetResizable(true);
    sessionScroll->setFrameShape(QFrame::NoFrame);
    sessionScroll->setWidget(m_session);
    m_arrangement = new ArrangementView(m_project, m_transport, m_mainStack);
    m_mainStack->addWidget(sessionScroll);
    m_mainStack->addWidget(m_arrangement);
    middle->addWidget(m_mainStack);
    middle->setStretchFactor(0, 0);
    middle->setStretchFactor(1, 1);
    middle->setSizes({336, 900});
    vsplit->addWidget(middle);

    // ---- bottom: info | detail (clip / device)
    auto* bottom = m_bottom = new QWidget(vsplit);
    bottom->setObjectName("bottomPanel");
    auto* bl = new QHBoxLayout(bottom);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(0);
    m_info = new InfoView(bottom);
    bl->addWidget(m_info);

    // the Clip / Device tabs on their side, between the Info View and the detail (0.21): the header keeps its width for the clip overview
    auto* tabs = new QWidget(bottom);
    tabs->setObjectName("detailTabs");
    tabs->setFixedWidth(20);
    auto* tl = new QVBoxLayout(tabs);
    tl->setContentsMargins(1, 2, 1, 2);
    tl->setSpacing(2);
    m_clipTab = new SideTab(tabs);   m_clipTab->setText("Clip");     m_clipTab->setCheckable(true);   m_clipTab->setFocusPolicy(Qt::NoFocus);
    m_deviceTab = new SideTab(tabs); m_deviceTab->setText("Device"); m_deviceTab->setCheckable(true); m_deviceTab->setFocusPolicy(Qt::NoFocus);
    InfoView::attach(m_clipTab, "Clip View (Shift+Tab)", "Edit the selected clip.");
    InfoView::attach(m_deviceTab, "Device View (Shift+Tab)", "The selected track's devices.");
    tl->addWidget(m_clipTab);
    tl->addWidget(m_deviceTab);
    tl->addStretch(1);
    bl->addWidget(tabs);

    auto* detail = new QWidget(bottom);
    auto* dl = new QVBoxLayout(detail);
    dl->setContentsMargins(0, 0, 0, 0);
    dl->setSpacing(0);
    auto* header = new QWidget(detail);
    header->setObjectName("detailHeader");
    header->setFixedHeight(22);
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(8, 0, 6, 0);
    hl->setSpacing(4);
    m_detailTitle = new QLabel(header);
    m_detailTitle->setObjectName("detailTitle");
    hl->addWidget(m_detailTitle, 1);
    dl->addWidget(header);

    m_detailStack = new QStackedWidget(detail);
    m_clipView = new ClipView(m_project, m_transport, m_detailStack);
    m_deviceView = new DeviceView(m_project, m_transport, m_detailStack);
    m_detailStack->addWidget(m_clipView);
    m_detailStack->addWidget(m_deviceView);
    connect(m_deviceView, &DeviceView::editorExpansionChanged, this, [this] { if (!clipViewShown()) fitDetailPane(false); });
    dl->addWidget(m_detailStack, 1);
    bl->addWidget(detail, 1);
    vsplit->addWidget(bottom);
    vsplit->setStretchFactor(0, 3);
    vsplit->setStretchFactor(1, 1);
    vsplit->setSizes({470, 300});
    QTimer::singleShot(0, this, [this] { fitDetailPane(m_detailStack->currentIndex() == 0); });   // once the window has its size: the Clip View's pane just as high as its boxes (0.37)
    root->addWidget(vsplit, 1);
    // the strip at the bottom, a status bar: the clip and the device chain in miniature
    m_overview = new OverviewBar(m_project, m_transport, central);
    m_overview->chainOverview()->setSelectionProvider([this] { return m_deviceView->selection(); });
    m_overview->chainOverview()->setThumbProvider([this] {   // the Device View's panels, grabbed (hidden or not: render lays them out)
        QList<QPair<int, QPixmap>> out;
        for (const auto& [id, w] : m_deviceView->blockWidgets()) {
            if (!w->isVisible() && w->size() != w->sizeHint() && w->sizeHint().isValid() && w->sizeHint().width() > 0) w->resize(w->sizeHint());
            if (w->width() <= 0 || w->height() <= 0) continue;
            out << qMakePair(id, w->grab());
        }
        return out;
    });
    m_overview->clipOverview()->setViewport([this](double& f, double& l) { return m_clipView->editorViewport(f, l); },
                                            [this](double f, double l) { m_clipView->setEditorViewport(f, l); });
    connect(m_overview->clipOverview(), &ClipOverview::clicked, this, [this] { showClipView(true); });
    connect(m_overview->chainOverview(), &ChainOverview::clicked, this, [this](int deviceId) {
        showClipView(false);
        if (deviceId) m_deviceView->setSelection({deviceId});
        m_overview->refresh();
    });
    root->addWidget(m_overview);
    setCentralWidget(central);
    m_overlay = new MapOverlay(m_project, m_mapper, m_session->grid(), central);

    // ---- wiring
    connect(m_controlBar, &ControlBar::viewRequested, this, &MainWindow::showSession);
    connect(m_controlBar, &ControlBar::drawModeChanged, m_clipView->pianoRoll(), &PianoRoll::setDrawMode);
    connect(m_controlBar, &ControlBar::midiKeyboardToggled, this, &MainWindow::setMidiKeyboard);
    connect(m_controlBar, &ControlBar::followToggled, m_arrangement, &ArrangementView::setFollow);
    connect(m_arrangement, &ArrangementView::followChanged, m_controlBar, &ControlBar::setFollow);
    m_controlBar->setFollow(m_arrangement->follow());
    connect(m_controlBar, &ControlBar::keyMapToggled, this, [this](bool on) { setMapMode(on ? Mapper::Mode::Key : Mapper::Mode::Off); });
    connect(m_controlBar, &ControlBar::midiMapToggled, this, [this](bool on) { setMapMode(on ? Mapper::Mode::Midi : Mapper::Mode::Off); });
    connect(m_midi, &audio::MidiInput::noteOn, this, &MainWindow::midiNoteOn);
    connect(m_midi, &audio::MidiInput::noteOff, this, &MainWindow::midiNoteOff);
    connect(m_midi, &audio::MidiInput::controlChange, this, &MainWindow::midiControl);
    connect(m_midi, &audio::MidiInput::pitchBend, this, &MainWindow::midiPitchBend);
    connect(m_midi, &audio::MidiInput::channelPressure, this, &MainWindow::midiPressure);
    connect(m_midi, &audio::MidiInput::activity, m_controlBar, &ControlBar::flashMidiIn);
    connect(m_midi, &audio::MidiInput::machineControl, this, [this](const QString& source, int command) {
        if (m_mapper->mode() == Mapper::Mode::Off) m_surface->machineControl(source, command);
    });
    // The APC Key 25: its pads play the Session View's box, its lights follow (ApcKey25.h).
    m_surface->setDeviceChoice([this] {
        const QList<int> selected = m_deviceView->selection();
        if (!selected.isEmpty()) return selected.first();
        return m_deviceView->expandedEditor() ? m_deviceView->expandedEditor()->ctx().deviceId : 0;
    });
    m_apc->setDeviceChoice([this] {
        const QList<int> selected = m_deviceView->selection();
        if (!selected.isEmpty()) return selected.first();
        return m_deviceView->expandedEditor() ? m_deviceView->expandedEditor()->ctx().deviceId : 0;
    });
    connect(m_midi, &audio::MidiInput::portsChanged, this, &MainWindow::updateApc);
    connect(m_apc, &ApcKey25::boxChanged, this, [this] {
        updateApcBox();
        if (!m_apc->connected()) return;
        m_session->grid()->ensureVisible(m_apc->firstScene() + ApcKey25::kRows - 1);   // the box's scenes come into view
        m_session->grid()->ensureVisible(m_apc->firstScene());
    });
    connect(m_apc, &ApcKey25::clipChosen, this, [this](int t, int s) { selectTrack(t); selectClip(m_project->tracks[t].clipSlots[s]); showClipView(true); });
    connect(m_apc, &ApcKey25::trackChosen, this, &MainWindow::selectTrack);
    connect(m_apc, &ApcKey25::sceneLaunched, this, [this](int scene) { m_session->grid()->ensureVisible(scene); });
    connect(m_apc, &ApcKey25::message, this, [this](const QString& title, const QString& text) { m_info->show(title, text); });
    connect(m_project, &Project::structureChanged, this, &MainWindow::updateApcBox);
    connect(m_project, &Project::restored, this, &MainWindow::updateApcBox);
    connect(m_clipTab, &QToolButton::clicked, this, [this] { showClipView(true); });
    connect(m_deviceTab, &QToolButton::clicked, this, [this] { showClipView(false); });

    connect(m_session, &SessionView::trackSelected, this, &MainWindow::selectTrack);
    connect(m_session, &SessionView::clipSelected, this, [this](int t, int s) { selectClip(m_project->tracks[t].clipSlots[s]); showClipView(true); });
    connect(m_session, &SessionView::openDeviceView, this, [this](int t) { selectTrack(t); showClipView(false); });
    connect(m_arrangement, &ArrangementView::trackSelected, this, &MainWindow::selectTrack);
    connect(m_arrangement, &ArrangementView::clipSelected, this, [this](int, ArrangementClip* a) { selectClip(a->clip); if (m_detailStack->currentIndex() != 0) showClipView(true); });
    connect(m_arrangement, &ArrangementView::openDeviceView, this, [this](int t) { selectTrack(t); showClipView(false); });
    connect(m_arrangement, &ArrangementView::commandRequested, this, &MainWindow::runCommand);
    connect(m_session->grid(), &ClipGrid::commandRequested, this, &MainWindow::runCommand);
    connect(m_clipView, &ClipView::commandRequested, this, &MainWindow::runCommand);

    auto addItem = [this](const QString& name, QChar kind) {
        if (kind == 's') { loadSample(name); return; }
        if (kind == 'c') { loadLibraryClip(name); return; }   // a clip from the User Library
        if (kind == 'n') {   // a .nam capture: a NAM device holding it, on the selected track
            devicedrag::Payload pl;
            pl.source = devicedrag::Payload::Model;
            pl.path = name;
            pl.name = QFileInfo(name).completeBaseName();
            const devicedrag::Result r = devicedrag::drop(*m_project, pl, m_project->selectedTrack, {}, false);
            m_info->show(r.title, r.text);
            if (r.ok) showClipView(false);
            return;
        }
        if (kind == 'r') {   // an impulse response: into the track's amp / NAM / Cabinet, or a new Cabinet
            devicedrag::Payload pl;
            pl.source = devicedrag::Payload::Ir;
            pl.path = name;
            pl.name = audio::isBuiltin(name) ? audio::builtinName(name) : QFileInfo(name).completeBaseName();
            const devicedrag::Result r = devicedrag::drop(*m_project, pl, m_project->selectedTrack, {}, false);
            m_info->show(r.title, r.text);
            if (r.ok) showClipView(false);
            return;
        }
        if (kind == 'u') {   // your preset: where its devices go on the selected track
            devicedrag::Payload pl;
            pl.source = devicedrag::Payload::Preset;
            pl.path = name;
            pl.name = QFileInfo(name).completeBaseName();
            const devicedrag::Result r = devicedrag::drop(*m_project, pl, m_project->selectedTrack, {}, false);
            m_info->show(r.title, r.text);
            if (r.ok) showClipView(false);
            return;
        }
        DeviceKind k = kind == 'f' ? DeviceKind::AudioEffect : kind == 'm' ? DeviceKind::MidiEffect : DeviceKind::Instrument;
        if (!m_project->addDeviceToSelected(name, k)) {
            m_info->show("Can't add " + name, "Audio tracks and return tracks only take audio effects. Select a MIDI track first.");
            return;
        }
        showClipView(false);
        m_info->show(name + " added", "On " + m_project->trackAt(m_project->selectedTrack)->name + ".");
    };
    connect(m_browser, &Browser::itemActivated, this, addItem);
    connect(m_deviceView, &DeviceView::sampleDropped, this, [this](const QString& path, int deviceId) {
        const int trackId = m_project->trackAt(m_project->selectedTrack) ? m_project->trackAt(m_project->selectedTrack)->id : -1;
        QGuiApplication::setOverrideCursor(Qt::BusyCursor);
        samples::loadAsync({path}, this, [this, path, deviceId, trackId](QVector<SamplePtr> loaded, QStringList errors) {
            QGuiApplication::restoreOverrideCursor();
            QString error = errors.value(0), where;
            if (loaded.value(0) && loadIntoSampler(m_project->indexOfTrack(trackId), deviceId, path, &error, &where))   // in memory now
                m_info->show("Sample loaded", QFileInfo(path).fileName() + " " + where);
            else m_info->show("Can't load " + QFileInfo(path).fileName(), error);
        });
    });
    connect(m_browser, &Browser::previewRequested, this, [this](const QString& path) {
        const int request = ++m_previewRequest;   // only the last file clicked plays
        samples::loadAsync({path}, this, [this, path, request](QVector<SamplePtr> loaded, QStringList errors) {
            if (request != m_previewRequest) return;
            if (loaded.value(0)) { m_engine->previewSample(loaded[0]); m_browser->previewStarted(path); }
            else { m_browser->previewEnded(); m_info->show("Can't preview " + QFileInfo(path).fileName(), errors.value(0)); }
        });
    });
    connect(m_browser, &Browser::previewStopRequested, this, [this] {
        ++m_previewRequest;   // a file still loading doesn't start afterwards
        m_engine->stopPreview();
    });
    m_browser->setPreviewProbe([this] {
        if (m_engine->previewPlaying()) return true;
        m_engine->stopPreview();   // played to its end: let the sample go
        return false;
    });
    setAcceptDrops(true);

    connect(m_project, &Project::restored, this, [this] {
        m_clipView->setClip(m_project->selectedClip);
        updateDetailTitle();
    });
    connect(m_project, &Project::structureChanged, this, &MainWindow::updateDetailTitle);

    buildMenus();
    qApp->installEventFilter(this);
    m_engine->setOutput(PreferencesDialog::savedAudio());   // before the first start
    m_engine->setInputDevice(PreferencesDialog::inputDevice());
    m_transport->setCountInBars(PreferencesDialog::countInBars());
    m_engine->setBackgroundContent(true);   // recorded drum kits load without stalling the window
    m_engine->start();
    updateApc();

    selectClip(m_project->selectedClip);
    showSession(true);
    showClipView(true);
    m_session->grid()->setFocus();   // not the browser's search field: Space should play straight away

    // The title shows the set's name, with a * while anything saved in it has changed:
    // undoable edits (the undo stack's clean state) and the loop, quantization, metronome and cue settings.
    m_savedExtras = currentExtras();
    connect(m_project->undoStack(), &QUndoStack::cleanChanged, this, &MainWindow::updateTitle);
    connect(m_transport, &Transport::stateChanged, this, &MainWindow::updateTitle);
    connect(m_project, &Project::mixerChanged, this, &MainWindow::updateTitle);
    connect(m_session, &SessionView::columnsResized, this, &MainWindow::updateTitle);
    connect(m_mapper, &Mapper::changed, this, &MainWindow::updateTitle);   // mappings are saved with the set
    updateTitle();
}

MainWindow::~MainWindow() {
    m_surface->pads()->shutdown();
    m_surface->pads()->setSender({});
    m_apc->darken();        // the controller's lights go out with the window
    m_apc->setSender({});
    m_engine->shutdown();   // stop the audio thread before anything it reads goes away
    delete m_recovery; m_recovery = nullptr; // finish value-only writer jobs before Project is destroyed
    // The project and transport are deleted after this destructor has run, and the undo
    // stack's clear() still emits cleanChanged: nothing may call back into this window.
    disconnect(m_project->undoStack(), nullptr, this, nullptr);
    disconnect(m_project, nullptr, this, nullptr);
    disconnect(m_transport, nullptr, this, nullptr);
}

void MainWindow::buildMenus() {
    // Window-wide shortcuts; each command acts on the view that has focus.
    auto command = [this](QMenu* menu, const QString& text, const QKeySequence& key, std::function<void()> run) {
        QAction* a = menu->addAction(text);
        if (!key.isEmpty()) { a->setShortcut(key); addAction(a); }
        connect(a, &QAction::triggered, this, run);
        return a;
    };

    QMenu* file = menuBar()->addMenu("&File");
    command(file, "New Set", QKeySequence(Qt::CTRL | Qt::Key_N), [this] { fileNew(); });
    command(file, "Open…", QKeySequence(Qt::CTRL | Qt::Key_O), [this] { fileOpen(); });
    m_recentMenu = file->addMenu("Open Recent");
    connect(m_recentMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildRecentMenu);
    file->addSeparator();
    command(file, "Save", QKeySequence(Qt::CTRL | Qt::Key_S), [this] { fileSave(); });
    command(file, "Save As…", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S), [this] { fileSaveAs(); });
    command(file, "Collect All and Save", {}, [this] { fileCollectAllAndSave(); });
    command(file, "Recover Set…", {}, [this] { enableRecovery(); offerRecovery(); });
    m_backupsAction = command(file, "Show Backups", {}, [this] {
        if (m_setPath.isEmpty()) return;
        const auto folder = RecoveryManager::backupFolder(m_setPath);
        if (!QDir(folder).exists()) { m_info->show("No project backups", "Enable project backups in Preferences → Startup and save the set."); return; }
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    });
    file->addSeparator();
    command(file, "Export Audio…", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R), [this] { fileExportAudio(); });
    file->addSeparator();
    QAction* quit = file->addAction("Quit");
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    // Undo / redo. While a drag is recording, wait for it to finish. Text fields
    // keep their own Ctrl+Z (Qt lets a focused QLineEdit claim the shortcut).
    QMenu* edit = menuBar()->addMenu("&Edit");
    QUndoStack* stack = m_project->undoStack();
    QAction* undo = edit->addAction("Undo");
    undo->setShortcut(QKeySequence::Undo);
    undo->setEnabled(false);
    QAction* redo = edit->addAction("Redo");
    redo->setShortcuts({QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), QKeySequence(Qt::CTRL | Qt::Key_Y)});
    redo->setEnabled(false);
    connect(undo, &QAction::triggered, this, [this, stack] { if (!m_project->editing()) stack->undo(); });
    connect(redo, &QAction::triggered, this, [this, stack] { if (!m_project->editing()) stack->redo(); });
    connect(stack, &QUndoStack::canUndoChanged, undo, &QAction::setEnabled);
    connect(stack, &QUndoStack::canRedoChanged, redo, &QAction::setEnabled);
    connect(stack, &QUndoStack::undoTextChanged, undo, [undo](const QString& t) { undo->setText(t.isEmpty() ? "Undo" : "Undo " + t); });
    connect(stack, &QUndoStack::redoTextChanged, redo, [redo](const QString& t) { redo->setText(t.isEmpty() ? "Redo" : "Redo " + t); });
    addAction(undo);
    addAction(redo);

    edit->addSeparator();
    command(edit, "Cut", QKeySequence::Cut, [this] { cutSelection(); });
    command(edit, "Copy", QKeySequence::Copy, [this] { copySelection(); });
    command(edit, "Paste", QKeySequence::Paste, [this] { pasteSelection(); });
    command(edit, "Duplicate", QKeySequence(Qt::CTRL | Qt::Key_D), [this] { duplicateSelection(); });
    command(edit, "Delete\tDel", {}, [this] {
        switch (focusedArea()) {
        case Area::Devices:     m_deviceView->deleteSelection(); break;
        case Area::Roll:        m_clipView->pianoRoll()->deleteSelected(); break;
        case Area::Sample:      m_clipView->sampleEditor()->deleteSelection(); break;
        case Area::Arrangement: m_arrangement->deleteSelection(); break;
        default:                m_session->grid()->deleteSelection(); break;
        }
    });
    edit->addSeparator();
    command(edit, "Save Clip to User Library…", {}, [this] { saveClipToLibrary(); });
    command(edit, "Rename", QKeySequence(Qt::CTRL | Qt::Key_R), [this] { renameSelection(); });

    QMenu* create = menuBar()->addMenu("&Create");
    command(create, "Insert MIDI Track", QKeySequence(Qt::CTRL | Qt::Key_T), [this] { insertTrack(TrackType::Midi); });
    command(create, "Insert Audio Track", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T), [this] { insertTrack(TrackType::Audio); });
    command(create, "Insert Guitar Track", {}, [this] {
        const int at = std::clamp(m_project->selectedTrack + 1, 0, int(m_project->tracks.size()));
        selectTrack(m_project->insertGuitarTrack(at));
        showClipView(false);   // straight to its Tuner and amp
    });
    command(create, "Insert Return Track", QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_T), [this] {
        const int i = m_project->insertReturn();
        if (i >= 0) selectTrack(i); else m_info->show("Insert Return Track", "Sessions mixes up to 8 return tracks.");
    });
    create->addSeparator();
    command(create, "Group Tracks", QKeySequence(Qt::CTRL | Qt::Key_G), [this] { groupSelectedTracks(); });
    command(create, "Ungroup Tracks", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G), [this] { ungroupSelectedTrack(); });
    create->addSeparator();
    // Ctrl+I inserts a scene in the Session View and time in the Arrangement.
    command(create, "Insert Scene / Time", QKeySequence(Qt::CTRL | Qt::Key_I), [this] {
        const Area a = focusedArea();
        if (a == Area::Arrangement || (a == Area::Devices && m_mainStack->currentIndex() != 0)) m_arrangement->insertTime(); else m_session->grid()->insertScene();
    });
    command(create, "Capture and Insert Scene", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I), [this] { m_session->grid()->captureScene(); });
    command(create, "Add Locator", {}, [this] { showSession(false); m_arrangement->addLocator(); });

    QMenu* arrange = menuBar()->addMenu("&Arrangement");
    command(arrange, "Loop Selection", QKeySequence(Qt::CTRL | Qt::Key_L), [this] {
        if (focusedArea() == Area::Sample && m_clipView->sampleEditor()->hasSelection()) m_clipView->sampleEditor()->loopSelection();   // the sample editor's selection becomes the clip's loop
        else m_arrangement->loopSelection();
    });
    command(arrange, "Split", QKeySequence(Qt::CTRL | Qt::Key_E), [this] { m_arrangement->splitAtSelection(); });
    command(arrange, "Consolidate", QKeySequence(Qt::CTRL | Qt::Key_J), [this] { m_arrangement->consolidateSelection(); });
    arrange->addSeparator();
    command(arrange, "Insert Time", {}, [this] { m_arrangement->insertTime(); });
    command(arrange, "Delete Time", {}, [this] { m_arrangement->deleteTime(); });
    command(arrange, "Duplicate Time", {}, [this] { m_arrangement->duplicateTime(); });
    arrange->addSeparator();
    command(arrange, "Zoom to Fit", {}, [this] { m_arrangement->zoomToFit(); });

    edit->addSeparator();
    command(edit, "Preferences…", QKeySequence(Qt::CTRL | Qt::Key_Comma), [this] { showPreferences(); });

    QMenu* view = menuBar()->addMenu("&View");
    // Tab / Shift+Tab are handled in eventFilter; the "\t" text only labels them in the menu.
    QAction* sv = view->addAction("Toggle Session / Arrangement\tTab");
    connect(sv, &QAction::triggered, this, [this] { showSession(!sessionViewShown()); });
    QAction* cv = view->addAction("Toggle Clip / Device View\tShift+Tab");
    connect(cv, &QAction::triggered, this, [this] { showClipView(!clipViewShown()); });
    m_autoAction = view->addAction("Automation Mode\tA");
    m_autoAction->setCheckable(true);
    connect(m_autoAction, &QAction::triggered, this, [this](bool on) { m_arrangement->setAutomationMode(on); if (on) showSession(false); });
    connect(m_arrangement, &ArrangementView::automationModeChanged, m_autoAction, &QAction::setChecked);
    m_keyMapAction = command(view, "Key Map Mode", QKeySequence(Qt::CTRL | Qt::Key_K), [this] {
        setMapMode(m_mapper->mode() == Mapper::Mode::Key ? Mapper::Mode::Off : Mapper::Mode::Key);
    });
    m_midiMapAction = command(view, "MIDI Map Mode", QKeySequence(Qt::CTRL | Qt::Key_M), [this] {
        setMapMode(m_mapper->mode() == Mapper::Mode::Midi ? Mapper::Mode::Off : Mapper::Mode::Midi);
    });
    m_keyMapAction->setCheckable(true);
    m_midiMapAction->setCheckable(true);
    m_keysAction = view->addAction("Computer MIDI Keyboard\tM");
    m_keysAction->setCheckable(true);
    connect(m_keysAction, &QAction::triggered, this, &MainWindow::setMidiKeyboard);
    view->addSeparator();
    QAction* browserToggle = m_browserAction = view->addAction("Browser");
    browserToggle->setCheckable(true);
    browserToggle->setChecked(true);
    browserToggle->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_B));
    addAction(browserToggle);
    connect(browserToggle, &QAction::toggled, m_browser, &QWidget::setVisible);
    QAction* fs = view->addAction("Full Screen");
    fs->setShortcut(QKeySequence(Qt::Key_F11));
    fs->setShortcutContext(Qt::WindowShortcut);
    addAction(fs);   // keeps the shortcut alive even if the menu bar is hidden
    connect(fs, &QAction::triggered, this, &MainWindow::toggleFullScreen);

    // Zoom scales the whole UI (Qt's scale factor), so it applies on the next start.
    QMenu* zoom = view->addMenu("Zoom");
    auto* zoomGroup = new QActionGroup(zoom);
    const double current = QSettings("Sessions", "Sessions").value("ui/zoom", 1.0).toDouble();
    for (int pct : {100, 125, 150, 175, 200}) {
        QAction* a = zoom->addAction(QString("%1 %").arg(pct));
        a->setCheckable(true);
        a->setChecked(std::abs(current * 100 - pct) < 1);
        zoomGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, pct] {
            // 0.26: applies at once. The interface scale is fixed when the app starts, so Sessions
            // reloads itself with the set exactly as it is (unsaved changes included, the window where
            // it was); only the undo history starts over.
            QSettings("Sessions", "Sessions").setValue("ui/zoom", pct / 100.0);
            restartReopeningSet();
        });
    }

    // Theme: Dark (the original), Mid and Light, after FORM's, the tinted ones, your own, and the
    // editor; applied at once and remembered. Rebuilt when a theme of yours is saved or deleted.
    m_themeMenu = view->addMenu("Theme");
    rebuildThemeMenu();
    connect(theme::notifier(), &theme::Notifier::listChanged, this, &MainWindow::rebuildThemeMenu);
    connect(theme::notifier(), &theme::Notifier::changed, this, [this] {
        for (QAction* a : m_themeMenu->actions()) if (a->isCheckable()) a->setChecked(a->text() == theme::current().name);
    });

    QMenu* help = menuBar()->addMenu("&Help");
    command(help, "Keyboard Shortcuts", QKeySequence(Qt::CTRL | Qt::Key_Slash), [this] { showShortcuts(); });
    command(help, "Check for Updates…", {}, [this] { checkForUpdates(true); });
    command(help, "Sample Packs…", {}, [this] { auto* d = new SamplePacks(this); d->setAttribute(Qt::WA_DeleteOnClose); d->open(); });
    command(help, "About Sessions", {}, [this] { QDialog* d = aboutDialog(); d->exec(); d->deleteLater(); });
}

QDialog* MainWindow::aboutDialog() {
    auto* dlg = new QDialog(this);
    dlg->setObjectName("aboutDialog");
    dlg->setWindowTitle("About Sessions");
    auto* lay = new QVBoxLayout(dlg);
    lay->setContentsMargins(36, 30, 36, 20);
    lay->setSpacing(18);
    auto* logo = new QLabel(dlg);
    logo->setObjectName("aboutLogo");
    logo->setPixmap(appicon::logo(theme::text, 340, dlg->devicePixelRatioF()));   // the mark in the chosen icon's colours
    logo->setAlignment(Qt::AlignHCenter);
    lay->addWidget(logo);
    const QString version = QCoreApplication::applicationVersion().isEmpty() ? QString("(dev)") : QCoreApplication::applicationVersion();
    auto* words = new QLabel(QString(
        "<p align='center'><b>Sessions %1</b></p>"
        "<p>Build ideas quickly in the Session grid, launch clips and scenes in real time, then shape them into "
        "complete tracks in the Arrangement timeline.</p>"
        "<p>Work seamlessly with MIDI and audio tracks, instruments, effects, and integrated Neural Amp Modeler "
        "support, all within a streamlined workflow built to keep the focus on making music.</p>"
        "<p align='center'>Made with love by Ozzy ♥</p>"
        "<p align='center'><a href='https://github.com/ozzy-apps/sessions' style='color:%4'>GitHub repository</a></p>"
        "<p align='center'>Found a bug? Have a request? Need hardware supported?<br/>"
        "<a href='mailto:sessions@ozzy.co.nl' style='color:%4'>sessions@ozzy.co.nl</a></p>"
        "<p align='center' style='color:%3'>Built with Qt %2.</p>").arg(version, QT_VERSION_STR, theme::textFaint.name(), theme::readout.name()), dlg);
    words->setObjectName("aboutText");
    words->setWordWrap(true);
    words->setFixedWidth(400);
    words->setTextInteractionFlags(Qt::TextBrowserInteraction);
    words->setOpenExternalLinks(true);
    lay->addWidget(words);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, dlg);
    connect(buttons, &QDialogButtonBox::accepted, dlg, &QDialog::accept);
    lay->addWidget(buttons);
    return dlg;
}

void MainWindow::rebuildThemeMenu() {
    m_themeMenu->clear();
    auto* group = new QActionGroup(m_themeMenu);
    auto add = [&](const theme::Theme& t) {
        QAction* a = m_themeMenu->addAction(t.name);
        a->setCheckable(true);
        a->setChecked(theme::current().name == t.name);
        group->addAction(a);
        connect(a, &QAction::triggered, this, [t] { theme::apply(t); theme::save(t); });
    };
    const QVector<theme::Theme>& builtin = theme::builtinThemes();
    for (int i = 0; i < builtin.size(); ++i) {
        if (i == 3) m_themeMenu->addSeparator();   // the three bases, then the tinted ones
        add(builtin[i]);
    }
    const QVector<theme::Theme> mine = theme::customThemes();
    if (!mine.isEmpty()) m_themeMenu->addSeparator();
    for (const theme::Theme& t : mine) add(t);
    m_themeMenu->addSeparator();
    QAction* customize = m_themeMenu->addAction("Customize…");
    customize->setObjectName("themeCustomize");
    connect(customize, &QAction::triggered, this, &MainWindow::showThemeEditor);
}

void MainWindow::showThemeEditor() {
    if (!m_themeEditor) m_themeEditor = new ThemeEditor(this);
    m_themeEditor->load(theme::current());
    m_themeEditor->show();
    m_themeEditor->raise();
    m_themeEditor->activateWindow();
}

void MainWindow::toggleFullScreen() {
    // XOR keeps the maximized bit, so leaving full screen restores the previous state.
    setWindowState(windowState() ^ Qt::WindowFullScreen);
}

// ---- the set's file -----------------------------------------------------

namespace {
const char* kRecentKey  = "files/recent";
const char* kLastDirKey = "files/lastDir";
constexpr int kMaxRecent = 10;
QString setFilter() { return QString("Sessions sets (*.%1)").arg(setfile::kSuffix); }
}

setfile::Extras MainWindow::currentExtras() const {
    setfile::Extras x;
    setfile::fillFromProject(x, *m_project);
    x.loopOn = m_transport->loopEnabled();
    x.loopStart = m_transport->loopStart();
    x.loopLength = m_transport->loopLength();
    x.launchQuantization = m_transport->launchQuantization();
    x.metronome = m_transport->metronome();
    x.punchIn = m_transport->punchIn();
    x.punchOut = m_transport->punchOut();
    return x;
}

void MainWindow::applyExtras(const setfile::Extras& x) {
    m_transport->setLaunchQuantization(x.launchQuantization);
    m_transport->setMetronome(x.metronome);
    m_transport->setLoop(x.loopStart, x.loopLength);
    m_transport->setLoopEnabled(x.loopOn);
    m_transport->setPunchIn(x.punchIn);
    m_transport->setPunchOut(x.punchOut);
}

bool MainWindow::setModified() const {
    return m_forcedModified || !m_project->undoStack()->isClean() || currentExtras() != m_savedExtras || m_transport->recordingInProgress();
}

void MainWindow::updateTitle() {
    setWindowTitle(m_setName + "[*] - Sessions");
    setWindowModified(setModified());
    if (m_backupsAction) m_backupsAction->setEnabled(!m_setPath.isEmpty());
}

bool MainWindow::loadSetFile(const QString& file, QString* error) {
    if (m_saveInProgress) { if (error) *error = "A save is still in progress"; return false; }
    EditState state;
    setfile::Extras x;
    if (!setfile::read(file, state, x, error)) return false;   // a broken file leaves the current set alone
    releaseMidiKeys();
    m_transport->reset();
    m_project->cueVolume = x.cueVolume;
    m_project->loadEditState(state);   // also clears the undo history
    setfile::applyToProject(x, *m_project);
    applyExtras(x);
    return true;
}

// After New / Open: remember where the set lives and show its first clip.
void MainWindow::setReplaced(const QString& path, const QString& name, bool keepRecovery) {
    if (m_recovery && !keepRecovery) m_recovery->clear();
    m_setPath = path;
    m_setName = name;
    m_browser->setProjectFolder(path.isEmpty() ? QString() : QFileInfo(path).absolutePath());
    // recordings go next to the set; an unsaved set records into ~/Music/Sessions/Recorded
    m_project->recordFolder = path.isEmpty() ? QString() : QFileInfo(path).absoluteDir().filePath("Samples/Recorded");
    m_savedExtras = currentExtras();
    m_forcedModified = false;
    ClipPtr first;   // Session scenes top-down, then the timeline
    for (int sc = 0; sc < m_project->scenes.size() && !first; ++sc)
        for (const Track& t : m_project->tracks) if ((first = t.clipSlots.value(sc))) break;
    for (const Track& t : m_project->tracks) if (!first && !t.arrangement.isEmpty()) first = t.arrangement.first().clip;
    selectTrack(0);
    selectClip(first);
    m_arrangement->fitSong();
    updateTitle();
}

void MainWindow::newSet() {
    if (m_saveInProgress) return;
    releaseMidiKeys();
    m_transport->reset();
    m_project->loadEmpty();
    applyExtras(setfile::Extras{});
    setReplaced({}, "Untitled");
}

bool MainWindow::openSet(const QString& path, QString* error) {
    if (!loadSetFile(path, error)) return false;
    const QFileInfo fi(path);
    setReplaced(fi.absoluteFilePath(), fi.completeBaseName());
    addRecent(fi.absoluteFilePath());
    return true;
}

bool MainWindow::saveSet(const QString& path, QString* error) {
    if (m_saveInProgress) { if (error) *error = "A save is still in progress"; return false; }
    QScopedValueRollback<bool> saving(m_saveInProgress, true);
    const auto baseline = m_project->captureEditState(); auto state = baseline; auto extras = currentExtras(); setfile::fillFromProject(extras, *m_project);
    if (!m_transport->captureRecovery(state, error)) return false;
    if (m_recovery) {
        m_info->show("Saving set", QFileInfo(path).fileName());
        QEventLoop wait; bool finished = false, ok = false; QString why; quint64 request = 0;
        connect(m_recovery, &RecoveryManager::manualSaveFinished, &wait, [&](quint64 id, bool success, const QString& message) {
            if (id != request) return; finished = true; ok = success; why = message; wait.quit();
        });
        request = m_recovery->saveManual(state, extras, QFileInfo(path).absoluteFilePath());
        wait.exec(); // service paints, edits, transport and recording; only file replacement waits
        if (!finished || !ok) {
            const QString message = finished ? why : "Save interrupted";
            if (error) *error = message; m_info->show("Save failed", message); return false;
        }
    } else if (!setfile::saveState(path, state, extras, error)) return false;
    auto nowExtras = currentExtras(); setfile::fillFromProject(nowExtras, *m_project);
    const bool unchanged = m_project->captureEditState() == baseline && nowExtras == extras;
    const QFileInfo fi(path);
    m_setPath = fi.absoluteFilePath();
    m_setName = fi.completeBaseName();
    m_browser->setProjectFolder(fi.absolutePath());
    m_project->recordFolder = fi.absoluteDir().filePath("Samples/Recorded");
    m_savedExtras = extras;
    m_forcedModified = !unchanged;
    if (unchanged) m_project->undoStack()->setClean();
    if (m_recovery) {
        m_recovery->backup(std::move(state), std::move(extras), m_setPath);
        if (unchanged) m_recovery->clear();
        else saveRecoveryNow();
    }
    addRecent(m_setPath);
    updateTitle();
    return true;
}

bool MainWindow::reopenSnapshot(const QString& snapshot, const QString& path, const QString& name, bool modified, QString* error) {
    const bool ok = loadSetFile(snapshot, error);
    if (!ok) return false;
    QFile::remove(snapshot);
    setReplaced(path, name.isEmpty() ? QStringLiteral("Untitled") : name);
    m_forcedModified = modified;
    updateTitle();
    return true;
}

bool MainWindow::maybeSave() {
    if (m_transport->recordingInProgress()) {
        releaseMidiKeys(); m_transport->stop();
        // Let the already captured latency tail finish before asking to save a take.
        QElapsedTimer deadline; deadline.start();
        while (m_transport->audioRecording() && deadline.elapsed() < 250) {
            QEventLoop wait; QTimer::singleShot(10, &wait, &QEventLoop::quit); wait.exec(QEventLoop::ExcludeUserInputEvents);
        }
    }
    if (!setModified()) return true;
    QMessageBox box(QMessageBox::Question, "Sessions", QString("Save changes to “%1”?").arg(m_setName),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
    box.setInformativeText("If you don't save, your changes are lost.");
    box.setDefaultButton(QMessageBox::Save);
    switch (box.exec()) {
    case QMessageBox::Save:    return fileSave() && (!m_forcedModified || maybeSave());
    case QMessageBox::Discard: return true;
    default:                   return false;
    }
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (m_saveInProgress) { e->ignore(); return; }
    if (!m_closeConfirmed && !maybeSave()) { e->ignore(); return; }
    m_closeConfirmed = true;   // don't ask twice when quitting closes the window again
    if (m_recovery) m_recovery->clear();
    saveWindowState();
    e->accept();
}

void MainWindow::saveWindowState() const {
    QSettings s("Sessions", "Sessions");
    s.setValue("window/geometry", saveGeometry());   // size, place, maximized / full screen
    s.setValue("window/sessionView", sessionViewShown());
    s.setValue("window/clipView", clipViewShown());
    s.setValue("window/browser", m_browser->isVisibleTo(this));
    s.remove("window/split");      // the vertical split isn't restored any more (0.37): the Clip View's pane starts tight
    s.remove("window/clipPane");
    if (m_browser->isVisibleTo(this)) s.setValue("window/browserWidth", m_hsplit->sizes().value(0));
}

void MainWindow::restoreWindowState() {
    QSettings s("Sessions", "Sessions");
    if (s.contains("window/geometry")) restoreGeometry(s.value("window/geometry").toByteArray());
    showSession(s.value("window/sessionView", true).toBool());
    showClipView(s.value("window/clipView", true).toBool());
    m_browserAction->setChecked(s.value("window/browser", true).toBool());
    // The Clip View's pane starts tight, just as high as its boxes, every time (0.37): the height you drag it to is
    // remembered for the session (Device View and back), not across starts, which is how the dividers came up wrong.
    m_clipPaneHeight = 0;
    fitDetailPane(m_detailStack->currentIndex() == 0);
    QTimer::singleShot(0, this, [this] { fitDetailPane(m_detailStack->currentIndex() == 0); });   // again once the window has its real size
    const int browserWidth = s.value("window/browserWidth", 0).toInt();
    if (browserWidth > 0) m_hsplit->setSizes({browserWidth, std::max(200, width() - browserWidth)});
}

void MainWindow::openStartupSet() {
    const QString mode = QSettings("Sessions", "Sessions").value("startup/open", "demo").toString();
    if (mode == "empty") { newSet(); return; }
    if (mode == "last") {
        const QString last = QSettings("Sessions", "Sessions").value(kRecentKey).toStringList().value(0);
        if (!last.isEmpty() && QFileInfo::exists(last)) fileOpenPath(last);
    }
}

void MainWindow::showPreferences() {
    PreferencesDialog dlg(m_engine, m_midi, [this] { restartReopeningSet(); }, this);
    connect(&dlg, &PreferencesDialog::midiControllerSettingsChanged, m_surface, &ControlSurface::reloadSettings);
    connect(&dlg, &PreferencesDialog::midiControllerSettingsChanged, this, &MainWindow::updateApc);
    connect(&dlg, &PreferencesDialog::recoverySettingsChanged, this, [this] {
        if (!m_recoveryTimer) return;
        m_recoveryTimer->stop(); if (const int interval = RecoveryManager::intervalSeconds()) m_recoveryTimer->start(interval * 1000);
    });
    dlg.exec();
    m_transport->setCountInBars(PreferencesDialog::countInBars());
}

void MainWindow::showShortcuts() {
    static const char* rows[][2] = {
        {"Tab / Shift+Tab", "Session ↔ Arrangement / Clip View ↔ Device View"},
        {"Space / Shift+Space", "Play from the start marker or stop / play on from where it stopped"},
        {"1 2 3 4 5 6 7 8 9 0", "Launch Session scenes 1–10 (number row; typing and custom Key Maps take priority)"},
        {"Ctrl+N / Ctrl+O / Ctrl+S / Ctrl+Shift+S", "New set / Open / Save / Save As"},
        {"Ctrl+Shift+R", "Export Audio"},
        {"Ctrl+Z / Ctrl+Shift+Z", "Undo / Redo"},
        {"Ctrl+X / Ctrl+C / Ctrl+V / Ctrl+D", "Cut / copy / paste / duplicate (a selected part of an Arrangement clip too)"},
        {"Ctrl+R / Delete", "Rename / delete the selection"},
        {"Ctrl+T / Ctrl+Shift+T / Ctrl+Alt+T", "Insert MIDI / audio / return track"},
        {"Ctrl+G / Ctrl+Shift+G", "Group the selected tracks / ungroup (Ctrl+click or Shift+click track titles to select several)"},
        {"In the Device View", "Click device titles to select (Ctrl / Shift for several): Delete, Ctrl+D, Ctrl+C / Ctrl+V, Ctrl+G (into a rack), Ctrl+Shift+G. "
                               "Drag a title to move the device (Ctrl: copy), double-click to fold it"},
        {"Ctrl+I / Ctrl+Shift+I", "Insert scene (time in the Arrangement) / Capture and Insert Scene"},
        {"Ctrl+L / Ctrl+E / Ctrl+J", "Loop selection / split / consolidate (Arrangement)"},
        {"A", "Automation mode (Arrangement)"},
        {"M", "Computer MIDI keyboard (A W S E D … play notes, Z / X octave, C / V velocity)"},
        {"Ctrl+M / Ctrl+K", "MIDI Map / Key Map mode (Esc leaves)"},
        {"B", "Draw Mode"},
        {"Ctrl+1 / Ctrl+2", "Note grid finer / coarser"},
        {"Ctrl+Alt+B", "Show / hide the browser"},
        {"Ctrl+,", "Preferences"},
        {"F11", "Full screen"},
    };
    QString html = "<table cellspacing='0' cellpadding='3'>";
    for (const auto& r : rows) html += QString("<tr><td style='color:#ffa63c;padding-right:14px'>%1</td><td>%2</td></tr>").arg(r[0], r[1]);
    html += "</table><p style='color:#8a8a8a'>Hover anything to read about it in the Info View (bottom left).</p>";
    QMessageBox box(this);
    box.setWindowTitle("Keyboard Shortcuts");
    box.setTextFormat(Qt::RichText);
    box.setText(html);
    box.exec();
}

void MainWindow::fileNew() {
    if (m_saveInProgress) return;
    if (!maybeSave()) return;
    newSet();
    m_info->show("New Set", "Two MIDI tracks and two return tracks. Add an instrument from the browser, and save with Ctrl+S.");
}

void MainWindow::fileOpen() {
    if (m_saveInProgress) return;
    const QString path = QFileDialog::getOpenFileName(this, "Open Set", setDialogDir(), setFilter() + ";;All files (*)");
    if (!path.isEmpty()) fileOpenPath(path);
}

void MainWindow::fileOpenPath(const QString& path) {
    if (m_saveInProgress) {
        QTimer::singleShot(100, this, [this, path] { fileOpenPath(path); }); return;
    }
    if (!maybeSave()) return;
    QString error;
    if (!openSet(path, &error)) {
        QMessageBox::warning(this, "Open Set", QString("Couldn't open “%1”.\n\n%2").arg(QFileInfo(path).fileName(), error));
        return;
    }
    QSettings("Sessions", "Sessions").setValue(kLastDirKey, QFileInfo(path).absolutePath());
    m_info->show("Opened " + m_setName, QDir::toNativeSeparators(m_setPath));
    const QStringList missing = missingSamples();
    if (!missing.isEmpty()) {
        QStringList names;
        for (const QString& m : missing.mid(0, 12)) names << QDir::toNativeSeparators(m);
        if (missing.size() > 12) names << QString("… and %1 more").arg(missing.size() - 12);
        QMessageBox::warning(this, "Missing Samples",
            QString("%1 of the samples this set uses can't be found. Those clips stay silent until the files are back where they were:\n\n%2")
                .arg(missing.size()).arg(names.join('\n')));
    }
}

// ---- samples and audio files ------------------------------------------------

void MainWindow::loadSample(const QString& path) {
    // the file loads in the background; then it goes to the track selected now
    const int trackId = m_project->trackAt(m_project->selectedTrack) ? m_project->trackAt(m_project->selectedTrack)->id : -1;
    QGuiApplication::setOverrideCursor(Qt::BusyCursor);
    samples::loadAsync({path}, this, [this, path, trackId](QVector<SamplePtr> loaded, QStringList errors) {
        QGuiApplication::restoreOverrideCursor();
        if (!loaded.value(0)) { m_info->show("Can't load " + QFileInfo(path).fileName(), errors.value(0)); return; }
        const int sel = m_project->indexOfTrack(trackId);
        if (sel >= 0) selectTrack(sel);
        loadSampleNow(path);   // it's in memory now (`loaded` holds it)
    });
}

void MainWindow::loadSampleNow(const QString& path) {
    const int sel = m_project->selectedTrack;
    const Track* t = m_project->trackAt(sel);
    if (!t) return;
    const QString name = QFileInfo(path).fileName();
    if (m_project->isReturn(sel)) { m_info->show("Can't load " + name, "Return tracks only take audio effects. Select a MIDI or audio track."); return; }
    if (t->type == TrackType::Midi) {
        QString error, where;
        if (loadIntoSampler(sel, -1, path, &error, &where)) { showClipView(false); m_info->show("Sample loaded", name + " " + where); }
        else m_info->show("Can't load " + name, error);
        return;
    }
    // An audio track: the selected slot if it's on this track, else its first empty slot.
    ClipGrid* g = m_session->grid();
    int scene = g->selectionKind() == ClipGrid::Sel::Slot && g->selectedTrack() == sel ? g->selectedScene() : -1;
    if (scene < 0) { scene = 0; while (scene < t->clipSlots.size() && t->clipSlots[scene]) ++scene; }
    QStringList errors;
    int track = -1;
    {
        EditScope edit(m_project, "Add Audio Clip");
        track = import::toSession(*m_project, sel, scene, import::audioClips(*m_project, {path}, &errors));
    }
    if (track < 0) { m_info->show("Can't load " + name, errors.join('\n')); return; }
    selectClip(m_project->tracks[track].clipSlots.value(scene));
    showClipView(true);
}

bool MainWindow::loadIntoSampler(int track, int deviceId, const QString& path, QString* error, QString* where) {
    EditScope edit(m_project, "Load Sample");
    if (const Track* t = m_project->trackAt(track))
        for (const Device& d : t->devices)
            if ((deviceId < 0 || d.id == deviceId) && deviceType(d.name) == DeviceType::DrumRack) {
                const int pad = devui::selectedDrumPad(d.id), note = drumrack::kFirstNote + pad;
                if (where) *where = QString("is on pad %1%2 of the Drum Rack.").arg(noteName(note)).arg(note / 12 - 2);
                return import::toDrumPad(*m_project, track, d.id, pad, path, error);
            }
    if (where) *where = "plays from C3 in the Sampler.";
    return import::toSampler(*m_project, track, deviceId, path, error);
}

QStringList MainWindow::missingSamples() const {
    QStringList out;
    auto note = [&out](const QString& path, bool loaded) { if (!path.isEmpty() && !loaded && !out.contains(path)) out << path; };
    auto noteClip = [&](const ClipPtr& c) { if (!c) return; note(c->samplePath, c->sample != nullptr); for (const Take& k : c->takes) note(k.samplePath, k.sample != nullptr); };
    for (int i = 0; i < m_project->trackCount(); ++i) {
        const Track& t = *m_project->trackAt(i);
        for (const ClipPtr& c : t.clipSlots) noteClip(c);
        for (const ArrangementClip& a : t.arrangement) noteClip(a.clip);
        for (const Device& d : t.devices) {
            note(d.samplePath, d.sample != nullptr);
            for (const DrumPad& p : d.pads) note(p.samplePath, p.sample != nullptr);
        }
    }
    return out;
}

double MainWindow::songLength() const {
    double end = 0;
    for (const Track& t : m_project->tracks) for (const ArrangementClip& a : t.arrangement) end = std::max(end, a.start + a.length);
    return end;
}

bool MainWindow::exportAudio(const QString& path, const audio::RenderOptions& options, QString* error, const std::function<bool(double)>& progress) {
    audio::Engine offline(m_project);   // its own DSP state: the engine you're listening to keeps playing
    return offline.renderOffline(path, options, progress, error);
}

void MainWindow::fileExportAudio() {
    const double song = songLength();
    QDialog dlg(this);
    dlg.setWindowTitle("Export Audio");
    auto* form = new QFormLayout(&dlg);
    auto* range = new QComboBox(&dlg);
    range->addItem(QString("Whole song (1.1.1 to bar %1)").arg(int(std::ceil(song / 4)) + 1));
    range->addItem(QString("Loop region (bars %1 to %2)").arg(int(m_transport->loopStart() / 4) + 1)
                       .arg(int((m_transport->loopStart() + m_transport->loopLength()) / 4) + 1));
    if (song <= 0) range->setCurrentIndex(1);
    form->addRow("Render", range);
    auto* tail = new DragValue(&dlg);   // 0.28: a drag field, like every value in the app
    tail->setRange(0, 30);
    tail->setStep(1);
    tail->setDefault(2);
    tail->setValue(2, false);
    tail->setDragSensitivity(300);
    tail->setAccentText(false);
    tail->setFormatter([](double v) { return QString("%1 s").arg(int(v)); });
    tail->setToolTip("Extra time after the end, so reverb and delay tails ring out. Drag up or down, double-click to type.");
    form->addRow("Tail", tail);
    auto* format = new QComboBox(&dlg);
    format->addItems({"WAV 16-bit", "WAV 24-bit", "WAV 32-bit float"});
    format->setCurrentIndex(1);
    form->addRow("Format", format);
    auto* normalize = new QCheckBox("Normalize (peak at -0.1 dB)", &dlg);
    form->addRow("", normalize);
    auto* note = new QLabel("Renders the Arrangement at 48 kHz stereo. Session clips you launched aren't included.", &dlg);
    note->setWordWrap(true);
    note->setObjectName("dimLabel");
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText("Export…");
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted) return;

    audio::RenderOptions o;
    if (range->currentIndex() == 0) {
        if (song <= 0) { QMessageBox::information(this, "Export Audio", "The Arrangement is empty. Record or drag some clips onto it first."); return; }
        o.startBeat = 0; o.lengthBeats = song;
    } else {
        o.startBeat = m_transport->loopStart(); o.lengthBeats = m_transport->loopLength();
    }
    o.tailSeconds = tail->value();
    o.format = format->currentIndex() == 0 ? samples::WavFormat::Int16 : format->currentIndex() == 1 ? samples::WavFormat::Int24 : samples::WavFormat::Float32;
    o.normalize = normalize->isChecked();

    const QString dir = m_setPath.isEmpty() ? setDialogDir() : QFileInfo(m_setPath).absolutePath();
    QString path = QFileDialog::getSaveFileName(this, "Export Audio", QDir(dir).filePath(m_setName + ".wav"), "WAV audio (*.wav)");
    if (path.isEmpty()) return;
    if (!path.endsWith(".wav", Qt::CaseInsensitive)) path += ".wav";

    QProgressDialog progress("Rendering…", "Cancel", 0, 1000, this);
    progress.setWindowTitle("Export Audio");
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    QString error;
    const bool ok = exportAudio(path, o, &error, [&progress](double f) {
        progress.setValue(int(f * 1000));
        QCoreApplication::processEvents();
        return !progress.wasCanceled();
    });
    progress.reset();
    if (ok) m_info->show("Exported " + QFileInfo(path).fileName(), QDir::toNativeSeparators(path));
    else if (error != "Cancelled.") QMessageBox::warning(this, "Export Audio", "Couldn't export.\n\n" + error);
}

int MainWindow::collectSamples(QString* error) {
    if (m_setPath.isEmpty()) { if (error) *error = "Save the set first, so it has a folder."; return -1; }
    const QDir setDir = QFileInfo(m_setPath).absoluteDir();
    const QString setRoot = setDir.absolutePath() + "/";
    // Every sample file the set uses that lives outside its folder.
    QStringList outside;
    auto consider = [&](const QString& p) {
        if (!p.isEmpty() && QFileInfo::exists(p) && !QFileInfo(p).absoluteFilePath().startsWith(setRoot) && !outside.contains(p)) outside << p;
    };
    auto considerClip = [&](const ClipPtr& c) { if (!c) return; consider(c->samplePath); for (const Take& k : c->takes) consider(k.samplePath); };
    for (int i = 0; i < m_project->trackCount(); ++i) {
        const Track& t = *m_project->trackAt(i);
        for (const ClipPtr& c : t.clipSlots) considerClip(c);
        for (const ArrangementClip& a : t.arrangement) considerClip(a.clip);
        for (const Device& d : t.devices) {
            consider(d.samplePath);
            consider(d.modelPath);   // a NAM capture travels with the set too
            for (const DrumPad& p : d.pads) consider(p.samplePath);
        }
    }
    if (outside.isEmpty()) return 0;
    const QString target = setDir.filePath("Samples/Imported");
    if (!QDir().mkpath(target)) { if (error) *error = "Couldn't create " + QDir::toNativeSeparators(target); return -1; }
    QHash<QString, QString> moved;
    for (const QString& src : outside) {
        const QFileInfo fi(src);
        QString dest = QDir(target).filePath(fi.fileName());
        for (int n = 2; QFileInfo::exists(dest) && QFileInfo(dest).size() != fi.size(); ++n)   // a different file by that name
            dest = QDir(target).filePath(QString("%1 %2.%3").arg(fi.completeBaseName()).arg(n).arg(fi.suffix()));
        if (!QFileInfo::exists(dest) && !QFile::copy(src, dest)) {
            if (error) *error = QString("Couldn't copy %1 to %2.").arg(QDir::toNativeSeparators(src), QDir::toNativeSeparators(dest));
            return -1;
        }
        moved.insert(src, dest);
    }
    EditScope edit(m_project, "Collect All and Save");
    auto repoint = [&moved](QString& path, SamplePtr& sample) {
        if (!moved.contains(path)) return;
        path = moved.value(path);
        if (SamplePtr s = samples::load(path)) sample = s;
    };
    auto repointClip = [&](const ClipPtr& c) { if (!c) return; repoint(c->samplePath, c->sample); for (Take& k : c->takes) repoint(k.samplePath, k.sample); };
    for (int i = 0; i < m_project->trackCount(); ++i) {
        Track& t = *m_project->trackAt(i);
        for (ClipPtr& c : t.clipSlots) repointClip(c);
        for (ArrangementClip& a : t.arrangement) repointClip(a.clip);
        for (Device& d : t.devices) {
            repoint(d.samplePath, d.sample);
            if (moved.contains(d.modelPath)) { d.modelPath = moved.value(d.modelPath); d.model = nam::load(d.modelPath); }
            for (DrumPad& p : d.pads) repoint(p.samplePath, p.sample);
        }
    }
    emit m_project->clipChanged();
    emit m_project->structureChanged();
    return int(moved.size());
}

bool MainWindow::fileCollectAllAndSave() {
    if (m_saveInProgress) return false;
    if (m_setPath.isEmpty() && !fileSaveAs()) return false;
    QString error;
    const int copied = collectSamples(&error);
    if (copied < 0) { QMessageBox::warning(this, "Collect All and Save", error); return false; }
    if (!fileSave()) return false;
    m_info->show("Collected and saved", copied ? QString("Copied %1 sample(s) into Samples/Imported next to the set.").arg(copied)
                                               : QString("Every sample is already in the set's folder."));
    return true;
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
    for (const QUrl& u : e->mimeData()->urls())
        if (u.isLocalFile() && QFileInfo(u.toLocalFile()).suffix().compare(setfile::kSuffix, Qt::CaseInsensitive) == 0) { e->acceptProposedAction(); return; }
}

void MainWindow::dropEvent(QDropEvent* e) {
    for (const QUrl& u : e->mimeData()->urls()) {
        const QString path = u.toLocalFile();
        if (QFileInfo(path).suffix().compare(setfile::kSuffix, Qt::CaseInsensitive) != 0) continue;
        e->acceptProposedAction();
        QTimer::singleShot(0, this, [this, path] { fileOpenPath(path); });   // after the drop has finished
        return;
    }
}

bool MainWindow::fileSave() {
    if (m_saveInProgress) return false;
    if (m_setPath.isEmpty()) return fileSaveAs();
    QString error;
    if (saveSet(m_setPath, &error)) { m_info->show("Saved " + m_setName, QDir::toNativeSeparators(m_setPath)); return true; }
    QMessageBox::warning(this, "Save", QString("Couldn't save “%1”.\n\n%2\n\nTry File → Save As… somewhere else.")
                                           .arg(QFileInfo(m_setPath).fileName(), error));
    return false;
}

bool MainWindow::fileSaveAs() {
    if (m_saveInProgress) return false;
    const QString suggested = m_setPath.isEmpty() ? QDir(setDialogDir()).filePath(m_setName + "." + setfile::kSuffix) : m_setPath;
    QString path = QFileDialog::getSaveFileName(this, "Save Set As", suggested, setFilter());
    if (path.isEmpty()) return false;
    if (QFileInfo(path).suffix().compare(setfile::kSuffix, Qt::CaseInsensitive) != 0) {
        path += QString(".") + setfile::kSuffix;
        // The dialog only asked about the name as typed.
        if (QFileInfo::exists(path) && QMessageBox::question(this, "Save Set As", QString("“%1” already exists. Replace it?").arg(QFileInfo(path).fileName()),
                                                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return false;
    }
    QString error;
    if (!saveSet(path, &error)) {
        QMessageBox::warning(this, "Save Set As", QString("Couldn't save “%1”.\n\n%2").arg(QFileInfo(path).fileName(), error));
        return false;
    }
    QSettings("Sessions", "Sessions").setValue(kLastDirKey, QFileInfo(path).absolutePath());
    m_info->show("Saved " + m_setName, QDir::toNativeSeparators(m_setPath));
    return true;
}

QString MainWindow::setDialogDir() const {
    const QString last = QSettings("Sessions", "Sessions").value(kLastDirKey).toString();
    if (!last.isEmpty() && QDir(last).exists()) return last;
    const QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    return !music.isEmpty() && QDir(music).exists() ? music : QDir::homePath();
}

void MainWindow::addRecent(const QString& path) {
    QSettings settings("Sessions", "Sessions");
    QStringList list = settings.value(kRecentKey).toStringList();
    list.removeAll(path);
    list.prepend(path);
    while (list.size() > kMaxRecent) list.removeLast();
    settings.setValue(kRecentKey, list);
}

void MainWindow::rebuildRecentMenu() {
    m_recentMenu->clear();
    const QStringList list = QSettings("Sessions", "Sessions").value(kRecentKey).toStringList();
    const QString home = QDir::homePath();
    for (int i = 0; i < list.size(); ++i) {
        const QFileInfo fi(list[i]);
        QString dir = fi.absolutePath();
        if (dir == home || dir.startsWith(home + "/")) dir = "~" + dir.mid(home.size());
        QString name = fi.completeBaseName();
        name.replace("&", "&&");
        QAction* a = m_recentMenu->addAction(QString("&%1  %2   %3").arg((i + 1) % 10).arg(name, QDir::toNativeSeparators(dir)));
        a->setEnabled(fi.exists());   // moved or deleted: shown, but greyed out
        connect(a, &QAction::triggered, this, [this, path = list[i]] { fileOpenPath(path); });
    }
    if (list.isEmpty()) {
        m_recentMenu->addAction("No Recent Sets")->setEnabled(false);
        return;
    }
    m_recentMenu->addSeparator();
    QAction* clear = m_recentMenu->addAction("Clear Recent");
    connect(clear, &QAction::triggered, this, [] { QSettings("Sessions", "Sessions").remove(kRecentKey); });
}

// View > Zoom: the new instance opens a snapshot of the set as it is, saved or not, and deletes it.
void MainWindow::restartReopeningSet() {
    if (m_saveInProgress) return;
    const QString snapshot = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                 .filePath(QString("sessions-restart-%1.%2").arg(QCoreApplication::applicationPid()).arg(setfile::kSuffix));
    QString error;
    if (!setfile::save(snapshot, *m_project, currentExtras(), &error)) {
        QMessageBox::warning(this, "Zoom", "Couldn't keep the set for the restart, so Sessions didn't restart.\n\n" + error);
        return;
    }
    QStringList args{"--reopen", snapshot, "--reopen-name", m_setName};
    if (!m_setPath.isEmpty()) args << "--reopen-path" << m_setPath;
    if (setModified()) args << "--reopen-modified";
    saveWindowState();   // the new instance opens where this one is
    QProcess next;
    next.setProgram(QCoreApplication::applicationFilePath());
    next.setArguments(args);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove("QT_SCALE_FACTOR");                // ours (main() sets it from the zoom): the new instance reads the new zoom
    env.remove("SESSIONS_SCALE_FROM_SETTINGS");
    next.setProcessEnvironment(env);
    if (!next.startDetached()) {
        QFile::remove(snapshot);
        QMessageBox::warning(this, "Zoom", "Couldn't start Sessions again. The new zoom applies the next time you start it.");
        return;
    }
    m_closeConfirmed = true;   // the new instance has the set
    if (m_recovery) m_recovery->clear();
    QCoreApplication::quit();
}

bool MainWindow::eventFilter(QObject* obj, QEvent* ev) {
    if (obj == this && ev->type() == QEvent::WindowDeactivate) releaseMidiKeys();   // never leave notes hanging
    const bool mapping = m_mapper && m_mapper->mode() != Mapper::Mode::Off;
    // Map modes: a click on a mappable control selects it instead of using it.
    if (mapping && (ev->type() == QEvent::MouseButtonPress || ev->type() == QEvent::MouseButtonDblClick)
        && obj->isWidgetType() && static_cast<QWidget*>(obj)->window() == this) {
        const QPoint global = static_cast<QMouseEvent*>(ev)->globalPosition().toPoint();
        for (QWidget* w = static_cast<QWidget*>(obj); w && w != this; w = w->parentWidget()) {
            const QString t = w->property("mapTarget").toString();
            if (!t.isEmpty()) { m_mapper->select(t); return true; }
            if (w == m_session->grid()) {
                const QString cell = m_session->grid()->mapTargetAt(w->mapFromGlobal(global));
                if (!cell.isEmpty()) { m_mapper->select(cell); return true; }
                break;
            }
        }
    }
    const bool press = ev->type() == QEvent::KeyPress, release = ev->type() == QEvent::KeyRelease;
    if ((press || release) && obj->isWidgetType() && static_cast<QWidget*>(obj)->window() == this) {
        auto* k = static_cast<QKeyEvent*>(ev);
        bool chord = k->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
        bool shift = k->modifiers() & Qt::ShiftModifier;
        QWidget* f = QApplication::focusWidget();
        const bool typing = f && (f->inherits("QLineEdit") || f->inherits("QComboBox") || f->inherits("QAbstractSpinBox")
                                  || f->inherits("QTextEdit") || f->inherits("QPlainTextEdit"));
        const bool modifierKey = k->key() == Qt::Key_Shift || k->key() == Qt::Key_Control || k->key() == Qt::Key_Alt || k->key() == Qt::Key_Meta;
        if (press && !chord && (k->key() == Qt::Key_Backtab || (k->key() == Qt::Key_Tab && shift))) { showClipView(!clipViewShown()); return true; }
        if (press && !chord && k->key() == Qt::Key_Tab) { showSession(!sessionViewShown()); return true; }
        if (press && !chord && k->key() == Qt::Key_Escape && m_arrangement->clipDragActive()) { m_arrangement->endClipDrag(); return true; }   // drop a dragged Session clip
        if (mapping && press && !chord) {
            if (k->key() == Qt::Key_Escape) { setMapMode(Mapper::Mode::Off); return true; }
            if ((k->key() == Qt::Key_Delete || k->key() == Qt::Key_Backspace) && !m_mapper->selected().isEmpty()) {
                m_mapper->removeMappings(m_mapper->selected());
                return true;
            }
            if (m_mapper->mode() == Mapper::Mode::Key && !k->isAutoRepeat() && !modifierKey && !typing) { m_mapper->key(k->key(), true); return true; }
        }
        if (!mapping && !chord && !typing && !k->isAutoRepeat() && !modifierKey && m_mapper->key(k->key(), press)) return true;   // Key Map
        if (!mapping && !typing && k->modifiers() == Qt::NoModifier && k->key() >= Qt::Key_0 && k->key() <= Qt::Key_9) {
            const int scene = k->key() == Qt::Key_0 ? 9 : k->key() - Qt::Key_1;
            if (scene < m_project->scenes.size()) {
                if (press && !k->isAutoRepeat()) { m_transport->launchScene(scene); m_session->grid()->ensureVisible(scene); }
                return true;
            }
        }
        if (m_midiKeys && !chord && midiKey(k, press)) return true;
    }
    return QMainWindow::eventFilter(obj, ev);
}

// ---- MIDI from keyboards and controllers --------------------------------

void MainWindow::updateApc() {
    QVector<ApcKey25::Port> ports;
    for (const audio::MidiInput::Port& p : m_midi->ports()) ports.push_back({p.id, p.name});
    m_apc->setEnabled(QSettings("Sessions", "Sessions").value("midi/controller", "auto").toString() != "off");
    m_apc->setPorts(ports);
    QVector<NektarPads::Port> nektar;
    for (const auto& port : m_midi->ports()) nektar.push_back({port.id, port.name, m_midi->enabled(port.name)});
    m_surface->pads()->setPorts(nektar);
    updateApcBox();
}

void MainWindow::updateApcBox() {
    if (m_apc->connected()) m_session->grid()->setControllerBox(m_apc->boxTracks(), m_apc->firstScene(), ApcKey25::kRows);
    else m_session->grid()->setControllerBox({}, 0, 0);
}

void MainWindow::midiNoteOn(int channel, int pitch, int velocity) {
    if (m_mapper->note(channel, pitch, true, velocity)) return;
    if (m_surface->pads()->takesNote(m_midi->sourceName(), channel, pitch)) {
        if (m_mapper->mode() == Mapper::Mode::Off) m_surface->pads()->note(m_midi->sourceName(), channel, pitch, velocity, true);
        return;
    }
    if (m_apc->takesNote(m_midi->sourceName(), channel, pitch)) {   // the APC's pads and buttons never play notes
        if (m_mapper->mode() == Mapper::Mode::Off) m_apc->note(m_midi->sourceName(), channel, pitch, velocity, true);
        return;
    }
    const int key = channel << 8 | pitch;
    if (m_midiHeld.contains(key)) { m_sustained.remove(key); releaseMidiNote(key); }   // played again while held by the pedal
    const QList<int> targets = keyboardTargets();
    for (int t : targets) m_transport->noteOn(t, pitch, velocity / 127.0);
    m_midiHeld.insert(key, targets);
    showHeldKeys();
}

void MainWindow::midiNoteOff(int channel, int pitch) {
    if (m_mapper->note(channel, pitch, false, 0)) return;
    if (m_surface->pads()->takesNote(m_midi->sourceName(), channel, pitch)) {
        if (m_mapper->mode() == Mapper::Mode::Off) m_surface->pads()->note(m_midi->sourceName(), channel, pitch, 0, false);
        return;
    }
    if (m_apc->takesNote(m_midi->sourceName(), channel, pitch)) {
        if (m_mapper->mode() == Mapper::Mode::Off) m_apc->note(m_midi->sourceName(), channel, pitch, 0, false);
        return;
    }
    const int key = channel << 8 | pitch;
    if (!m_midiHeld.contains(key)) return;
    if (m_sustain) { m_sustained.insert(key); showHeldKeys(); return; }   // the pedal holds it (the key is up)
    releaseMidiNote(key);
}

void MainWindow::releaseMidiNote(int key) {
    for (int t : m_midiHeld.take(key)) m_transport->noteOff(t, key & 0x7F);
    showHeldKeys();
}

// The piano roll lights the keys you're holding down (not the ones the pedal sustains).
void MainWindow::showHeldKeys() {
    QSet<int> pitches;
    for (auto it = m_midiHeld.cbegin(); it != m_midiHeld.cend(); ++it) if (!m_sustained.contains(it.key())) pitches.insert(it.key() & 0x7F);
    for (const HeldKey& h : m_heldKeys) pitches.insert(h.pitch);
    m_clipView->pianoRoll()->setHeldPitches(pitches);
}

void MainWindow::midiPitchBend(int, int value) {
    for (int t : keyboardTargets()) m_transport->perform(t, 0, (value - 8192) / 8192.0);
}

void MainWindow::midiPressure(int, int value) {
    for (int t : keyboardTargets()) m_transport->perform(t, 2, value / 127.0);
}

void MainWindow::midiControl(int channel, int controller, int value) {
    const QString source = m_midi->sourceName();
    if (m_mapper->controlChange(channel, controller, value, m_apc->isRelativeKnob(source, channel, controller))) return;
    if (m_mapper->mode() == Mapper::Mode::Off && m_surface->controlChange(source, channel, controller, value)) return;
    if (m_mapper->mode() == Mapper::Mode::Off && m_apc->controlChange(source, channel, controller, value)) return;
    if (controller == 1) for (int t : keyboardTargets()) m_transport->perform(t, 1, value / 127.0);   // the mod wheel
    if (controller == 64) {   // sustain pedal
        m_sustain = value >= 64;
        if (!m_sustain) {
            const QList<int> held = m_sustained.values();
            m_sustained.clear();
            for (int key : held) releaseMidiNote(key);
        }
    }
}

void MainWindow::setMapMode(Mapper::Mode mode) {
    releaseMidiKeys();
    m_mapper->setMode(mode);
    m_overlay->setVisible(mode != Mapper::Mode::Off);
    m_overlay->raise();
    m_controlBar->setMapButtons(mode == Mapper::Mode::Key, mode == Mapper::Mode::Midi);
    m_keyMapAction->setChecked(mode == Mapper::Mode::Key);
    m_midiMapAction->setChecked(mode == Mapper::Mode::Midi);
    if (mode == Mapper::Mode::Midi)
        m_info->show("MIDI Map Mode", m_midi->available()
            ? "Click a control or a clip slot, then move a knob or fader, or hit a key or pad, on your MIDI controller. Delete removes the mapping. Esc or Ctrl+M leaves."
            : "MIDI input isn't available: " + m_midi->status());
    else if (mode == Mapper::Mode::Key)
        m_info->show("Key Map Mode", "Click a button or a clip slot, then press a computer key to play it with. Delete removes the mapping. Esc or Ctrl+K leaves.");
    else
        m_info->show("Mapping done", "Your mappings are saved with the set.");
}

// ---- computer MIDI keyboard ---------------------------------------------

void MainWindow::setMidiKeyboard(bool on) {
    if (!on) releaseMidiKeys();
    m_midiKeys = on;
    m_controlBar->setMidiKeyboard(on);
    if (m_keysAction) m_keysAction->setChecked(on);
    updateKeyboardInfo();
    m_info->show(on ? "Computer MIDI Keyboard on" : "Computer MIDI Keyboard off",
                 on ? "A W S E D F T G Y H U J K O L P play notes on the armed track (or the selected one). Z / X: octave, C / V: velocity. M turns it off."
                    : "Letter keys are shortcuts again.");
}

QList<int> MainWindow::keyboardTargets() const {
    QList<int> armed;
    for (int i = 0; i < m_project->tracks.size(); ++i)
        if (m_project->tracks[i].armed && m_project->tracks[i].type == TrackType::Midi) armed << i;
    const int sel = m_project->selectedTrack;
    if (armed.isEmpty() && sel < m_project->tracks.size() && m_project->tracks[sel].type == TrackType::Midi) armed << sel;
    return armed;
}

void MainWindow::updateKeyboardInfo() {
    m_controlBar->setMidiKeyboardInfo(QString("Octave: C%1 · Velocity: %2").arg(m_octave).arg(int(std::round(m_velocity * 127))));
}

// Returns true when the key belongs to the keyboard (so it isn't also a shortcut).
bool MainWindow::midiKey(QKeyEvent* k, bool press) {
    QWidget* f = QApplication::focusWidget();
    if (f && (f->inherits("QLineEdit") || f->inherits("QComboBox"))) return false;   // typing wins
    static const QHash<int, int> notes = {
        {Qt::Key_A, 0}, {Qt::Key_W, 1}, {Qt::Key_S, 2}, {Qt::Key_E, 3}, {Qt::Key_D, 4}, {Qt::Key_F, 5},
        {Qt::Key_T, 6}, {Qt::Key_G, 7}, {Qt::Key_Y, 8}, {Qt::Key_H, 9}, {Qt::Key_U, 10}, {Qt::Key_J, 11},
        {Qt::Key_K, 12}, {Qt::Key_O, 13}, {Qt::Key_L, 14}, {Qt::Key_P, 15},
    };
    const int key = k->key();
    if (notes.contains(key)) {
        if (k->isAutoRepeat()) return true;
        if (press) {
            if (m_heldKeys.contains(key)) return true;
            const int pitch = std::clamp((m_octave + 2) * 12 + notes.value(key), 0, 127);
            const QList<int> targets = keyboardTargets();
            for (int t : targets) m_transport->noteOn(t, pitch, m_velocity);
            m_heldKeys.insert(key, {pitch, targets});
        } else if (m_heldKeys.contains(key)) {
            const HeldKey h = m_heldKeys.take(key);
            for (int t : h.tracks) m_transport->noteOff(t, h.pitch);
        }
        showHeldKeys();
        return true;
    }
    if (!press) return key == Qt::Key_Z || key == Qt::Key_X || key == Qt::Key_C || key == Qt::Key_V;
    switch (key) {
    case Qt::Key_Z: m_octave = std::max(-2, m_octave - 1); break;
    case Qt::Key_X: m_octave = std::min(8, m_octave + 1); break;
    case Qt::Key_C: m_velocity = std::max(1 / 127.0, m_velocity - 20 / 127.0); break;
    case Qt::Key_V: m_velocity = std::min(1.0, m_velocity + 20 / 127.0); break;
    default: return false;
    }
    updateKeyboardInfo();
    m_info->show("Computer MIDI Keyboard", QString("Octave C%1, velocity %2.").arg(m_octave).arg(int(std::round(m_velocity * 127))));
    return true;
}

void MainWindow::releaseMidiKeys() {
    for (const HeldKey& h : m_heldKeys) for (int t : h.tracks) m_transport->noteOff(t, h.pitch);
    m_heldKeys.clear();
    showHeldKeys();
}

// ---- edit commands ------------------------------------------------------

void MainWindow::copySelection() {
    if (focusedArea() == Area::Devices) { m_deviceView->copySelection(); return; }
    if (focusedArea() == Area::Arrangement && m_arrangement->hasTimeSelection()) {   // a part of a clip (or a stretch of the song): its material
        m_pieces = m_arrangement->copyRange();
        if (m_pieces.isEmpty()) { m_info->show("Nothing to copy", "The time selection holds no clips."); return; }
        m_clipboard = std::make_shared<Clip>(*m_pieces.first().clip);   // a slot takes the (whole) first clip
        m_info->show("Copied", QString("%1 clip part%2. Ctrl+V pastes at the insert marker (a part keeps its whole sample: extend it and the rest is there).")
                               .arg(m_pieces.size()).arg(m_pieces.size() == 1 ? "" : "s"));
        return;
    }
    ClipPtr clip = focusedArea() == Area::Arrangement ? m_arrangement->selectedClip() : m_session->grid()->selectedClip();
    if (focusedArea() == Area::Roll || focusedArea() == Area::Sample) clip = m_project->selectedClip;
    if (!clip) return;
    m_pieces.clear();
    m_clipboard = std::make_shared<Clip>(*clip);
    m_info->show("Copied", "\"" + clip->name + "\". Select a slot, or a place on the timeline, and paste with Ctrl+V.");
}

void MainWindow::cutSelection() {
    if (focusedArea() == Area::Devices) { m_deviceView->copySelection(); m_deviceView->deleteSelection(); return; }
    if (focusedArea() == Area::Arrangement && m_arrangement->hasTimeSelection()) {
        copySelection();
        if (!m_pieces.isEmpty()) m_arrangement->cutRange();
        return;
    }
    if (focusedArea() == Area::Arrangement) {
        if (!m_arrangement->selectedClip()) return;
        copySelection();
        m_arrangement->deleteSelection();
        return;
    }
    if (focusedArea() == Area::Session || focusedArea() == Area::Other) {
        if (!m_session->grid()->selectedClip()) return;
        copySelection();
        m_session->grid()->deleteSelection();
    }
}

void MainWindow::pasteSelection() {
    if (focusedArea() == Area::Devices) { m_deviceView->paste(); return; }
    if (focusedArea() == Area::Arrangement && !m_pieces.isEmpty()) { m_arrangement->pastePieces(m_pieces); return; }
    if (!m_clipboard) return;
    if (focusedArea() == Area::Arrangement) m_arrangement->pasteClip(m_clipboard);
    else m_session->grid()->pasteClip(m_clipboard);
}

void MainWindow::duplicateSelection() {
    switch (focusedArea()) {
    case Area::Devices:     m_deviceView->duplicateSelection(); break;
    case Area::Roll:        m_clipView->pianoRoll()->duplicateSelected(); break;
    case Area::Arrangement: m_arrangement->duplicateSelection(); break;
    default:                m_session->grid()->duplicateSelection(); break;
    }
}

void MainWindow::runCommand(const QString& command) {
    if (command == "cut") cutSelection();
    else if (command == "copy") copySelection();
    else if (command == "paste") pasteSelection();
    else if (command == "saveClip") saveClipToLibrary();
    else if (command == "showSample") {
        const ClipPtr clip = m_project->selectedClip;
        if (clip && clip->isAudio() && QFileInfo::exists(clip->samplePath)) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(clip->samplePath).absolutePath()));
        else m_info->show("Show Sample", "The clip's file isn't there.");
    }
}

void MainWindow::saveClipToLibrary() {
    ClipPtr clip = m_project->selectedClip;
    if (!clip) clip = focusedArea() == Area::Arrangement ? m_arrangement->selectedClip() : m_session->grid()->selectedClip();
    if (!clip) { m_info->show("Save Clip to User Library", "Select a clip first."); return; }
    bool ok = false;
    const QString name = QInputDialog::getText(this, "Save Clip to User Library", "Name:", QLineEdit::Normal, clip->name, &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    const QString path = presets::clipPathFor(name);
    if (QFileInfo::exists(path)
        && QMessageBox::question(this, "Save Clip", "There's a clip called \"" + name + "\" in the library already. Replace it?") != QMessageBox::Yes) return;
    QString error;
    if (!presets::saveClip(path, name, *clip, &error)) { QMessageBox::warning(this, "Save Clip", "Couldn't save the clip:\n" + error); return; }
    m_info->show("Saved " + name, (error.isEmpty() ? QString() : error + " ") + "It's in the browser under User Library → Clips: drag it onto a slot or a lane, or double-click it.");
    m_browser->showUserLibrary();
}

void MainWindow::loadLibraryClip(const QString& path) {
    int t = -1;
    if (m_mainStack->currentIndex() == 0) {
        ClipGrid* g = m_session->grid();
        t = g->placeClipFiles({path}, m_project->selectedTrack, g->selectionKind() == ClipGrid::Sel::Slot ? g->selectedScene() : 0);
    } else {
        t = m_arrangement->placeClipFiles({path}, m_project->selectedTrack, m_transport->startMarker());
    }
    if (t >= 0) { selectTrack(t); m_info->show("Clip loaded", presets::clipNameOf(path) + " on " + m_project->trackAt(t)->name + "."); showClipView(true); }
}

void MainWindow::renameSelection() {
    const Area a = focusedArea();
    if (a == Area::Arrangement || (a == Area::Devices && m_mainStack->currentIndex() != 0)) m_arrangement->renameSelectedTrack();
    else m_session->grid()->renameSelection();
}

void MainWindow::insertTrack(TrackType type) {
    const int sel = m_project->selectedTrack;
    const int at = m_project->isReturn(sel) ? int(m_project->tracks.size()) : sel + 1;
    selectTrack(m_project->insertTrack(at, type));
}

void MainWindow::groupSelectedTracks() {
    if (focusedArea() == Area::Devices && m_deviceView->hasSelection()) { m_deviceView->groupSelection(); return; }   // devices into a rack
    QList<int> picked = m_project->selectedTrackIndexes();
    if (picked.isEmpty() && m_project->trackAt(m_project->selectedTrack) && !m_project->isReturn(m_project->selectedTrack))
        picked << m_project->selectedTrack;
    const int g = m_project->groupTracks(picked);
    if (g < 0) { m_info->show("Group Tracks", "Select the tracks to group first: Ctrl+click or Shift+click their titles."); return; }
    selectTrack(g);
    m_info->show("Grouped " + QString::number(m_project->membersOf(g).size()) + " track(s)",
                 "They play through " + m_project->tracks[g].name + ". Click its ▾ to fold them away; Ctrl+Shift+G undoes the group.");
}

void MainWindow::ungroupSelectedTrack() {
    if (focusedArea() == Area::Devices && m_deviceView->hasSelection()) { m_deviceView->ungroupSelection(); return; }
    int g = m_project->selectedTrack;
    if (m_project->trackAt(g) && !m_project->trackAt(g)->isGroup()) g = m_project->groupOf(g);
    if (g < 0 || !m_project->trackAt(g) || !m_project->trackAt(g)->isGroup()) { m_info->show("Ungroup Tracks", "Select a group track (or a track in one) first."); return; }
    m_project->ungroup(g);
    selectTrack(std::clamp(g, 0, m_project->trackCount() - 1));
}

bool MainWindow::sessionViewShown() const { return m_mainStack->currentIndex() == 0; }
bool MainWindow::clipViewShown() const { return m_detailStack->currentIndex() == 0; }

void MainWindow::showSession(bool session) {
    // A clip being dragged in the Session View comes along (Tab while dragging, like FORM):
    // the grid lets go of it and the Arrangement follows the mouse until the button goes up.
    // Switching back the same way returns it to the grid, drag and all.
    if (!session && sessionViewShown() && m_session->grid()->clipDragActive()) {
        int track = -1, scene = -1;
        const ClipPtr clip = m_session->grid()->takeClipDrag(&track, &scene);
        m_mainStack->setCurrentIndex(1);
        m_controlBar->setSessionViewActive(false);
        m_arrangement->setFocus();
        m_arrangement->beginClipDrag(clip, track, scene);
        return;
    }
    if (session && !sessionViewShown() && m_arrangement->clipDragActive()) {
        int track = -1, scene = -1;
        m_arrangement->endClipDrag(&track, &scene);
        m_mainStack->setCurrentIndex(0);
        m_controlBar->setSessionViewActive(true);
        m_session->grid()->resumeClipDrag(track, scene);
        return;
    }
    m_mainStack->setCurrentIndex(session ? 0 : 1);
    m_controlBar->setSessionViewActive(session);
    if (!session) m_arrangement->setFocus();
}

void MainWindow::showClipView(bool clip) {
    const bool wasClip = m_detailStack->currentIndex() == 0;
    if (wasClip && !clip && m_bottom->maximumHeight() == QWIDGETSIZE_MAX) m_clipPaneHeight = m_bottom->height();   // remember how high you dragged it
    m_detailStack->setCurrentIndex(clip ? 0 : 1);
    m_clipTab->setChecked(clip);
    m_deviceTab->setChecked(!clip);
    fitDetailPane(clip, wasClip != clip);   // the Clip View already showing keeps the height it has (0.37)
    updateDetailTitle();
}

int MainWindow::clipPaneMinHeight() const {
    const int header = 22;
    return header + (m_clipView ? m_clipView->minimumSizeHint().height() : 160);
}

// The detail pane's height: the Device View gets exactly what its devices need and its divider
// is fixed; the Clip View keeps the height you drag it to (the piano roll likes room).
int MainWindow::devicePaneHeight() const {
    const int header = 22, chainMargins = 12, rackFrame = 2;
    const int scrollBar = style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, m_deviceView);
    return header + chainMargins + rackFrame + devui::kBodyHeight + 22 + scrollBar;
}

void MainWindow::fitDetailPane(bool clip, bool resize) {
    if (!m_bottom || !m_vsplit) return;
    QSplitterHandle* handle = m_vsplit->handle(1);
    if (m_deviceView->editorExpanded() && !clip) {
        m_bottom->setMinimumHeight(300); m_bottom->setMaximumHeight(QWIDGETSIZE_MAX);
        if (handle) { handle->setEnabled(true); handle->setCursor(Qt::SplitVCursor); }
        const auto sizes = m_vsplit->sizes(); const int total = sizes.value(0) + sizes.value(1);
        const int desired = std::clamp(total - 210, 300, 610);
        m_vsplit->setSizes({std::max(100, total - desired), desired});
    } else if (clip) {
        // never squashed below its boxes (they'd overlap), always leaving the main view 120 px; a start is tight: just the boxes
        const int minH = clipPaneMinHeight();
        m_bottom->setMinimumHeight(minH);
        m_bottom->setMaximumHeight(QWIDGETSIZE_MAX);
        if (handle) { handle->setEnabled(true); handle->setCursor(Qt::SplitVCursor); }
        const QList<int> sizes = m_vsplit->sizes();
        const int total = sizes.value(0) + sizes.value(1);
        if (resize && total > 0) {
            const int want = std::clamp(m_clipPaneHeight > 0 ? m_clipPaneHeight : minH, minH, std::max(minH, total - 120));
            m_vsplit->setSizes({total - want, want});
        } else if (total > 0 && sizes.value(1) < minH) {
            m_vsplit->setSizes({total - minH, minH});
        }
    } else {
        m_bottom->setFixedHeight(devicePaneHeight());
        if (handle) { handle->setEnabled(false); handle->setCursor(Qt::ArrowCursor); }
    }
}

void MainWindow::selectTrack(int track) {
    if (track < 0 || track > m_project->trackCount()) return;   // trackCount() itself is the Master
    // selecting a track outside the multi-selection (Ctrl/Shift+click on titles) starts a new one
    if (const Track* t = m_project->trackAt(track); t && !m_project->selectedTrackIds.contains(t->id))
        m_project->selectedTrackIds = m_project->isReturn(track) || m_project->isMaster(track) ? QSet<int>{} : QSet<int>{t->id};
    if (m_project->selectedTrack != track) {
        m_project->selectedTrack = track;
        emit m_project->selectionChanged();
    }
    m_arrangement->selectTrack(track);
    m_session->grid()->selectTrack(track);
    updateDetailTitle();
}

void MainWindow::selectClip(std::shared_ptr<Clip> clip) {
    m_project->selectedClip = clip;
    // Make sure the owning track is selected so colours and the playhead match.
    int owner = m_project->trackOfClip(clip);
    if (owner >= 0) selectTrack(owner);
    m_clipView->setClip(clip);
    updateDetailTitle();
}

void MainWindow::updateDetailTitle() {
    bool clip = m_detailStack->currentIndex() == 0;
    const Track* track = m_project->trackAt(m_project->selectedTrack);
    QString t = track ? track->name : "";
    if (clip) m_detailTitle->setText(m_project->selectedClip ? m_project->selectedClip->name + "   ·   Clip View" : "Clip View");
    else m_detailTitle->setText(t + "   ·   Device View");
}

MainWindow::Area MainWindow::focusedArea() const {
    QWidget* f = QApplication::focusWidget();
    for (QWidget* w = f; w; w = w->parentWidget()) {
        if (w == m_deviceView) return Area::Devices;
        if (w == m_clipView->pianoRoll()) return Area::Roll;
        if (w == m_clipView->sampleEditor()) return Area::Sample;
        if (w == m_arrangement) return Area::Arrangement;
        if (w == m_session) return Area::Session;
    }
    return m_mainStack->currentIndex() == 0 ? Area::Session : Area::Arrangement;
}

void MainWindow::keyPressEvent(QKeyEvent* e) {
    QWidget* f = QApplication::focusWidget();
    bool typing = f && (f->inherits("QLineEdit") || f->inherits("QComboBox"));
    bool ctrl = e->modifiers() & Qt::ControlModifier;

    if (e->key() == Qt::Key_Space && !typing) {   // Space: from the start marker; Shift+Space: on from where it stopped
        if (e->modifiers() & Qt::ShiftModifier) m_transport->continuePlay(); else m_transport->togglePlay();
        e->accept(); return;
    }
    if (e->key() == Qt::Key_B && !ctrl && !typing) { m_controlBar->toggleDrawMode(); e->accept(); return; }
    if (e->key() == Qt::Key_M && !ctrl && !typing) { setMidiKeyboard(!m_midiKeys); e->accept(); return; }
    if (e->key() == Qt::Key_A && !ctrl && !typing) {
        m_arrangement->setAutomationMode(!m_arrangement->automationMode());
        if (m_arrangement->automationMode()) showSession(false);
        e->accept(); return;
    }
    if (ctrl && e->key() == Qt::Key_1) { m_clipView->pianoRoll()->setGridSize(m_clipView->pianoRoll()->gridSize() / 2); e->accept(); return; }
    if (ctrl && e->key() == Qt::Key_2) { m_clipView->pianoRoll()->setGridSize(m_clipView->pianoRoll()->gridSize() * 2); e->accept(); return; }
    if (ctrl && e->key() == Qt::Key_A && focusedArea() == Area::Roll) { m_clipView->pianoRoll()->selectAll(); e->accept(); return; }
    if (ctrl && e->key() == Qt::Key_A && focusedArea() == Area::Sample) { m_clipView->sampleEditor()->selectAll(); e->accept(); return; }
    if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && !typing) {
        // DragValue widgets handle their own reset; only act when a view area has focus.
        if (f && f->inherits("sessions::DragValue")) { QMainWindow::keyPressEvent(e); return; }
        switch (focusedArea()) {
        case Area::Devices:     m_deviceView->deleteSelection(); break;
        case Area::Roll:        m_clipView->pianoRoll()->deleteSelected(); break;
        case Area::Sample:      m_clipView->sampleEditor()->deleteSelection(); break;
        case Area::Arrangement: m_arrangement->deleteSelection(); break;
        default:                m_session->grid()->deleteSelection(); break;
        }
        e->accept(); return;
    }
    QMainWindow::keyPressEvent(e);
}

} // namespace sessions

#include <QDir>
#include <QTimer>
#include <QEventLoop>
#include <QPixmap>

namespace sessions {

void MainWindow::captureScreenshots(const QString& dir) {
    QDir().mkpath(dir);
    auto shot = [this, dir](const QString& name) { this->grab().save(dir + "/" + name + ".png"); };
    // Session + Clip View, with the Verse scene playing (the track clocks running)
    showSession(true); showClipView(true);
    m_transport->launchScene(1);
    for (int i = 0; i < 100 && m_project->tracks[1].playingSlot != 1; ++i) {   // until the engine has started it
        QEventLoop wait;
        QTimer::singleShot(20, &wait, &QEventLoop::quit);
        wait.exec();
    }
    QEventLoop settle;
    QTimer::singleShot(900, &settle, &QEventLoop::quit);   // let the clocks run a little
    settle.exec();
    shot("01-session-clip");
    m_transport->stop();
    m_transport->stop();
    // Session + Device View
    showClipView(false);
    QApplication::processEvents();
    shot("02-session-device");
    // Launch a scene so the playhead moves, then Arrangement
    m_transport->launchScene(1);
    m_transport->setBeat(10.5);
    showSession(false); showClipView(true);
    selectClip(m_project->tracks[1].clipSlots[1]);
    QApplication::processEvents();
    shot("03-arrangement-clip");
    m_transport->stop();
    m_transport->backToArrangement();
    // Automation mode: the pad's fade-in
    m_arrangement->setAutomationMode(true);
    selectTrack(3);
    showClipView(false);
    QApplication::processEvents();
    shot("04-arrangement-automation");
    m_arrangement->setAutomationMode(false);
    // Clip View tabs, on the Keys chords with a scale
    showSession(true);
    showClipView(true);
    ClipPtr chords = m_project->tracks[2].clipSlots[2];
    chords->scaleOn = true; chords->scaleRoot = 9; chords->scale = 1;   // A minor
    selectClip(chords);
    m_clipView->pianoRoll()->refreshRows();
    QApplication::processEvents();
    shot("05-clip-notes");
    chords->scaleOn = false;

    // Audio: a generated two-bar drum loop on a new audio track, its waveform in the
    // Arrangement and the sample editor, then a Sampler holding it.
    const QString loop = QDir(dir).filePath("drum loop 100.wav");
    {
        const int rate = 44100, frames = int(rate * 4.8);   // two bars at 100 BPM
        std::vector<float> st(size_t(frames) * 2, 0.0f);
        uint32_t noise = 22222;
        for (int beat = 0; beat < 8; ++beat) {
            const int at = int(beat * 0.6 * rate);
            for (int i = 0; i < int(0.35 * rate) && at + i < frames; ++i) {   // kick on every beat
                const float t = float(i) / rate;
                const float v = 0.7f * std::sin(2 * 3.14159f * (50 + 90 * std::exp(-t * 30)) * t) * std::exp(-t * 9);
                st[size_t(at + i) * 2] += v; st[size_t(at + i) * 2 + 1] += v;
            }
            const int off = at + int(0.3 * rate);
            for (int i = 0; i < int(0.08 * rate) && off + i < frames; ++i) {   // hats on the off-beats
                noise = noise * 1664525u + 1013904223u;
                const float v = 0.25f * (float(noise >> 9) / float(1 << 23) - 1.0f) * std::exp(-float(i) / rate * 50);
                st[size_t(off + i) * 2] += v; st[size_t(off + i) * 2 + 1] += v;
            }
        }
        samples::writeWav(loop, st.data(), frames, rate, samples::WavFormat::Int16);
    }
    const int audioTrack = m_project->insertTrack(int(m_project->tracks.size()), TrackType::Audio);
    import::toArrangement(*m_project, audioTrack, 8, import::audioClips(*m_project, {loop}));
    import::toSession(*m_project, audioTrack, 0, import::audioClips(*m_project, {loop}));
    ClipPtr audioClip = m_project->tracks[audioTrack].arrangement.value(0).clip;
    if (audioClip) audioClip->warpMarkers.insert(1, {4, audioClip->secondsAt(4)});
    selectTrack(audioTrack);
    showSession(false);
    showClipView(true);
    selectClip(audioClip);
    m_arrangement->fitSong();
    QApplication::processEvents();
    shot("06-arrangement-audio");
    showSession(true);
    QApplication::processEvents();
    shot("07-sample-editor");
    loadIntoSampler(1, -1, loop);   // Bass: its instrument becomes a Sampler
    selectTrack(1);
    showClipView(false);
    QApplication::processEvents();
    shot("08-sampler");
    // MIDI Map mode, with the Keys volume on CC 21 and a pad on the Chorus scene
    showSession(true);
    m_project->mappings = {{Mapping::MidiCC, 0, 21, QString("track:%1:volume").arg(m_project->tracks[2].id)},
                           {Mapping::MidiNote, 9, 36, "scene:2"}};
    setMapMode(Mapper::Mode::Midi);
    m_mapper->select(QString("track:%1:volume").arg(m_project->tracks[2].id));
    QApplication::processEvents();
    shot("09-midi-map");
    setMapMode(Mapper::Mode::Off);

    // A device chain in the Device View: the Keys track's FM Synth into an EQ, a Compressor and its Reverb.
    {
        Track& keys = m_project->tracks[2];
        keys.devices.insert(1, m_project->makeDevice("EQ", DeviceKind::AudioEffect));
        keys.devices.insert(2, m_project->makeDevice("Compressor", DeviceKind::AudioEffect));
        keys.devices[1].params[1].value = 0.62;   // a little more mid
        keys.devices[1].params[8].value = 0.25;   // low cut on
        emit m_project->structureChanged();
        selectTrack(2);
        showSession(true);
        showClipView(false);
        QApplication::processEvents();
        shot("10-device-chain");
    }

    // Milestone 7: Drums and Bass in a group (the audio track's I/O in the mixer), then folded in the
    // Arrangement; a clip envelope on the Keys chords; the loop with Warp off.
    {
        const int g = m_project->groupTracks({0, 1});
        const int audio = m_project->indexOfTrack(m_project->tracks.last().id);
        m_project->tracks[audio].armed = true;
        emit m_project->mixerChanged();
        showSession(true);
        showClipView(true);
        selectTrack(g);
        QApplication::processEvents();
        shot("11-group-and-audio-input");
        m_project->setFolded(g, true);
        showSession(false);
        QApplication::processEvents();
        shot("12-group-folded-arrangement");
        m_project->setFolded(g, false);
        m_project->tracks[audio].armed = false;
        ClipPtr keys = m_project->tracks[g + 3].clipSlots.value(2);
        if (keys) keys->envelopes = {{target::volume(), {{0, 0.75}, {2, 0.3}, {4, 0.75}, {6, 0.55}, {8, 0.75}}}};
        showSession(true);
        selectClip(keys);
        m_clipView->showEnvelope(true);
        QApplication::processEvents();
        shot("13-clip-envelope");
        m_clipView->showEnvelope(false);
        if (audioClip) { audioClip->warp = false; audioClip->grain = 0; }
        selectClip(audioClip);
        QApplication::processEvents();
        shot("14-warp-off");
    }

    // Milestone 8: a Drum Rack (the pads, a beat in the piano roll), an Auto Filter turned by a
    // macro, and punch-in / out on the Arrangement.
    {
        const int t = m_project->insertTrack(int(m_project->tracks.size()), TrackType::Midi);
        Track& drums = m_project->tracks[t];
        drums.name = "Rack";
        drums.devices = {m_project->makeDevice("Drum Rack", DeviceKind::Instrument), m_project->makeDevice("Auto Filter", DeviceKind::AudioEffect),
                         m_project->makeDevice("Macros", DeviceKind::AudioEffect)};
        import::toDrumPad(*m_project, t, drums.devices[0].id, 12, loop);   // C2: the drum loop
        Track& rack = m_project->tracks[t];
        rack.devices[2].maps = {{0, rack.devices[1].id, 0, 0.15, 0.85}, {0, rack.devices[1].id, 1, 0.1, 0.5},
                                {1, rack.devices[0].id, drumrack::param(6, drumrack::Decay), 0.2, 1.0}};
        rack.devices[2].params[0].value = 0.6;
        ClipPtr beat = m_project->newClip("Rack Beat", 4);
        for (int s16 = 0; s16 < 16; ++s16) {
            if (s16 % 4 == 0) beat->notes.push_back({36, s16 * 0.25, 0.25, 1.0, 1.0});
            if (s16 % 8 == 4) beat->notes.push_back({38, s16 * 0.25, 0.25, 0.9, 1.0});
            beat->notes.push_back({s16 % 4 == 2 ? 46 : 42, s16 * 0.25, 0.2, s16 % 2 ? 0.6 : 0.85, 1.0});
        }
        rack.clipSlots[0] = beat;
        devui::selectDrumPad(rack.devices[0].id, 12);
        emit m_project->structureChanged();
        showSession(true);
        selectTrack(t);
        showClipView(false);
        QApplication::processEvents();
        shot("15-drum-rack-and-macros");
        showClipView(true);
        selectClip(beat);
        QApplication::processEvents();
        shot("16-drum-rack-piano-roll");
        m_transport->setLoop(16, 8);
        m_transport->setPunchIn(true);
        m_transport->setPunchOut(true);
        showSession(false);
        QApplication::processEvents();
        shot("17-punch-in-out");
        m_transport->setPunchIn(false);
        m_transport->setPunchOut(false);
    }

    // Milestone 9: racks. The Keys track gets the Layered Keys rack (its Pad chain shown) and a
    // Parallel Crush rack after it.
    {
        int keys = 0;
        for (int i = 0; i < m_project->tracks.size(); ++i) if (m_project->tracks[i].name == "Keys") keys = i;
        selectTrack(keys);
        m_project->addDeviceToSelected("Layered Keys", DeviceKind::Instrument);
        m_project->addDeviceToSelected("Parallel Crush", DeviceKind::AudioEffect);
        devui::selectChain(m_project->tracks[keys].devices[0].id, 1);
        showSession(true);
        showClipView(false);
        QApplication::processEvents();
        QApplication::processEvents();
        shot("18-instrument-rack");
    }

    // Milestone 10: a guitar track, Tuner, a boost and the amp's Metal head
    {
        const int g = m_project->insertGuitarTrack(int(m_project->tracks.size()));
        Track& gt = m_project->tracks[g];
        Device boost = m_project->makeDevice("Metal Boost", DeviceKind::AudioEffect);
        gt.devices.insert(1, boost);
        Device& amp = gt.devices[2];
        amp.params[guitaramp::Head].value = choicePosition(3, guitaramp::kHeads);
        amp.params[guitaramp::Gain].value = 0.65;
        amp.params[guitaramp::Mid].value = 0.42;
        emit m_project->structureChanged();
        selectTrack(g);
        showSession(true);
        showClipView(false);
        QApplication::processEvents();
        shot("19-guitar-track");
    }

    // Milestone 11: on the guitar track the Tuner folded, the boost and the amp selected, and the
    // browser's User Library with two presets (saved into a scratch folder, not yours)
    {
        QTemporaryDir scratch;
        qputenv("SESSIONS_PRESETS", scratch.path().toLocal8Bit());
        const int g = m_project->selectedTrack;
        const int tuner = m_project->tracks[g].devices[0].id, boost = m_project->tracks[g].devices[1].id, amp = m_project->tracks[g].devices[2].id;
        presets::save(presets::pathFor("Boost into Metal Head"), "Boost into Metal Head", m_project->deviceBlocks(g, {boost, amp}));
        presets::save(presets::pathFor("My Metal Rig"), "My Metal Rig", {m_project->makeDeviceBlock("Metal Rig", DeviceKind::AudioEffect)});
        m_project->setDeviceFolded(g, tuner, true);
        m_browser->showUserLibrary();
        QApplication::processEvents();
        m_deviceView->setSelection({boost, amp});
        QApplication::processEvents();
        shot("20-devices-selected-user-library");
        qunsetenv("SESSIONS_PRESETS");
        emit presets::Library::instance()->changed();
    }

    // Milestone 12: take lanes (three passes of a loop-recorded guitar phrase, comped) and a Compressor
    // on the Bass keyed from the Drums (sidechain).
    {
        // three "takes": plucked tones on different beats of two bars, as recordings of a loop would be
        const int rate = 48000, frames = rate * 4;   // two bars at 120 BPM
        QStringList takePaths;
        for (int k = 0; k < 3; ++k) {
            std::vector<float> st(size_t(frames) * 2, 0.0f);
            for (int n = 0; n < 8; ++n) {
                if ((n + k) % 3 == 0) continue;   // each pass misses different notes
                const int at = n * rate / 2;
                const float hz = 110.0f * std::pow(2.0f, float((n * 5 + k * 7) % 12) / 12.0f);
                for (int i = 0; i < rate / 2 && at + i < frames; ++i) {
                    const float t = float(i) / rate;
                    const float v = 0.55f * std::sin(2 * 3.14159f * hz * t) * std::exp(-t * 6) * (1 - std::exp(-t * 400));
                    st[size_t(at + i) * 2] += v; st[size_t(at + i) * 2 + 1] += v;
                }
            }
            takePaths << QDir(dir).filePath(QString("take%1.wav").arg(k + 1));
            samples::writeWav(takePaths.last(), st.data(), frames, rate, samples::WavFormat::Int16);
        }
        const int gt = m_project->insertTrack(int(m_project->tracks.size()), TrackType::Audio);
        Track& gtr = m_project->tracks[gt];
        gtr.name = "Guitar Takes";
        ClipPtr takesClip = m_project->newClip("Guitar Takes", 8);
        takesClip->loop = false;
        takesClip->warpMarkers = {{0, 0}, {8, 4.0}};
        for (int k = 0; k < 3; ++k) takesClip->takes.push_back({takePaths[k], samples::load(takePaths[k]), QString("Take %1").arg(k + 1), 0});
        takesClip->samplePath = takePaths.last();
        takesClip->sample = takesClip->takes.last().sample;
        takesClip->useTake(0, 2, 0);
        takesClip->useTake(5, 6.5, 1);
        gtr.arrangement = {{0, 8, 0, takesClip}};
        emit m_project->structureChanged();
        selectTrack(gt);
        showSession(false);
        showClipView(true);
        selectClip(takesClip);
        m_clipView->showTakes(true);
        m_arrangement->fitSong();
        QApplication::processEvents();
        shot("21-takes-comping");
        m_clipView->showTakes(false);

        int bass = -1, drums = -1;
        for (int i = 0; i < m_project->tracks.size(); ++i) {
            if (m_project->tracks[i].name == "Bass") bass = i;
            if (m_project->tracks[i].name == "Drums") drums = i;
        }
        if (bass >= 0 && drums >= 0) {
            Track& b = m_project->tracks[bass];
            Device comp = m_project->makeDevice("Compressor", DeviceKind::AudioEffect);
            comp.params[0].value = 0.55; comp.params[1].value = 0.6; comp.params[3].value = 0.5;
            comp.sidechainTrack = m_project->tracks[drums].id;
            b.devices.push_back(comp);
            emit m_project->structureChanged();
            selectTrack(bass);
            showSession(true);
            showClipView(false);
            m_deviceView->setSelection({comp.id});
            QApplication::processEvents();
            shot("22-sidechain-compressor");
        }
        for (const QString& t : takePaths) QFile::remove(t);
    }

    // Milestone 13: the Monolith synth with Growl Bass on a new track, its Mod page showing the routes
    {
        const int t = m_project->insertTrack(int(m_project->tracks.size()), TrackType::Midi);
        Track& mono = m_project->tracks[t];
        mono.name = "Growl";
        mono.devices = {m_project->makeDevice("Growl Bass", DeviceKind::Instrument)};
        ClipPtr line = m_project->newClip("Growl Line", 4);
        for (int k = 0; k < 8; ++k) line->notes.push_back({k % 4 == 3 ? 43 : 36, k * 0.5, 0.45, 0.9, 1.0});
        mono.clipSlots[0] = line;
        emit m_project->structureChanged();
        selectTrack(t);
        showSession(true);
        showClipView(false);
        QApplication::processEvents();
        auto monoDrawer = [this](const char* drawer) {   // the Monolith's drawers (0.20): click one, let it slide out
            for (DevicePanel* dp : m_deviceView->panels())
                if (dp->body()) for (QPushButton* b : dp->body()->findChildren<QPushButton*>("monoDrawer")) if (b->text() == drawer && !b->isChecked()) b->click();
            QEventLoop wait;
            QTimer::singleShot(260, &wait, &QEventLoop::quit);
            wait.exec();
            QApplication::processEvents();
        };
        shot("23-monolith-main");
        monoDrawer("Env");
        shot("23-monolith-env-drawer");
        monoDrawer("Mod");
        shot("23-monolith-mod-drawer");
        monoDrawer("FX");
        shot("23-monolith-fx-drawer");
        monoDrawer("Curve");
        shot("23-monolith-curve-drawer");
    }

    // Milestone 14: the Light theme (Session + Clip View, Arrangement + Device View) and Mid, then a
    // Session clip dragged into the Arrangement through Tab: held over the Pad lane, and dropped there.
    {
        int keys = 0, pad = 0;
        for (int i = 0; i < m_project->tracks.size(); ++i) { if (m_project->tracks[i].name == "Keys") keys = i; if (m_project->tracks[i].name == "Pad") pad = i; }
        m_arrangement->setAutomationMode(false);
        theme::apply(theme::Kind::Light);
        showSession(true); showClipView(true);
        selectTrack(keys);
        selectClip(m_project->tracks[keys].clipSlots[2]);
        QApplication::processEvents();
        shot("24-light-theme-session");
        showSession(false); showClipView(false);
        QApplication::processEvents();
        shot("25-light-theme-arrangement");
        theme::apply(theme::Kind::Mid);
        showSession(true); showClipView(true);
        QApplication::processEvents();
        shot("26-mid-theme-session");
        theme::apply(theme::Kind::Dark);
        QApplication::processEvents();
        auto mouse = [](QWidget* w, QEvent::Type type, const QPointF& pos, Qt::MouseButtons buttons) {
            QMouseEvent e(type, pos, w->mapToGlobal(pos.toPoint()), Qt::LeftButton, buttons, Qt::NoModifier);
            QApplication::sendEvent(w, &e);
        };
        ClipGrid* grid = m_session->grid();
        QRect slot;
        for (const auto& cell : grid->mapCells()) if (cell.second == QString("slot:%1:2").arg(m_project->tracks[keys].id)) slot = cell.first;
        mouse(grid, QEvent::MouseButtonPress, slot.center(), Qt::LeftButton);
        mouse(grid, QEvent::MouseMove, slot.center() + QPoint(30, 0), Qt::LeftButton);
        showSession(false);   // what Tab does while a clip is dragged: the Arrangement takes it
        const QPointF over(m_arrangement->beatToX(20.3), m_arrangement->laneRect(pad).center().y());
        mouse(m_arrangement, QEvent::MouseMove, over, Qt::LeftButton);
        QApplication::processEvents();
        shot("27-session-clip-into-arrangement");
        mouse(m_arrangement, QEvent::MouseButtonRelease, over, Qt::NoButton);
        QApplication::processEvents();
        shot("28-session-clip-dropped");
    }

    // Milestone 15: a guitar track with a NAM capture (an example written next to the loop) and a cabinet IR
    const QString capture = QDir(dir).filePath("example capture.nam");
    const QString cabIr = QDir(dir).filePath("cab.wav");
    nam::writeExample(capture, 7);
    {
        const int rate = 48000, frames = 1800;   // a short cabinet-like IR: a thump that rings down
        std::vector<float> st(size_t(frames) * 2);
        uint32_t seed = 12345;
        float lp = 0;
        for (int i = 0; i < frames; ++i) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            const float noise = float(int32_t(seed)) / 2147483648.0f;
            lp += 0.25f * (noise - lp);
            const float v = (0.7f * lp + 0.3f * std::sin(2 * float(M_PI) * 95.0f * i / rate)) * std::exp(-i / 260.0f);
            st[size_t(i) * 2] = st[size_t(i) * 2 + 1] = v;
        }
        samples::writeWav(cabIr, st.data(), frames, rate, samples::WavFormat::Float32);
    }
    {
        const int g = m_project->insertGuitarTrack(int(m_project->tracks.size()));
        Track& gt = m_project->tracks[g];
        gt.name = "NAM Guitar";
        gt.devices.removeAt(1);   // the Guitar Amp: the capture takes its place
        Device capt = m_project->makeDevice("NAM", DeviceKind::AudioEffect);
        capt.modelPath = capture;
        capt.model = nam::load(capture);
        capt.samplePath = cabIr;
        capt.sample = samples::load(cabIr);
        capt.params[namdev::Input].value = 0.6;
        gt.devices.push_back(capt);
        emit m_project->structureChanged();
        selectTrack(g);
        showSession(true);
        showClipView(false);
        QApplication::processEvents();
        shot("29-nam-capture");
    }

    // Milestone 16: racks, completed. A drum track whose E1 pad plays a sub bass through a Saturator
    // (shown next to the Drum Rack) and whose snare runs through a Redux; then the Bass and Keys
    // Split rack with its key zones in the zone editor, after an Octave Layers MIDI Effect Rack.
    {
        const int t = m_project->insertTrack(int(m_project->tracks.size()), TrackType::Midi);
        m_project->tracks[t].name = "Pad Chains";
        Device drum = m_project->makeDevice("Drum Rack", DeviceKind::Instrument);
        auto onPad = [&drum](Device d, int pad) { d.rack = drum.id; d.chain = pad; return d; };
        Device sat = m_project->makeDevice("Saturator", DeviceKind::AudioEffect);
        sat.params[0].value = 0.45;
        m_project->tracks[t].devices = {drum, onPad(m_project->makeDevice("Redux", DeviceKind::AudioEffect), 2),
                                        onPad(m_project->makeDevice("Sub Bass", DeviceKind::Instrument), 4), onPad(sat, 4)};
        ClipPtr beat = m_project->newClip("Pad Beat", 4);
        for (int s16 = 0; s16 < 16; ++s16) {
            if (s16 % 4 == 0) beat->notes.push_back({36, s16 * 0.25, 0.25, 1.0, 1.0});
            if (s16 % 8 == 4) beat->notes.push_back({38, s16 * 0.25, 0.25, 0.9, 1.0});
            if (s16 % 6 == 3) beat->notes.push_back({40, s16 * 0.25, 0.5, 0.9, 1.0});
            beat->notes.push_back({42, s16 * 0.25, 0.2, s16 % 2 ? 0.6 : 0.85, 1.0});
        }
        m_project->tracks[t].clipSlots[0] = beat;
        devui::selectDrumPad(drum.id, 4);
        emit m_project->structureChanged();
        selectTrack(t);
        showSession(true);
        showClipView(false);
        QApplication::processEvents();
        QApplication::processEvents();
        shot("30-drum-rack-pad-chains");

        const int k = m_project->insertTrack(int(m_project->tracks.size()), TrackType::Midi);
        m_project->tracks[k].name = "Split Keys";
        selectTrack(k);
        m_project->addDeviceToSelected("Octave Layers", DeviceKind::MidiEffect);
        m_project->addDeviceToSelected("Bass and Keys Split", DeviceKind::Instrument);
        for (const Device& d : m_project->tracks[k].devices)
            if (deviceType(d.name) == DeviceType::InstrumentRack) {
                devui::showZones(d.id, true);
                devui::setZoneMode(d.id, ChainZones::Key);
                devui::selectChain(d.id, 1);
            }
        emit m_project->structureChanged();
        showSession(true);
        showClipView(false);
        QApplication::processEvents();
        QApplication::processEvents();
        if (auto* scroll = m_deviceView->findChild<QScrollArea*>())   // the split rack, zone editor and all, in view
            for (RackPanel* rp : m_deviceView->findChildren<RackPanel*>())
                if (const Device* d = devui::findDevice(m_project, k, rp->rackId()); d && rp->isVisibleTo(m_deviceView) && deviceType(d->name) == DeviceType::InstrumentRack)
                    scroll->horizontalScrollBar()->setValue(rp->x() - 6);
        QApplication::processEvents();
        shot("31-rack-zones");
    }

    // Milestone 19: the Drum Studio (its Kit and Grooves pages) with a Rock groove in a slot, a Cabinet
    // holding a built-in IR with the browser on Impulse Responses, the Midnight theme, the theme editor.
    {
        const int t = m_project->insertTrack(int(m_project->tracks.size()), TrackType::Midi);
        Track& drums = m_project->tracks[t];
        drums.name = "Studio Drums";
        drums.devices = {m_project->makeDevice("Drum Studio", DeviceKind::Instrument)};
        const int rock = grooves::find("Rock", "Driving");
        if (rock >= 0) drums.clipSlots[0] = grooves::makeClip(*m_project, grooves::all()[size_t(rock)], 0.3, 0.2);
        emit m_project->structureChanged();
        selectTrack(t);
        showSession(true);
        showClipView(false);
        QApplication::processEvents();
        auto studioPage = [this](const char* page) {
            for (DevicePanel* dp : m_deviceView->panels())
                if (dp->body()) for (QPushButton* b : dp->body()->findChildren<QPushButton*>("studioPage")) if (b->text() == page) b->click();
            QApplication::processEvents();
        };
        studioPage("Kit");
        shot("32-drum-studio-kit");
        studioPage("Grooves");
        shot("33-drum-studio-grooves");
        studioPage("Kit");
        selectClip(drums.clipSlots[0]);
        showClipView(true);
        QApplication::processEvents();
        shot("34-drum-studio-groove-clip");

        const int g = m_project->insertTrack(int(m_project->tracks.size()), TrackType::Audio);
        Track& cabTrack = m_project->tracks[g];
        cabTrack.name = "DI Bass";
        Device cabinet = m_project->makeDevice("Cabinet", DeviceKind::AudioEffect);
        cabinet.samplePath = "builtin:ir/5/0";
        cabinet.sample = samples::load(cabinet.samplePath);
        cabTrack.devices = {cabinet};
        emit m_project->structureChanged();
        m_browser->showCategory(Browser::irCategory());
        selectTrack(g);
        showClipView(false);
        QApplication::processEvents();
        shot("35-cabinet-and-ir-library");
        m_browser->showCategory("Instruments");

        theme::apply(theme::themeNamed("Midnight"));
        selectTrack(t);
        selectClip(m_project->tracks[t].clipSlots[0]);
        showSession(true); showClipView(true);
        QApplication::processEvents();
        shot("36-midnight-theme");
        theme::apply(theme::themeNamed("Paper"));
        showSession(false); showClipView(false);
        QApplication::processEvents();
        shot("37-paper-theme-arrangement");
        theme::apply(theme::Kind::Dark);
        ThemeEditor editor(this);
        editor.load(theme::themeNamed("Plum"));
        editor.resize(620, 640);
        editor.show();
        QApplication::processEvents();
        editor.grab().save(dir + "/38-theme-editor.png");
        editor.hide();
        theme::apply(theme::Kind::Dark);
        showSession(true); showClipView(true);
        QApplication::processEvents();
    }

    // Every device's panel on its own (devices/NN-name.png), from a scratch track holding them all.
    {
        const int t = m_project->insertTrack(int(m_project->tracks.size()), TrackType::Midi);
        for (const DeviceSpec& spec : deviceSpecs())
            if (specOf(spec.type) == &spec) m_project->tracks[t].devices.push_back(m_project->makeDevice(spec.name, spec.kind));
        Device* sampler = nullptr;
        for (Device& d : m_project->tracks[t].devices) if (deviceType(d.name) == DeviceType::Sampler) sampler = &d;
        if (sampler) { sampler->samplePath = loop; sampler->sample = samples::load(loop); }
        for (Device& d : m_project->tracks[t].devices)
            if (deviceType(d.name) == DeviceType::Nam) { d.modelPath = capture; d.model = nam::load(capture); d.samplePath = cabIr; d.sample = samples::load(cabIr); }
        QDir(dir).mkpath("devices");
        int n = 1;
        for (const Device& d : m_project->tracks[t].devices) {
            DevicePanel panel(m_project, m_transport, t, d, nullptr, {});
            panel.setStyleSheet(panel.styleSheet());
            panel.ensurePolished();
            const QString base = QString("%1/devices/%2-%3").arg(dir).arg(n++, 2, 10, QChar('0')).arg(QString(d.name).toLower().replace(' ', '-'));
            panel.grab().save(base + ".png");
            if (deviceType(d.name) == DeviceType::Monolith && panel.body())   // each drawer open too
                for (QPushButton* b : panel.body()->findChildren<QPushButton*>("monoDrawer")) {
                    b->click();
                    QEventLoop wait;
                    QTimer::singleShot(260, &wait, &QEventLoop::quit);
                    wait.exec();
                    QApplication::processEvents();
                    panel.grab().save(base + "-" + b->text().toLower() + ".png");
                }
        }
        m_project->tracks.removeAt(t);
        emit m_project->structureChanged();
    }
    QFile::remove(loop);
    QFile::remove(capture);
    QFile::remove(cabIr);
    m_closeConfirmed = true;   // the caller quits right after; never stop to ask about saving
}

} // namespace sessions
