#include "Theme.h"
#include <QApplication>
#include <QProxyStyle>
#include <QPainter>
#include <QStyleOption>
#include <QWidget>
#include <QSettings>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStringView>
#include <algorithm>
#include <cmath>

namespace sessions::theme {

namespace {

Theme s_current = [] { Theme t; t.name = "Dark"; t.builtin = true; return t; }();

// The table as data: name, and the three base values.
struct Entry { const char* name; const char* dark; const char* mid; const char* light; };
const std::vector<Entry>& entries() {
    static const std::vector<Entry> e = {
#define SESSIONS_THEME_ENTRY(name, dark, mid, light) {#name, dark, mid, light},
        SESSIONS_THEME_COLOURS(SESSIONS_THEME_ENTRY)
#undef SESSIONS_THEME_ENTRY
    };
    return e;
}

QColor* slot(const QString& name) {
#define SESSIONS_THEME_SLOT(n, dark, mid, light) if (name == QLatin1String(#n)) return &n;
    SESSIONS_THEME_COLOURS(SESSIONS_THEME_SLOT)
#undef SESSIONS_THEME_SLOT
    return nullptr;
}

QString baseKey(Kind k) { return k == Kind::Light ? "light" : k == Kind::Mid ? "mid" : "dark"; }
Kind kindFromKey(const QString& s) { return s == "light" ? Kind::Light : s == "mid" ? Kind::Mid : Kind::Dark; }

// Pushes a lightness away from the background's (contrast > 0) or towards it (< 0).
double stretch(double l, double bgL, double contrast) {
    return std::clamp(bgL + (l - bgL) * (1 + contrast * (contrast > 0 ? 1.2 : 0.8)), 0.0, 1.0);
}

} // namespace

bool Theme::operator==(const Theme& o) const {
    return name == o.name && base == o.base && std::abs(hue - o.hue) < 1e-9 && std::abs(tint - o.tint) < 1e-9
        && std::abs(contrast - o.contrast) < 1e-9 && accent == o.accent && overrides == o.overrides;
}

Kind kind() { return s_current.base; }
const Theme& current() { return s_current; }

QString name(Kind k) { return k == Kind::Light ? "Light" : k == Kind::Mid ? "Mid" : "Dark"; }

Kind savedKind() { return savedTheme().base; }

Theme savedTheme() {
    const QString s = QSettings("Sessions", "Sessions").value("ui/theme", "dark").toString();
    const QString low = s.toLower();
    if (low == "dark" || low == "mid" || low == "light") return builtinThemes()[int(kindFromKey(low))];
    return themeNamed(s);
}

void save(Kind k) { QSettings("Sessions", "Sessions").setValue("ui/theme", baseKey(k)); }
void save(const Theme& t) {
    const QString low = t.name.toLower();
    QSettings("Sessions", "Sessions").setValue("ui/theme", t.builtin && (low == "dark" || low == "mid" || low == "light") ? low : t.name);
}

Notifier* notifier() {
    static Notifier n;
    return &n;
}

QStringList colourNames() {
    QStringList out;
    for (const Entry& e : entries()) out << QString::fromLatin1(e.name);
    return out;
}

QColor baseColour(Kind k, const QString& name) {
    for (const Entry& e : entries())
        if (name == QLatin1String(e.name)) return QColor(k == Kind::Light ? e.light : k == Kind::Mid ? e.mid : e.dark);
    return {};
}

QColor colour(const QString& name) {
    const QColor* c = slot(name);
    return c ? *c : QColor();
}

QString group(const QString& n) {
    static const QStringList transport = {"play", "record", "mute", "solo", "peakClip", "peakHold"};
    static const QStringList session = {"slotEmpty", "slotScene", "returnColumn", "stopRow", "sceneNumber", "slotStop", "slotArmed", "clipGlyph", "clipPlaying",
                                        "groupGlyph", "groupPlaying", "clockFill", "strip"};
    static const QStringList editors = {"editorBg", "ruler", "rulerText", "barLine", "beatLine", "centreLine", "envGrid", "envelope", "envelopeOff", "autoOff",
                                        "breakpointEdge", "warpMarker", "compLane", "compHead", "takeLaneA", "takeLaneB", "takeHead", "takeChosen"};
    static const QStringList arrangement = {"lane", "laneSelected", "laneGroup", "laneReturn", "arrBar", "arrBeat", "overview", "overviewView", "locatorStrip", "locatorLine",
                                            "timeSelection", "insertMarker", "headerMixer", "headerMixerSel", "chooserBg", "chooserText", "clipInk", "missingText",
                                            "takeFill", "punchLine", "dropGhost"};
    static const QStringList widgets = {"meterBg", "meterBg2", "levelMarker", "faderZero", "faderHandle", "faderLine", "knobArc", "knobFace", "knobPointer"};
    static const QStringList deviceView = {"devicePanelOn", "devicePanelOff", "deviceTitle", "deviceTitleSel", "rackPanel", "rackEdge"};
    if (transport.contains(n)) return "Transport";
    if (session.contains(n)) return "Session View";
    if (editors.contains(n)) return "Editors";
    if (arrangement.contains(n)) return "Arrangement";
    if (n.startsWith("roll") || n.endsWith("Key") || n.endsWith("KeyText") || n.startsWith("note") || n == "boxSelect" || n.startsWith("vel") || n == "grid" || n == "gridDark") return "Piano Roll";
    if (widgets.contains(n)) return "Widgets";
    if (deviceView.contains(n)) return "Device View";
    if (n.startsWith("dev")) return "Device Panels";
    return "Window";
}

bool isAccentColour(const QString& n) { return n == "accent" || n == "devCurve" || n == "devCurveFill" || n == "readout"; }

bool isTintable(const QString& n) {
    // the greys: everything that isn't a signal colour; the accent family follows the accent instead
    static const QStringList fixed = {"play", "record", "mute", "solo", "peakClip", "envelope", "envelopeOff", "warpMarker", "clipPlaying", "groupPlaying",
                                      "slotArmed", "missingText", "takeFill", "punchLine", "devMissing", "devCyan", "devCyanFill", "devCyanDark", "timeSelection",
                                      "osmiumGreen", "osmiumEnv", "osmiumLfo", "osmiumCurve"};
    return !fixed.contains(n) && !isAccentColour(n);
}

// What a theme makes of one colour, before the overrides: the base's value, tinted and stretched
// when it's a grey, swapped for the accent when it follows the accent.
namespace {
QColor derive(const Theme& t, const Entry& e) {
    const QString n = QString::fromLatin1(e.name);
    const QColor baseAccent = baseColour(t.base, "accent");
    QColor c(t.base == Kind::Light ? e.light : t.base == Kind::Mid ? e.mid : e.dark);
    if (t.accent.isValid()) {
        if (isAccentColour(n) && c.rgb() == baseAccent.rgb()) { QColor a = t.accent; a.setAlpha(c.alpha()); return a; }
        if (n == "onAccent" || n == "devButtonOnText") {   // dark ink on a bright accent, light ink on a deep one (by luma)
            const double luma = 0.299 * t.accent.redF() + 0.587 * t.accent.greenF() + 0.114 * t.accent.blueF();
            return luma < 0.55 ? QColor("#f4f4f4") : QColor("#151515");
        }
    }
    if (!isTintable(n)) return c;
    const bool tinted = t.hue >= 0 && t.tint > 0.001;
    if (!tinted && std::abs(t.contrast) < 0.001) return c;
    if (c.hslSaturationF() > 0.14) return c;   // a coloured entry (a track colour, cyan): left alone
    const double bgL = QColor(t.base == Kind::Light ? "#b8b8b5" : t.base == Kind::Mid ? "#848484" : "#2b2b2b").lightnessF();
    double l = c.lightnessF();
    if (std::abs(t.contrast) >= 0.001 && c.alpha() == 255) l = stretch(l, bgL, t.contrast);
    double sat = c.hslSaturationF(), hue = c.hslHueF();
    if (tinted) {
        // the tint fades out towards black and white, where saturation shows little anyway
        const double room = 1 - std::abs(2 * l - 1);
        sat = std::clamp(t.tint * 0.42 * (0.35 + 0.65 * room), 0.0, 1.0);
        hue = std::fmod(t.hue, 360.0) / 360.0;
    }
    QColor out = QColor::fromHslF(std::clamp(hue < 0 ? 0.0 : hue, 0.0, 0.9999), sat, l);
    out.setAlpha(c.alpha());
    return out;
}
}

QColor resolve(const Theme& t, const QString& name) {
    if (const auto it = t.overrides.constFind(name); it != t.overrides.constEnd() && it->isValid()) return *it;
    for (const Entry& e : entries()) if (name == QLatin1String(e.name)) return derive(t, e);
    return {};
}

void apply(const Theme& t) {
    s_current = t;
    for (const Entry& e : entries()) {
        QColor* c = slot(QString::fromLatin1(e.name));
        if (c) *c = resolve(t, QString::fromLatin1(e.name));
    }
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        app->setStyleSheet(stylesheet());
        emit notifier()->changed();
        for (QWidget* w : QApplication::allWidgets()) w->update();
    }
}

void apply(Kind k) { apply(builtinThemes()[int(k)]); }

namespace {
// The app's style: the platform's, with the dialog buttons' icons off and the message-box glyphs
// drawn flat in the theme's colours (they're drawn at call time, so they follow a theme change).
class AppStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;
    static constexpr const char* kName = "sessionsAppStyle";
    int styleHint(StyleHint hint, const QStyleOption* opt, const QWidget* w, QStyleHintReturn* ret) const override {
        if (hint == SH_DialogButtonBox_ButtonsHaveIcons) return 0;
        if (hint == SH_MessageBox_CenterButtons) return 0;
        return QProxyStyle::styleHint(hint, opt, w, ret);
    }
    QIcon standardIcon(StandardPixmap sp, const QStyleOption* opt, const QWidget* w) const override {
        QColor disc; QString glyph;
        switch (sp) {
        case SP_MessageBoxQuestion:    disc = accent; glyph = "?"; break;
        case SP_MessageBoxInformation: disc = accent; glyph = "i"; break;
        case SP_MessageBoxWarning:     disc = solo;   glyph = "!"; break;
        case SP_MessageBoxCritical:    disc = record; glyph = "!"; break;
        default: return QProxyStyle::standardIcon(sp, opt, w);
        }
        QIcon icon;
        for (int size : {32, 48, 64}) {
            QPixmap pm(size, size);
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(disc);
            p.drawEllipse(QRectF(1, 1, size - 2, size - 2));
            QFont f = p.font();
            f.setPixelSize(int(size * 0.62));
            f.setWeight(QFont::DemiBold);
            p.setFont(f);
            p.setPen(onAccent);
            p.drawText(QRectF(0, 0, size, size).adjusted(0, -size * 0.02, 0, 0), Qt::AlignCenter, glyph);
            icon.addPixmap(pm);
        }
        return icon;
    }
};
}

void installAppStyle() {
    if (!qApp || qApp->style()->objectName() == AppStyle::kName) return;
    auto* style = new AppStyle(qApp->style()->name());
    style->setObjectName(AppStyle::kName);
    qApp->setStyle(style);
}

const QVector<Theme>& builtinThemes() {
    static const QVector<Theme> themes = [] {
        auto make = [](const char* name, Kind base, double hue, double tint, double contrast, const char* accent) {
            Theme t;
            t.name = name; t.base = base; t.hue = hue; t.tint = tint; t.contrast = contrast;
            if (accent && *accent) t.accent = QColor(accent);
            t.builtin = true;
            return t;
        };
        return QVector<Theme>{
            make("Dark", Kind::Dark, -1, 0, 0, ""),          // the original, pixel-identical
            make("Mid", Kind::Mid, -1, 0, 0, ""),
            make("Light", Kind::Light, -1, 0, 0, ""),
            make("Midnight", Kind::Dark, 222, 0.55, 0.05, "#ffb050"),       // blue-black, amber
            make("Graphite", Kind::Dark, 200, 0.12, 0.15, "#4fd1c5"),       // cool neutral, teal
            make("Ember", Kind::Dark, 18, 0.4, 0, "#ff7a59"),               // warm brown, coral
            make("Forest", Kind::Dark, 150, 0.35, 0, "#b6e65a"),            // deep green, lime
            make("Plum", Kind::Dark, 278, 0.45, 0, "#ff79c6"),              // purple, pink
            make("Slate", Kind::Mid, 205, 0.45, 0.1, "#f2b544"),            // blue-grey, gold
            make("Paper", Kind::Light, 42, 0.35, 0, "#d9822b"),             // warm paper, burnt orange
            make("Sky", Kind::Light, 212, 0.4, 0.05, "#3b82f6"),            // cool light, blue (light ink on it)
            make("High Contrast", Kind::Dark, -1, 0, 0.6, "#ffd60a"),       // the dark stretched, yellow
        };
    }();
    return themes;
}

QString folder() {
    const QByteArray env = qgetenv("SESSIONS_THEMES");
    if (!env.isEmpty()) return QString::fromLocal8Bit(env);
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/Themes";
}

QString pathFor(const QString& name) {
    QString file = name.trimmed();
    static const QRegularExpression bad("[\\\\/:*?\"<>|]");
    file.replace(bad, "_");
    if (file.isEmpty()) file = "Theme";
    return QDir(folder()).filePath(file + ".sessionstheme");
}

QString toJson(const Theme& t) {
    QJsonObject o;
    o.insert("format", "sessions-theme");
    o.insert("version", 1);
    o.insert("name", t.name);
    o.insert("base", baseKey(t.base));
    o.insert("hue", t.hue);
    o.insert("tint", t.tint);
    o.insert("contrast", t.contrast);
    if (t.accent.isValid()) o.insert("accent", t.accent.name(QColor::HexArgb));
    QJsonObject colours;
    QStringList keys = t.overrides.keys();
    keys.sort();
    for (const QString& k : keys) if (t.overrides[k].isValid()) colours.insert(k, t.overrides[k].name(QColor::HexArgb));
    o.insert("colours", colours);
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented));
}

Theme readTheme(const QString& path, bool* ok) {
    Theme t;
    if (ok) *ok = false;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return t;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (o.value("format").toString() != "sessions-theme") return t;
    t.name = o.value("name").toString(QFileInfo(path).completeBaseName());
    t.base = kindFromKey(o.value("base").toString("dark"));
    t.hue = o.value("hue").toDouble(-1);
    t.tint = std::clamp(o.value("tint").toDouble(0), 0.0, 1.0);
    t.contrast = std::clamp(o.value("contrast").toDouble(0), -1.0, 1.0);
    if (o.contains("accent")) t.accent = QColor(o.value("accent").toString());
    const QJsonObject colours = o.value("colours").toObject();
    const QStringList known = colourNames();
    for (auto it = colours.begin(); it != colours.end(); ++it)
        if (known.contains(it.key())) { const QColor c(it.value().toString()); if (c.isValid()) t.overrides.insert(it.key(), c); }
    if (ok) *ok = true;
    return t;
}

QVector<Theme> customThemes() {
    QVector<Theme> out;
    QDirIterator it(folder(), {"*.sessionstheme"}, QDir::Files | QDir::Readable);
    while (it.hasNext()) {
        bool ok = false;
        Theme t = readTheme(it.next(), &ok);
        if (ok) out.push_back(t);
    }
    std::sort(out.begin(), out.end(), [](const Theme& a, const Theme& b) { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
    return out;
}

Theme themeNamed(const QString& name, bool* found) {
    if (found) *found = true;
    for (const Theme& t : builtinThemes()) if (t.name.compare(name, Qt::CaseInsensitive) == 0) return t;
    for (const Theme& t : customThemes()) if (t.name.compare(name, Qt::CaseInsensitive) == 0) return t;
    if (found) *found = false;
    return builtinThemes().first();
}

bool saveCustom(const Theme& theme, QString* error) {
    Theme t = theme;
    t.builtin = false;
    t.name = t.name.trimmed();
    if (t.name.isEmpty()) { if (error) *error = "Give the theme a name."; return false; }
    QDir().mkpath(folder());
    QSaveFile f(pathFor(t.name));
    if (!f.open(QIODevice::WriteOnly)) { if (error) *error = "Can't write " + f.fileName() + ": " + f.errorString(); return false; }
    f.write(toJson(t).toUtf8());
    if (!f.commit()) { if (error) *error = "Can't write " + f.fileName() + ": " + f.errorString(); return false; }
    emit notifier()->listChanged();
    return true;
}

bool removeCustom(const QString& name) {
    const bool ok = QFile::remove(pathFor(name));
    if (ok) emit notifier()->listChanged();
    return ok;
}

// The stylesheet with {colour} placeholders, filled from the table. Dark renders exactly as the
// original literal stylesheet did (the Dark column holds those literals).
QString stylesheet() {
    QString css = R"(
QMainWindow, QWidget { background: {bg}; color: {text}; font-size: 11px; }
QWidget#panel { background: {panel}; }
QWidget#controlBar { background: {panel}; border-bottom: 1px solid {line}; }
QWidget#browser { background: {panel}; border-right: 1px solid {line}; }
QListWidget#browserSidebar { background: {sidebar}; border: 0; }
QTreeWidget#browserItems { background: {panel}; }
QWidget#browserContentHeader { background: {columnHeader}; border-bottom: 1px solid {line}; }
QLabel#browserLocation { background: transparent; color: {textSoft}; font-size: 10px; font-weight: 600; }
QLabel#browserCount, QLabel#browserPreviewName { background: transparent; color: {textFaint}; font-size: 10px; }
QLineEdit#browserSearch { padding-left: 4px; border-radius: 3px; }
QWidget#browserPreviewBar { background: {panel}; border-top: 1px solid {line}; }
QToolButton#browserPreview { padding: 3px 8px; border-radius: 3px; border: 1px solid {line}; }
QToolButton#browserPreview:checked { background: {accent}; color: {onAccent}; border-color: {accent}; }
QSplitter#browserPanes::handle { background: {line}; }
QSplitter#browserPanes::handle:hover { background: {splitHover}; }
QLabel#browserColumnHeader { background: {columnHeader}; color: {textSoft}; font-weight: 600; padding-left: 6px; border-bottom: 1px solid {line}; }
QWidget#infoView { background: {panel}; border-right: 1px solid {line}; }
QWidget#detailHeader { background: {panel}; border-bottom: 1px solid {line}; }
QWidget#detailTabs { background: {panel}; border-right: 1px solid {line}; }
QWidget#detailTabs QToolButton { min-width: 16px; max-width: 16px; min-height: 58px; padding: 0; font-size: 10px; }
QWidget#clipProps { background: {line}; border-right: 1px solid {line}; }
QWidget#clipBox { background: {panel}; }
QLabel#clipBoxTitle { background: {panel2}; color: {text}; font-weight: 600; font-size: 10px; padding-left: 4px; border-bottom: 1px solid {line}; }
QWidget#clipProps QPushButton, QWidget#clipProps QToolButton, QWidget#clipProps QComboBox { min-height: 12px; max-height: 15px; padding: 0 3px; font-size: 10px; }
QWidget#clipProps QCheckBox { font-size: 10px; spacing: 4px; }
QWidget#clipProps QLineEdit#clipName { padding: 1px 3px; max-height: 15px; }
QWidget#clipToggles { background: {panel}; border-right: 1px solid {line}; }
QToolButton#clipBoxToggle { min-width: 12px; max-width: 12px; min-height: 12px; max-height: 12px; padding: 0; border-radius: 7px; font-size: 8px; font-weight: 600; }
QWidget#bottomPanel { border-top: 1px solid {line}; }
QWidget#overviewBar { background: {panel}; border-top: 1px solid {line}; }
QLabel#overviewStatus { color: {textFaint}; font-size: 10px; padding-left: 8px; }
QToolButton#swatch { border: 1px solid {line}; border-radius: 2px; min-width: 34px; min-height: 14px; }

QToolButton, QPushButton {
    background: {panel2}; border: 1px solid {line}; border-radius: 2px;
    color: {text}; padding: 1px 6px; min-height: 16px; font-size: 10px;
}
QToolButton:hover, QPushButton:hover { background: {hover}; }
QToolButton:pressed, QPushButton:pressed { background: {panel}; }
QToolButton:checked, QPushButton:checked { background: {accent}; color: {onAccent}; }
QToolButton:disabled, QPushButton:disabled { color: {disabled}; }
QDialog QPushButton { padding: 3px 14px; min-height: 20px; }   /* no min-width: the stylesheet would size every button to it and crop a long label (Close without Saving) */
QDialog QPushButton:default { border-color: {accent}; }
QDialogButtonBox { dialogbuttonbox-buttons-have-icons: 0; }
QMessageBox QLabel { font-size: 11px; }
QToolButton#playButton:checked { background: {play}; }
QToolButton#recButton:checked  { background: {record}; }
QToolButton#muteButton:checked { background: {mute}; color: {onAccent}; }
QToolButton#soloButton:checked { background: {solo}; color: {onAccent}; }
QToolButton#armButton:checked  { background: {record}; color: {onAccent}; }

QComboBox { background: {field}; border: 1px solid {line}; color: {text}; padding: 0 4px; min-height: 16px; font-size: 10px; }
QComboBox::drop-down { border: 0; width: 14px; }
QComboBox QAbstractItemView { background: {field}; color: {text}; selection-background-color: {accent}; selection-color: {onAccent}; }

QLineEdit { background: {field}; border: 1px solid {line}; border-radius: 2px; color: {text}; padding: 2px 4px; selection-background-color: {accent}; }
QLineEdit#clipName { border: 0; font-weight: 600; }

QTreeView, QListView {
    background: {panel}; border: 0; outline: 0; color: {text};
    show-decoration-selected: 1;
}
QTreeView::item, QListView::item { padding: 2px 4px; }
QTreeView::item:hover, QListView::item:hover { background: {itemHover}; }
QTreeView::item:selected, QListView::item:selected { background: {panel2}; color: {textBright}; }
QTreeView::branch { background: {panel}; }

QScrollBar:vertical { background: {bg}; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: {scrollHandle}; min-height: 20px; border-radius: 3px; margin: 2px; }
QScrollBar:horizontal { background: {bg}; height: 10px; margin: 0; }
QScrollBar::handle:horizontal { background: {scrollHandle}; min-width: 20px; border-radius: 3px; margin: 2px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }

QSplitter::handle { background: {line}; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }
QSplitter#sessionSplit::handle:vertical { height: 5px; background: {splitHandle}; border-top: 1px solid {splitEdge}; }
QSplitter#sessionSplit::handle:vertical:hover { background: {splitHover}; }
QSplitter#browserSplit::handle:horizontal { width: 5px; background: {splitHandle}; border-left: 1px solid {splitEdge}; }
QSplitter#browserSplit::handle:horizontal:hover { background: {splitHover}; }

QCheckBox { spacing: 6px; }
QCheckBox::indicator { width: 11px; height: 11px; border: 1px solid {checkBorder}; border-radius: 2px; background: {field}; }
QCheckBox::indicator:checked { background: {accent}; border-color: {accent}; }
QCheckBox:disabled { color: {disabled}; }
QLabel#dimLabel { color: {dim}; font-size: 9px; }
QLabel#infoTitle { color: {text}; font-size: 11px; font-weight: 600; }
QLabel#infoBody  { color: {dim}; font-size: 10px; }
QLabel#detailTitle { font-weight: 600; }
QMenuBar { background: {panel}; border-bottom: 1px solid {line}; }
QMenuBar::item { padding: 4px 10px; background: transparent; border-radius: 2px; }
QMenuBar::item:selected { background: {hover}; }
QMenuBar::item:pressed { background: {panel2}; }
QMenu { background: {panel}; border: 1px solid {line}; padding: 4px 0; }
QMenu::item { padding: 4px 28px 4px 16px; }
QMenu::item:selected { background: {accent}; color: {onAccent}; }
QMenu::item:disabled { color: {disabled}; }
QMenu::separator { height: 1px; background: {line}; margin: 4px 10px; }
QMenu::right-arrow { width: 7px; height: 7px; margin-right: 8px; }
QMenu::indicator { width: 10px; height: 10px; margin-left: 4px; }

/* Preferences: a sidebar of categories, pages of rows (Edit > Preferences…) */
QListWidget#prefsSidebar { background: {sidebar}; border: 0; border-right: 1px solid {line}; outline: 0; font-size: 11px; padding-top: 12px; }
QListWidget#prefsSidebar::item { padding: 7px 16px; border-left: 2px solid transparent; color: {textSoft}; }
QListWidget#prefsSidebar::item:hover { background: {itemHover}; color: {text}; }
QListWidget#prefsSidebar::item:selected { background: {panel2}; color: {textBright}; border-left: 2px solid {accent}; }
QWidget#prefsPage { background: {bg}; }
QLabel#prefsTitle { font-size: 15px; font-weight: 600; color: {text}; }
QWidget#prefsRow { border-bottom: 1px solid {line}; }
QLabel#prefsRowLabel { color: {textSoft}; font-weight: 600; }
QWidget#prefsFooter { background: {panel}; border-top: 1px solid {line}; }
QToolButton#iconChoice { background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 6px 2px; font-size: 10px; color: {textSoft}; }
QToolButton#iconChoice:hover { background: {hover}; color: {text}; }
QToolButton#iconChoice:checked { background: {panel2}; border: 1px solid {accent}; color: {text}; }
QToolTip { background: {field}; color: {text}; border: 1px solid {line}; }
)";
    static const QRegularExpression token("\\{([A-Za-z0-9]+)\\}");
    QString out;
    int at = 0;
    for (QRegularExpressionMatch m = token.match(css); m.hasMatch(); m = token.match(css, at)) {
        out += QStringView(css).mid(at, m.capturedStart() - at);
        const QString key = m.captured(1);
        QColor c;
#define SESSIONS_THEME_LOOKUP(name, dark, mid, light) if (key == QLatin1String(#name)) c = name;
        SESSIONS_THEME_COLOURS(SESSIONS_THEME_LOOKUP)
#undef SESSIONS_THEME_LOOKUP
        out += c.isValid() ? c.name() : m.captured(0);
        at = m.capturedEnd();
    }
    out += QStringView(css).mid(at);
    return out;
}

} // namespace sessions::theme
