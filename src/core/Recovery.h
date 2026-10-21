#pragma once

#include "SetFile.h"
#include <QDateTime>
#include <QObject>
#include <QThread>
#include <memory>
#include <optional>

namespace sessions {

struct RecoveryEntry {
    QString file, name, originalPath, error;
    QDateTime time;
};
struct RecoveryLease;

// Called on the UI thread. Jobs own value snapshots; the worker never reads Project,
// Transport or audio state. One active job and coalesced pending jobs bound the queue.
class RecoveryManager : public QObject {
    Q_OBJECT
public:
    explicit RecoveryManager(const QString& root = {}, QObject* parent = nullptr);
    ~RecoveryManager() override;
    static QString defaultRoot();
    static int intervalSeconds();
    static int backupCount();
    static QString backupFolder(const QString& setPath);
    QString root() const { return m_root; }
    bool busy() const { return m_busy || m_manualWrites || m_pending.has_value() || !m_backups.isEmpty(); }
    QVector<RecoveryEntry> candidates() const;
    bool restore(const QString& file, EditState& state, setfile::Extras& extras,
                 RecoveryEntry& entry, QString* error = nullptr);
    bool discard(const QString& file, QString* error = nullptr);
    void checkpoint(EditState state, setfile::Extras extras, const QString& name, const QString& originalPath);
    void backup(EditState state, setfile::Extras extras, const QString& setPath);
    quint64 saveManual(EditState state, setfile::Extras extras, const QString& setPath);
    void clear(); // retire only this document; queued old writes cannot recreate it

signals:
    void snapshotWritten(const QString& file);
    void backupWritten(const QString& file);
    void writeFailed(const QString& operation, const QString& error);
    void idle();
    void manualSaveFinished(quint64 request, bool ok, const QString& error);

private:
    struct Snapshot {
        EditState state;
        setfile::Extras extras;
        QString name, originalPath;
        std::shared_ptr<RecoveryLease> lease;
        int keep = 3;
        quint64 epoch = 0;
    };
    void next();
    static QString write(const Snapshot& snapshot, bool backup, QString& error);
    std::shared_ptr<RecoveryLease> leaseFor(const QString& dir, QString* error) const;
    bool ownFile(const QString& file) const;
    QString m_root;
    QThread m_thread;
    QObject* m_worker = nullptr;
    std::shared_ptr<RecoveryLease> m_session;
    std::optional<Snapshot> m_pending;
    QMap<QString, Snapshot> m_backups;
    bool m_busy = false;
    quint64 m_epoch = 0;
    quint64 m_nextManual = 0;
    int m_manualWrites = 0;
};
}
