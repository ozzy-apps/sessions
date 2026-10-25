#include "UpdateDialog.h"
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>
namespace sessions {
UpdateDialog::UpdateDialog(const ReleaseInfo& release, QWidget* parent) : QDialog(parent) {
    setObjectName("updateDialog");
    setWindowTitle("Sessions update");
    resize(540, 370);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 20);
    layout->setSpacing(14);
    auto* title = new QLabel(QString("Sessions %1 is available").arg(release.version), this);
    QFont font = title->font();
    font.setPointSizeF(font.pointSizeF() + 3);
    font.setBold(true);
    title->setFont(font);
    layout->addWidget(title);
    auto* detail =
        new QLabel(QString("Installed: %1\n%2")
                       .arg(QCoreApplication::applicationVersion(),
                            release.asset.isEmpty() ? "Download from GitHub Releases." : "Download: " + release.asset),
                   this);
    detail->setWordWrap(true);
    layout->addWidget(detail);
    auto* notes = new QTextBrowser(this);
    notes->setObjectName("updateNotes");
    notes->setPlainText(release.notes.isEmpty() ? "See GitHub Releases for the changes in this version."
                                                : release.notes);
    notes->setOpenExternalLinks(false);
    layout->addWidget(notes, 1);
    auto* hint = new QLabel("Postpone reminds you after 24 hours. Skip applies to this version.", this);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto* buttons = new QDialogButtonBox(this);
    auto* skip = buttons->addButton("Skip this version", QDialogButtonBox::RejectRole);
    skip->setObjectName("updateSkip");
    auto* later = buttons->addButton("Postpone", QDialogButtonBox::ActionRole);
    later->setObjectName("updatePostpone");
    auto* download = buttons->addButton("Download", QDialogButtonBox::AcceptRole);
    download->setObjectName("updateDownload");
    download->setDefault(true);
    connect(skip, &QPushButton::clicked, this, [this] { done(Skip); });
    connect(later, &QPushButton::clicked, this, [this] { done(Postpone); });
    connect(download, &QPushButton::clicked, this, [this] { done(Download); });
    layout->addWidget(buttons);
}
} // namespace sessions
