#include "MainWindow.h"
#include "RecoveryDialog.h"
#include "InfoView.h"
#include "core/Recovery.h"
#include "core/Transport.h"
#include <QMessageBox>
#include <QTimer>

namespace sessions {
void MainWindow::enableRecovery(const QString& root) {
    if (m_recovery) return;
    m_recovery = new RecoveryManager(root, this);
    m_recoveryTimer = new QTimer(this); m_recoveryTimer->setObjectName("recoveryTimer");
    connect(m_recoveryTimer, &QTimer::timeout, this, &MainWindow::saveRecoveryNow);
    connect(m_recovery, &RecoveryManager::writeFailed, this, [this](const QString& operation, const QString& error) {
        m_info->show(operation + " failed", error);
    });
    connect(m_transport, &Transport::recordingChanged, this, [this] {
        // Capture a newly finished take promptly rather than waiting another two minutes.
        if (!m_transport->sessionRecording() && !m_transport->audioRecording() && !m_transport->arrangementRecord())
            QTimer::singleShot(0, this, [this] { if (RecoveryManager::intervalSeconds()) saveRecoveryNow(); });
    });
    if (const int seconds = RecoveryManager::intervalSeconds()) m_recoveryTimer->start(seconds * 1000);
}
void MainWindow::saveRecoveryNow() {
    if (!m_recovery || m_closeConfirmed) return;
    const bool recording = m_transport->sessionRecording() || m_transport->audioRecording()
        || (m_transport->arrangementRecord() && m_transport->playing());
    if (!setModified() && !recording) { m_recovery->clear(); return; }
    auto state = m_project->captureEditState(); QString error;
    if (!m_transport->captureRecovery(state, &error)) { m_info->show("Recording checkpoint failed", error); return; }
    auto extras = currentExtras(); setfile::fillFromProject(extras, *m_project);
    m_recovery->checkpoint(std::move(state), std::move(extras), m_setName, m_setPath);
}
bool MainWindow::recoverSet(const QString& file, QString* error) {
    if (m_saveInProgress) { if (error) *error = "A save is still in progress"; return false; }
    enableRecovery(); EditState state; setfile::Extras extras; RecoveryEntry entry;
    if (!m_recovery->restore(file, state, extras, entry, error)) return false;
    releaseMidiKeys(); m_transport->reset(); m_project->loadEditState(state);
    setfile::applyToProject(extras, *m_project); applyExtras(extras);
    setReplaced({}, entry.name.endsWith(" (Recovered)") ? entry.name : entry.name + " (Recovered)", true);
    m_forcedModified = true; updateTitle(); saveRecoveryNow();
    return true;
}
bool MainWindow::offerRecovery(bool startup) {
    if (m_saveInProgress) return false;
    enableRecovery(); if (startup && m_recovery->candidates().isEmpty()) return false;
    RecoveryDialog dialog(m_recovery, this);
    if (dialog.exec() != QDialog::Accepted || !maybeSave()) return false;
    QString error;
    if (!recoverSet(dialog.selectedFile(), &error)) { QMessageBox::warning(this, "Recover Set", error); return false; }
    return true;
}
}
