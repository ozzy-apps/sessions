#pragma once
#include <QDialog>
#include <QJsonArray>
#include <QNetworkAccessManager>
class QListWidget;
class QLabel;
class QPushButton;
namespace sessions {
class SamplePacks : public QDialog {
    Q_OBJECT
  public:
    explicit SamplePacks(QWidget* parent = nullptr);
    static QJsonArray catalog(const QByteArray& data);
    static bool validContentFolder(const QString& folder);

  private:
    void populate(const QJsonArray& packs);
    void selectPack();
    QListWidget* m_list;
    QLabel* m_detail;
    QPushButton* m_download;
    QJsonArray m_packs;
    QNetworkAccessManager m_network;
};
} // namespace sessions
