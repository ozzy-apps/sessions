#include "AppIcon.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QPainter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QSvgRenderer>
#include <cmath>

namespace sessions::appicon {

namespace {
const QColor kNone;   // invalid: the ink
const char* kSegmentIds[6] = {"top-left", "top-right", "middle-left", "middle-right", "bottom-left", "bottom-right"};
}

const QVector<Variant>& variants() {
    static const QVector<Variant> v = {
        {"ember",   "Ember",   "The app's accent, amber at the top to red at the bottom.", ":/icon/variants/A-ember.svg",
         {QColor("#ffc06a"), QColor("#ffa63c"), QColor("#ff8a2a"), QColor("#ff6a3d"), QColor("#ff4f6a"), QColor("#e23d7a")}},
        {"trio",    "Trio",    "Hot, teal and violet, one per row.", ":/icon/variants/B-trio.svg",
         {QColor("#ffc06a"), QColor("#ff8a2a"), QColor("#7fe0ef"), QColor("#3fb5cc"), QColor("#c3b2ff"), QColor("#8f78e8")}},
        {"clips",   "Clips",   "Six clip colours from the Session grid.", ":/icon/variants/C-clips.svg",
         {QColor("#f0804d"), QColor("#f0c04d"), QColor("#8fd64d"), QColor("#4dd6c8"), QColor("#4d8ef0"), QColor("#d64db8")}},
        {"playing", "Playing", "Grey clips, the one that's playing in the accent.", ":/icon/variants/D-playing.svg",
         {QColor("#ffa63c"), kNone, kNone, kNone, kNone, kNone}},
        {"paper",   "Paper",   "The clip colours on a light tile.", ":/icon/variants/E-paper.svg",
         {QColor("#f0804d"), QColor("#f0c04d"), QColor("#8fd64d"), QColor("#4dd6c8"), QColor("#4d8ef0"), QColor("#d64db8")}},
        {"dusk",    "Dusk",    "Teal into violet.", ":/icon/variants/F-dusk.svg",
         {QColor("#7fe0ef"), QColor("#5cc6dc"), QColor("#5fa0ea"), QColor("#7a86ea"), QColor("#9f78e8"), QColor("#c56ad4")}},
    };
    return v;
}

QString defaultKey() { return "ember"; }

const Variant& variant(const QString& key) {
    for (const Variant& v : variants()) if (v.key == key) return v;
    return variants()[0];
}

QString currentKey() {
    const QString key = QSettings("Sessions", "Sessions").value("ui/icon", defaultKey()).toString();
    return variant(key).key;
}

const Variant& current() { return variant(currentKey()); }

QIcon icon(const QString& key) { return QIcon(variant(key).resource); }

QString desktopOverridePath() {
    return QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation) + "/sessions.desktop";
}

QString iconFilePath(const QString& key) {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/Icons/sessions-" + variant(key).key + ".svg";
}

namespace {
bool writeFile(const QString& path, const QByteArray& bytes, QString* error) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) { if (error) *error = "Can't write " + path + ": " + f.errorString(); return false; }
    f.write(bytes);
    if (!f.commit()) { if (error) *error = "Can't write " + path + ": " + f.errorString(); return false; }
    return true;
}

// The desktop entry override: the shipped entry (assets/sessions.desktop, in the resources) with the
// icon pointing at the written SVG, marked as ours so only ours is ever removed.
bool writeDesktopOverride(const QString& key, QString* error) {
    const QString path = desktopOverridePath();
    if (key == defaultKey()) {
        QFile f(path);
        if (f.exists() && f.open(QIODevice::ReadOnly) && f.readAll().contains("X-Sessions-IconChoice=")) { f.close(); f.remove(); }
        return true;
    }
    QFile svg(variant(key).resource);
    if (!svg.open(QIODevice::ReadOnly)) { if (error) *error = "The icon isn't in the resources."; return false; }
    const QString iconPath = iconFilePath(key);
    if (!writeFile(iconPath, svg.readAll(), error)) return false;
    QFile tmpl(":/sessions.desktop");
    if (!tmpl.open(QIODevice::ReadOnly)) { if (error) *error = "The desktop entry isn't in the resources."; return false; }
    QString entry = QString::fromUtf8(tmpl.readAll());
    entry.replace(QRegularExpression("^Icon=.*$", QRegularExpression::MultilineOption), "Icon=" + iconPath);
    if (!entry.endsWith('\n')) entry += '\n';
    entry += "X-Sessions-IconChoice=" + key + "\n";
    return writeFile(path, entry.toUtf8(), error);
}
}

bool setCurrent(const QString& key, QString* error) {
    const QString k = variant(key).key;
    QSettings("Sessions", "Sessions").setValue("ui/icon", k);
    applyWindowIcon();
    return writeDesktopOverride(k, error);
}

void applyWindowIcon() {
    if (qApp) qApp->setWindowIcon(icon(currentKey()));
}

QPixmap logo(const QColor& ink, int width, qreal dpr, const QString& key) {
    QFile f(":/logo/sessions-logo.svg");
    if (!f.open(QIODevice::ReadOnly)) return {};
    QString svg = QString::fromUtf8(f.readAll());
    svg.replace("#222221", ink.name(QColor::HexRgb));   // the root fill: the wordmark
    const Variant& v = variant(key);
    for (int i = 0; i < 6; ++i) {   // each segment gets its own fill (the ink for a grey clip)
        const QColor c = v.segments[i].isValid() ? v.segments[i] : ink;
        svg.replace(QString("id=\"%1\"").arg(kSegmentIds[i]), QString("id=\"%1\" fill=\"%2\"").arg(kSegmentIds[i], c.name(QColor::HexRgb)));
    }
    QSvgRenderer r(svg.toUtf8());
    const QSizeF natural = r.defaultSize();
    if (natural.width() <= 0) return {};
    const int height = int(std::lround(width * natural.height() / natural.width()));
    QPixmap pm(QSize(width, height) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    r.render(&p, QRectF(0, 0, width, height));
    return pm;
}

} // namespace sessions::appicon
