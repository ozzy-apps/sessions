#pragma once
// Device panels: the body of each device in the Device View, in the manner of the classic
// devices: small labelled knobs, toggle and choice buttons, and dark displays that show
// what the device does and can be dragged.
//
// DeviceBody holds what every body shares: reading and writing the device's parameters by
// index (undo, automation override and MIDI mapping included), the processor's meters, and
// the automation-follow refresh. Each device builds its own layout from the building blocks
// (knob(), toggle(), choices(), chooser(), section()) and its own Display subclasses.
// createBody() picks the body for a device type; unknown devices get a plain knob grid.

#include "core/DeviceSpecs.h"
#include "core/Theme.h"
#include <QColor>
#include <QWidget>
#include <functional>
#include <vector>

class QHBoxLayout;
class QLabel;
class QMenu;
class QPainter;

namespace sessions {

class Project;
class Transport;
struct Device;
struct MacroMap;

namespace devui {

constexpr int kBodyHeight = 192;   // every device is this tall

Device* findDevice(Project* project, int track, int deviceId);   // nullptr if it's gone

// The Drum Rack pad selected in a rack's panel (where the browser loads a sample). Kept per
// device for the session, not saved.
int  selectedDrumPad(int deviceId);
void selectDrumPad(int deviceId, int pad);

// A Drum Rack pad for people: its note and its sample's name ("C1 Kick").
QString drumPadLabel(const Device& drum, int pad);
// The devices on a Drum Rack's pad (its chain), in playing order; the instrument among them, if any.
QList<const Device*> padDevices(const Project* project, int track, int drumId, int pad);
const Device*        padInstrument(const Project* project, int track, int drumId, int pad);

// The chain shown in a rack's panel (its devices appear next to it). Per rack, not saved.
// rackSignals() announces a change (of a rack's chain, or a Drum Rack's pad) so the Device View
// can show that chain's devices.
int  selectedChain(int rackId);
void selectChain(int rackId, int chain);
class RackSignals : public QObject {
    Q_OBJECT
signals:
    void chainSelected(int rackId, int chain);
};
RackSignals* rackSignals();

// A rack panel's zone editor: shown or not, and which zones it edits (ChainZones::Which). Per rack, not saved.
bool zonesShown(int rackId);
void showZones(int rackId, bool on);
int  zoneMode(int rackId);
void setZoneMode(int rackId, int which);

// Colours (mid-grey bodies, near-black displays, orange curves, cyan second curves).
// They live in core/Theme.h (the dev* entries, one value per theme) so View > Theme recolours the panels too.
namespace col {
inline const QColor& display     = theme::devDisplay;
inline const QColor& displayEdge = theme::devDisplayEdge;
inline const QColor& grid        = theme::devGrid;
inline const QColor& gridStrong  = theme::devGridStrong;
inline const QColor& gridText    = theme::devGridText;
inline const QColor& label       = theme::devLabel;
inline const QColor& value       = theme::devValue;
inline const QColor& curve       = theme::devCurve;
inline const QColor& curveFill   = theme::devCurveFill;
inline const QColor& cyan        = theme::devCyan;
inline const QColor& cyanFill    = theme::devCyanFill;
inline const QColor& handle      = theme::devHandle;
inline const QColor& button      = theme::devButton;
inline const QColor& buttonText  = theme::devButtonText;
inline const QColor& buttonOn    = theme::accent;
inline const QColor& buttonOnText= theme::devButtonOnText;
inline const QColor& separator   = theme::devSeparator;
}

// What a Monolith modulation-source chip carries while it's dragged onto a knob: the source's index as text.
inline constexpr char kModSourceMime[] = "application/x-sessions-modsource";

struct DeviceContext {
    Project*   project = nullptr;
    Transport* transport = nullptr;
    int        track = -1;   // index into the project's tracks (returns follow them)
    int        trackId = 0;
    int        deviceId = 0;
    DeviceType type = DeviceType::Unknown;
};

// Paints a choice's icon (wave shapes, filter types, LFO shapes) into `r` in `color`.
using IconFn = std::function<void(QPainter&, const QRectF& r, int index, const QColor& color)>;
void waveIcon(QPainter& p, const QRectF& r, int shape, const QColor& c);     // 0 saw, 1 square, 2 triangle, 3 sine
void filterIcon(QPainter& p, const QRectF& r, int type, const QColor& c);    // 0 LP, 1 HP, 2 BP, 3 notch
void lfoIcon(QPainter& p, const QRectF& r, int shape, const QColor& c);      // lfoShape()'s shapes

class DeviceBody : public QWidget {
    Q_OBJECT
public:
    DeviceBody(const DeviceContext& ctx, QWidget* parent);

    const DeviceContext& ctx() const { return m_ctx; }
    const Device* device() const;
    int     count() const;
    double  pos(int i) const;      // knob position (0..1) as shown: follows automation while it plays
    double  real(int i) const;     // in the parameter's units (Hz, dB, seconds, …)
    int     pick(int i) const;     // a choice's index
    bool    on(int i) const;       // a toggle
    QString text(int i) const;     // the value as the panel prints it
    QString name(int i) const;
    std::vector<float> positions() const;   // what the engine hands the processor
    double  posFor(int i, double real) const;   // the knob position that gives `real`
    float   meter(int k) const;    // the processor's live meter (see DeviceProcessor::meter)
    float   scope(int k) const;    // the processor's scope values (see DeviceProcessor::scope)
    QString target(int i) const;   // the MIDI-map target of parameter i

    // A knob turn: one undo step per knob while it moves.
    void edit(int i, double pos);
    // A display drag that moves several parameters: one undo step per gesture.
    void editMany(const std::vector<std::pair<int, double>>& changes, int gesture);

    // ---- building blocks; refresh() keeps them current
    QWidget* knob(int i, int size = 26, const QString& label = {}, bool bipolar = false);
    QWidget* toggle(int i, const QString& text = {}, int width = 0);
    QWidget* choices(int i, std::vector<QString> labels = {}, IconFn icon = {}, int width = 0, int height = 16);
    QWidget* chooser(int i, int width = 0);
    QWidget* field(int i, int width = 44);   // a compact value field: drag up / down, Delete resets
    QLabel*  section(const QString& title);
    void     watch(QWidget* w);   // repainted on every refresh (displays)
    void     animate(class Display* d);   // ticked while shown (meters, LFOs)

    virtual DeviceBody* createEditor(QWidget*) { return nullptr; }
    virtual void refresh();
    virtual void tick();          // ~30 Hz while shown: meters and animation
    virtual bool modulated(int /*i*/) const { return false; }   // the device's own modulation turns this knob (Monolith): its value shows in cyan
    void follow(double beat);     // automation playback

    // ---- macro knobs (the Macros device and racks; Device::maps)
    QString macroLabel(int k) const;                        // what macro k turns (its first mapping), else "Macro N"
    void    macroMenu(int k, const QPoint& globalPos);      // right-click: Map to ▸ device ▸ knob, Clear
    void    fillMacroTargets(QMenu* menu, int k);           // the knobs macro k can turn
    void    addMacroMap(int k, int deviceId, int param);    // one macro per knob; the first mapping keeps the knob's value
    void    removeMacroMap(int row);
    void    clearMacro(int k);
    void    setMacroRange(int row, int which, double x);    // which: 0 Min, 1 Max
    QHBoxLayout* macroMapRow(QWidget* parent, const MacroMap& m, int row, int labelWidth);

signals:
    void editorRequested();

protected:
    DeviceContext m_ctx;
    std::vector<std::function<void()>> m_refreshers;
    std::vector<QWidget*> m_watched;
    std::vector<class Display*> m_animated;
    std::vector<double>   m_auto;   // the envelope's value while automation plays, else < 0
};

// The body for a device (a generic knob grid when it has none of its own).
DeviceBody* createBody(const DeviceContext& ctx, QWidget* parent);

// ---- displays ---------------------------------------------------------------

// A dark, framed display. Subclasses draw inside inner() and may handle drags.
class Display : public QWidget {
public:
    Display(DeviceBody* body, int w, int h);
    virtual void tick() { update(); }   // for animated displays (see DeviceBody::animate)
protected:
    void paintEvent(QPaintEvent*) override;
    virtual void draw(QPainter& p, const QRectF& r) = 0;
    QRectF inner() const { return QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5); }
    DeviceBody* m_body;
};

// Helpers for displays.
void   drawFreqGrid(QPainter& p, const QRectF& r, double dbRange);   // log 20 Hz – 20 kHz, ±dbRange
double freqToX(double f, const QRectF& r);
double xToFreq(double x, const QRectF& r);
void   drawHandle(QPainter& p, const QPointF& at, const QColor& c, const QString& label = {}, bool hollow = false);
void   drawCaption(QPainter& p, const QRectF& r, const QString& text, Qt::Alignment align = Qt::AlignTop | Qt::AlignRight);

// ADSR display: the envelope's shape with three handles (attack peak; decay end / sustain
// level; release end). Indices of the attack, decay, sustain and release parameters; a
// negative sustain index draws a decay to silence.
class AdsrDisplay : public Display {
public:
    AdsrDisplay(DeviceBody* body, int w, int h, int a, int d, int s, int r, QColor color = col::curve, const QString& caption = {});
protected:
    void draw(QPainter& p, const QRectF& r) override;
    virtual void drawLive(QPainter&, const QRectF&) {}   // over the curve: where the envelope is right now (the Monolith's)
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    QPointF point(int which, const QRectF& r) const;   // 0 attack peak, 1 decay end, 2 sustain end, 3 release end
    int sustainParam() const { return m_s; }
private:
    int m_a, m_d, m_s, m_r, m_drag = -1;
    QColor m_color;
    QString m_caption;
};

// Filter response with a handle at cutoff / resonance. `type` < 0 uses `fixedType`.
// `live` (optional) gives the cutoff the processor is at right now (Auto Filter).
class FilterDisplay : public Display {
public:
    FilterDisplay(DeviceBody* body, int w, int h, int cutoff, int reso, int type, int fixedType = 0,
                  std::function<float()> live = {});
protected:
    void draw(QPainter& p, const QRectF& r) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
private:
    int m_cutoff, m_reso, m_type, m_fixed;
    std::function<float()> m_live;
    bool m_drag = false;
};

} // namespace devui
} // namespace sessions
