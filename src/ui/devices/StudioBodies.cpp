#include "Bodies.h"
#include "audio/Engine.h"
#include "core/Import.h"
#include "core/Sample.h"
#include "core/SampleZones.h"
#include "core/Transport.h"
#include "ui/InfoView.h"
#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>

namespace sessions::devui {
namespace {
using T = DeviceType;
class StatusPlot final : public Display {
  public:
    StatusPlot(DeviceBody* b, int w = 230, int h = 172) : Display(b, w, h) {
        b->animate(this);
    }

  protected:
    void draw(QPainter& p, const QRectF& r) override {
        p.setPen(col::value);
        const T type = m_body->ctx().type;
        const Device* d = m_body->device();
        if (type == T::Shifter || type == T::PitchCorrector) {
            float hz = m_body->scope(0), shift = m_body->scope(1);
            QString text =
                type == T::PitchCorrector
                    ? (hz > 0 ? QString::number(hz, 'f', 1) + " Hz\n" + QString::number(shift, 'f', 2) + " st"
                              : "Unvoiced")
                    : "Pitch / Frequency / Ring";
            p.drawText(r.adjusted(8, 8, -8, -30), Qt::AlignCenter, text);
            drawCaption(p, r, type == T::PitchCorrector ? "Monophonic input" : "Pitch latency: 2048 samples",
                        Qt::AlignBottom | Qt::AlignHCenter);
        } else if (type == T::HybridReverb) {
            if (d && d->sample) {
                QPainterPath path;
                for (int x = 0; x < int(r.width()); ++x) {
                    float lo, hi;
                    double at = x / r.width() * d->sample->seconds();
                    d->sample->range(at, at + d->sample->seconds() / r.width(), lo, hi);
                    path.moveTo(r.left() + x, r.center().y() + lo * r.height() * .35);
                    path.lineTo(r.left() + x, r.center().y() + hi * r.height() * .35);
                }
                p.setPen(col::cyan);
                p.drawPath(path);
                p.setPen(col::value);
                drawCaption(p, r, QFileInfo(d->samplePath).fileName(), Qt::AlignBottom | Qt::AlignHCenter);
            } else
                p.drawText(r, Qt::AlignCenter, "No impulse response");
        } else if (type == T::Looper) {
            const char* states[] = {"Stop", "Record", "Play", "Overdub"};
            int state = std::clamp(int(m_body->scope(2)), 0, 3);
            float fraction = m_body->scope(0);
            p.fillRect(QRectF(r.left(), r.top(), r.width() * fraction, r.height()), col::cyanFill);
            p.drawText(r, Qt::AlignCenter,
                       QString(m_body->scope(5) > 0 ? "Queued" : states[state]) + "\n" +
                           QString::number(m_body->scope(1) / (m_body->ctx().transport
                                                                   ? m_body->ctx().transport->engine()->sampleRate()
                                                                   : 48000),
                                           'f', 2) +
                           " s");
            if (m_body->scope(6) > 0 && m_body->scope(1) > 0)
                drawCaption(p, r, "Unsaved loop", Qt::AlignBottom | Qt::AlignHCenter);
        } else if (type == T::Multisampler)
            p.drawText(r, Qt::AlignCenter,
                       QString::number(d ? d->sampleZones.size() : 0) + " zones\n" +
                           QString::number(int(m_body->scope(0))) + " voices");
    }
};
class PitchBody final : public DeviceBody {
  public:
    PitchBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        setObjectName("pitchBody");
        setFixedWidth(610);
        auto* root = rootRow(this);
        root->addWidget(new StatusPlot(this));
        auto* col = vbox(3);
        auto* row = hbox(3);
        row->addWidget(chooser(pitchfx::Mode, 100));
        row->addWidget(chooser(pitchfx::Root, 54));
        row->addWidget(chooser(pitchfx::Scale, 100));
        row->addStretch();
        col->addLayout(row);
        auto* grid = new QGridLayout;
        grid->setSpacing(2);
        grid->setContentsMargins(0, 0, 0, 0);
        std::vector<int> params = {pitchfx::Pitch, pitchfx::Fine, pitchfx::Formant, pitchfx::Mix, pitchfx::Output};
        params.push_back(pitchfx::Strength);
        params.push_back(pitchfx::Retune);
        params.push_back(pitchfx::Frequency);
        for (int i = 0; i < int(params.size()); ++i)
            grid->addWidget(knob(params[i], 22), i / 4, i % 4);
        col->addLayout(grid);
        col->addWidget(toggle(pitchfx::Preserve, "Preserve Formants", 128));
        col->addStretch();
        root->addLayout(col);
    }
};
class HybridBody final : public DeviceBody {
  public:
    HybridBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        setObjectName("hybridReverbBody");
        setFixedWidth(540);
        auto* root = rootRow(this);
        auto* left = vbox(3);
        left->addWidget(new StatusPlot(this, 230, 140));
        auto* load = new QPushButton("Load IR", this);
        load->setFixedHeight(26);
        connect(load, &QPushButton::clicked, this, [this] {
            QString path =
                QFileDialog::getOpenFileName(this, "Load impulse response", {}, "Audio (*.wav *.aif *.aiff *.flac)");
            if (path.isEmpty())
                return;
            QString error;
            EditScope edit(m_ctx.project, "Load Reverb IR");
            if (!import::loadIr(*m_ctx.project, m_ctx.track, m_ctx.deviceId, path, &error))
                QMessageBox::warning(this, "Load IR", error);
        });
        left->addWidget(load);
        auto* leftGroup = new QWidget(this);
        leftGroup->setLayout(left);
        leftGroup->setFixedWidth(230);
        root->addWidget(leftGroup);
        auto* col = vbox(3);
        col->addWidget(chooser(hybrid::Routing, 120));
        auto* grid = new QGridLayout;
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(1);
        int count = 0;
        for (int i = 1; i < hybrid::kParams; ++i)
            if (i != hybrid::Freeze) {
                grid->addWidget(knob(i, 20,
                                     i == hybrid::PreDelay  ? "Delay"
                                     : i == hybrid::Damping ? "Damp"
                                     : i == hybrid::Mix     ? "Mix"
                                                            : QString()),
                                count / 5, count % 5);
                ++count;
            }
        col->addLayout(grid);
        col->addWidget(toggle(hybrid::Freeze, "Freeze", 66));
        col->addStretch();
        root->addLayout(col);
    }
};
class LooperBody final : public DeviceBody {
    QPushButton* saveButton = nullptr;
    void command(int state, int toggle = -1) {
        // These are transport commands. Project undo must not re-enter Record.
        auto* d = findDevice(m_ctx.project, m_ctx.track, m_ctx.deviceId);
        if (!d)
            return;
        d->params[looper::State].value = choicePosition(state, 4);
        const int commands[] = {looper::StopTrigger, looper::RecordTrigger, looper::PlayTrigger,
                                looper::OverdubTrigger};
        const int trigger = commands[state];
        d->params[trigger].value = d->params[trigger].value >= .5 ? 0 : 1;
        if (toggle >= 0)
            d->params[toggle].value = d->params[toggle].value >= .5 ? 0 : 1;
        if (toggle == looper::Clear) {
            d->samplePath.clear();
            d->sample.reset();
        }
        emit m_ctx.project->deviceChanged();
    }
    void saveLoop() {
        auto* engine = m_ctx.transport ? m_ctx.transport->engine() : nullptr;
        auto processor = engine ? engine->deviceProcessor(m_ctx.deviceId) : nullptr;
        if (!processor)
            return;
        int frames = int(scope(1));
        if (frames <= 0)
            return;
        QString path = QFileDialog::getSaveFileName(this, "Save loop", {}, "WAV (*.wav)");
        if (path.isEmpty())
            return;
        if (!path.endsWith(".wav", Qt::CaseInsensitive))
            path += ".wav";
        auto capture = std::make_shared<audio::DeviceCapture>();
        capture->stereo.resize(size_t(frames + 128) * 2);
        processor->retainCapture(capture);
        if (!processor->requestCapture(capture.get()))
            return;
        auto* timer = new QTimer(qApp);
        QPointer<Project> project = m_ctx.project;
        QPointer<Transport> transport = m_ctx.transport;
        const int trackId = m_ctx.trackId, id = m_ctx.deviceId;
        connect(timer, &QTimer::timeout, timer, [timer, project, transport, processor, capture, path, trackId, id] {
            if (!project || !transport || transport->engine()->deviceProcessor(id) != processor) {
                timer->deleteLater();
                return;
            }
            int state = capture->state.load(std::memory_order_acquire);
            if (!state)
                return;
            timer->stop();
            timer->deleteLater();
            if (state < 0) {
                QMessageBox::warning(nullptr, "Save loop",
                                     "Loop changed while saving. Stop overdubbing and save again.");
                return;
            }
            QString error;
            if (!samples::writeWav(path, capture->stereo.data(), capture->frames, capture->rate,
                                   samples::WavFormat::Float32, &error)) {
                QMessageBox::warning(nullptr, "Save loop", error);
                return;
            }
            samples::loadAsync(
                {path}, project, [project, trackId, id, path](QVector<SamplePtr> samples, QStringList errors) {
                    if (!project || samples.isEmpty() || !samples[0])
                        return;
                    int track = project->master.id == trackId ? project->trackCount() : project->indexOfTrack(trackId);
                    EditScope edit(project, "Save Loop");
                    auto* d = findDevice(project, track, id);
                    if (!d)
                        return;
                    d->samplePath = path;
                    d->sample = samples[0];
                    emit project->deviceChanged();
                });
        });
        timer->start(30);
    }

  public:
    LooperBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        setObjectName("looperBody");
        setFixedWidth(630);
        auto* root = rootRow(this);
        root->addWidget(new StatusPlot(this));
        auto* col = vbox(3);
        auto* controls = hbox(3);
        const char* states[] = {"Stop", "Record", "Play", "Overdub"};
        for (int s = 0; s < 4; ++s) {
            auto* b = new QPushButton(states[s], this);
            b->setFixedHeight(28);
            connect(b, &QPushButton::clicked, this, [this, s] { command(s); });
            const int commands[] = {looper::StopTrigger, looper::RecordTrigger, looper::PlayTrigger,
                                    looper::OverdubTrigger};
            b->setProperty("mapTarget", target(commands[s]));
            controls->addWidget(b);
        }
        col->addLayout(controls);
        auto* stateRow = hbox(3);
        stateRow->addWidget(chooser(looper::Bars, 108));
        stateRow->addWidget(chooser(looper::State, 90));
        col->addLayout(stateRow);
        auto* knobs = hbox(2);
        for (int i : {looper::Feedback, looper::Mix, looper::Output})
            knobs->addWidget(knob(i, 22));
        col->addLayout(knobs);
        auto* row = hbox(2);
        row->addWidget(toggle(looper::Reverse, "Reverse", 68));
        row->addWidget(toggle(looper::HalfSpeed, "Half Speed", 80));
        for (auto pair : {std::pair<int, QString>{looper::Undo, "Undo"}, {looper::Clear, "Clear"}}) {
            auto* b = new QPushButton(pair.second, this);
            b->setFixedHeight(22);
            b->setProperty("mapTarget", target(pair.first));
            connect(b, &QPushButton::clicked, this,
                    [this, pair] { command(pair.first == looper::Undo ? 2 : 0, pair.first); });
            row->addWidget(b);
        }
        col->addLayout(row);
        auto* save = new QPushButton("Save Loop", this);
        saveButton = save;
        save->setFixedHeight(24);
        connect(save, &QPushButton::clicked, this, [this] { saveLoop(); });
        col->addWidget(save);
        col->addStretch();
        root->addLayout(col);
    }
    void tick() override {
        DeviceBody::tick();
        saveButton->setEnabled(scope(1) > 0 && int(scope(2)) != 1 && int(scope(2)) != 3);
    }
};

void importZones(DeviceBody* b) {
    QStringList paths = QFileDialog::getOpenFileNames(b, "Import samples or SFZ", {},
                                                      "Samples / SFZ (*.sfz *.wav *.aif *.aiff *.flac *.ogg *.mp3)");
    if (paths.isEmpty())
        return;
    QVector<SampleZone> zones;
    QString error;
    if (paths.size() == 1 && paths[0].endsWith(".sfz", Qt::CaseInsensitive)) {
        if (!samplezones::readSfz(paths[0], zones, &error)) {
            QMessageBox::warning(b, "Import SFZ", error);
            return;
        }
    } else
        zones = samplezones::mapAudioFiles(paths);
    QStringList samples;
    for (const auto& z : zones)
        samples << z.samplePath;
    QPointer<Project> project = b->ctx().project;
    const int trackId = b->ctx().trackId, id = b->ctx().deviceId;
    QPointer<QWidget> parent = b;
    samples::loadAsync(samples, project,
                       [project, parent, trackId, id, zones](QVector<SamplePtr> loaded, QStringList errors) mutable {
                           if (!project)
                               return;
                           for (int i = 0; i < zones.size(); ++i)
                               zones[i].sample = loaded.value(i);
                           if (!errors.isEmpty() && parent)
                               QMessageBox::warning(parent, "Import samples", errors.join('\n'));
                           const int track = project->indexOfTrack(trackId);
                           EditScope edit(project, "Import Multisample");
                           if (auto* d = findDevice(project, track, id)) {
                               d->sampleZones = zones;
                               emit project->structureChanged();
                           }
                       });
}
class ZoneEditor final : public DeviceBody {
    QTableWidget* table = nullptr;
    void populate() {
        if (!table)
            return;
        QSignalBlocker blocker(table);
        const Device* d = device();
        if (!d)
            return;
        table->setRowCount(d->sampleZones.size());
        for (int row = 0; row < d->sampleZones.size(); ++row) {
            const auto& z = d->sampleZones[row];
            QStringList values = {QFileInfo(z.samplePath).fileName(),
                                  QString::number(z.keyLow),
                                  QString::number(z.keyHigh),
                                  QString::number(z.velocityLow),
                                  QString::number(z.velocityHigh),
                                  QString::number(z.root),
                                  QString::number(z.gain),
                                  QString::number(z.pan),
                                  QString::number(z.tune),
                                  z.loop ? "1" : "0",
                                  QString::number(z.loopStart),
                                  QString::number(z.loopEnd),
                                  QString::number(z.sequenceLength),
                                  QString::number(z.sequencePosition),
                                  z.oneShot ? "1" : "0"};
            for (int c = 0; c < values.size(); ++c) {
                auto* item = new QTableWidgetItem(values[c]);
                if (c == 0) {
                    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                    item->setToolTip(z.samplePath);
                    if (!z.sample)
                        item->setForeground(theme::devMissing);
                }
                table->setItem(row, c, item);
            }
        }
    }
    void changed(int row, int column) {
        if (column == 0)
            return;
        const QString value = table->item(row, column)->text();
        bool ok = false;
        double v = value.toDouble(&ok);
        if (!ok) {
            populate();
            return;
        }
        EditScope edit(m_ctx.project, "Edit Sample Zone");
        auto* d = findDevice(m_ctx.project, m_ctx.track, m_ctx.deviceId);
        if (!d || row >= d->sampleZones.size())
            return;
        auto& z = d->sampleZones[row];
        switch (column) {
        case 1:
            z.keyLow = std::clamp(int(v), 0, z.keyHigh);
            break;
        case 2:
            z.keyHigh = std::clamp(int(v), z.keyLow, 127);
            break;
        case 3:
            z.velocityLow = std::clamp(int(v), 1, z.velocityHigh);
            break;
        case 4:
            z.velocityHigh = std::clamp(int(v), z.velocityLow, 127);
            break;
        case 5:
            z.root = std::clamp(int(v), 0, 127);
            break;
        case 6:
            z.gain = std::clamp(v, -60., 24.);
            break;
        case 7:
            z.pan = std::clamp(v, -1., 1.);
            break;
        case 8:
            z.tune = std::clamp(v, -24., 24.);
            break;
        case 9:
            z.loop = v != 0;
            break;
        case 10:
            z.loopStart = std::max(0., v);
            break;
        case 11:
            z.loopEnd = std::max(0., v);
            break;
        case 12:
            z.sequenceLength = std::clamp(int(v), 1, 100);
            z.sequencePosition = std::min(z.sequencePosition, z.sequenceLength);
            break;
        case 13:
            z.sequencePosition = std::clamp(int(v), 1, z.sequenceLength);
            break;
        case 14:
            z.oneShot = v != 0;
            break;
        }
        emit m_ctx.project->deviceChanged();
        QTimer::singleShot(0, this, [this] { populate(); });
    }

  public:
    ZoneEditor(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        setObjectName("multisamplerEditor");
        setMinimumSize(1000, 420);
        setMaximumHeight(QWIDGETSIZE_MAX);
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(12, 8, 12, 8);
        auto* toolbar = hbox(8);
        auto* import = new QPushButton("Import", this);
        connect(import, &QPushButton::clicked, this, [this] { importZones(this); });
        toolbar->addWidget(import);
        auto* remove = new QPushButton("Remove Zone", this);
        connect(remove, &QPushButton::clicked, this, [this] {
            int row = table->currentRow();
            EditScope edit(m_ctx.project, "Remove Sample Zone");
            auto* d = findDevice(m_ctx.project, m_ctx.track, m_ctx.deviceId);
            if (d && row >= 0 && row < d->sampleZones.size()) {
                d->sampleZones.removeAt(row);
                emit m_ctx.project->deviceChanged();
                populate();
            }
        });
        toolbar->addWidget(remove);
        toolbar->addStretch();
        root->addLayout(toolbar);
        table = new QTableWidget(this);
        table->setObjectName("sampleZoneTable");
        table->setColumnCount(15);
        table->setHorizontalHeaderLabels({"Sample", "Key Low", "Key High", "Vel Low", "Vel High", "Root", "Gain dB",
                                          "Pan", "Tune st", "Loop", "Loop Start", "Loop End", "RR Length",
                                          "RR Position", "One Shot"});
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        table->verticalHeader()->setDefaultSectionSize(26);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        root->addWidget(table, 1);
        connect(table, &QTableWidget::cellChanged, this, [this](int r, int c) { changed(r, c); });
        populate();
        connect(m_ctx.project, &Project::structureChanged, this, [this] { populate(); });
        connect(m_ctx.project, &Project::deviceChanged, this,
                [this] { QTimer::singleShot(0, this, [this] { populate(); }); });
    }
};
class MultisamplerBody final : public DeviceBody {
  public:
    MultisamplerBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        setObjectName("multisamplerBody");
        setFixedWidth(650);
        auto* root = rootRow(this);
        auto* left = vbox(3);
        left->addWidget(new StatusPlot(this, 190, 110));
        auto* import = new QPushButton("Import", this);
        import->setFixedHeight(26);
        connect(import, &QPushButton::clicked, this, [this] { importZones(this); });
        left->addWidget(import);
        auto* expand = new QPushButton("Expand", this);
        expand->setFixedHeight(26);
        connect(expand, &QPushButton::clicked, this, &DeviceBody::editorRequested);
        left->addWidget(expand);
        auto* leftGroup = new QWidget(this);
        leftGroup->setLayout(left);
        leftGroup->setFixedWidth(190);
        root->addWidget(leftGroup);
        auto* grid = new QGridLayout;
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(3);
        for (int i = 0; i < multisampler::kParams; ++i) {
            if (i == multisampler::Loop)
                grid->addWidget(toggle(i, "Loop", 60), i / 4, i % 4);
            else
                grid->addWidget(knob(i, 22), i / 4, i % 4);
        }
        root->addLayout(grid);
    }
    DeviceBody* createEditor(QWidget* p) override {
        return new ZoneEditor(ctx(), p);
    }
};
} // namespace
DeviceBody* createStudioBody(const DeviceContext& c, QWidget* p) {
    switch (c.type) {
    case T::Shifter:
    case T::PitchCorrector:
        return new PitchBody(c, p);
    case T::HybridReverb:
        return new HybridBody(c, p);
    case T::Looper:
        return new LooperBody(c, p);
    case T::Multisampler:
        return new MultisamplerBody(c, p);
    default:
        return nullptr;
    }
}
} // namespace sessions::devui
