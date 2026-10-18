#pragma once
// The app icon's colour treatment (Preferences > Look > App icon; QSettings "ui/icon"). The icon is
// the Sessions mark (six clip-shaped segments) on a charcoal tile; six treatments ship in the
// resources (assets/icon/variants/). The choice sets the window icon, writes a per-user desktop
// entry so the app menu and the taskbar follow, and colours the mark in the About dialog's logo.
#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>
#include <QVector>

namespace sessions::appicon {

struct Variant {
    QString key;        // saved in QSettings and in the desktop entry's icon name
    QString name;       // shown in Preferences
    QString blurb;      // one line about it
    QString resource;   // the SVG in the resources
    QColor  segments[6];   // top-left, top-right, middle-left, middle-right, bottom-left, bottom-right; invalid = "the ink" (a grey clip)
};

const QVector<Variant>& variants();
const Variant& variant(const QString& key);   // unknown → the default (ember)
QString        defaultKey();
QString        currentKey();                  // "ui/icon"
const Variant& current();
QIcon          icon(const QString& key);

// Saves the choice, sets the application's window icon and writes (or, for the default, removes)
// the desktop entry override. Returns false with `error` set when the override can't be written;
// the choice is still saved and the window icon still set.
bool setCurrent(const QString& key, QString* error = nullptr);
void applyWindowIcon();                       // at startup: the saved choice

// The full logo (the mark and the wordmark) with the wordmark in `ink` and the mark in a
// variant's colours (a grey clip takes the ink), `width` pixels wide at `dpr`.
QPixmap logo(const QColor& ink, int width, qreal dpr, const QString& key = currentKey());

// ~/.local/share/applications/sessions.desktop (XDG_DATA_HOME): a copy of the app's desktop entry
// with the chosen icon, so the app menu and the dock show it too. The icon file itself is written
// next to the app's data (`iconFilePath`), so it needs no installation.
QString desktopOverridePath();
QString iconFilePath(const QString& key);

} // namespace sessions::appicon
