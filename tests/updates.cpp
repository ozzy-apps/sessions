#include "core/Updates.h"
#include "core/Theme.h"
#include "ui/SamplePacks.h"
#include "ui/UpdateDialog.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <cstdio>
#include <cstring>
using namespace sessions;
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* text) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", text);
    }
}
QJsonObject release(QString tag = "v0.52.0") {
    const QString base = "https://github.com/ozzy-apps/sessions/releases/";
    QJsonArray assets;
    for (const QString& name :
         {"sessions-linux-amd64.deb", "Sessions-linux-x86_64.AppImage", "Sessions-linux-x86_64.flatpak",
          "Sessions-windows-x86_64.zip", "sessions-drums-amd64.deb"})
        assets.append(QJsonObject{{"name", name}, {"browser_download_url", base + "download/" + tag + "/" + name}});
    return {{"tag_name", tag},
            {"html_url", base + "tag/" + tag},
            {"body", "New devices and workflow improvements."},
            {"assets", assets}};
}
class Reply : public QNetworkReply {
    QByteArray body;
    qint64 at = 0;

  public:
    Reply(const QNetworkRequest& request, QByteArray data, int status, QObject* parent)
        : QNetworkReply(parent), body(std::move(data)) {
        setRequest(request);
        setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        if (status != 200)
            setError(QNetworkReply::ContentNotFoundError, "Unavailable");
        open(QIODevice::ReadOnly);
        QTimer::singleShot(0, this, [this] {
            emit readyRead();
            setFinished(true);
            emit finished();
        });
    }
    void abort() override {
        setError(QNetworkReply::OperationCanceledError, "Cancelled");
    }
    qint64 bytesAvailable() const override {
        return body.size() - at + QNetworkReply::bytesAvailable();
    }
    qint64 readData(char* out, qint64 count) override {
        const qint64 n = std::min(count, qint64(body.size()) - at);
        if (!n)
            return -1;
        std::memcpy(out, body.constData() + at, size_t(n));
        at += n;
        return n;
    }
};
class Network : public QNetworkAccessManager {
  public:
    QByteArray body = QJsonDocument(release()).toJson();
    int status = 200, requests = 0;
    QNetworkReply* createRequest(Operation, const QNetworkRequest& r, QIODevice*) override {
        ++requests;
        return new Reply(r, body, status, this);
    }
};
} // namespace
int main(int argc, char** argv) {
    QTemporaryDir temporary;
    qputenv("XDG_CONFIG_HOME", temporary.path().toUtf8());
    qputenv("XDG_DATA_HOME", temporary.path().toUtf8());
    QApplication app(argc, argv);
    app.setApplicationVersion("0.51.0");
    Q_INIT_RESOURCE(resources);
    QSettings settings("Sessions", "Sessions");
    settings.clear();
    check(Updates::newer("v0.52.0", "0.51.0"), "higher stable version");
    check(Updates::newer("1.0.0", "0.99.9"), "major transition");
    check(!Updates::newer("0.51.0", "0.51.0+20261025"), "build timestamp is not a new release");
    check(!Updates::newer("0.50.9", "0.51.0"), "older version cannot downgrade");
    check(!Updates::newer("0.52.0-rc.1", "0.51.0"), "prerelease excluded");
    check(!Updates::newer("oops", "0.51.0") && !Updates::newer("1.0.0", "dev"), "invalid versions excluded");
    for (const auto kind : {PackageKind::Debian, PackageKind::AppImage, PackageKind::Flatpak, PackageKind::Windows}) {
        const auto r = Updates::parse(release(), kind, "x86_64");
        check(r.valid() && !r.download.isEmpty() && !r.asset.contains("drum"), "platform asset available");
        check((kind == PackageKind::Windows) == r.asset.contains("windows"), "Windows and Linux assets isolated");
    }
    check(Updates::parse(release(), PackageKind::Source, "x86_64").download.isEmpty(),
          "source build uses release page");
    check(Updates::parse(release(), PackageKind::Debian, "arm64").download.isEmpty(),
          "unsupported architecture uses page");
    auto bad = release();
    bad["draft"] = true;
    check(!Updates::parse(bad, PackageKind::Debian, "x86_64").valid(), "draft excluded");
    bad = release();
    bad["prerelease"] = true;
    check(!Updates::parse(bad, PackageKind::Debian, "x86_64").valid(), "prerelease flag excluded");
    bad = release();
    bad["html_url"] = "https://evil.example/releases/tag/v1";
    check(!Updates::parse(bad, PackageKind::Debian, "x86_64").valid(), "foreign release rejected");
    bad = release();
    bad["assets"] =
        QJsonArray{QJsonObject{{"name", "sessions-amd64.deb"}, {"browser_download_url", "file:///tmp/package.deb"}}};
    check(Updates::parse(bad, PackageKind::Debian, "x86_64").download.isEmpty(), "unsafe asset uses release page");
    check(!Updates::safeReleaseUrl(QUrl("https://github.com.evil.example/ozzy-apps/sessions/releases/latest")),
          "lookalike host rejected");
    const auto now = QDateTime::fromString("2026-10-25T12:00:00Z", Qt::ISODate);
    Network network;
    Updates updates("0.51.0", nullptr, &network);
    updates.postpone("0.52.0", now);
    check(!Updates::shouldPrompt("0.52.0", now.addSecs(86399)), "24 hour postponement");
    check(Updates::shouldPrompt("0.52.0", now.addSecs(86400)), "postponement expires");
    check(Updates::shouldPrompt("0.53.0", now), "newer release bypasses older postponement");
    updates.skip("0.52.0");
    check(!Updates::shouldPrompt("0.52.0", now.addDays(10)) && Updates::shouldPrompt("0.53.0", now),
          "version-specific persistent skip");
    QSignalSpy available(&updates, &Updates::available), checked(&updates, &Updates::checked);
    updates.check(false);
    QTest::qWait(10);
    check(available.isEmpty() && checked.isEmpty(), "startup skip is silent");
    updates.check(true);
    check(available.wait(1000), "manual check bypasses skip");
    settings.setValue("updates/enabled", false);
    int requests = network.requests;
    updates.check(false);
    QTest::qWait(10);
    check(network.requests == requests, "startup preference honoured");
    network.status = 404;
    updates.check(true);
    check(checked.wait(1000) && !checked.last()[1].toBool(), "no release handled");
    network.status = 503;
    updates.check(true);
    check(checked.wait(1000) && checked.last()[1].toBool(), "manual failure reported");
    settings.setValue("updates/enabled", true);
    checked.clear();
    updates.check(false);
    QTest::qWait(10);
    check(checked.isEmpty(), "automatic failure silent");
    network.status = 200;
    network.body = "invalid";
    updates.check(true);
    check(checked.wait(1000) && checked.last()[1].toBool(), "invalid metadata handled");
    network.body = QJsonDocument(release("v0.51.0")).toJson();
    updates.check(true);
    check(checked.wait(1000) && !checked.last()[1].toBool(), "up-to-date status");
    QFile catalog(":/content-packs.json");
    catalog.open(QIODevice::ReadOnly);
    check(SamplePacks::catalog(catalog.readAll()).size() == 1, "bundled catalog offline");
    check(SamplePacks::catalog("{}").isEmpty() && SamplePacks::catalog("invalid").isEmpty(), "invalid catalog ignored");
    check(SamplePacks::catalog(R"({"schema":1,"packs":[{"id":"x","name":"X","url":"javascript:bad"}]})").isEmpty(),
          "non-HTTPS pack rejected");
    check(!SamplePacks::validContentFolder(temporary.path()), "missing manifest rejected");
    QFile manifest(temporary.filePath("library.json"));
    manifest.open(QIODevice::WriteOnly);
    manifest.write("{\"banks\":[{\"name\":\"Test\"}]}");
    manifest.close();
    check(SamplePacks::validContentFolder(temporary.path()), "extracted content accepted");
    const auto r = Updates::parse(release(), PackageKind::Debian, "x86_64");
    const QString capture = qEnvironmentVariable("SESSIONS_RELEASE_CAPTURE");
    for (const auto kind : {theme::Kind::Dark, theme::Kind::Light}) {
        theme::apply(kind);
        UpdateDialog dialog(r);
        for (const QString& id : {"updateDownload", "updatePostpone", "updateSkip"})
            check(dialog.findChild<QPushButton*>(id), "update choice present");
        if (!capture.isEmpty()) {
            QDir().mkpath(capture);
            dialog.show();
            app.processEvents();
            dialog.grab().save(capture + (kind == theme::Kind::Dark ? "/update-dark.png" : "/update-light.png"));
        }
        dialog.findChild<QPushButton*>("updateSkip")->click();
        check(dialog.result() == UpdateDialog::Skip, "Skip result distinct");
    }
    std::printf("Updates: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
