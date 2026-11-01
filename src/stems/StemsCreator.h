#pragma once
#include "core/Model.h"
#include <QProcess>
#include <QTemporaryDir>
#include <QHash>
#include <atomic>
namespace sessions {
class Transport;
// The native worker is a separate process: inference never enters the audio callback.
class StemsCreator : public QObject {
    Q_OBJECT
public:
    struct Backend {
        QString program, models, runtime;
        QStringList arguments;
        bool verifyModels = true;
    };
    StemsCreator(Project* project, Transport* transport, Backend backend, QObject* parent = nullptr);
    ~StemsCreator() override;
    static StemsCreator* forProject(Project* project, Transport* transport);
    void loadSource(int deviceId, const QString& file);
    void start(int deviceId);
    void cancel();
    bool busy() const { return m_busy; }
    bool loadingSource(int id) const { return m_loading.contains(id); }
    int activeDevice() const { return m_device; }
    double progress() const { return m_progress; }
    QString status(int deviceId) const;
    QString unavailableReason() const;
    Device* device(int deviceId) const;
signals:
    void changed();
    void finished(bool success, int groupId, const QString& error);
private:
    void processOutput();
    void processFinished(int code, QProcess::ExitStatus status);
    void finish(const QString& error, int groupId = -1);
    void checkHost();
    Project* m_project;
    Transport* m_transport;
    Backend m_backend;
    QProcess m_process;
    enum class Phase { Idle, Preparing, Running, Decoding } m_phase = Phase::Idle;
    bool m_busy = false, m_importing = false;
    int m_device = 0;
    quint64 m_epoch = 0, m_ticket = 0, m_jobEpoch = 0;
    QSet<int> m_loading;
    QHash<int, quint64> m_loadTickets;
    QHash<int, QString> m_status;
    QByteArray m_stdout, m_stderr;
    QString m_jobStatus, m_sourcePath, m_sourceName, m_finalFolder, m_workerError;
    double m_progress = 0, m_startBeat = 0;
    qint64 m_frames = 0;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::shared_ptr<QTemporaryDir> m_work;
};
} // namespace sessions
