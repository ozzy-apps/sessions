#include "Preferences.h"
#include "AppIcon.h"
#include "audio/Engine.h"
#include "audio/Driver.h"
#include "audio/MidiInput.h"
#include "audio/DrumSamples.h"
#include "audio/DrumStream.h"
#include "core/WarpPreferences.h"
#include "core/Recovery.h"
#include <QComboBox>
#include <QCheckBox>
#include <QFormLayout>
#include <QSignalBlocker>
#include <algorithm>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QButtonGroup>
#include <QSettings>
#include <QStackedWidget>
#include <QToolButton>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QVBoxLayout>
#include <cmath>

namespace sessions {

namespace {
QSettings settings() { return QSettings("Sessions", "Sessions"); }
const int kBuffers[] = {10, 20, 30, 50, 100};
constexpr int kLabelWidth = 150;   // the left column of a row: what the setting is about

// A page: its title, then rows. A row is the setting's name on the left and its controls on the
// right, with a note under them when one helps, and a hairline under the row.
QLabel* note(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setObjectName("dimLabel");
    l->setWordWrap(true);
    return l;
}

QWidget* row(const QString& label, QWidget* control, const QString& hint, QWidget* parent) {
    auto* w = new QWidget(parent);
    w->setObjectName("prefsRow");
    w->setAttribute(Qt::WA_StyledBackground);
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 12, 0, 12);
    h->setSpacing(16);
    auto* l = new QLabel(label, w);
    l->setObjectName("prefsRowLabel");
    l->setFixedWidth(kLabelWidth);
    l->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    l->setContentsMargins(0, 2, 0, 0);
    h->addWidget(l);
    auto* right = new QVBoxLayout;
    right->setContentsMargins(0, 0, 0, 0);
    right->setSpacing(6);
    right->addWidget(control);
    if (!hint.isEmpty()) right->addWidget(note(hint, w));
    h->addLayout(right, 1);
    return w;
}

QWidget* page(const QString& title, QVBoxLayout*& rows, QWidget* parent) {
    auto* w = new QWidget(parent);
    w->setObjectName("prefsPage");
    w->setAttribute(Qt::WA_StyledBackground);
    rows = new QVBoxLayout(w);
    rows->setContentsMargins(28, 22, 28, 16);
    rows->setSpacing(0);
    auto* t = new QLabel(title, w);
    t->setObjectName("prefsTitle");
    rows->addWidget(t);
    rows->addSpacing(8);
    return w;
}
}

int PreferencesDialog::bufferMs() { return QSettings("Sessions", "Sessions").value("audio/bufferMs", 30).toInt(); }
QString PreferencesDialog::outputDevice() { return QSettings("Sessions", "Sessions").value("audio/device").toString(); }
QString PreferencesDialog::inputDevice() { return QSettings("Sessions", "Sessions").value("audio/input").toString(); }
int PreferencesDialog::countInBars() { return QSettings("Sessions", "Sessions").value("record/countIn", 0).toInt(); }

namespace { int g_zoomDragUp = -1; }   // -1: not read yet
bool PreferencesDialog::zoomDragUp() {
    if (g_zoomDragUp < 0) g_zoomDragUp = QSettings("Sessions", "Sessions").value("ui/zoomDragUp", false).toBool() ? 1 : 0;
    return g_zoomDragUp == 1;
}
void PreferencesDialog::setZoomDragUp(bool up) {
    g_zoomDragUp = up ? 1 : 0;
    QSettings("Sessions", "Sessions").setValue("ui/zoomDragUp", up);
}
double PreferencesDialog::dragZoom(double dyPixels) { return std::exp((zoomDragUp() ? -dyPixels : dyPixels) * 0.012); }

PreferencesDialog::PreferencesDialog(audio::Engine* engine, audio::MidiInput* midi, std::function<void()> restartForZoom, QWidget* parent)
    : QDialog(parent), m_engine(engine), m_midi(midi) {
    setWindowTitle("Preferences");
    setObjectName("preferences");
    resize(860, 580);
    setMinimumSize(720, 480);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    lay->addLayout(body, 1);
    auto* sidebar = new QListWidget(this);   // the categories
    sidebar->setObjectName("prefsSidebar");
    sidebar->setFixedWidth(172);
    sidebar->setFocusPolicy(Qt::NoFocus);
    sidebar->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    body->addWidget(sidebar);
    auto* tabs = new QStackedWidget(this);   // one page per category
    tabs->setObjectName("prefsPages");
    body->addWidget(tabs, 1);
    auto addPage = [&](QWidget* w, const QString& name) { tabs->addWidget(w); sidebar->addItem(name); };
    QVBoxLayout* rows = nullptr;

    // ---- Audio
    auto* audioTab = page("Audio", rows, tabs);
    QVBoxLayout* outer = rows;
    m_system = new QComboBox(audioTab);
    m_system->setObjectName("audioSystem");
    m_system->addItem("PipeWire / JACK: low latency, your interface's channels", "jack");
    m_system->addItem("PulseAudio: the desktop's sound", "pulse");
    m_system->setCurrentIndex(std::max(0, m_system->findData(savedAudio().system)));
    m_system->setToolTip("PipeWire / JACK hands Sessions your audio interface's inputs and outputs with a small buffer, "
                         "so you can play a guitar through the amp and hear it at once. PulseAudio goes through the desktop's mixer.");
    outer->addWidget(row("Audio system", m_system, {}, audioTab));

    // JACK: buffer and ports
    m_jackBox = new QWidget(audioTab);
    auto* jf = new QVBoxLayout(m_jackBox);
    jf->setContentsMargins(0, 0, 0, 0);
    jf->setSpacing(0);
    m_frames = new QComboBox(m_jackBox);
    m_frames->setObjectName("audioFrames");
    for (int f : {32, 64, 128, 256, 512, 1024}) m_frames->addItem(QString("%1 samples (%2 ms)").arg(f).arg(f * 1000.0 / 48000, 0, 'f', 1), f);
    m_frames->setCurrentIndex(std::max(0, m_frames->findData(savedAudio().frames)));
    m_frames->setToolTip("Smaller: you hear what you play sooner (64 or 128 for playing guitar through the amp). Larger: safer against crackles.");
    jf->addWidget(row("Buffer", m_frames, "Smaller: you hear what you play sooner (64 or 128 for playing guitar through the amp). Larger: safer against crackles.", m_jackBox));
    const char* labels[4] = {"Main out L", "Main out R", "Input 1", "Input 2"};
    auto* outs = new QWidget(m_jackBox); auto* ol = new QVBoxLayout(outs); ol->setContentsMargins(0, 0, 0, 0); ol->setSpacing(6);
    auto* ins = new QWidget(m_jackBox);  auto* il = new QVBoxLayout(ins);  il->setContentsMargins(0, 0, 0, 0); il->setSpacing(6);
    for (int k = 0; k < 4; ++k) {
        m_jackPorts[k] = new QComboBox(m_jackBox);
        m_jackPorts[k]->setObjectName(QString("jackPort%1").arg(k));
        m_jackPorts[k]->setMinimumContentsLength(28);
        m_jackPorts[k]->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        auto* line = new QHBoxLayout; line->setContentsMargins(0, 0, 0, 0); line->setSpacing(8);
        auto* cap = new QLabel(labels[k], m_jackBox); cap->setFixedWidth(64);
        line->addWidget(cap); line->addWidget(m_jackPorts[k], 1);
        (k < 2 ? ol : il)->addLayout(line);
        connect(m_jackPorts[k], &QComboBox::activated, this, &PreferencesDialog::applyAudio);
    }
    auto* rescan = new QPushButton("Look for interfaces again", m_jackBox);
    rescan->setObjectName("rescanPorts");
    auto* rescanLine = new QHBoxLayout; rescanLine->setContentsMargins(0, 0, 0, 0); rescanLine->addWidget(rescan); rescanLine->addStretch(1);
    il->addLayout(rescanLine);
    jf->addWidget(row("Outputs", outs, "The interface channels Main L and R play to.", m_jackBox));
    jf->addWidget(row("Inputs", ins, "Input 1 and Input 2 are what an audio track's Audio From picks (1/2, 1 or 2). "
                                     "For a guitar in your interface's first input: Input 1 = its first capture channel, "
                                     "then set the track's Audio From to 1 and Monitor to In.", m_jackBox));
    outer->addWidget(m_jackBox);
    connect(m_frames, &QComboBox::activated, this, &PreferencesDialog::applyAudio);
    connect(rescan, &QPushButton::clicked, this, [this] { fillJackPorts(); });

    // PulseAudio: devices and buffer
    m_pulseBox = new QWidget(audioTab);
    auto* af = new QVBoxLayout(m_pulseBox);
    af->setContentsMargins(0, 0, 0, 0);
    af->setSpacing(0);
    m_device = new QComboBox(m_pulseBox);
    m_device->setObjectName("audioDevice");
    m_device->addItem("System default", QString());
    for (const audio::OutputDevice& d : audio::listOutputDevices()) m_device->addItem(d.description, d.name);
    const int cur = m_device->findData(outputDevice());
    m_device->setCurrentIndex(cur >= 0 ? cur : 0);
    af->addWidget(row("Output device", m_device, {}, m_pulseBox));
    m_buffer = new QComboBox(m_pulseBox);
    m_buffer->setObjectName("audioBuffer");
    for (int ms : kBuffers) m_buffer->addItem(QString("%1 ms").arg(ms), ms);
    m_buffer->setCurrentIndex(std::max(0, m_buffer->findData(bufferMs())));
    m_buffer->setToolTip("Smaller: what you play is heard sooner. Larger: safer against crackles on a busy computer.");
    af->addWidget(row("Buffer", m_buffer, "Smaller: what you play is heard sooner. Larger: safer against crackles on a busy computer.", m_pulseBox));
    m_inputDevice = new QComboBox(m_pulseBox);
    m_inputDevice->setObjectName("inputDevice");
    m_inputDevice->addItem("System default", QString());
    for (const audio::OutputDevice& d : audio::listInputDevices()) m_inputDevice->addItem(d.description, d.name);
    const int icur = m_inputDevice->findData(inputDevice());
    m_inputDevice->setCurrentIndex(icur >= 0 ? icur : 0);
    m_inputDevice->setToolTip("Where audio tracks record from: your audio interface, a microphone, or a monitor of another program's sound.");
    af->addWidget(row("Input device", m_inputDevice, "Where audio tracks record from: your audio interface, a microphone, or a monitor of another program's sound.", m_pulseBox));
    outer->addWidget(m_pulseBox);
    connect(m_inputDevice, &QComboBox::activated, this, [this] {
        const QString device = m_inputDevice->currentData().toString();
        QSettings("Sessions", "Sessions").setValue("audio/input", device);
        m_engine->setInputDevice(device);
        refreshAudioStatus();
    });
    connect(m_device, &QComboBox::activated, this, &PreferencesDialog::applyAudio);
    connect(m_buffer, &QComboBox::activated, this, &PreferencesDialog::applyAudio);

    auto* status = new QWidget(audioTab); auto* stl = new QVBoxLayout(status); stl->setContentsMargins(0, 0, 0, 0); stl->setSpacing(4);
    m_audioStatus = new QLabel(status);
    m_audioStatus->setWordWrap(true);
    stl->addWidget(m_audioStatus);
    m_inputStatus = new QLabel(status);
    m_inputStatus->setWordWrap(true);
    m_inputStatus->setObjectName("dimLabel");
    stl->addWidget(m_inputStatus);
    outer->addWidget(row("Status", status, {}, audioTab));
    // Recorded drum libraries: how much of them stays in memory.
    auto* drumBox = new QWidget(audioTab); auto* dbl = new QVBoxLayout(drumBox); dbl->setContentsMargins(0, 0, 0, 0); dbl->setSpacing(4);
    auto* drumMemory = new QComboBox(drumBox);
    drumMemory->setObjectName("drumMemoryBudget");
    for (int gb : {2, 4, 8, 16}) drumMemory->addItem(QString("Up to %1 GB").arg(gb), gb);
    drumMemory->addItem("No limit", 0);
    drumMemory->setCurrentIndex(std::max(0, drumMemory->findData(QSettings("Sessions", "Sessions").value("drums/memoryBudgetGB", 4).toInt())));
    auto* drumUsed = new QLabel(drumBox); drumUsed->setObjectName("dimLabel"); drumUsed->setWordWrap(true);
    auto showUsed = [drumUsed] {
        const double gb = audio::drums::ContentLoader::instance()->memoryUsed() / double(1ll << 30);
        const double whole = audio::drums::ContentLoader::instance()->memoryWhole() / double(1ll << 30);
        QString text = QString("In memory now: %1 GB").arg(gb, 0, 'f', gb < 10 ? 2 : 1);
        if (whole - gb >= .01) text += QString(" (%1 GB without streaming)").arg(whole, 0, 'f', whole < 10 ? 2 : 1);
        drumUsed->setText(text);
    };
    showUsed();
    connect(audio::drums::ContentLoader::instance(), &audio::drums::ContentLoader::finished, drumUsed, showUsed);
    connect(drumMemory, &QComboBox::activated, this, [drumMemory, showUsed] {
        QSettings("Sessions", "Sessions").setValue("drums/memoryBudgetGB", drumMemory->currentData().toInt());
        audio::drums::ContentLoader::instance()->setBudget(audio::drums::ContentLoader::savedBudget());
        showUsed();
    });
    auto* drumStream = new QCheckBox("Stream long recordings from disk", drumBox);
    drumStream->setObjectName("drumStream");
    drumStream->setChecked(audio::drums::streamingEnabled());
    drumStream->setToolTip("Keeps the first half second of each long recording in memory and plays the rest from disk.");
    connect(drumStream, &QCheckBox::toggled, this, [showUsed](bool on) {
        QSettings("Sessions", "Sessions").setValue("drums/stream", on);
        audio::drums::ContentLoader::instance()->reload();   // the kits in use play as before until they're read again
        showUsed();
    });
    dbl->addWidget(drumMemory); dbl->addWidget(drumStream); dbl->addWidget(drumUsed);
    outer->addWidget(row("Drum libraries", drumBox, "Recorded kits load in the background; the kit plays its previous sound until the new one is ready. "
                                                    "Kits in use always stay loaded. Kits you've switched away from stay ready for switching back until this much memory is used. "
                                                    "Streaming keeps the start of each long recording in memory and reads the rest from disk as it plays, "
                                                    "so a large kit needs a fraction of the memory.", audioTab));
    outer->addStretch(1);
    connect(m_system, &QComboBox::activated, this, [this] { showSystem(); applyAudio(); });
    addPage(audioTab, "Audio");
    fillJackPorts();
    showSystem();
    refreshAudioStatus();

    // ---- Record
    auto* recTab = page("Record / Warp", rows, tabs);
    auto* countIn = new QComboBox(recTab);
    countIn->setObjectName("countIn");
    for (int bars : {0, 1, 2, 4}) countIn->addItem(bars == 0 ? QString("None") : bars == 1 ? QString("1 Bar") : QString("%1 Bars").arg(bars), bars);
    countIn->setCurrentIndex(std::max(0, countIn->findData(countInBars())));
    countIn->setToolTip("Bars of click before a recording starts from a standstill (Arrangement Record, or ● in a slot).");
    rows->addWidget(row("Count-in", countIn, "Bars of click before a recording starts from a standstill (Arrangement Record, or ● in a slot). "
                                             "Also on the metronome button's right-click menu. During the count-in the position shows "
                                             "the bars left in red; nothing is recorded until it ends.", recTab));
    connect(countIn, &QComboBox::activated, this, [countIn] { QSettings("Sessions", "Sessions").setValue("record/countIn", countIn->currentData().toInt()); });
    auto* warpMode = new QComboBox(recTab);
    warpMode->setObjectName("defaultWarpMode");
    for (WarpMode mode : {WarpMode::Beats, WarpMode::Tones, WarpMode::Texture, WarpMode::Repitch, WarpMode::Complex, WarpMode::ComplexPro})
        warpMode->addItem(warpModeName(mode), int(mode));
    warpMode->setCurrentIndex(warpMode->findData(int(clipsettings::defaultWarpMode())));
    warpMode->setToolTip("Warp algorithm for newly imported audio clips. Existing clips keep their own settings.");
    rows->addWidget(row("Default Warp Mode", warpMode, "Used for newly imported audio clips. Existing clips keep their settings.", recTab));
    connect(warpMode, &QComboBox::activated, this, [warpMode] {
        clipsettings::setDefaultWarpMode(WarpMode(warpMode->currentData().toInt()));
    });
    rows->addStretch(1);
    addPage(recTab, "Record / Warp");

    // ---- MIDI
    auto* midiTab = page("MIDI", rows, tabs);
    auto* portsBox = new QWidget(midiTab); auto* ml = new QVBoxLayout(portsBox); ml->setContentsMargins(0, 0, 0, 0); ml->setSpacing(6);
    m_ports = new QListWidget(portsBox);
    m_ports->setObjectName("midiPorts");
    m_ports->setFixedHeight(128);
    ml->addWidget(m_ports);
    m_midiStatus = new QLabel(portsBox);
    m_midiStatus->setObjectName("dimLabel");
    m_midiStatus->setWordWrap(true);
    ml->addWidget(m_midiStatus);
    rows->addWidget(row("Inputs", portsBox, "Checked inputs play the armed track (or the selected one) and drive what you map in MIDI Map mode (Ctrl+M). "
                                            "Keyboards and controllers plugged in later are connected automatically.", midiTab));
    auto* controller = new QWidget(midiTab);
    auto* controllerForm = new QFormLayout(controller);
    controllerForm->setContentsMargins(0, 0, 0, 0);
    controllerForm->setSpacing(6);
    auto* profile = new QComboBox(controller);
    profile->setObjectName("controllerProfile");
    profile->addItem("Automatic: Nektar Impact LX49+ / LX61+, Akai APC Key 25", "auto");
    profile->addItem("Off", "off");
    profile->setCurrentIndex(std::max(0, profile->findData(settings().value("midi/controller", "auto"))));
    controllerForm->addRow("Profile", profile);
    m_controllerInput = new QComboBox(controller);
    m_controllerInput->setObjectName("controllerInput");
    controllerForm->addRow("Input", m_controllerInput);
    auto* bankLine = new QWidget(controller);
    auto* bankLayout = new QHBoxLayout(bankLine);
    bankLayout->setContentsMargins(0, 0, 0, 0);
    auto* bank = new QComboBox(bankLine);
    bank->setObjectName("controllerBank");
    for (int first = 0; first < 64; first += 8) bank->addItem(QString("Tracks %1–%2").arg(first + 1).arg(first + 8), first);
    bank->setCurrentIndex(std::max(0, bank->findData(settings().value("midi/controllerBank", 0))));
    auto* pickup = new QCheckBox("Pickup", bankLine);
    pickup->setObjectName("controllerPickup");
    pickup->setChecked(settings().value("midi/controllerPickup", true).toBool());
    pickup->setToolTip("A fader or pan knob takes control when it reaches the current value. This prevents jumps.");
    bankLayout->addWidget(bank, 1); bankLayout->addWidget(pickup);
    controllerForm->addRow("Mixer", bankLine);
    auto setControllerEnabled = [=] { bankLine->setEnabled(profile->currentData().toString() != "off"); m_controllerInput->setEnabled(profile->currentData().toString() != "off"); };
    setControllerEnabled();
    connect(profile, &QComboBox::activated, this, [=] {
        settings().setValue("midi/controller", profile->currentData());
        setControllerEnabled(); emit midiControllerSettingsChanged();
    });
    connect(m_controllerInput, &QComboBox::activated, this, [this] {
        settings().setValue("midi/controllerInput", m_controllerInput->currentData()); emit midiControllerSettingsChanged();
    });
    connect(bank, &QComboBox::activated, this, [=] {
        settings().setValue("midi/controllerBank", bank->currentData()); emit midiControllerSettingsChanged();
    });
    connect(pickup, &QCheckBox::toggled, this, [this](bool on) {
        settings().setValue("midi/controllerPickup", on); emit midiControllerSettingsChanged();
    });
    rows->addWidget(row("Control surface", controller,
        "LX49+/61+: select User Preset 2 (GM Mixer), Global MIDI channel 16. Faders 1–8: volume; knobs: pan; buttons: mute; fader 9: Master. "
        "Transport accepts MMC. APC Key 25 (either model): the pads launch the clips in the red box on the Session View; "
        "Shift + track buttons 1–4 move it, 5–8 set the knobs to Volume, Pan, Send or Device. Custom MIDI Maps take priority.", midiTab));
    rows->addStretch(1);
    connect(m_ports, &QListWidget::itemChanged, this, [this](QListWidgetItem* it) {
        m_midi->setEnabled(it->data(Qt::UserRole).toString(), it->checkState() == Qt::Checked);
    });
    connect(m_midi, &audio::MidiInput::portsChanged, this, &PreferencesDialog::refreshMidiPorts);
    addPage(midiTab, "MIDI");
    refreshMidiPorts();

    // ---- Look
    auto* lookTab = page("Look", rows, tabs);
    auto* zoom = new QComboBox(lookTab);
    zoom->setObjectName("uiZoom");
    for (int pct : {100, 125, 150, 175, 200}) zoom->addItem(QString("%1 %").arg(pct), pct / 100.0);
    const double z = settings().value("ui/zoom", 1.0).toDouble();
    for (int i = 0; i < zoom->count(); ++i) if (std::abs(zoom->itemData(i).toDouble() - z) < 0.01) zoom->setCurrentIndex(i);
    rows->addWidget(row("Zoom", zoom, "The whole interface, larger (for 1440p and 4K screens). It applies at once: Sessions reloads with the set as it is, "
                                      "unsaved changes included and the window where it was; only the undo history starts over.", lookTab));
    connect(zoom, &QComboBox::activated, this, [this, zoom, restartForZoom] {
        const double z = zoom->currentData().toDouble();
        if (std::abs(z - settings().value("ui/zoom", 1.0).toDouble()) < 0.01) return;
        QSettings("Sessions", "Sessions").setValue("ui/zoom", z);
        accept();
        if (restartForZoom) restartForZoom();
    });
    auto* zoomDir = new QComboBox(lookTab);
    zoomDir->setObjectName("zoomDragDirection");
    zoomDir->addItem("Drag down to zoom in", false);
    zoomDir->addItem("Drag up to zoom in", true);
    zoomDir->setCurrentIndex(zoomDragUp() ? 1 : 0);
    rows->addWidget(row("Zoom drag", zoomDir, "In the Arrangement's overview strip, the clip overviews and the rulers, dragging sideways scrolls and dragging up or down zooms. "
                                              "This picks which way zooms in.", lookTab));
    connect(zoomDir, &QComboBox::activated, this, [zoomDir] { setZoomDragUp(zoomDir->currentData().toBool()); });

    // The app icon: the six colour treatments of the mark, the chosen one outlined.
    auto* icons = new QWidget(lookTab);
    auto* iconRow = new QHBoxLayout(icons);
    iconRow->setContentsMargins(0, 0, 0, 0);
    iconRow->setSpacing(6);
    auto* iconGroup = new QButtonGroup(icons);
    const QString chosen = appicon::currentKey();
    for (const appicon::Variant& v : appicon::variants()) {
        auto* b = new QToolButton(icons);
        b->setObjectName("iconChoice");
        b->setProperty("iconKey", v.key);
        b->setIcon(appicon::icon(v.key));
        b->setIconSize(QSize(48, 48));
        b->setText(v.name);
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        b->setToolTip(v.blurb);
        b->setCheckable(true);
        b->setChecked(v.key == chosen);
        b->setFixedWidth(76);
        iconGroup->addButton(b);
        iconRow->addWidget(b);
        connect(b, &QToolButton::clicked, this, [this, key = v.key] {
            QString error;
            if (!appicon::setCurrent(key, &error)) QMessageBox::warning(this, "App icon", error);
        });
    }
    iconRow->addStretch(1);
    rows->addWidget(row("App icon", icons, "The icon in the window's title bar and the taskbar. The app menu follows through a desktop entry written for your user "
                                           "(~/.local/share/applications/sessions.desktop); some desktops show it after a new login. The About dialog's logo takes the same colours.", lookTab));
    rows->addStretch(1);
    addPage(lookTab, "Look");

    // ---- Startup
    auto* startTab = page("Startup", rows, tabs);
    auto* openBox = new QWidget(startTab); auto* sl = new QVBoxLayout(openBox); sl->setContentsMargins(0, 0, 0, 0); sl->setSpacing(6);
    auto* group = new QButtonGroup(startTab);
    const QString mode = settings().value("startup/open", "demo").toString();
    const std::pair<QString, QString> choices[] = {{"demo", "Open the demo set"}, {"empty", "Start with an empty set"}, {"last", "Reopen the last set"}};
    for (const auto& [key, text] : choices) {
        auto* b = new QRadioButton(text, startTab);
        b->setChecked(mode == key);
        group->addButton(b);
        sl->addWidget(b);
        connect(b, &QRadioButton::toggled, this, [key = key](bool on) { if (on) QSettings("Sessions", "Sessions").setValue("startup/open", key); });
    }
    rows->addWidget(row("Open", openBox, "When Sessions starts without a set to open.", startTab));
    auto* autosave = new QComboBox(startTab); autosave->setObjectName("recoveryInterval");
    for (int seconds : {0, 60, 120, 300, 600}) autosave->addItem(seconds ? QString("Every %1 min").arg(seconds / 60) : "Off", seconds);
    if (autosave->findData(RecoveryManager::intervalSeconds()) < 0) autosave->addItem(QString("Every %1 s").arg(RecoveryManager::intervalSeconds()), RecoveryManager::intervalSeconds());
    autosave->setCurrentIndex(autosave->findData(RecoveryManager::intervalSeconds()));
    connect(autosave, &QComboBox::currentIndexChanged, this, [this, autosave](int) {
        settings().setValue("recovery/intervalSeconds", autosave->currentData()); emit recoverySettingsChanged();
    });
    rows->addWidget(row("Recovery snapshots", autosave, "Keeps three snapshots separately from your saved set. Recover them after an interrupted session.", startTab));
    auto* backups = new QComboBox(startTab); backups->setObjectName("recoveryBackups");
    for (int count : {0, 5, 10, 20}) backups->addItem(count ? QString("Keep %1 saves").arg(count) : "Off", count);
    if (backups->findData(RecoveryManager::backupCount()) < 0) backups->addItem(QString("Keep %1 saves").arg(RecoveryManager::backupCount()), RecoveryManager::backupCount());
    backups->setCurrentIndex(backups->findData(RecoveryManager::backupCount()));
    connect(backups, &QComboBox::currentIndexChanged, this, [this, backups](int) {
        settings().setValue("recovery/backupCount", backups->currentData()); emit recoverySettingsChanged();
    });
    rows->addWidget(row("Project backups", backups, "Saved in Backups beside the set. File → Show Backups opens its folder.", startTab));
    rows->addStretch(1);
    addPage(startTab, "Startup");

    // the footer: Close, as a plain button
    auto* footer = new QWidget(this);
    footer->setObjectName("prefsFooter");
    footer->setAttribute(Qt::WA_StyledBackground);
    auto* fl = new QHBoxLayout(footer);
    fl->setContentsMargins(16, 10, 16, 10);
    fl->addStretch(1);
    auto* close = new QPushButton("Close", footer);
    close->setObjectName("prefsClose");
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    fl->addWidget(close);
    lay->addWidget(footer);

    // the page you were on last
    connect(sidebar, &QListWidget::currentRowChanged, this, [tabs](int i) {
        if (i < 0) return;
        tabs->setCurrentIndex(i);
        QSettings("Sessions", "Sessions").setValue("prefs/page", i);
    });
    sidebar->setCurrentRow(std::clamp(settings().value("prefs/page", 0).toInt(), 0, sidebar->count() - 1));
}

audio::DriverOptions PreferencesDialog::savedAudio() {
    QSettings s("Sessions", "Sessions");
    audio::DriverOptions o;
    o.device = s.value("audio/device").toString();
    o.bufferMs = s.value("audio/bufferMs", 30).toInt();
    o.system = s.value("audio/system", audio::jackAvailable() ? "jack" : "pulse").toString();
    o.frames = s.value("audio/frames", 128).toInt();
    o.outPorts = {s.value("audio/out1").toString(), s.value("audio/out2").toString()};
    o.inPorts = {s.value("audio/in1").toString(), s.value("audio/in2").toString()};
    return o;
}

// The interface's channels as PipeWire names them ("UMC202HD 192k Analog Stereo:capture_FL"),
// shown as "UMC202HD 192k Analog Stereo · capture FL".
void PreferencesDialog::fillJackPorts() {
    const audio::DriverOptions saved = savedAudio();
    const QStringList playback = audio::listJackPorts(false), capture = audio::listJackPorts(true);
    for (int k = 0; k < 4; ++k) {
        QComboBox* c = m_jackPorts[k];
        QSignalBlocker block(c);
        c->clear();
        c->addItem(k < 2 ? "Automatic (the first output)" : "Automatic (the first input)", QString());
        c->addItem("None", QString("-"));
        for (const QString& port : k < 2 ? playback : capture) {
            QString text = port;
            const int colon = port.lastIndexOf(':');
            if (colon > 0) text = port.left(colon) + "  ·  " + port.mid(colon + 1).replace('_', ' ');
            c->addItem(text, port);
        }
        const QString want = k < 2 ? saved.outPorts.value(k) : saved.inPorts.value(k - 2);
        int at = c->findData(want);
        if (at < 0 && !want.isEmpty() && want != "-") { c->addItem(want + "  (not connected now)", want); at = c->count() - 1; }
        c->setCurrentIndex(std::max(0, at));
    }
}

void PreferencesDialog::showSystem() {
    const bool jack = m_system->currentData().toString() == "jack";
    m_jackBox->setVisible(jack);
    m_pulseBox->setVisible(!jack);
}

void PreferencesDialog::applyAudio() {
    QSettings s("Sessions", "Sessions");
    s.setValue("audio/system", m_system->currentData().toString());
    s.setValue("audio/device", m_device->currentData().toString());
    s.setValue("audio/bufferMs", m_buffer->currentData().toInt());
    s.setValue("audio/frames", m_frames->currentData().toInt());
    const char* keys[4] = {"audio/out1", "audio/out2", "audio/in1", "audio/in2"};
    for (int k = 0; k < 4; ++k) s.setValue(keys[k], m_jackPorts[k]->currentData().toString());
    m_engine->setOutput(savedAudio());
    refreshAudioStatus();
}

void PreferencesDialog::refreshAudioStatus() {
    if (m_engine->duplexAudio()) {
        const double out = m_engine->latencySeconds(), in = m_engine->inputLatencySeconds();
        QString text = QString("Playing through %1. Playing through the app (input to output): about %2 ms.")
                           .arg(m_engine->outputName()).arg(int(std::round((in + out) * 1000)));
        if (!m_engine->audioProblem().isEmpty()) text += "\n\n⚠ " + m_engine->audioProblem();
        m_audioStatus->setText(text);
    } else {
        m_audioStatus->setText(m_engine->hasAudio()
            ? QString("Playing through %1, about %2 ms from key to speaker.").arg(m_engine->outputName()).arg(int(std::round(m_engine->latencySeconds() * 1000)))
            : m_engine->outputName());
    }
    m_inputStatus->setText(m_engine->inputOpen()
        ? QString("Listening to %1.").arg(m_engine->inputStatus())
        : m_engine->inputStatus() == "Off" ? QString("The input opens when you arm an audio track (or set its Monitor to In).")
                                           : m_engine->inputStatus());
}

void PreferencesDialog::refreshMidiPorts() {
    QSignalBlocker block(m_ports);
    m_ports->clear();
    if (m_controllerInput) {
        QSignalBlocker inputBlock(m_controllerInput);
        const QString selected = settings().value("midi/controllerInput").toString();
        m_controllerInput->clear();
        m_controllerInput->addItem("Automatic (Impact LX49+ / LX61+)", QString());
        for (const auto& port : m_midi->ports()) m_controllerInput->addItem(port.name, port.name);
        if (!selected.isEmpty() && m_controllerInput->findData(selected) < 0) m_controllerInput->addItem(selected + " (disconnected)", selected);
        m_controllerInput->setCurrentIndex(std::max(0, m_controllerInput->findData(selected)));
    }
    if (!m_midi->available()) {
        m_midiStatus->setText(m_midi->status());
        return;
    }
    for (const audio::MidiInput::Port& p : m_midi->ports()) {
        auto* it = new QListWidgetItem(p.name, m_ports);
        it->setData(Qt::UserRole, p.name);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(m_midi->enabled(p.name) ? Qt::Checked : Qt::Unchecked);
    }
    m_midiStatus->setText(m_ports->count() ? m_midi->status() : "No MIDI inputs found. Plug in a keyboard or controller; it appears here.");
}

} // namespace sessions
