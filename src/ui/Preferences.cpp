#include "Preferences.h"
#include "audio/Engine.h"
#include "audio/Driver.h"
#include "audio/MidiInput.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QButtonGroup>
#include <QSettings>
#include <QTabWidget>
#include <QVBoxLayout>
#include <cmath>

namespace sessions {

namespace {
QSettings settings() { return QSettings("Sessions", "Sessions"); }
const int kBuffers[] = {10, 20, 30, 50, 100};
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
    setMinimumWidth(460);
    auto* lay = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    lay->addWidget(tabs);

    // ---- Audio
    auto* audioTab = new QWidget(tabs);
    auto* outer = new QVBoxLayout(audioTab);
    auto* top = new QFormLayout;
    m_system = new QComboBox(audioTab);
    m_system->setObjectName("audioSystem");
    m_system->addItem("PipeWire / JACK: low latency, your interface's channels", "jack");
    m_system->addItem("PulseAudio: the desktop's sound", "pulse");
    m_system->setCurrentIndex(std::max(0, m_system->findData(savedAudio().system)));
    m_system->setToolTip("PipeWire / JACK hands Sessions your audio interface's inputs and outputs with a small buffer, "
                         "so you can play a guitar through the amp and hear it at once. PulseAudio goes through the desktop's mixer.");
    top->addRow("Audio system", m_system);
    outer->addLayout(top);

    // JACK: buffer and ports
    m_jackBox = new QWidget(audioTab);
    auto* jf = new QFormLayout(m_jackBox);
    jf->setContentsMargins(0, 0, 0, 0);
    m_frames = new QComboBox(m_jackBox);
    m_frames->setObjectName("audioFrames");
    for (int f : {32, 64, 128, 256, 512, 1024}) m_frames->addItem(QString("%1 samples (%2 ms)").arg(f).arg(f * 1000.0 / 48000, 0, 'f', 1), f);
    m_frames->setCurrentIndex(std::max(0, m_frames->findData(savedAudio().frames)));
    m_frames->setToolTip("Smaller: you hear what you play sooner (64 or 128 for playing guitar through the amp). Larger: safer against crackles.");
    jf->addRow("Buffer", m_frames);
    const char* labels[4] = {"Main out L", "Main out R", "Input 1", "Input 2"};
    for (int k = 0; k < 4; ++k) {
        m_jackPorts[k] = new QComboBox(m_jackBox);
        m_jackPorts[k]->setObjectName(QString("jackPort%1").arg(k));
        m_jackPorts[k]->setMinimumContentsLength(28);
        m_jackPorts[k]->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        jf->addRow(labels[k], m_jackPorts[k]);
        connect(m_jackPorts[k], &QComboBox::activated, this, &PreferencesDialog::applyAudio);
    }
    auto* rescan = new QPushButton("Look for interfaces again", m_jackBox);
    jf->addRow("", rescan);
    auto* jackNote = new QLabel("Input 1 and Input 2 are what an audio track's Audio From picks (1/2, 1 or 2). "
                                "For a guitar in your interface's first input: Input 1 = its first capture channel, "
                                "then set the track's Audio From to 1 and Monitor to In.", m_jackBox);
    jackNote->setObjectName("dimLabel");
    jackNote->setWordWrap(true);
    jf->addRow("", jackNote);
    outer->addWidget(m_jackBox);
    connect(m_frames, &QComboBox::activated, this, &PreferencesDialog::applyAudio);
    connect(rescan, &QPushButton::clicked, this, [this] { fillJackPorts(); });

    // PulseAudio: devices and buffer
    m_pulseBox = new QWidget(audioTab);
    auto* af = new QFormLayout(m_pulseBox);
    af->setContentsMargins(0, 0, 0, 0);
    m_device = new QComboBox(m_pulseBox);
    m_device->setObjectName("audioDevice");
    m_device->addItem("System default", QString());
    for (const audio::OutputDevice& d : audio::listOutputDevices()) m_device->addItem(d.description, d.name);
    const int cur = m_device->findData(outputDevice());
    m_device->setCurrentIndex(cur >= 0 ? cur : 0);
    af->addRow("Output device", m_device);
    m_buffer = new QComboBox(m_pulseBox);
    m_buffer->setObjectName("audioBuffer");
    for (int ms : kBuffers) m_buffer->addItem(QString("%1 ms").arg(ms), ms);
    m_buffer->setCurrentIndex(std::max(0, m_buffer->findData(bufferMs())));
    m_buffer->setToolTip("Smaller: what you play is heard sooner. Larger: safer against crackles on a busy computer.");
    af->addRow("Buffer", m_buffer);
    m_inputDevice = new QComboBox(m_pulseBox);
    m_inputDevice->setObjectName("inputDevice");
    m_inputDevice->addItem("System default", QString());
    for (const audio::OutputDevice& d : audio::listInputDevices()) m_inputDevice->addItem(d.description, d.name);
    const int icur = m_inputDevice->findData(inputDevice());
    m_inputDevice->setCurrentIndex(icur >= 0 ? icur : 0);
    m_inputDevice->setToolTip("Where audio tracks record from: your audio interface, a microphone, or a monitor of another program's sound.");
    af->addRow("Input device", m_inputDevice);
    outer->addWidget(m_pulseBox);
    connect(m_inputDevice, &QComboBox::activated, this, [this] {
        const QString device = m_inputDevice->currentData().toString();
        QSettings("Sessions", "Sessions").setValue("audio/input", device);
        m_engine->setInputDevice(device);
        refreshAudioStatus();
    });
    connect(m_device, &QComboBox::activated, this, &PreferencesDialog::applyAudio);
    connect(m_buffer, &QComboBox::activated, this, &PreferencesDialog::applyAudio);

    m_audioStatus = new QLabel(audioTab);
    m_audioStatus->setWordWrap(true);
    m_audioStatus->setObjectName("dimLabel");
    outer->addWidget(m_audioStatus);
    m_inputStatus = new QLabel(audioTab);
    m_inputStatus->setWordWrap(true);
    m_inputStatus->setObjectName("dimLabel");
    outer->addWidget(m_inputStatus);
    outer->addStretch(1);
    connect(m_system, &QComboBox::activated, this, [this] { showSystem(); applyAudio(); });
    tabs->addTab(audioTab, "Audio");
    fillJackPorts();
    showSystem();
    refreshAudioStatus();

    // ---- Record
    auto* recTab = new QWidget(tabs);
    auto* rf = new QFormLayout(recTab);
    auto* countIn = new QComboBox(recTab);
    countIn->setObjectName("countIn");
    for (int bars : {0, 1, 2, 4}) countIn->addItem(bars == 0 ? QString("None") : bars == 1 ? QString("1 Bar") : QString("%1 Bars").arg(bars), bars);
    countIn->setCurrentIndex(std::max(0, countIn->findData(countInBars())));
    countIn->setToolTip("Bars of click before a recording starts from a standstill (Arrangement Record, or ● in a slot).");
    rf->addRow("Count-in", countIn);
    auto* recNote = new QLabel("Also on the metronome button's right-click menu. During the count-in the position shows "
                               "the bars left in red; nothing is recorded until it ends.", recTab);
    recNote->setObjectName("dimLabel");
    recNote->setWordWrap(true);
    rf->addRow("", recNote);
    connect(countIn, &QComboBox::activated, this, [countIn] { QSettings("Sessions", "Sessions").setValue("record/countIn", countIn->currentData().toInt()); });
    tabs->addTab(recTab, "Record");

    // ---- MIDI
    auto* midiTab = new QWidget(tabs);
    auto* ml = new QVBoxLayout(midiTab);
    auto* hint = new QLabel("Checked inputs play the armed track (or the selected one) and drive what you map in MIDI Map mode (Ctrl+M). "
                            "Keyboards and controllers plugged in later are connected automatically.", midiTab);
    hint->setWordWrap(true);
    ml->addWidget(hint);
    m_ports = new QListWidget(midiTab);
    m_ports->setObjectName("midiPorts");
    ml->addWidget(m_ports, 1);
    m_midiStatus = new QLabel(midiTab);
    m_midiStatus->setObjectName("dimLabel");
    m_midiStatus->setWordWrap(true);
    ml->addWidget(m_midiStatus);
    connect(m_ports, &QListWidget::itemChanged, this, [this](QListWidgetItem* it) {
        m_midi->setEnabled(it->data(Qt::UserRole).toString(), it->checkState() == Qt::Checked);
    });
    connect(m_midi, &audio::MidiInput::portsChanged, this, &PreferencesDialog::refreshMidiPorts);
    tabs->addTab(midiTab, "MIDI");
    refreshMidiPorts();

    // ---- Look
    auto* lookTab = new QWidget(tabs);
    auto* lf = new QFormLayout(lookTab);
    auto* zoom = new QComboBox(lookTab);
    for (int pct : {100, 125, 150, 175, 200}) zoom->addItem(QString("%1 %").arg(pct), pct / 100.0);
    const double z = settings().value("ui/zoom", 1.0).toDouble();
    for (int i = 0; i < zoom->count(); ++i) if (std::abs(zoom->itemData(i).toDouble() - z) < 0.01) zoom->setCurrentIndex(i);
    lf->addRow("Zoom", zoom);
    auto* restart = new QPushButton("Restart Now", lookTab);
    restart->setEnabled(false);
    lf->addRow("", restart);
    auto* zoomNote = new QLabel("The zoom applies when Sessions restarts; the set reopens as it is.", lookTab);
    zoomNote->setObjectName("dimLabel");
    zoomNote->setWordWrap(true);
    lf->addRow("", zoomNote);
    connect(zoom, &QComboBox::activated, this, [zoom, restart] {
        QSettings("Sessions", "Sessions").setValue("ui/zoom", zoom->currentData().toDouble());
        restart->setEnabled(true);
    });
    connect(restart, &QPushButton::clicked, this, [this, restartForZoom] { accept(); if (restartForZoom) restartForZoom(); });
    auto* zoomDir = new QComboBox(lookTab);
    zoomDir->setObjectName("zoomDragDirection");
    zoomDir->addItem("Drag down to zoom in", false);
    zoomDir->addItem("Drag up to zoom in", true);
    zoomDir->setCurrentIndex(zoomDragUp() ? 1 : 0);
    lf->addRow("Zoom drag", zoomDir);
    auto* dirNote = new QLabel("In the Arrangement's overview strip, the clip overviews and the rulers, dragging sideways scrolls and dragging up or down zooms. "
                               "This picks which way zooms in.", lookTab);
    dirNote->setObjectName("dimLabel");
    dirNote->setWordWrap(true);
    lf->addRow("", dirNote);
    connect(zoomDir, &QComboBox::activated, this, [zoomDir] { setZoomDragUp(zoomDir->currentData().toBool()); });
    tabs->addTab(lookTab, "Look");

    // ---- Startup
    auto* startTab = new QWidget(tabs);
    auto* sl = new QVBoxLayout(startTab);
    sl->addWidget(new QLabel("When Sessions starts without a set to open:", startTab));
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
    sl->addStretch(1);
    tabs->addTab(startTab, "Startup");

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);
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
