#include "MainWindow.h"
#include "UpdateDialog.h"
#include "core/Updates.h"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QMessageBox>
namespace sessions {
void MainWindow::checkForUpdates(bool manual) {
    if (!m_updates) {
        m_updates = new Updates(QCoreApplication::applicationVersion(), this);
        connect(m_updates, &Updates::checked, this, [this](const QString& text, bool error) {
            if (error)
                QMessageBox::warning(this, "Check for updates", text);
            else
                QMessageBox::information(this, "Check for updates", text);
        });
        connect(m_updates, &Updates::available, this, [this](const ReleaseInfo& release, bool) {
            auto* dialog = new UpdateDialog(release, this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            connect(dialog, &QDialog::finished, this, [this, release](int choice) {
                if (choice == UpdateDialog::Download) {
                    if (QDesktopServices::openUrl(release.download.isEmpty() ? release.page : release.download))
                        m_updates->postpone(release.version);
                    else
                        QMessageBox::warning(this, "Sessions update", "Couldn't open the download in your browser.");
                } else if (choice == UpdateDialog::Skip)
                    m_updates->skip(release.version);
                else
                    m_updates->postpone(release.version);
            });
            dialog->open();
        });
    }
    m_updates->check(manual);
}
} // namespace sessions
