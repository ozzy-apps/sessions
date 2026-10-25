#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QUrl>

namespace sessions {
enum class PackageKind { Debian, AppImage, Flatpak, Windows, Source };
struct ReleaseInfo {
    QString version;
    QString notes;
    QUrl page;
    QUrl download;
    QString asset;
    bool valid() const {
        return !version.isEmpty() && page.isValid();
    }
};

class Updates : public QObject {
    Q_OBJECT
  public:
    explicit Updates(const QString& current, QObject* parent = nullptr, QNetworkAccessManager* network = nullptr);
    void check(bool manual = false);
    void postpone(const QString& version, const QDateTime& now = QDateTime::currentDateTimeUtc());
    void skip(const QString& version);
    static bool newer(const QString& candidate, const QString& current);
    static ReleaseInfo parse(const QJsonObject& json, PackageKind kind, const QString& architecture);
    static PackageKind packageKind();
    static bool shouldPrompt(const QString& version, const QDateTime& now = QDateTime::currentDateTimeUtc());
    static QUrl releasesUrl();
    static bool safeReleaseUrl(const QUrl& url);
  signals:
    void available(const sessions::ReleaseInfo& release, bool manual);
    void checked(const QString& message, bool error);

  private:
    QString m_current;
    QNetworkAccessManager m_network;
    QNetworkAccessManager* m_client;
    bool m_busy = false;
};
} // namespace sessions
Q_DECLARE_METATYPE(sessions::ReleaseInfo)
