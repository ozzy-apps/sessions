#pragma once
// Sessions colour themes. One place for every colour, plus the app stylesheet.
//
// Three base palettes, after FORM's: Dark (the original), Mid and Light. Every colour is a
// mutable global with one value per base in the table below; views read them when they paint,
// so apply() recolours the whole window at once (it also re-applies the stylesheet). The device
// panels' palette (devui::col in ui/devices/DeviceBody.h) refers to the dev* entries here.
//
// A Theme (0.19) is a base palette plus a tint (a hue the greys take, how much, and a contrast
// stretch), an accent colour, and any colours overridden one by one. The built-in themes are
// Dark, Mid and Light (the bases as they are) and eight tinted ones (Midnight, Graphite, Ember,
// Forest, Plum, Slate, Paper, Sky, High Contrast); your own are .sessionstheme files in folder()
// (View > Theme > Customize…). Dark stays pixel-identical: its theme changes nothing.
//
// Adding a colour: one line in SESSIONS_THEME_COLOURS with its Dark, Mid and Light values
// (#rrggbb or #aarrggbb). No hex literals in views: the dark column *is* the dark theme.

#include <QColor>
#include <QString>
#include <QObject>
#include <QHash>
#include <QStringList>
#include <QVector>

namespace sessions::theme {

enum class Kind { Dark, Mid, Light };

//                 name            Dark         Mid          Light
#define SESSIONS_THEME_COLOURS(X) \
    /* ---- window chrome: the stylesheet and most widgets */ \
    X(bg,            "#2b2b2b",   "#848484",   "#b8b8b5") \
    X(panel,         "#363636",   "#969696",   "#cececa") \
    X(panel2,        "#424242",   "#aaaaaa",   "#ddddda") \
    X(line,          "#1a1a1a",   "#666666",   "#939391") \
    X(text,          "#d2d2d2",   "#171717",   "#242624") \
    X(dim,           "#8c8c8c",   "#393939",   "#575d58") \
    X(accent,        "#ffa63c",   "#f0b43c",   "#e0901e") \
    X(onAccent,      "#151515",   "#151515",   "#151515")   /* text on accent / mute / solo / record buttons */ \
    X(readout,       "#ffa63c",   "#171717",   "#242624")   /* numbers in value fields: the accent in the dark, the text colour on light surfaces (FORM) */ \
    X(field,         "#202020",   "#a5a5a5",   "#e2e2de") \
    X(inset,         "#1e1e1e",   "#a8a8a8",   "#e4e4e0")   /* recessed boxes drawn by hand: header fields, choosers, clock faces */ \
    X(hover,         "#4d4d4d",   "#b4b4b4",   "#e6e6e3") \
    X(itemHover,     "#474747",   "#a6a6a6",   "#dcdcd8") \
    X(textBright,    "#ffffff",   "#000000",   "#000000")   /* selected list items, hovered ✕ */ \
    X(selection,     "#575b61",   "#5e646b",   "#a9b1bb") \
    X(selectionText, "#ffffff",   "#ffffff",   "#141414")   /* names on selected headers and titles */ \
    X(outline,       "#ffffff",   "#ffffff",   "#1c1c1c")   /* the selected slot / clip / track / note outline, focus rings */ \
    X(playhead,      "#ffffff",   "#ffffff",   "#1c1c1c") \
    X(textSoft,      "#bdbdbd",   "#2a2a2a",   "#3a3d3a")   /* column headers, header button labels, clocks */ \
    X(textFaint,     "#9a9a9a",   "#333333",   "#4e524f")   /* editor captions, scene ▶, meter scales */ \
    X(neutral,       "#8a8a8a",   "#3f3f3f",   "#6b6f6c")   /* the Main track's grey, browser section titles, clock rims */ \
    X(faint,         "#777777",   "#3a3a3a",   "#5a5e5b")   /* stop glyphs, "Vel", empty-editor hints */ \
    X(hint,          "#4a4a4a",   "#666666",   "#9a9a97")   /* "Drop … Here", a rack chain's dashed drop zone */ \
    X(dropEdge,      "#555555",   "#666666",   "#9e9e9b")   /* the Device View's dashed drop zone */ \
    X(disabled,      "#6c6c6c",   "#5a5a5a",   "#8b8f8c") \
    X(inactive,      "#6a6a6a",   "#5c5c5c",   "#8a8a87")   /* the loop brace with the loop off, scroll position marks */ \
    X(sidebar,       "#303030",   "#8e8e8e",   "#c4c4c0")   /* browser sidebar */ \
    X(columnHeader,  "#3e3e3e",   "#a2a2a2",   "#d6d6d2") \
    X(scrollHandle,  "#555555",   "#5c5c5c",   "#8a8a87") \
    X(splitHandle,   "#1e1e1e",   "#6e6e6e",   "#a3a3a0") \
    X(splitEdge,     "#3a3a3a",   "#8c8c8c",   "#c8c8c4") \
    X(splitHover,    "#4a4a4a",   "#9e9e9e",   "#e0e0dc") \
    X(checkBorder,   "#5a5a5a",   "#5a5a5a",   "#8c8c89") \
    X(buttonOff,     "#555555",   "#666666",   "#9e9e9b")   /* header buttons (activator / solo / arm) when off, device activators */ \
    X(ink,           "#141414",   "#141414",   "#141414")   /* dark text on coloured surfaces: clips, name blocks, lit buttons */ \
    X(inkLight,      "#f0f0f0",   "#f0f0f0",   "#f0f0f0")   /* light text on dark track colours */ \
    X(shade,         "#5a000000", "#5a000000", "#5a000000") /* over the clip being dragged */ \
    X(progress,      "#78000000", "#78000000", "#78000000") /* a playing clip's progress bar */ \
    X(outOfClip,     "#46000000", "#46000000", "#46000000") /* past the clip's end in the editors */ \
    /* ---- transport colours, the same in every theme */ \
    X(play,          "#6fd35c",   "#6fd35c",   "#6fd35c") \
    X(record,        "#ff5a4a",   "#ff5a4a",   "#ff5a4a") \
    X(mute,          "#f2c94c",   "#f2c94c",   "#f2c94c") \
    X(solo,          "#5aa9f0",   "#5aa9f0",   "#5aa9f0") \
    X(peakClip,      "#ff4a3a",   "#ff4a3a",   "#ff4a3a") \
    X(peakHold,      "#dddddd",   "#f0f0f0",   "#333333") \
    /* ---- Session View */ \
    X(slotEmpty,     "#323232",   "#8d8d8d",   "#c6c6c2") \
    X(slotScene,     "#3b3b3b",   "#929292",   "#cbcbc7") \
    X(returnColumn,  "#2a2a2a",   "#848484",   "#c0c0bc") \
    X(stopRow,       "#2f2f2f",   "#8a8a8a",   "#c2c2be") \
    X(sceneNumber,   "#2a2a2a",   "#7c7c7c",   "#c0c0bc") \
    X(slotStop,      "#5c5c5c",   "#5a5a5a",   "#8e8e8b")   /* the ■ in an empty slot */ \
    X(slotArmed,     "#a04040",   "#a04040",   "#c4504a")   /* the ● in an empty slot of an armed track */ \
    X(clipGlyph,     "#1a1a1a",   "#1a1a1a",   "#1a1a1a")   /* ▶ on a clip */ \
    X(clipPlaying,   "#0c8f0c",   "#0c8f0c",   "#0c8f0c") \
    X(groupGlyph,    "#d8d8d8",   "#d8d8d8",   "#d8d8d8")   /* ▶ on a group slot */ \
    X(groupPlaying,  "#3fd13f",   "#3fd13f",   "#3fd13f") \
    X(clockFill,     "#d6d6d6",   "#202020",   "#3a3d3a") \
    X(strip,         "#313131",   "#8c8c8c",   "#c3c3bf")   /* mixer strips */ \
    /* ---- the editors: Arrangement, sample / takes / envelope editors, piano roll */ \
    X(editorBg,      "#262626",   "#7a7a7a",   "#b0b0ad") \
    X(ruler,         "#3b3b3b",   "#929292",   "#cbcbc7") \
    X(rulerText,     "#a8a8a8",   "#262626",   "#3c3f3c") \
    X(barLine,       "#1e1e1e",   "#5e5e5e",   "#8e8e8b") \
    X(beatLine,      "#2b2b2b",   "#727272",   "#a2a2a0") \
    X(centreLine,    "#3a3a3a",   "#6a6a6a",   "#9c9c99") \
    X(envGrid,       "#323232",   "#747474",   "#a4a4a1") \
    X(envelope,      "#ff4f6a",   "#ff4f6a",   "#ff4f6a")   /* clip envelopes: red */ \
    X(envelopeOff,   "#8a6a70",   "#7a4a55",   "#8a5560") \
    X(autoOff,       "#8c8c8c",   "#3f3f3f",   "#6b6f6c")   /* a not-yet-automated value (dashed) */ \
    X(breakpointEdge,"#1a1a1a",   "#1a1a1a",   "#1a1a1a") \
    X(warpMarker,    "#f2c94c",   "#f2c94c",   "#f2c94c") \
    X(compLane,      "#2e2e2e",   "#828282",   "#b8b8b5") \
    X(compHead,      "#353535",   "#8c8c8c",   "#c2c2be") \
    X(takeLaneA,     "#282828",   "#7d7d7d",   "#b3b3b0") \
    X(takeLaneB,     "#242424",   "#777777",   "#adadaa") \
    X(takeHead,      "#303030",   "#868686",   "#bcbcb8") \
    X(takeChosen,    "#40ffffff", "#40ffffff", "#401c1c1c") /* the stretch being chosen */ \
    /* ---- Arrangement */ \
    X(lane,          "#2f2f2f",   "#878787",   "#bdbdba") \
    X(laneSelected,  "#363636",   "#929292",   "#c8c8c4") \
    X(laneGroup,     "#292929",   "#7f7f7f",   "#b6b6b3") \
    X(laneReturn,    "#2a2a2a",   "#808080",   "#b4b4b1") \
    X(arrBar,        "#1b1b1b",   "#5e5e5e",   "#8e8e8b") \
    X(arrBeat,       "#2a2a2a",   "#707070",   "#a4a4a1") \
    X(beatAlt,       "#0bffffff", "#0dffffff", "#0a000000") /* every second beat (or bar) of the grids, a shade lighter (0.21) */ \
    X(overview,      "#232323",   "#747474",   "#a8a8a5") \
    X(overviewView,  "#16ffffff", "#16ffffff", "#16000000") /* what's on screen, in the overview */ \
    X(locatorStrip,  "#303030",   "#868686",   "#bcbcb8") \
    X(locatorLine,   "#32d2d2d2", "#3cf0f0f0", "#3c1e1e1e") \
    X(timeSelection, "#3c78a0dc", "#463c6ec8", "#3c285ab4") \
    X(insertMarker,  "#cfd8e6",   "#1c2430",   "#2a3542") \
    X(headerMixer,   "#3c3c3c",   "#949494",   "#cdcdc9") \
    X(headerMixerSel,"#50545a",   "#62686e",   "#afb6bf") \
    X(chooserBg,     "#c8141414", "#dca0a0a0", "#dcebebe8") /* the automation parameter chooser */ \
    X(chooserText,   "#d0d0d0",   "#141414",   "#202220") \
    X(clipInk,       "#0f0f0f",   "#0f0f0f",   "#0f0f0f")   /* waveforms, note previews and loop seams on clips (alpha applied in place) */ \
    X(missingText,   "#781414",   "#781414",   "#781414")   /* "missing: file" on an audio clip */ \
    X(takeFill,      "#46ff5a4a", "#46ff5a4a", "#46ff5a4a") /* a recording in progress */ \
    X(punchLine,     "#6eff5a4a", "#6eff5a4a", "#6eff5a4a") \
    X(dropGhost,     "#8c000000", "#8c000000", "#8c000000") /* a Session clip dragged in: its outline over the lane */ \
    /* ---- piano roll */ \
    X(rollBg,        "#272727",   "#7c7c7c",   "#b2b2af") \
    X(rollEmpty,     "#222222",   "#727272",   "#a6a6a3")   /* folded-away gaps */ \
    X(grid,          "#333333",   "#929292",   "#d4d4d0")   /* white-key rows */ \
    X(gridDark,      "#2a2a2a",   "#868686",   "#c2c2be")   /* black-key rows */ \
    X(rollInScale,   "#3a3a3a",   "#949494",   "#d2d2ce") \
    X(rollOutScale,  "#262626",   "#7e7e7e",   "#b8b8b5") \
    X(rollRoot,      "#45403a",   "#9a9080",   "#d8cfc0") \
    X(whiteKey,      "#d6d6d6",   "#e0e0e0",   "#f2f2f0") \
    X(blackKey,      "#1e1e1e",   "#1e1e1e",   "#2a2a2a") \
    X(whiteKeyText,  "#666666",   "#666666",   "#555555") \
    X(blackKeyText,  "#9a9a9a",   "#9a9a9a",   "#bdbdbd") \
    X(rollPadText,   "#50ffffff", "#5affffff", "#6e000000") /* Drum Rack pad names on their rows */ \
    X(rollBeyond,    "#5f000000", "#5f000000", "#5f000000") /* past the clip's end */ \
    X(rollBar,       "#121212",   "#4e4e4e",   "#808080") \
    X(rollBeat,      "#202020",   "#626262",   "#969694") \
    X(rollSub,       "#2b2b2b",   "#727272",   "#a6a6a3") \
    X(waveform,      "#cfd9de",   "#1e2a30",   "#1e2a30")   /* the sample editor's waveform: one fixed colour against the grid, not the clip's (0.21) */ \
    X(rollRulerText, "#a2a2a2",   "#262626",   "#3c3f3c") \
    X(noteSelected,  "#eef3fa",   "#ffffff",   "#1b2a3a") \
    X(noteEdge,      "#8c000000", "#8c000000", "#8c000000") \
    X(boxSelect,     "#cccccc",   "#202020",   "#333333") \
    X(velLane,       "#2a2a2a",   "#808080",   "#b4b4b1") \
    X(velLine,       "#181818",   "#5a5a5a",   "#8a8a87") \
    /* ---- widgets: meters, faders, knobs */ \
    X(meterBg,       "#141414",   "#2a2a2a",   "#5a5a58") \
    X(meterBg2,      "#1c1c1c",   "#2e2e2e",   "#5e5e5c") \
    X(levelMarker,   "#d6d6d6",   "#f0f0f0",   "#2a2d2a") \
    X(faderZero,     "#5a5a5a",   "#6a6a6a",   "#9a9a97") \
    X(faderHandle,   "#c8c8c8",   "#e4e4e4",   "#f0f0ee") \
    X(faderLine,     "#333333",   "#333333",   "#6a6a68") \
    X(knobArc,       "#555555",   "#5c5c5c",   "#9a9a97") \
    X(knobFace,      "#2c2c2c",   "#b2b2b2",   "#e6e6e2") \
    X(knobPointer,   "#e2e2e2",   "#141414",   "#202220") \
    /* ---- Device View */ \
    X(devicePanelOn, "#2e2e2e",   "#8f8f8f",   "#c9c9c5") \
    X(devicePanelOff,"#262626",   "#828282",   "#bcbcb8") \
    X(deviceTitle,   "#4a4a4a",   "#6e6e6e",   "#a8a8a4") \
    X(deviceTitleSel,"#6b7079",   "#5a626c",   "#8d96a3") \
    X(rackPanel,     "#1f1f1f",   "#767676",   "#b4b4b0") \
    X(rackEdge,      "#5a5a5a",   "#5a5a5a",   "#8c8c89") \
    /* ---- device panels (devui::col in ui/devices/DeviceBody.h refers to these) */ \
    X(osmiumBg,      "#111715",   "#c0c9c3",   "#eef4ef") \
    X(osmiumSurface, "#1b2420",   "#d1d9d3",   "#ffffff") \
    X(osmiumInset,   "#141c18",   "#bbc8be",   "#e5eee7") \
    X(osmiumBorder,  "#344139",   "#86988b",   "#c5d3c9") \
    X(osmiumGreen,   "#a6ed8c",   "#306b20",   "#326c23") \
    X(osmiumEnv,     "#f1c17b",   "#805006",   "#895200") \
    X(osmiumLfo,     "#88d6d0",   "#216c67",   "#176f69") \
    X(osmiumCurve,   "#c5b3f4",   "#665192",   "#6f509e") \
    X(devDisplay,    "#171717",   "#b4b4b4",   "#ebebe8") \
    X(devDisplayEdge,"#0c0c0c",   "#5c5c5c",   "#8f8f8c") \
    X(devGrid,       "#272727",   "#a4a4a4",   "#dcdcd9") \
    X(devGridStrong, "#363636",   "#949494",   "#cdcdca") \
    X(devGridText,   "#5e5e5e",   "#4a4a4a",   "#7a7a77") \
    X(devLabel,      "#9c9c9c",   "#2a2a2a",   "#4a4e4b") \
    X(devValue,      "#e0e0e0",   "#141414",   "#1e201e") \
    X(devCurve,      "#ffa63c",   "#f0b43c",   "#e0901e") \
    X(devCurveFill,  "#2cffa63c", "#2cf0b43c", "#2ce0901e") \
    X(devCyan,       "#5cc8e8",   "#238495",   "#288b9d") \
    X(devCyanFill,   "#285cc8e8", "#28238495", "#28288b9d") \
    X(devCyanDark,   "#285a78",   "#17596a",   "#1a5a6c") \
    X(devBandNeutral,"#b8b8b8",   "#2a2a2a",   "#3a3d3a")   /* the EQ's outer bands (the inner ones keep their colours) */ \
    X(devHandle,     "#f4f4f4",   "#141414",   "#1e201e") \
    X(devButton,     "#3b3b3b",   "#a0a0a0",   "#d6d6d2") \
    X(devButtonText, "#b8b8b8",   "#1e1e1e",   "#2a2d2a") \
    X(devButtonOnText,"#161616",  "#161616",   "#161616") \
    X(devSeparator,  "#1f1f1f",   "#6c6c6c",   "#a6a6a3") \
    X(devCell,       "#202020",   "#bebebe",   "#f3f3f1")   /* wavetable frames */ \
    X(devPad,        "#3a3a3a",   "#9a9a9a",   "#cfcfcb") \
    X(devPadEmpty,   "#262626",   "#868686",   "#bebebb") \
    X(devPadEdge,    "#111111",   "#4a4a4a",   "#8a8a87") \
    X(devChip,       "#242424",   "#a8a8a8",   "#dededb")   /* Monolith's modulation-source chips */ \
    X(devChain,      "#333333",   "#9a9a9a",   "#cacac6") \
    X(devChainSel,   "#5d5d5d",   "#5a626c",   "#8d96a3") \
    X(devKeyWhite,   "#cfcfcf",   "#e2e2e2",   "#f4f4f2") \
    X(devKeyBlack,   "#232323",   "#232323",   "#2a2a2a") \
    X(devDot,        "#c8c8c8",   "#1e1e1e",   "#3a3d3a") \
    X(devHighlight,  "#16ffffff", "#1effffff", "#16000000") /* the playing step, the key zone */ \
    X(devMissing,    "#ff6a5a",   "#b8302a",   "#c0352c")   /* a missing capture or IR in a device panel */

// The colours: mutable globals, initialised to the Dark values. Read them when you paint.
#define SESSIONS_THEME_DECLARE(name, dark, mid, light) inline QColor name(dark);
SESSIONS_THEME_COLOURS(SESSIONS_THEME_DECLARE)
#undef SESSIONS_THEME_DECLARE

inline QColor withAlpha(const QColor& c, int alpha) { QColor x = c; x.setAlpha(alpha); return x; }

// A theme: a base palette, tinted, with an accent and overrides (see the top of this file).
struct Theme {
    QString name;
    Kind    base = Kind::Dark;
    double  hue = -1;        // 0 .. 360: the hue the greys take; < 0: none
    double  tint = 0;        // 0 .. 1: how strongly they take it
    double  contrast = 0;    // -1 .. 1: the greys pushed away from (or towards) the background's lightness
    QColor  accent;          // invalid: the base's own (#ffa63c in the dark)
    QHash<QString, QColor> overrides;   // colour name -> value, on top of all that
    bool    builtin = false;
    bool operator==(const Theme& o) const;
    bool operator!=(const Theme& o) const { return !(*this == o); }
};

// Switches every colour to `kind`'s base palette (the built-in Dark / Mid / Light themes), or to a
// theme: re-applies the stylesheet to the running QApplication, tells notifier() and repaints every
// widget. Doesn't save: see save().
void apply(Kind kind);
void apply(const Theme& theme);
const Theme& current();
Kind  kind();              // the current theme's base
// View > Theme is remembered in QSettings ("ui/theme": dark / mid / light, or a theme's name); Dark when unset.
Kind  savedKind();
Theme savedTheme();
void  save(Kind kind);
void  save(const Theme& theme);
QString name(Kind kind);   // "Dark", "Mid", "Light"

// The built-in themes (Dark, Mid and Light first), your own (folder()'s .sessionstheme files, by
// name), and one by name (Dark when there's none; `found` says).
const QVector<Theme>& builtinThemes();
QVector<Theme> customThemes();
Theme   themeNamed(const QString& name, bool* found = nullptr);
QString folder();                                              // ~/.local/share/Sessions/Sessions/Themes (SESSIONS_THEMES overrides it)
QString pathFor(const QString& name);                          // where a theme called `name` is saved
bool    saveCustom(const Theme& theme, QString* error = nullptr);   // writes it (replacing one of that name) and tells notifier()->listChanged()
bool    removeCustom(const QString& name);
Theme   readTheme(const QString& path, bool* ok = nullptr);    // a .sessionstheme file
QString toJson(const Theme& theme);                            // its file's text

// The colours by name, for the editor: every entry in table order, its group ("Window", "Session
// View", …), a base's value for it, and what a theme makes of it (resolve) before overrides apply.
QStringList colourNames();
QString     group(const QString& name);
QColor      baseColour(Kind base, const QString& name);
QColor      colour(const QString& name);                       // the current value
QColor      resolve(const Theme& theme, const QString& name);  // the theme's value (overrides included)
bool        isAccentColour(const QString& name);               // follows the accent
bool        isTintable(const QString& name);                   // a grey the tint and contrast reach

// Widgets that keep their own stylesheets (built from these colours) connect to this and rebuild them.
class Notifier : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
signals:
    void changed();       // the colours changed (apply)
    void listChanged();   // your themes came or went (saveCustom / removeCustom)
};
Notifier* notifier();

QString stylesheet();   // for the current colours
// Wraps the platform style (main and the smoke test, once): dialog buttons carry no icons, and the
// message-box glyphs (question, information, warning, critical) are flat discs in the theme's colours.
void installAppStyle();

} // namespace sessions::theme
