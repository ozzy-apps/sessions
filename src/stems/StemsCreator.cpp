#include "StemsCreator.h"
#include "ModelAssets.h"
#include "StemImport.h"
#include "audio/Resample.h"
#include "core/DeviceSpecs.h"
#include "core/Transport.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QRunnable>
#include <QSettings>
#include <QThreadPool>
#include <QTimer>
#include <stdexcept>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
namespace sessions {
StemsCreator::StemsCreator(Project* project, Transport* transport, Backend backend, QObject* parent)
    : QObject(parent ? parent : project), m_project(project), m_transport(transport), m_backend(std::move(backend)) {
    setObjectName("stemsCreator");
#ifdef Q_OS_WIN
    m_process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
#endif
    if (m_backend.program.isEmpty()) m_backend.program = stems::workerProgram();
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &StemsCreator::processOutput);
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] { m_stderr = (m_stderr + m_process.readAllStandardError()).right(16384); });
    connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &StemsCreator::processFinished);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (m_busy && error == QProcess::FailedToStart) finish("Couldn't start the separation worker: " + m_process.errorString());
    });
    connect(project, &Project::aboutToReset, this, [this] { ++m_epoch; m_status.clear(); m_loading.clear(); cancel(); });
    connect(project, &Project::structureChanged, this, &StemsCreator::checkHost);
    connect(project, &Project::restored, this, &StemsCreator::checkHost);
    connect(project, &Project::deviceChanged, this, &StemsCreator::checkHost);
}
StemsCreator::~StemsCreator() {
    // QObject parents can be destroying their model members already. Reaping a child
    // process must not emit callbacks into the project or its remaining widgets.
    disconnect(&m_process, nullptr, this, nullptr);
    disconnect(this, nullptr, nullptr, nullptr);
    m_busy = false;
    if (m_cancel) m_cancel->store(true);
    if (m_process.state() != QProcess::NotRunning) { m_process.kill(); m_process.waitForFinished(1500); }
    if (!m_finalFolder.isEmpty()) QDir(m_finalFolder).removeRecursively();
}
StemsCreator* StemsCreator::forProject(Project* project, Transport* transport) {
    if (auto* existing = project->findChild<StemsCreator*>("stemsCreator", Qt::FindDirectChildrenOnly)) return existing;
    return new StemsCreator(project, transport, {});
}
Device* StemsCreator::device(int id) const {
    for (int i = 0; i <= m_project->trackCount(); ++i) if (auto* track = m_project->trackAt(i))
        for (auto& d : track->devices) if (d.id == id && deviceType(d.name) == DeviceType::StemsCreator) return &d;
    return nullptr;
}
QString StemsCreator::status(int id) const {
    if (m_busy && id == m_device) return m_jobStatus;
    if (m_status.contains(id)) return m_status.value(id);
    const auto* source = device(id);
    return source && source->sample ? "Ready" : "Load an audio file";
}
QString StemsCreator::unavailableReason() const {
    if (!QFileInfo(m_backend.program).isFile()) return "The separation worker is missing. Reinstall Sessions.";
    if (!m_backend.verifyModels) return {};
    const QString runtime = m_backend.runtime.isEmpty() ? stems::runtimeLibrary() : m_backend.runtime;
    if (!QFileInfo(runtime).isFile()) return "The neural runtime is missing. Run the stems asset setup when building from source.";
    if (!stems::modelFilesPresent(m_backend.models.isEmpty() ? stems::modelDirectory() : m_backend.models))
        return "The five separation models are missing. Choose the model folder.";
    return {};
}
void StemsCreator::loadSource(int id, const QString& path) {
    if (!device(id) || (m_busy && id == m_device)) return;
    if (!QFileInfo(path).isFile()) { m_status[id] = "The audio file doesn't exist."; emit changed(); return; }
    const quint64 load = ++m_loadTickets[id], epoch = m_epoch;
    m_loading.insert(id);
    m_status[id] = "Loading audio…"; emit changed();
    samples::loadAsync({path}, this, [this, id, path, load, epoch](QVector<SamplePtr> samples, QStringList errors) {
        if (epoch != m_epoch || m_loadTickets.value(id) != load) return;
        m_loading.remove(id);
        if (!device(id)) { m_status.remove(id); emit changed(); return; }
        if (samples.isEmpty() || !samples[0]) { m_status[id] = errors.isEmpty() ? "Couldn't load the audio." : errors.join("\n"); emit changed(); return; }
        {
            EditScope edit(m_project, "Load Stems Source");
            auto* d = device(id); d->samplePath = QFileInfo(path).absoluteFilePath(); d->sample = samples[0];
            emit m_project->deviceChanged();
        }
        m_status[id] = "Ready"; emit changed();
    });
}
void StemsCreator::checkHost() {
    if (!m_busy || m_importing) return;
    const auto* host = device(m_device);
    if (m_epoch != m_jobEpoch || !host || host->samplePath != m_sourcePath) cancel();
}
void StemsCreator::start(int id) {
    if (m_busy || m_loading.contains(id)) return;
    const auto* host = device(id);
    auto reject = [&](const QString& error) { m_status[id] = error; emit changed(); emit finished(false, -1, error); };
    if (!host || !host->sample) { reject("Load an audio file first."); return; }
    if (m_transport->recordingInProgress()) { reject("Stop recording before creating stems."); return; }
    const QString missing = unavailableReason();
    if (!missing.isEmpty()) { reject(missing); return; }
    QString why;
    if (!stems::roomForStems(*m_project, &why)) { reject(why); return; }
    const auto source = host->sample;
    if (source->rate <= 0 || source->frames() <= 0 || source->seconds() > 3600) { reject("Choose audio between one sample and one hour long."); return; }
    QString parent = QSettings("Sessions", "Sessions").value("stems/outputFolder").toString();
    if (parent.isEmpty()) parent = m_transport->recordFolder() + "/Stems";
    if (!QDir().mkpath(parent)) { reject("Couldn't create the stems output folder."); return; }
    auto work = std::make_shared<QTemporaryDir>(QDir(parent).filePath(".sessions-stems-XXXXXX"));
    if (!work->isValid()) { reject("Couldn't prepare the separation folder."); return; }
    m_work = work; m_device = id; m_sourcePath = host->samplePath;
    m_sourceName = QFileInfo(m_sourcePath).completeBaseName();
    m_sourceName.replace(QRegularExpression(R"([\\/:*?"<>|])"), "_"); m_sourceName = m_sourceName.trimmed().left(80);
    if (m_sourceName.isEmpty()) m_sourceName = "Audio";
    m_busy = true; m_phase = Phase::Preparing; m_jobEpoch = m_epoch;
    m_progress = 0; m_jobStatus = "Preparing audio…"; m_workerError.clear(); m_stdout.clear(); m_stderr.clear();
    m_startBeat = m_transport->startMarker();
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    const auto stop = m_cancel; const quint64 ticket = ++m_ticket;
    const int threads = std::clamp(int(std::round(paramValue(DeviceType::StemsCreator, 0, host->params.value(0).value))), 1, 8);
    emit changed();
    QPointer<StemsCreator> weak(this);
    QThreadPool::globalInstance()->start(QRunnable::create([weak, source, work, stop, ticket, threads] {
        QString error; const qint64 count = std::max<qint64>(1, qint64(std::llround(source->frames() * 44100. / source->rate)));
        try {
            samples::WavWriter writer;
            if (!writer.open(QDir(work->path()).filePath("input.wav"), 44100, samples::WavFormat::Float32, &error)) throw std::runtime_error(error.toStdString());
            audio::resample::prepare();
            std::vector<float> data(8192);
            const double step = source->rate / 44100.;
            for (qint64 done = 0; done < count;) {
                if (stop->load()) throw std::runtime_error("Cancelled.");
                const int n = int(std::min<qint64>(4096, count - done));
                for (int i = 0; i < n; ++i) {
                    const double at = (done + i) * step;
                    data[size_t(i) * 2] = audio::resample::read(source->left, at, step);
                    data[size_t(i) * 2 + 1] = audio::resample::read(source->right.empty() ? source->left : source->right, at, step);
                }
                writer.write(data.data(), n); done += n;
            }
            if (!writer.close(&error)) throw std::runtime_error(error.toStdString());
        } catch (const std::exception& e) { error = QString::fromUtf8(e.what()); }
        // Queue through the app, then inspect the project guard on its owning thread.
        // The project can disappear while preparation is running.
        QMetaObject::invokeMethod(QCoreApplication::instance(), [weak, ticket, threads, count, error] {
            if (!weak || !weak->m_busy || weak->m_ticket != ticket) return;
            auto* self = weak.data();
            if (self->m_cancel->load()) { self->finish("Cancelled."); return; }
            if (!error.isEmpty()) { self->finish(error); return; }
            self->m_frames = count; self->m_phase = Phase::Running; self->m_jobStatus = "Analyzing audio…";
            QStringList args = self->m_backend.arguments;
            args << "--input" << QDir(self->m_work->path()).filePath("input.wav") << "--output" << self->m_work->path()
                 << "--models" << (self->m_backend.models.isEmpty() ? stems::modelDirectory() : self->m_backend.models)
                 << "--runtime" << (self->m_backend.runtime.isEmpty() ? stems::runtimeLibrary() : self->m_backend.runtime)
                 << "--threads" << QString::number(threads);
            self->m_process.setWorkingDirectory(self->m_work->path());
            self->m_process.start(self->m_backend.program, args); emit self->changed();
        }, Qt::QueuedConnection);
    }));
}
void StemsCreator::processOutput() {
    m_stdout += m_process.readAllStandardOutput();
    if (m_stdout.size() > 1024 * 1024) { m_workerError = "The separation worker returned excessive output."; m_process.kill(); return; }
    int end;
    while ((end = m_stdout.indexOf('\n')) >= 0) {
        const QByteArray line = m_stdout.left(end); m_stdout.remove(0, end + 1);
        const auto object = QJsonDocument::fromJson(line).object();
        if (object["schema"].toInt() != 1) continue;
        const QString phase = object["phase"].toString(), detail = object["detail"].toString().left(1024);
        if (phase == "error") m_workerError = detail;
        if (m_cancel && m_cancel->load()) continue;
        if (phase == "loading") m_jobStatus = "Loading separation models…";
        else if (phase == "separating") m_jobStatus = "Separating " + detail + "…";
        else if (phase == "complete") m_jobStatus = "Loading separated audio…";
        const double progress = object["progress"].toDouble();
        if (std::isfinite(progress)) m_progress = std::max(m_progress, std::clamp(progress, 0., 1.) * .95);
        emit changed();
    }
}
void StemsCreator::processFinished(int code, QProcess::ExitStatus status) {
    if (!m_busy) return;
    processOutput(); m_stderr = (m_stderr + m_process.readAllStandardError()).right(16384);
    if (m_cancel->load() || m_epoch != m_jobEpoch) { finish("Cancelled."); return; }
    if (code != 0 || status != QProcess::NormalExit || !m_workerError.isEmpty()) {
        finish(m_workerError.isEmpty() ? "Separation failed. " + QString::fromUtf8(m_stderr).right(1024) : m_workerError); return;
    }
    QString why;
    auto result = stems::readResult(m_work->path(), m_frames, &why);
    if (!result.valid()) { finish(why); return; }
    QFile::remove(QDir(m_work->path()).filePath("input.wav"));
    const QString parent = QFileInfo(m_work->path()).absolutePath(), base = m_sourceName + " Stems";
    QString final = QDir(parent).filePath(base);
    for (int i = 2; QFileInfo::exists(final); ++i) final = QDir(parent).filePath(base + " " + QString::number(i));
    if (!QDir().rename(m_work->path(), final)) { finish("Couldn't publish the separated audio folder."); return; }
    m_work->setAutoRemove(false); m_finalFolder = final;
    QStringList paths;
    for (auto& file : result.files) { file.path = QDir(final).filePath(file.name + ".wav"); paths.push_back(file.path); }
    m_phase = Phase::Decoding; m_jobStatus = "Loading separated audio…"; emit changed();
    const auto ticket = m_ticket;
    samples::loadAsync(paths, this, [this, ticket, result](QVector<SamplePtr> decoded, QStringList errors) {
        if (!m_busy || ticket != m_ticket) return;
        if (m_cancel->load() || m_epoch != m_jobEpoch || !device(m_device)) { finish("Cancelled."); return; }
        if (!errors.isEmpty()) { finish(errors.join("\n")); return; }
        QString why;
        m_importing = true;
        const int group = stems::importTracks(*m_project, result, decoded, m_sourceName + " Stems", m_startBeat, &why);
        m_importing = false;
        if (group < 0) { finish(why); return; }
        const int id = m_project->tracks[group].id;
        m_finalFolder.clear(); // these files now belong to the set and must survive undo/redo
        finish({}, id);
    });
}
void StemsCreator::cancel() {
    if (!m_busy || !m_cancel) return;
    m_cancel->store(true); m_jobStatus = "Canceling…"; emit changed();
    if (m_phase == Phase::Running && m_process.state() != QProcess::NotRunning) {
        m_process.terminate(); const auto ticket = m_ticket;
        QTimer::singleShot(1500, this, [this, ticket] { if (m_busy && ticket == m_ticket && m_process.state() != QProcess::NotRunning) m_process.kill(); });
    }
}
void StemsCreator::finish(const QString& error, int groupId) {
    if (!m_busy) return;
    if (m_epoch == m_jobEpoch) m_status[m_device] = error.isEmpty() ? "Created five grouped tracks" : error == "Cancelled." ? "Cancelled" : error;
    if (!error.isEmpty() && !m_finalFolder.isEmpty()) QDir(m_finalFolder).removeRecursively();
    m_finalFolder.clear(); m_work.reset(); m_busy = false; m_phase = Phase::Idle;
    m_progress = error.isEmpty() ? 1 : 0; ++m_ticket;
    emit changed(); emit finished(error.isEmpty(), groupId, error);
}
} // namespace sessions
