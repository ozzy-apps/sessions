#include "SamplePacks.h"
#include "audio/DrumSamples.h"
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkReply>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>
namespace sessions {
QJsonArray SamplePacks::catalog(const QByteArray& data) {
    QJsonArray result;
    if (data.size() > 256 * 1024)
        return result;
    const auto document = QJsonDocument::fromJson(data).object();
    if (document["schema"].toInt() != 1)
        return result;
    for (const auto& value : document["packs"].toArray()) {
        const auto pack = value.toObject();
        const QUrl url(pack["url"].toString());
        if (!pack["id"].toString().isEmpty() && !pack["name"].toString().isEmpty() && url.isValid() &&
            url.scheme() == "https" && url.userInfo().isEmpty())
            result.append(pack);
    }
    return result;
}
bool SamplePacks::validContentFolder(const QString& folder) {
    QFile file(QDir(folder).filePath("library.json"));
    return file.open(QIODevice::ReadOnly) && file.size() <= 16 * 1024 * 1024 &&
           !QJsonDocument::fromJson(file.readAll()).object()["banks"].toArray().isEmpty();
}
SamplePacks::SamplePacks(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Sample packs");
    setObjectName("samplePacks");
    resize(580, 420);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 20);
    layout->setSpacing(14);
    m_list = new QListWidget(this);
    m_list->setObjectName("samplePackList");
    layout->addWidget(m_list, 1);
    m_detail = new QLabel(this);
    m_detail->setWordWrap(true);
    m_detail->setTextFormat(Qt::PlainText);
    layout->addWidget(m_detail);
    auto* instructions = new QLabel("Download and extract the ZIP. For Drum Studio libraries, choose the extracted "
                                    "drum-content folder below. Other samples can be added through Browser → Places.",
                                    this);
    instructions->setWordWrap(true);
    layout->addWidget(instructions);
    m_download = new QPushButton("Download", this);
    m_download->setObjectName("samplePackDownload");
    auto* locate = new QPushButton("Locate Drum Studio content…", this);
    locate->setObjectName("samplePackLocate");
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->addButton(locate, QDialogButtonBox::ActionRole);
    buttons->addButton(m_download, QDialogButtonBox::ActionRole);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_list, &QListWidget::currentRowChanged, this, [this] { selectPack(); });
    connect(m_download, &QPushButton::clicked, this, [this] {
        const int index = m_list->currentRow();
        if (index < 0 || index >= m_packs.size())
            return;
        const QUrl url(m_packs.at(index).toObject()["url"].toString());
        if (!QDesktopServices::openUrl(url))
            QMessageBox::warning(this, "Sample packs", "Couldn't open the download in your browser.");
    });
    connect(locate, &QPushButton::clicked, this, [this] {
        const QString folder = QFileDialog::getExistingDirectory(
            this, "Choose drum-content folder", QSettings("Sessions", "Sessions").value("content/drumRoot").toString());
        if (folder.isEmpty())
            return;
        if (!validContentFolder(folder)) {
            QMessageBox::warning(this, "Sample packs",
                                 "Choose the extracted folder containing library.json and the audio folder.");
            return;
        }
        QSettings("Sessions", "Sessions").setValue("content/drumRoot", QDir(folder).absolutePath());
        audio::drums::ContentLoader::instance()->reload();
        m_detail->setText("Drum Studio content: " + QDir(folder).absolutePath());
    });
    QFile bundled(":/content-packs.json");
    if (bundled.open(QIODevice::ReadOnly))
        populate(catalog(bundled.readAll()));
    QNetworkRequest request(
        QUrl("https://raw.githubusercontent.com/ozzy-apps/sessions/main/distribution/content-packs.json"));
    request.setRawHeader("User-Agent", "Sessions");
    request.setTransferTimeout(10000);
    auto* reply = m_network.get(request);
    connect(reply, &QNetworkReply::readyRead, this, [reply] {
        if (reply->bytesAvailable() > 256 * 1024)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const auto packs = reply->error() == QNetworkReply::NoError ? catalog(reply->readAll()) : QJsonArray();
        if (!packs.isEmpty())
            populate(packs);
        reply->deleteLater();
    });
}
void SamplePacks::populate(const QJsonArray& packs) {
    m_packs = packs;
    m_list->clear();
    for (const auto& value : packs)
        m_list->addItem(value.toObject()["name"].toString());
    if (!packs.isEmpty())
        m_list->setCurrentRow(0);
    selectPack();
}
void SamplePacks::selectPack() {
    const int index = m_list->currentRow();
    m_download->setEnabled(index >= 0 && index < m_packs.size());
    m_detail->setText(index < 0 ? "No sample packs available."
                                : m_packs.at(index).toObject()["description"].toString());
}
} // namespace sessions
