#include "Updates.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSettings>
#include <QSysInfo>
#include <QVersionNumber>

namespace sessions {
namespace {
QString stableVersion(QString value) {
    static const QRegularExpression pattern("^v?([0-9]+\\.[0-9]+\\.[0-9]+)(?:\\+[A-Za-z0-9.-]+)?$");
    return pattern.match(value).captured(1);
}
} // namespace
Updates::Updates(const QString& current, QObject* parent, QNetworkAccessManager* network)
    : QObject(parent), m_current(current), m_client(network ? network : &m_network) {}
QUrl Updates::releasesUrl() {
    return QUrl("https://github.com/ozzy-apps/sessions/releases/latest");
}
bool Updates::safeReleaseUrl(const QUrl& url) {
    return url.scheme() == "https" && url.host() == "github.com" && url.userInfo().isEmpty() &&
           (url.port() == -1 || url.port() == 443) && url.query().isEmpty() && url.fragment().isEmpty() &&
           url.path().startsWith("/ozzy-apps/sessions/releases/") && !url.path().contains("/../");
}
bool Updates::newer(const QString& candidate, const QString& current) {
    const QString a = stableVersion(candidate), b = stableVersion(current);
    return !a.isEmpty() && !b.isEmpty() && QVersionNumber::fromString(a) > QVersionNumber::fromString(b);
}
PackageKind Updates::packageKind() {
#ifdef Q_OS_WIN
    return PackageKind::Windows;
#else
    if (qEnvironmentVariableIsSet("FLATPAK_ID"))
        return PackageKind::Flatpak;
    if (qEnvironmentVariableIsSet("APPIMAGE"))
        return PackageKind::AppImage;
    return QCoreApplication::applicationDirPath() == "/usr/bin" ? PackageKind::Debian : PackageKind::Source;
#endif
}
ReleaseInfo Updates::parse(const QJsonObject& json, PackageKind kind, const QString& architecture) {
    ReleaseInfo result;
    if (json["draft"].toBool() || json["prerelease"].toBool())
        return result;
    result.version = stableVersion(json["tag_name"].toString());
    result.page = QUrl(json["html_url"].toString());
    if (result.version.isEmpty() || !safeReleaseUrl(result.page))
        return {};
    result.notes = json["body"].toString().left(12000);
    QString extension;
    switch (kind) {
    case PackageKind::Debian:
        extension = ".deb";
        break;
    case PackageKind::AppImage:
        extension = ".AppImage";
        break;
    case PackageKind::Flatpak:
        extension = ".flatpak";
        break;
    case PackageKind::Windows:
        extension = ".zip";
        break;
    case PackageKind::Source:
        break;
    }
    const bool x64 = architecture == "x86_64" || architecture == "amd64";
    for (const auto& value : json["assets"].toArray()) {
        const auto asset = value.toObject();
        const QString name = asset["name"].toString();
        if (extension.isEmpty() || !x64 || !name.endsWith(extension) || name.contains("drum", Qt::CaseInsensitive))
            continue;
        if (kind == PackageKind::Windows && !name.contains("windows", Qt::CaseInsensitive))
            continue;
        if (kind != PackageKind::Windows && name.contains("windows", Qt::CaseInsensitive))
            continue;
        if (!name.contains("x86_64") && !name.contains("amd64"))
            continue;
        const QUrl url(asset["browser_download_url"].toString());
        if (safeReleaseUrl(url) && url.path().startsWith("/ozzy-apps/sessions/releases/download/")) {
            result.download = url;
            result.asset = name;
            break;
        }
    }
    return result;
}
bool Updates::shouldPrompt(const QString& version, const QDateTime& now) {
    QSettings s("Sessions", "Sessions");
    if (s.value("updates/skippedVersion").toString() == version)
        return false;
    const auto until = s.value("updates/postponedUntil").toDateTime();
    return s.value("updates/postponedVersion").toString() != version || !until.isValid() || now >= until;
}
void Updates::postpone(const QString& version, const QDateTime& now) {
    QSettings s("Sessions", "Sessions");
    s.setValue("updates/postponedVersion", version);
    s.setValue("updates/postponedUntil", now.addSecs(24 * 60 * 60));
}
void Updates::skip(const QString& version) {
    QSettings("Sessions", "Sessions").setValue("updates/skippedVersion", version);
}
void Updates::check(bool manual) {
    if (m_busy || (!manual && !QSettings("Sessions", "Sessions").value("updates/enabled", true).toBool()))
        return;
    if (stableVersion(m_current).isEmpty()) {
        if (manual)
            emit checked("This development build has no release version.", false);
        return;
    }
    m_busy = true;
    QNetworkRequest request(QUrl("https://api.github.com/repos/ozzy-apps/sessions/releases/latest"));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", ("Sessions/" + m_current).toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(10000);
    auto* reply = m_client->get(request);
    connect(reply, &QNetworkReply::readyRead, this, [reply] {
        if (reply->bytesAvailable() > 1024 * 1024)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, manual] {
        m_busy = false;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto data = reply->readAll();
        const bool failed = reply->error() != QNetworkReply::NoError;
        reply->deleteLater();
        if (status == 404) {
            if (manual)
                emit checked("No public releases have been published yet.", false);
            return;
        }
        if (failed || status != 200 || data.size() > 1024 * 1024) {
            if (manual)
                emit checked("Couldn't check for updates. Try again later or visit GitHub Releases.", true);
            return;
        }
        const auto release =
            parse(QJsonDocument::fromJson(data).object(), packageKind(), QSysInfo::currentCpuArchitecture());
        if (!release.valid()) {
            if (manual)
                emit checked("The release information is unavailable. Try again later.", true);
        } else if (newer(release.version, m_current)) {
            if (manual || shouldPrompt(release.version))
                emit available(release, manual);
        } else if (manual)
            emit checked("Sessions is up to date.", false);
    });
}
} // namespace sessions
