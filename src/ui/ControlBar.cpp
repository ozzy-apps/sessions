#include "ControlBar.h"
#include "Icons.h"
#include "core/Theme.h"
#include "InfoView.h"
#include "core/Model.h"
#include "core/Transport.h"
#include "audio/Engine.h"
#include "widgets/DragValue.h"
#include <QHBoxLayout>
#include <QToolButton>
#include <QComboBox>
#include <QLabel>
#include <QMenu>
#include <QSettings>
#include <QDateTime>
#include <QSignalBlocker>
#include <cmath>

namespace sessions {

// Hand-made stylesheets, built from the theme (and rebuilt when View > Theme changes).
static QString positionStyle(bool counting) {   // the count-in shows in red
    return QString("background:%1;border:1px solid %2;padding:0 4px;min-height:16px;%3")
        .arg(theme::field.name(), theme::line.name(), counting ? "color:" + theme::record.name() + ";" : QString());
}
static QString reEnableStyle() { return QString("background:%1;color:%2;font-weight:600;").arg(theme::record.name(), theme::onAccent.name()); }

static QToolButton* makeButton(QWidget* parent, const QString& text, const QString& title, const QString& help, bool checkable = false) {
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setCheckable(checkable);
    b->setFocusPolicy(Qt::NoFocus);
    InfoView::attach(b, title, help);
    return b;
}

ControlBar::ControlBar(Project* project, Transport* transport, QWidget* parent)
    : QWidget(parent), m_project(project), m_transport(transport)
{
    setObjectName("controlBar");
    setFixedHeight(34);
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(8, 0, 8, 0);
    lay->setSpacing(5);

    // ---- follow, tempo & timing
    m_follow = makeButton(this, "Follow", "Follow", "On: the Arrangement scrolls along with the playhead while it plays.", true);
    m_follow->setChecked(true);
    connect(m_follow, &QToolButton::toggled, this, &ControlBar::followToggled);
    lay->addWidget(m_follow);

    auto* tap = makeButton(this, "TAP", "Tap Tempo", "Click in time with the music to set the tempo.");
    m_tapClock.start();
    connect(tap, &QToolButton::clicked, this, [this] {
        qint64 now = m_tapClock.elapsed();
        m_taps.erase(std::remove_if(m_taps.begin(), m_taps.end(), [now](qint64 t) { return now - t > 2500; }), m_taps.end());
        m_taps.push_back(now);
        if (m_taps.size() > 1) {
            double bpm = 60000.0 * (m_taps.size() - 1) / double(now - m_taps.first());
            m_tempo->setValue(std::round(bpm));
        }
    });
    lay->addWidget(tap);

    m_tempo = new DragValue(this);
    m_tempo->setRange(20, 999);
    m_tempo->setDefault(120);
    m_tempo->setDragSensitivity(2000);
    m_tempo->setMinimumChars(7);
    m_tempo->setFormatter([](double v) { return QString::number(v, 'f', 2); });
    m_tempo->setValue(project->tempo);
    InfoView::attach(m_tempo, "Tempo", "Drag up or down to change. Shift+drag for 0.01 BPM steps. Double-click to type. Click it, then press Delete to reset to 120.");
    m_tempo->setProperty("mapTarget", "tempo");
    connect(m_tempo, &DragValue::valueChanged, this, [this](double v) {
        if (v == m_project->tempo) return;
        EditScope edit(m_project, "Tempo", mixerMergeKey(0, 99));
        m_project->setTempo(v);
    });
    connect(project, &Project::tempoChanged, this, [this] { m_tempo->setValue(m_project->tempo, false); });
    lay->addWidget(m_tempo);

    auto* sig = new DragValue(this);
    sig->setEditable(false);
    sig->setAccentText(false);
    sig->setMinimumChars(5);
    sig->setFormatter([](double) { return "4 / 4"; });
    sig->setCursor(Qt::ArrowCursor);
    InfoView::attach(sig, "Time Signature", "Fixed at 4/4 for now.");
    lay->addWidget(sig);

    m_metro = makeButton(this, "◉", "Metronome", "Toggles the click. Right-click for the count-in before recordings.", true);
    connect(m_metro, &QToolButton::toggled, transport, &Transport::setMetronome);
    m_metro->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_metro, &QWidget::customContextMenuRequested, this, [this](const QPoint& at) {   // Count-In's metronome menu
        QMenu menu(this);
        menu.addSection("Count-In");
        for (int bars : {0, 1, 2, 4}) {
            QAction* a = menu.addAction(bars == 0 ? QString("None") : bars == 1 ? QString("1 Bar") : QString("%1 Bars").arg(bars));
            a->setCheckable(true);
            a->setChecked(m_transport->countInBars() == bars);
            connect(a, &QAction::triggered, this, [this, bars] {
                m_transport->setCountInBars(bars);
                QSettings("Sessions", "Sessions").setValue("record/countIn", bars);
            });
        }
        menu.exec(m_metro->mapToGlobal(at));
    });
    m_metro->setProperty("mapTarget", "transport:metronome");
    lay->addWidget(m_metro);

    m_quant = new QComboBox(this);
    m_quant->addItem("None", 0.0);
    m_quant->addItem("1 Bar", 4.0);
    m_quant->addItem("1/2", 2.0);
    m_quant->addItem("1/4", 1.0);
    m_quant->setCurrentIndex(1);
    m_quant->setFocusPolicy(Qt::NoFocus);
    InfoView::attach(m_quant, "Global Launch Quantization", "When launched clips actually start. Clips wait for the next point on this grid.");
    connect(m_quant, &QComboBox::currentIndexChanged, this, [this](int i) { m_transport->setLaunchQuantization(m_quant->itemData(i).toDouble()); });
    lay->addWidget(m_quant);

    lay->addStretch(1);

    // ---- transport
    m_position = new QLabel("1. 1. 1", this);
    m_position->setMinimumWidth(64);
    m_position->setAlignment(Qt::AlignCenter);
    m_position->setStyleSheet(positionStyle(false));
    InfoView::attach(m_position, "Arrangement Position", "Bars. Beats. Sixteenths.");
    lay->addWidget(m_position);

    m_play = makeButton(this, "", "Play", "Space starts and stops playback.", true);
    m_play->setObjectName("playButton");
    connect(m_play, &QToolButton::clicked, transport, &Transport::togglePlay);
    m_play->setProperty("mapTarget", "transport:play");
    lay->addWidget(m_play);

    m_stop = makeButton(this, "", "Stop", "Stops everything. Press again while stopped to return to the start.");
    connect(m_stop, &QToolButton::clicked, transport, &Transport::stop);
    m_stop->setProperty("mapTarget", "transport:stop");
    lay->addWidget(m_stop);

    m_rec = makeButton(this, "", "Arrangement Record",
        "While on, Session clips you launch are written into the Arrangement, and notes you play on armed tracks are recorded onto the timeline.", true);
    m_rec->setObjectName("recButton");
    connect(m_rec, &QToolButton::clicked, transport, &Transport::setArrangementRecord);
    m_rec->setProperty("mapTarget", "transport:record");
    lay->addWidget(m_rec);

    // loop start | loop switch | loop length, in bars. beats. sixteenths
    auto loopField = [this](const QString& title, const QString& help, double min) {
        auto* d = new DragValue(this);
        d->setRange(min, 4096);
        d->setStep(1);
        d->setDragSensitivity(1600);
        d->setMinimumChars(7);
        InfoView::attach(d, title, help + " Drag up or down in beats, or double-click to type beats.");
        return d;
    };
    m_loopStart = loopField("Loop Start", "Where the Arrangement loop begins.", 0);
    m_loopStart->setFormatter([](double b) {
        const int s = int(std::round(b * 4));
        return QString("%1. %2. %3").arg(s / 16 + 1).arg((s / 4) % 4 + 1).arg(s % 4 + 1);
    });
    connect(m_loopStart, &DragValue::valueEdited, this, [this](double v) { m_transport->setLoop(v, m_transport->loopLength()); });
    lay->addWidget(m_loopStart);
    m_punchIn = makeButton(this, "⇥", "Punch-In",
        "On: Arrangement recording starts at the loop start, wherever you press Play. Audio and notes before it are left alone.", true);
    connect(m_punchIn, &QToolButton::toggled, transport, &Transport::setPunchIn);
    lay->addWidget(m_punchIn);
    m_loop = makeButton(this, "⟲", "Loop", "Loops the Arrangement between Loop Start and Loop Length (drag the brace above the ruler too).", true);
    connect(m_loop, &QToolButton::toggled, transport, &Transport::setLoopEnabled);
    m_loop->setProperty("mapTarget", "transport:loop");
    lay->addWidget(m_loop);
    m_loopLength = loopField("Loop Length", "How long the Arrangement loop is.", 1);
    m_loopLength->setFormatter([](double b) {
        const int s = int(std::round(b * 4));
        return QString("%1. %2. %3").arg(s / 16).arg((s / 4) % 4).arg(s % 4);
    });
    connect(m_loopLength, &DragValue::valueEdited, this, [this](double v) { m_transport->setLoop(m_transport->loopStart(), v); });
    m_punchOut = makeButton(this, "⇤", "Punch-Out", "On: Arrangement recording stops at the loop end, even if playback goes on.", true);
    connect(m_punchOut, &QToolButton::toggled, transport, &Transport::setPunchOut);
    lay->addWidget(m_punchOut);
    lay->addWidget(m_loopLength);

    m_draw = makeButton(this, "✎", "Draw Mode (B)", "Click in the note editor to add notes, click a note to delete it, drag to paint.", true);
    connect(m_draw, &QToolButton::toggled, this, &ControlBar::drawModeChanged);
    lay->addWidget(m_draw);

    m_keys = makeButton(this, "⌨", "Computer MIDI Keyboard (M)",
        "Play the armed track (or the selected one) with A W S E D F T G Y H U J K O L P. Z / X: octave down / up. C / V: velocity down / up.", true);
    connect(m_keys, &QToolButton::toggled, this, &ControlBar::midiKeyboardToggled);
    lay->addWidget(m_keys);

    m_reEnable = makeButton(this, "↺ Automation", "Re-Enable Automation",
        "You've moved an automated control by hand, so its automation is off. Click to let every envelope take over again.");
    m_reEnable->setStyleSheet(reEnableStyle());
    refreshIcons();
    connect(theme::notifier(), &theme::Notifier::changed, this, &ControlBar::refreshIcons);
    connect(theme::notifier(), &theme::Notifier::changed, this, [this] {   // View > Theme: the hand-made stylesheets follow
        m_position->setStyleSheet(positionStyle(m_position->property("counting").toBool()));
        m_reEnable->setStyleSheet(reEnableStyle());
    });
    m_reEnable->hide();
    connect(m_reEnable, &QToolButton::clicked, project, &Project::reEnableAutomation);
    connect(project, &Project::automationChanged, this, [this] { m_reEnable->setVisible(m_project->anyAutomationOverridden()); });
    lay->addWidget(m_reEnable);

    lay->addStretch(1);

    // Key / MIDI Map modes and the MIDI input light, at the right of the control bar
    m_keyMap = makeButton(this, "KEY", "Key Map Mode (Ctrl+K)", "Click a control or clip slot, then press a computer key to play it with that key. Delete removes; Esc leaves.", true);
    m_midiMap = makeButton(this, "MIDI", "MIDI Map Mode (Ctrl+M)", "Click a control or clip slot, then move a knob or hit a key or pad on your MIDI controller. Delete removes; Esc leaves.", true);
    connect(m_keyMap, &QToolButton::clicked, this, &ControlBar::keyMapToggled);
    connect(m_midiMap, &QToolButton::clicked, this, &ControlBar::midiMapToggled);
    m_midiLight = new QLabel("●", this);
    m_midiLight->setObjectName("dimLabel");
    m_midiLight->setFixedWidth(10);
    InfoView::attach(m_midiLight, "MIDI Input", "Lights up when MIDI comes in from a keyboard or controller.");
    lay->addWidget(m_keyMap);
    lay->addWidget(m_midiMap);
    lay->addWidget(m_midiLight);

    m_cpu = new QLabel("CPU 0%", this);
    m_cpu->setObjectName("dimLabel");
    m_cpu->setMinimumWidth(52);
    InfoView::attach(m_cpu, "CPU Load", "Share of each audio block's time the engine needs.");
    lay->addWidget(m_cpu);

    m_session = makeButton(this, "▦", "Session View (Tab)", "The clip launching grid.", true);
    m_arrange = makeButton(this, "☰", "Arrangement View (Tab)", "The timeline.", true);
    m_session->setChecked(true);
    connect(m_session, &QToolButton::clicked, this, [this] { emit viewRequested(true); });
    connect(m_arrange, &QToolButton::clicked, this, [this] { emit viewRequested(false); });
    lay->addWidget(m_session);
    lay->addWidget(m_arrange);

    connect(transport, &Transport::tick, this, [this] {
        m_position->setText(m_transport->positionText());
        const bool counting = m_transport->countingIn();
        if (counting != m_position->property("counting").toBool()) {   // the count-in shows in red
            m_position->setProperty("counting", counting);
            m_position->setStyleSheet(positionStyle(counting));
        }
        const bool lit = QDateTime::currentMSecsSinceEpoch() < m_midiLitUntil;
        if (lit != (m_midiLight->property("lit").toBool())) {
            m_midiLight->setProperty("lit", lit);
            m_midiLight->setStyleSheet(lit ? "color:" + theme::accent.name() + ";" : QString());
        }
        if (m_cpuTicks++ % 30) return;   // twice a second is plenty
        audio::Engine* e = m_transport->engine();
        if (e->hasAudio()) {
            m_cpu->setText(QString("CPU %1%").arg(int(std::round(e->cpuLoad() * 100))));
            m_cpu->setStyleSheet("");
            InfoView::attach(m_cpu, "CPU Load", QString("Share of each audio block's time the engine needs.\n\nOutput: %1, about %2 ms latency.")
                                                    .arg(e->outputName()).arg(int(std::round(e->latencySeconds() * 1000))));
        } else if (e->audioDisabled()) {
            m_cpu->setText("Audio Off");
            m_cpu->setStyleSheet("");
            InfoView::attach(m_cpu, "Audio Off", "Sessions was started without sound output (SESSIONS_AUDIO=none or an offscreen session).");
        } else {
            m_cpu->setText("No Audio");
            m_cpu->setStyleSheet("color:" + theme::record.name() + ";");
            InfoView::attach(m_cpu, "No Audio Output", e->outputName() + "\n\nThe transport still runs; nothing is heard. Check that PipeWire or PulseAudio is running, then restart Sessions.");
        }
    });
    connect(transport, &Transport::stateChanged, this, &ControlBar::refresh);
    refresh();
}

void ControlBar::setMapButtons(bool keyMap, bool midiMap) { m_keyMap->setChecked(keyMap); m_midiMap->setChecked(midiMap); }
void ControlBar::flashMidiIn() { m_midiLitUntil = QDateTime::currentMSecsSinceEpoch() + 120; }

bool ControlBar::drawMode() const { return m_draw->isChecked(); }
bool ControlBar::midiKeyboard() const { return m_keys->isChecked(); }
void ControlBar::setMidiKeyboard(bool on) { m_keys->setChecked(on); }
void ControlBar::setMidiKeyboardInfo(const QString& text) {
    InfoView::attach(m_keys, "Computer MIDI Keyboard (M)",
        "Play the armed track (or the selected one) with A W S E D F T G Y H U J K O L P. Z / X: octave down / up. C / V: velocity down / up.\n\n" + text);
}
void ControlBar::toggleDrawMode() { m_draw->toggle(); }

// The transport glyphs, painted (Icons.h): a dark glyph on the lit (accent / green / red) backgrounds
void ControlBar::refreshIcons() {
    for (auto [b, g] : {std::pair{m_play, icons::Glyph::Play}, std::pair{m_stop, icons::Glyph::Stop}, std::pair{m_rec, icons::Glyph::Record}}) {
        b->setIcon(icons::icon(g, theme::text, theme::onAccent, 12));
        b->setIconSize(QSize(12, 12));
        b->setMinimumWidth(26);
    }
}

void ControlBar::setSessionViewActive(bool session) {
    m_session->setChecked(session);
    m_arrange->setChecked(!session);
}

void ControlBar::refresh() {
    m_play->setChecked(m_transport->playing());
    m_loop->setChecked(m_transport->loopEnabled());
    m_loopStart->setValue(m_transport->loopStart(), false);
    m_loopLength->setValue(m_transport->loopLength(), false);
    m_metro->setChecked(m_transport->metronome());
    { QSignalBlocker b1(m_punchIn), b2(m_punchOut); m_punchIn->setChecked(m_transport->punchIn()); m_punchOut->setChecked(m_transport->punchOut()); }
    m_rec->setChecked(m_transport->arrangementRecord());
    QSignalBlocker block(m_quant);   // a loaded set brings its own launch quantization
    const int q = m_quant->findData(m_transport->launchQuantization());
    if (q >= 0) m_quant->setCurrentIndex(q);
}

} // namespace sessions
