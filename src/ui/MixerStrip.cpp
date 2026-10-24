#include "MixerStrip.h"
#include <QAbstractItemView>
#include <QSignalBlocker>
#include "Icons.h"
#include "InfoView.h"
#include "core/Model.h"
#include "core/Transport.h"
#include "core/Theme.h"
#include "widgets/Knob.h"
#include "widgets/Fader.h"
#include "widgets/LevelFader.h"
#include "widgets/DragValue.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QToolButton>
#include <QLabel>
#include <QComboBox>
#include <QPainter>
#include <QApplication>
#include <QContextMenuEvent>
#include <QInputDialog>
#include <QLineEdit>
#include "InlineEdit.h"

namespace sessions {

namespace {

QWidget* labelled(QWidget* w, const QString& text, QWidget* parent) {
    auto* box = new QWidget(parent);
    auto* l = new QVBoxLayout(box);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    l->addWidget(w, 0, Qt::AlignHCenter);
    auto* lab = new QLabel(text, box);
    lab->setObjectName("dimLabel");
    lab->setAlignment(Qt::AlignCenter);
    l->addWidget(lab);
    return box;
}

// The Sends area: controls zig-zag down two columns, the title in the top right.
class ZigZag : public QWidget {
public:
    ZigZag(const QString& title, int count, QWidget* parent) : QWidget(parent) {
        setFixedHeight(MixerStrip::sendsHeight(count));
        if (count > 0 && !title.isEmpty()) {
            m_title = new QLabel(title, this);
            m_title->setObjectName("dimLabel");
        }
    }
    void add(QWidget* w) { w->setParent(this); m_items.push_back(w); }

protected:
    void resizeEvent(QResizeEvent*) override {
        if (m_title) m_title->setGeometry(width() / 2 + 4, 0, width() / 2 - 4, 12);
        for (int k = 0; k < m_items.size(); ++k) {
            const QSize s = m_items[k]->sizeHint();
            const int cx = k % 2 == 0 ? width() / 2 - 14 : width() / 2 + 14;
            m_items[k]->setGeometry(cx - s.width() / 2, 2 + k * 15 + (k % 2 ? 6 : 0), s.width(), s.height());
        }
    }

private:
    QLabel* m_title = nullptr;
    QVector<QWidget*> m_items;
};

DragValue* volumeField(QWidget* parent) {
    auto* d = new DragValue(parent);
    d->setRange(0, 1);
    d->setDefault(0.75);
    d->setMinimumChars(5);
    d->setFormatter(dbText);
    return d;
}

QToolButton* stripButton(const QString& text, const char* object, QSize size, QWidget* parent) {
    auto* b = new QToolButton(parent);
    b->setCheckable(true);
    b->setObjectName(object);
    b->setText(text);
    b->setFocusPolicy(Qt::NoFocus);
    b->setFixedSize(size);
    return b;
}

const QColor& kStripBg = theme::strip;

} // namespace

int MixerStrip::sendsHeight(int returns) { return returns > 0 ? 2 + (returns - 1) * 15 + 6 + 32 : 0; }

void MixerStrip::contextMenuEvent(QContextMenuEvent* e) {
    if (!m_project->trackAt(m_index)) return;
    if (!m_project->isReturn(m_index)) m_project->selectedTrack = m_index;
    showTrackMenu(this, e->globalPos(), m_project, m_index, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, "Rename Track", "Name:", QLineEdit::Normal, m_project->trackAt(m_index)->name, &ok).trimmed();
        if (ok && !name.isEmpty()) m_project->renameTrack(m_index, name);
    });
}

MixerStrip::MixerStrip(Project* project, Transport* transport, int trackIndex, QWidget* parent)
    : QWidget(parent), m_project(project), m_transport(transport), m_index(trackIndex), m_isReturn(project->isReturn(trackIndex))
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(2, 4, 2, 6);
    lay->setSpacing(6);
    const int trackId = track()->id;
    const bool audio = !m_isReturn && track()->type == TrackType::Audio;
    const bool group = !m_isReturn && track()->isGroup();

    // ---- I/O (audio tracks): the input and the Monitor switch
    auto* io = new QWidget(this);
    io->setFixedHeight(kIoHeight);
    if (audio) {
        auto* il = new QVBoxLayout(io);
        il->setContentsMargins(2, 0, 2, 0);
        il->setSpacing(3);
        m_input = new QComboBox(io);

        m_input->setFocusPolicy(Qt::NoFocus);
        m_input->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_input->setMinimumWidth(0);
        m_input->setObjectName("audioFrom");
        InfoView::attach(m_input, "Audio From",
            "Select a hardware input, track, group or Master to record. Internal sources use the output after effects and fader. Master and feedback routes record with monitoring disabled.");
        il->addWidget(m_input);
        auto* mon = new QHBoxLayout;
        mon->setSpacing(0);
        const char* names[3] = {"In", "Auto", "Off"};
        for (int k = 0; k < 3; ++k) {
            m_monitor[k] = new QToolButton(io);
            m_monitor[k]->setText(names[k]);
            m_monitor[k]->setObjectName(QString("monitor") + names[k]);
            m_monitor[k]->setCheckable(true);
            m_monitor[k]->setFocusPolicy(Qt::NoFocus);
            m_monitor[k]->setFixedHeight(15);
            m_monitor[k]->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            m_monitor[k]->setStyleSheet("QToolButton{padding:0;font-size:9px;min-height:0;border-radius:0;}");
            InfoView::attach(m_monitor[k], "Monitor",
                "In: you always hear the input through this track. Auto: only while it's armed (●). Off: never (it still records).");
            mon->addWidget(m_monitor[k]);
            connect(m_monitor[k], &QToolButton::clicked, this, [this, k] {
                EditScope scope(m_project, "Monitor");
                track()->monitor = Monitor(k);
                emit m_project->mixerChanged();
            });
        }
        il->addLayout(mon);
        connect(m_input, &QComboBox::activated, this, [this](int i) {
            EditScope scope(m_project, "Audio From");
            track()->inputSource = m_input->itemData(i).toInt();
            if (i < 3) track()->inputChannel = i;
            emit m_project->mixerChanged();
        });
    }
    lay->addWidget(io);

    auto makeKnob = [&](const QString& label, const QString& help, bool bipolar, int size) {
        auto* k = new Knob(this, size);
        k->setBipolar(bipolar);
        k->setRange(bipolar ? -1 : 0, 1);
        k->setDefault(0);
        InfoView::attach(k, label, help + " Drag up or down, Shift for fine. Click it, then press Delete to reset.");
        return k;
    };
    // One undo step per drag; automated controls get overridden when you touch them.
    auto edit = [this, trackId](const QString& label, int what, const QString& tgt, std::function<void(Track&)> apply) {
        EditScope scope(m_project, label, mixerMergeKey(trackId, what));
        apply(*track());
        m_project->touchTarget(m_index, tgt);
        emit m_project->mixerChanged();
    };

    // ---- Sends (returns keep the space empty, so the volume sections line up)
    auto* sends = new ZigZag(m_isReturn ? QString() : QString("Sends"), int(project->returns.size()), this);
    if (!m_isReturn) {
        for (int k = 0; k < project->returns.size(); ++k) {
            const QString letter = QString(QChar('A' + k));
            Knob* knob = makeKnob("Send " + letter, "How much of this track goes to " + project->returns[k].name + ".", false, 20);
            m_sends.push_back(knob);
            sends->add(labelled(knob, letter, sends));
            connect(knob, &DragValue::valueEdited, this, [edit, k, letter](double v) {
                edit("Send " + letter, 10 + k, target::send(k), [k, v](Track& t) { if (k < t.sends.size()) t.sends[k] = v; });
            });
        }
    }
    lay->addWidget(sends);

    // ---- volume section: field, pan, activator, solo, arm | meter with the fader
    auto* vol = new QHBoxLayout;
    vol->setContentsMargins(0, 0, 0, 0);
    vol->setSpacing(4);
    auto* left = new QVBoxLayout;
    left->setContentsMargins(0, 0, 0, 0);
    left->setSpacing(5);
    m_volText = volumeField(this);
    InfoView::attach(m_volText, "Track Volume", "In dB. Drag up or down, or double-click to type. Delete resets to 0 dB.");
    left->addWidget(m_volText, 0, Qt::AlignHCenter);
    m_pan = makeKnob("Pan", "Stereo position.", true, 24);
    left->addWidget(m_pan, 0, Qt::AlignHCenter);
    left->addSpacing(4);
    m_act = stripButton(m_isReturn ? QString(QChar('A' + trackIndex - int(project->tracks.size()))) : QString::number(trackIndex + 1),
                        "muteButton", QSize(26, 20), this);
    InfoView::attach(m_act, "Track Activator", "Turns the track on and off (mute).");
    left->addWidget(m_act, 0, Qt::AlignHCenter);
    if (!m_isReturn) {
        m_solo = stripButton("S", "soloButton", QSize(20, 15), this);
        InfoView::attach(m_solo, group ? "Solo" : "Solo", group ? "Plays only this group's tracks (returns stay on). Ctrl+click to solo several."
                                                               : "Plays only this track (returns stay on). Ctrl+click to solo several.");
        left->addWidget(m_solo, 0, Qt::AlignHCenter);
        if (!group) {
            m_arm = stripButton("", "armButton", QSize(20, 15), this);
            m_arm->setIcon(icons::icon(icons::Glyph::Record, theme::text, theme::onAccent, 11));
            m_arm->setIconSize(QSize(11, 11));
            connect(theme::notifier(), &theme::Notifier::changed, m_arm, [b = m_arm] { b->setIcon(icons::icon(icons::Glyph::Record, theme::text, theme::onAccent, 11)); });
            InfoView::attach(m_arm, "Arm Recording", audio
                ? "Arms the track: you hear the input (Monitor Auto) and empty slots show ● to record it. With Arrangement Record on, playing records onto the timeline."
                : "Arms the track: the computer keyboard (M) and MIDI keyboards play it, and empty slots show ● to record a clip.");
            left->addWidget(m_arm, 0, Qt::AlignHCenter);
        }
    }
    left->addStretch(1);
    m_level = new LevelFader(this);
    InfoView::attach(m_level, "Volume",
        "The meter shows the track's level after the fader; the marker is the fader. Drag up or down (Shift for fine); click it, then Delete resets to 0 dB.");
    vol->addStretch(1);
    vol->addLayout(left);
    vol->addWidget(m_level);
    vol->addStretch(1);
    lay->addLayout(vol, 1);

    // ---- MIDI / Key Map targets (Mapping.h)
    const QString tid = QString("track:%1:").arg(trackId);
    m_level->setProperty("mapTarget", tid + "volume");
    m_pan->setProperty("mapTarget", tid + "pan");
    for (int k = 0; k < m_sends.size(); ++k) m_sends[k]->setProperty("mapTarget", tid + QString("send:%1").arg(k));
    m_act->setProperty("mapTarget", tid + "active");
    if (m_solo) m_solo->setProperty("mapTarget", tid + "solo");
    if (m_arm) m_arm->setProperty("mapTarget", tid + "arm");

    // ---- wiring
    connect(m_pan, &DragValue::valueEdited, this, [edit](double v) { edit("Pan", 2, target::pan(), [v](Track& t) { t.pan = v; }); });
    auto setVol = [this, edit](double v) {
        edit("Volume", 1, target::volume(), [v](Track& t) { t.volume = v; });
        m_level->setValue(v, false);
        m_volText->setValue(v, false);
    };
    connect(m_level,   &DragValue::valueEdited, this, setVol);
    connect(m_volText, &DragValue::valueEdited, this, setVol);
    connect(m_act, &QToolButton::clicked, this, [this](bool on) {
        EditScope scope(m_project, on ? "Track On" : "Track Off");
        track()->active = on;
        emit m_project->mixerChanged();
    });
    if (m_solo) connect(m_solo, &QToolButton::clicked, this, [this](bool on) {   // solo isn't undoable
        bool multi = QApplication::keyboardModifiers() & Qt::ControlModifier;
        if (!multi) for (Track& t : m_project->tracks) t.solo = false;
        track()->solo = on;
        emit m_project->mixerChanged();
    });
    if (m_arm) connect(m_arm, &QToolButton::clicked, this, [this](bool on) { track()->armed = on; emit m_project->mixerChanged(); emit m_project->structureChanged(); });

    connect(transport, &Transport::tick, this, [this] {
        // an armed audio track shows its input on the meter too, so you can set your level
        if (const Track* t = track()) m_level->setLevel(t->type == TrackType::Audio && t->armed ? std::max(t->meter, t->inputMeter) : t->meter);
        followAutomation();
    });
    connect(project, &Project::mixerChanged, this, &MixerStrip::syncFromModel);
    connect(project, &Project::automationChanged, this, &MixerStrip::syncFromModel);
    syncFromModel();
}

Track* MixerStrip::track() const { return m_project->trackAt(m_index); }

// Controls with live automation, or a clip envelope while the clip plays, show that value.
void MixerStrip::followAutomation() {
    const Track* t = track();
    if (!t) return;
    const double beat = m_transport->beat();
    const bool playing = m_transport->playing();
    const ClipPtr clip = playing ? m_project->playingClip(m_index, beat) : nullptr;
    bool driven = false;
    if (!t->automation.isEmpty() || (clip && !clip->envelopes.isEmpty())) {
        auto value = [&](const QString& tgt, double& out) {
            out = m_project->drivenValue(m_index, tgt, beat, playing);
            driven = driven || out >= 0;
            return out >= 0;
        };
        double v;
        if (value(target::volume(), v)) { m_level->setValue(v, false); m_volText->setValue(v, false); }
        if (value(target::pan(), v)) m_pan->setValue(v * 2 - 1, false);
        for (int k = 0; k < m_sends.size(); ++k) if (value(target::send(k), v)) m_sends[k]->setValue(v, false);
    }
    // nothing drives them any more (the clip stopped): back to the track's own settings
    if (m_wasDriven && !driven) { m_wasDriven = false; syncFromModel(); return; }
    m_wasDriven = driven;
}

void MixerStrip::syncFromModel() {
    const Track* t = track();
    if (!t) return;
    for (int k = 0; k < m_sends.size(); ++k) m_sends[k]->setValue(t->sends.value(k), false);
    m_pan->setValue(t->pan, false);
    m_level->setValue(t->volume, false);
    m_volText->setValue(t->volume, false);
    m_act->setChecked(t->active);
    if (m_solo) m_solo->setChecked(t->solo);
    if (m_arm) m_arm->setChecked(t->armed);
    if (m_input && !m_input->view()->isVisible()) {
        const QSignalBlocker block(m_input);
        m_input->clear();
        m_input->addItem("In 1/2", 0);
        m_input->addItem("In 1", 0);
        m_input->addItem("In 2", 0);
        m_input->addItem("Master", -1);
        for (const Track& source : m_project->tracks)
            if (source.id != t->id) m_input->addItem(source.isGroup() ? "Group: " + source.name : source.name, source.id);
        int selected = t->inputSource ? m_input->findData(t->inputSource) : std::clamp(t->inputChannel, 0, 2);
        if (selected < 0) {
            m_input->addItem("Missing source", t->inputSource);
            selected = m_input->count() - 1;
        }
        m_input->setCurrentIndex(selected);
        m_input->setToolTip(m_project->blockedAudioInputs().contains(t->id)
            ? "Monitoring disabled: this input would feed back. Recording remains available."
            : "Audio From: " + m_input->currentText() + (t->inputSource ? " (after effects and fader)" : ""));
    }
    const bool blockedInput = t->type == TrackType::Audio && m_project->blockedAudioInputs().contains(t->id);
    for (int k = 0; k < 3; ++k) if (m_monitor[k]) {
        m_monitor[k]->setChecked(blockedInput ? k == int(Monitor::Off) : int(t->monitor) == k);
        m_monitor[k]->setEnabled(!blockedInput || k == int(Monitor::Off));
        m_monitor[k]->setToolTip(blockedInput ? "Monitoring disabled to prevent feedback. Recording remains available." : QString());
    }
    followAutomation();
}

void MixerStrip::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), kStripBg);
    // a hairline between the Sends and the volume section section borders
    const int y = 4 + sendsHeight(int(m_project->returns.size())) + 2;
    if (!m_project->returns.isEmpty()) p.fillRect(QRect(0, y, width(), 1), theme::editorBg);
}

// ---- Main strip --------------------------------------------------------

MainStrip::MainStrip(Project* project, Transport* transport, QWidget* parent)
    : QWidget(parent), m_project(project)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(2, 4, 2, 6);
    lay->setSpacing(6);

    lay->addSpacing(MixerStrip::kIoHeight);   // the tracks' I/O area

    // Sends: each return's Pre / Post switch
    auto* sends = new ZigZag("Sends", int(project->returns.size()), this);
    for (int k = 0; k < project->returns.size(); ++k) {
        auto* b = new QToolButton(sends);
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setFixedSize(30, 15);
        b->setStyleSheet("QToolButton{padding:0;font-size:9px;min-height:0;}");
        InfoView::attach(b, "Pre / Post (" + project->returns[k].name + ")",
            "Post: tracks feed this return after their faders, so turning a track down turns its send down too. Pre: before the faders.");
        sends->add(b);
        m_prePost.push_back(b);
        connect(b, &QToolButton::clicked, this, [this, k](bool pre) {
            if (k >= m_project->returns.size()) return;
            EditScope scope(m_project, pre ? "Send Pre-Fader" : "Send Post-Fader");
            m_project->returns[k].preFader = pre;
            emit m_project->mixerChanged();
        });
    }
    lay->addWidget(sends);

    auto* vol = new QHBoxLayout;
    vol->setSpacing(4);
    auto* left = new QVBoxLayout;
    left->setSpacing(5);
    m_volText = volumeField(this);
    InfoView::attach(m_volText, "Master Volume", "In dB: the final output level, after the Master's devices. Keep peaks below 0 dB; anything above is softly limited.");
    left->addWidget(m_volText, 0, Qt::AlignHCenter);
    m_cue = new Knob(this, 24);
    m_cue->setRange(0, 1);
    m_cue->setDefault(0.6);
    InfoView::attach(m_cue, "Cue Volume", "How loud the metronome click and sample previews play.");
    left->addWidget(labelled(m_cue, "Cue", this), 0, Qt::AlignHCenter);
    left->addStretch(1);
    m_level = new LevelFader(this);
    InfoView::attach(m_level, "Master Volume", "The meter shows the master output; the marker is the master fader. Keep peaks below 0 dB.");
    vol->addStretch(1); vol->addLayout(left); vol->addWidget(m_level); vol->addStretch(1);
    lay->addLayout(vol, 1);

    m_level->setProperty("mapTarget", "main:volume");
    m_cue->setProperty("mapTarget", "main:cue");

    auto setVol = [this](double v) {
        EditScope scope(m_project, "Master Volume", mixerMergeKey(0, 1));
        m_project->mainVolume = v;
        m_level->setValue(v, false);
        m_volText->setValue(v, false);
        emit m_project->mixerChanged();
    };
    connect(m_level,   &DragValue::valueEdited, this, setVol);
    connect(m_volText, &DragValue::valueEdited, this, setVol);
    connect(m_cue,     &DragValue::valueEdited, this, [this](double v) { m_project->cueVolume = v; emit m_project->mixerChanged(); });
    connect(transport, &Transport::tick, this, [this] { m_level->setLevel(m_project->mainMeter); });
    connect(project, &Project::mixerChanged, this, &MainStrip::syncFromModel);
    syncFromModel();
}

void MainStrip::syncFromModel() {
    m_cue->setValue(m_project->cueVolume, false);
    m_level->setValue(m_project->mainVolume, false);
    m_volText->setValue(m_project->mainVolume, false);
    for (int k = 0; k < m_prePost.size() && k < m_project->returns.size(); ++k) {
        m_prePost[k]->setChecked(m_project->returns[k].preFader);
        m_prePost[k]->setText(m_project->returns[k].preFader ? "Pre" : "Post");
    }
}

void MainStrip::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), kStripBg);
    const int y = 4 + MixerStrip::sendsHeight(int(m_project->returns.size())) + 2;
    if (!m_project->returns.isEmpty()) p.fillRect(QRect(0, y, width(), 1), theme::editorBg);
}

} // namespace sessions
