#include "ArrangementView.h"
#include "InfoView.h"
#include "InlineEdit.h"
#include "core/Model.h"
#include "core/Transport.h"
#include "core/ArrangementOps.h"
#include "core/Import.h"
#include "Browser.h"
#include "GridShade.h"
#include "Icons.h"
#include "Preferences.h"
#include "core/Theme.h"
#include "widgets/Fader.h"
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QCursor>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QToolButton>
#include <QMenu>
#include <QGuiApplication>
#include <QFileInfo>
#include <QMessageBox>
#include <QMimeData>
#include <QDragEnterEvent>
#include "DeviceDrag.h"
#include "core/Grooves.h"
#include "core/Presets.h"
#include <algorithm>
#include <cmath>

namespace sessions {

namespace {
const QColor& kAutoColor = theme::record;
const QColor& kAutoOff = theme::autoOff;
}

ArrangementView::ArrangementView(Project* project, Transport* transport, QWidget* parent)
    : QWidget(parent), m_project(project), m_transport(transport)
{
    setMouseTracking(true);
    setAcceptDrops(true);
    setFocusPolicy(Qt::ClickFocus);
    InfoView::attach(this, "Arrangement View",
        "Drag empty space to select time (Shift extends): Ctrl+L loops it, Ctrl+E splits, Ctrl+J consolidates, Delete clears. "
        "Drag clips to move them, their right edge to lengthen. Double-click a lane to add a clip. "
        "Top strip: double-click to add a locator. Below it, drag the loop brace. Drag the ruler up/down to zoom. A toggles automation.");

    m_backToArr = new QToolButton(this);
    m_backToArr->setText("Back to Arrangement");
    m_backToArr->setStyleSheet(QString("background:%1;color:%2;font-weight:600;").arg(theme::accent.name(), theme::onAccent.name()));
    connect(theme::notifier(), &theme::Notifier::changed, this, [this] {
        m_backToArr->setStyleSheet(QString("background:%1;color:%2;font-weight:600;").arg(theme::accent.name(), theme::onAccent.name()));
    });
    m_backToArr->setFocusPolicy(Qt::NoFocus);
    m_backToArr->hide();
    InfoView::attach(m_backToArr, "Back to Arrangement", "Session clips have taken over some tracks. Click to hand every track back to the timeline.");
    connect(m_backToArr, &QToolButton::clicked, transport, &Transport::backToArrangement);

    m_autoButton = new QToolButton(this);
    m_autoButton->setText("Automation  (A)");
    m_autoButton->setCheckable(true);
    m_autoButton->setFocusPolicy(Qt::NoFocus);
    InfoView::attach(m_autoButton, "Automation Mode (A)",
        "Shows one parameter's envelope in each lane. Pick the parameter under the track name. Click a lane to add a breakpoint, drag to move it, double-click to delete it.");
    connect(m_autoButton, &QToolButton::toggled, this, &ArrangementView::setAutomationMode);

    connect(project, &Project::structureChanged, this, [this] {
        validSel();
        m_selTrack = std::clamp(m_selTrack, 0, std::max(0, m_project->trackCount() - 1));
        fitLanes();
        update();
    });
    connect(project, &Project::clipChanged, this, qOverload<>(&QWidget::update));
    connect(project, &Project::mixerChanged, this, qOverload<>(&QWidget::update));
    connect(project, &Project::automationChanged, this, qOverload<>(&QWidget::update));
    connect(project, &Project::locatorsChanged, this, qOverload<>(&QWidget::update));
    connect(project, &Project::selectionChanged, this, qOverload<>(&QWidget::update));
    connect(project, &Project::restored, this, [this] {   // undo rebuilt the clip vectors
        m_sel = nullptr;
        m_drag = Drag::None;
        m_selPoint = {};
        m_selLocator = -1;
        update();
    });
    connect(transport, &Transport::tick, this, [this] {
        if (m_follow && m_transport->playing() && m_drag == Drag::None) {   // Follow: page on with the playhead
            const double x = beatToX(m_transport->beat());
            if (x < 0 || x > laneWidth() - 12) m_offset = std::max(0.0, m_transport->beat() - 0.05 * laneWidth() / m_zoom);
        }
        bool anyOv = false;
        for (const Track& t : m_project->tracks) anyOv |= t.overridden;
        m_backToArr->setVisible(anyOv);
        // repaint while something moves, and once more when the takeover ends (Back to Arrangement: the clips come back solid)
        if (m_transport->playing() || anyOv || anyOv != m_lastOverridden || !m_transport->takes().isEmpty() || m_transport->audioRecording()) update();
        m_lastOverridden = anyOv;
    });
    connect(transport, &Transport::sessionChanged, this, qOverload<>(&QWidget::update));
    connect(transport, &Transport::stateChanged, this, qOverload<>(&QWidget::update));
    connect(transport, &Transport::recordingChanged, this, qOverload<>(&QWidget::update));
}

// ---- geometry -------------------------------------------------------------

int ArrangementView::laneTop(int i) const {
    int y = kTopH;
    for (int k = 0; k < i; ++k) y += laneHeight(k);
    return y;
}

// a folded group's tracks have no lane; returns and the Main lane are shorter
int ArrangementView::laneHeight(int i) const {
    if (i >= m_project->tracks.size()) return m_returnH;
    return m_project->hiddenByFold(i) ? 0 : m_trackH;
}

int ArrangementView::laneCount() const { return m_project->trackCount(); }

QRect ArrangementView::laneRect(int lane) const { return QRect(0, laneTop(lane), laneWidth(), laneHeight(lane)); }

QRect ArrangementView::headerRect(int lane) const { return QRect(laneWidth(), laneTop(lane), m_headerW, laneHeight(lane) - 1); }

// ---- header parts: the name block, then the mixer column ------------

QRect ArrangementView::nameRect(int lane) const { const QRect h = headerRect(lane); return QRect(h.left(), h.top(), kNameW, h.height()); }
QRect ArrangementView::mixerRect(int lane) const { const QRect h = headerRect(lane); return QRect(h.left() + kNameW + 1, h.top(), m_headerW - kNameW - 1, h.height()); }
QRect ArrangementView::actRect(int lane) const { const QRect m = mixerRect(lane); return QRect(m.left() + 4, m.top() + 4, 50, 14); }
QRect ArrangementView::soloRect(int lane) const { const QRect a = actRect(lane); return QRect(a.right() + 4, a.top(), 18, 14); }
QRect ArrangementView::armRect(int lane) const { const QRect s = soloRect(lane); return QRect(s.right() + 4, s.top(), 18, 14); }
QRect ArrangementView::prePostRect(int lane) const { const QRect a = actRect(lane); return QRect(a.right() + 4, a.top(), 40, 14); }
QRect ArrangementView::chooserRect(int lane) const {
    const QRect n = nameRect(lane);
    return n.height() >= 38 ? QRect(n.left() + 4, n.top() + 20, kNameW - 8, 14) : QRect();
}

QRect ArrangementView::fieldRect(const Field& f) const {
    QRect r;
    if (f.kind == Field::MainVolume) {
        const QRect m = mixerRect(laneCount());
        return QRect(m.left() + 4, m.top() + 4, 50, 14);
    }
    if (f.lane < 0 || f.lane >= laneCount()) return {};
    const QRect m = mixerRect(f.lane);
    switch (f.kind) {
    case Field::Volume: r = QRect(m.left() + 4, m.top() + 21, 50, 14); break;
    case Field::Pan:    r = QRect(m.left() + 58, m.top() + 21, 44, 14); break;
    case Field::Send:   r = QRect(m.left() + 4 + (f.send % 2) * 54, m.top() + 38 + (f.send / 2) * 17, 50, 14); break;
    default: return {};
    }
    return r.bottom() <= m.bottom() - 1 ? r : QRect();
}

ArrangementView::Field ArrangementView::fieldAt(const QPoint& p) const {
    Field main{Field::MainVolume, laneCount(), -1};
    if (fieldRect(main).contains(p)) return main;
    const int lane = trackAt(p.y());
    if (lane < 0) return {};
    for (Field f : {Field{Field::Volume, lane, -1}, Field{Field::Pan, lane, -1}})
        if (fieldRect(f).contains(p)) return f;
    if (!m_project->isReturn(lane))
        for (int k = 0; k < m_project->returns.size(); ++k)
            if (Field f{Field::Send, lane, k}; fieldRect(f).contains(p)) return f;
    return {};
}

double ArrangementView::fieldValue(const Field& f) const {
    if (f.kind == Field::MainVolume) return m_project->mainVolume;
    const Track* t = m_project->trackAt(f.lane);
    if (!t) return 0;
    const QString tgt = f.kind == Field::Volume ? target::volume() : f.kind == Field::Pan ? target::pan() : target::send(f.send);
    // automated, or a playing clip's envelope: what's playing
    if (const double v = m_project->drivenValue(f.lane, tgt, m_transport->beat(), m_transport->playing()); v >= 0)
        return f.kind == Field::Pan ? v * 2 - 1 : v;
    return f.kind == Field::Volume ? t->volume : f.kind == Field::Pan ? t->pan : t->sends.value(f.send);
}

void ArrangementView::setFieldValue(const Field& f, double v) {
    if (f.kind == Field::MainVolume) { m_project->mainVolume = std::clamp(v, 0.0, 1.0); emit m_project->mixerChanged(); return; }
    Track* t = m_project->trackAt(f.lane);
    if (!t) return;
    switch (f.kind) {
    case Field::Volume: t->volume = std::clamp(v, 0.0, 1.0); m_project->touchTarget(f.lane, target::volume()); break;
    case Field::Pan:    t->pan = std::clamp(v, -1.0, 1.0); m_project->touchTarget(f.lane, target::pan()); break;
    case Field::Send:   if (f.send < t->sends.size()) t->sends[f.send] = std::clamp(v, 0.0, 1.0); m_project->touchTarget(f.lane, target::send(f.send)); break;
    default: return;
    }
    emit m_project->mixerChanged();
}

QString ArrangementView::fieldText(const Field& f) const {
    const double v = fieldValue(f);
    if (f.kind == Field::Pan) {
        const long p = std::lround(v * 50);
        return p == 0 ? QString("C") : QString("%1%2").arg(std::abs(p)).arg(p < 0 ? "L" : "R");
    }
    if (f.kind == Field::Send) return v < 0.001 ? QString("-inf") : QString::number(20 * std::log10(v), 'f', 1);
    return dbText(v);
}

// ---- the overview ----------------------------------------------------------------

double ArrangementView::overviewEnd() const {
    double end = 32;
    for (const Track& t : m_project->tracks) for (const ArrangementClip& a : t.arrangement) end = std::max(end, a.start + a.length + 8);
    return std::max(end, xToBeat(laneWidth()));
}

void ArrangementView::centerOn(double beat) {
    const double visible = laneWidth() / m_zoom;
    m_offset = std::max(0.0, beat - visible / 2);
    m_userZoomed = true;
    update();
}

int ArrangementView::trackAt(int y) const {
    if (y < kTopH || y >= height() - kTimeH) return -1;
    for (int i = 0; i < m_project->trackCount(); ++i)
        if (y < laneTop(i) + laneHeight(i)) return i;
    return -1;
}

void ArrangementView::fitLanes() {
    int shown = 0;
    for (int i = 0; i < m_project->tracks.size(); ++i) if (!m_project->hiddenByFold(i)) ++shown;
    const int nT = std::max(1, shown), nR = int(m_project->returns.size());
    m_returnH = m_autoMode ? 52 : 40;
    const int avail = height() - kTopH - kTimeH - (nR + 1) * m_returnH - 4;   // returns, then the Main lane
    m_trackH = std::clamp(avail / nT, 56, 120);
}

void ArrangementView::resizeEvent(QResizeEvent*) {
    fitLanes();
    if (!m_userZoomed) zoomToFit();
    const QSize bs = m_backToArr->sizeHint();
    m_backToArr->move(laneWidth() - bs.width() - 8, kLocY + 1);
    m_autoButton->setGeometry(laneWidth() + 6, kOverH + (kTopH - kOverH - 22) / 2, m_headerW - 12, 22);
}

double ArrangementView::grid() const {
    for (double g : {0.125, 0.25, 0.5, 1.0, 2.0, 4.0}) if (g * m_zoom >= 10) return g;
    return 16;
}

double ArrangementView::snap(double beat, bool fine) const {
    if (fine) return std::max(0.0, beat);
    const double g = grid();
    return std::max(0.0, std::round(beat / g) * g);
}

ArrangementClip* ArrangementView::clipAt(int track, double beat) {
    if (track < 0 || track >= m_project->tracks.size()) return nullptr;
    for (ArrangementClip& a : m_project->tracks[track].arrangement)
        if (beat >= a.start && beat < a.start + a.length) return &a;
    return nullptr;
}

ArrangementClip* ArrangementView::validSel() {
    if (!m_sel) return nullptr;
    for (Track& t : m_project->tracks)
        for (ArrangementClip& a : t.arrangement) if (&a == m_sel) return m_sel;
    m_sel = nullptr;
    return nullptr;
}

// ---- automation helpers ---------------------------------------------------

QString ArrangementView::autoTarget(int lane) const {
    const Track* t = m_project->trackAt(lane);
    if (!t) return target::volume();
    const QString chosen = m_autoTargets.value(t->id);
    if (!chosen.isEmpty() && m_project->automatableTargets(lane).contains(chosen)) return chosen;
    for (const Envelope& e : t->automation) if (!e.points.isEmpty()) return e.target;
    return target::volume();
}

double ArrangementView::valueToY(int lane, double v) const {
    const int top = laneTop(lane) + 4, h = laneHeight(lane) - 9;
    return top + (1.0 - std::clamp(v, 0.0, 1.0)) * h;
}

double ArrangementView::yToValue(int lane, double y) const {
    const int top = laneTop(lane) + 4, h = laneHeight(lane) - 9;
    return std::clamp(1.0 - (y - top) / std::max(1, h), 0.0, 1.0);
}

int ArrangementView::pointAt(int lane, const QPointF& pos) const {
    const Track* t = m_project->trackAt(lane);
    const Envelope* e = t ? t->envelope(autoTarget(lane)) : nullptr;
    if (!e) return -1;
    for (int i = 0; i < e->points.size(); ++i) {
        const QPointF pt(beatToX(e->points[i].beat), valueToY(lane, e->points[i].value));
        if (std::abs(pt.x() - pos.x()) <= 5 && std::abs(pt.y() - pos.y()) <= 5) return i;
    }
    return -1;
}

int ArrangementView::locatorAt(double x) const {
    for (int i = 0; i < m_project->locators.size(); ++i)
        if (std::abs(beatToX(m_project->locators[i].beat) - x) <= 6) return i;
    return -1;
}

void ArrangementView::chooseTarget(int lane, const QPoint& globalPos) {
    const Track* t = m_project->trackAt(lane);
    if (!t) return;
    QMenu menu(this);
    const QString current = autoTarget(lane);
    QMenu* sub = nullptr;
    int lastDevice = -1;
    for (const QString& tgt : m_project->automatableTargets(lane)) {
        const QStringList parts = tgt.split(':');
        QMenu* into = &menu;
        if (parts[0] == "device") {
            const int id = parts[1].toInt();
            if (id != lastDevice) {
                lastDevice = id;
                QString name;
                for (const Device& d : t->devices) if (d.id == id) name = d.name;
                sub = menu.addMenu(name);
            }
            into = sub;
        }
        QString label = m_project->targetName(lane, tgt);
        if (parts[0] == "device") label = label.section(" · ", 1);
        const Envelope* e = t->envelope(tgt);
        if (e && !e->points.isEmpty()) label += "   ●";
        QAction* a = into->addAction(label);
        a->setCheckable(true);
        a->setChecked(tgt == current);
        const int id = t->id;
        connect(a, &QAction::triggered, this, [this, id, tgt] { m_autoTargets.insert(id, tgt); update(); });
    }
    menu.exec(globalPos);
}

// ---- painting -------------------------------------------------------------

void ArrangementView::drawClip(QPainter& p, const Track& t, const ArrangementClip& a, int y, int h, double alpha) {
    if (!a.clip) return;
    const double x = beatToX(a.start), w = a.length * m_zoom;
    const int lw = laneWidth();
    if (x > lw || x + w < 0) return;
    p.save();
    p.setClipRect(QRectF(std::max(0.0, x), y, std::min(w, lw - x), h));
    QColor c = m_project->clipColor(a.clip); c.setAlphaF(alpha);
    p.fillRect(QRectF(x, y + 1, w - 1, 12), c);
    c.setAlphaF(alpha * 0.5);
    p.fillRect(QRectF(x, y + 13, w - 1, h - 15), c);
    QFont f = font(); f.setBold(true); f.setPointSizeF(8); p.setFont(f);
    p.setPen(theme::withAlpha(theme::ink, int(255 * alpha)));
    p.drawText(QPointF(x + 4, y + 10), a.clip->name);

    const double L = a.clip->length;
    if (a.clip->isAudio()) {   // the waveform, through the warp map, looping like the clip
        const SamplePtr smp = a.clip->reverse ? samples::reversed(a.clip->sample) : a.clip->sample;
        const double origin = a.start - a.offset, end = a.start + a.length;
        const double top = y + 14, half = (h - 16) / 2.0, mid = top + half;
        if (!smp) {
            p.setPen(theme::withAlpha(theme::missingText, int(255 * alpha)));
            p.drawText(QRectF(x + 4, top, w - 8, h - 16), Qt::AlignLeft | Qt::AlignVCenter, "missing: " + QFileInfo(a.clip->samplePath).fileName());
        } else if (L > 0) {
            const QColor ink = theme::withAlpha(theme::clipInk, int(190 * alpha));
            const double xs = std::max(0.0, x), xe = std::min(double(lw), x + w - 1);
            const double perPx = 1.0 / m_zoom;
            for (double px = std::floor(xs); px < xe; px += 1) {
                const double b = xToBeat(px);
                if (b < a.start || b >= end) continue;
                const double local = a.clip->positionFrom(a.offset, b - a.start);   // the first pass from the offset, then round the brace (0.23)
                if (local < 0) continue;
                const SampleData* src = smp.get();
                double s0 = a.clip->secondsAt(local), s1 = a.clip->secondsAt(local + perPx);
                if (!a.clip->takes.isEmpty()) {   // a loop-recorded clip: what the comp plays here
                    const int k = a.clip->takeAt(local);
                    src = k >= 0 ? a.clip->takes[k].sample.get() : nullptr;
                    s0 = a.clip->takeSeconds(k, local); s1 = a.clip->takeSeconds(k, local + perPx);
                    if (!src || s1 < 0) continue;
                }
                float lo, hi;
                src->range(std::max(0.0, std::min(s0, s1)), std::max(s0, s1), lo, hi);
                p.fillRect(QRectF(px, mid - hi * half, 1, std::max(1.0, double(hi - lo) * half)), ink);
            }
            forClipPasses(a.offset, a.clip->loopStart, L, a.clip->loop, 0, a.length, [&](double at, double, double) {   // loop seams
                const double cycle = a.start + at;
                if (cycle > a.start + 1e-9 && cycle < end) p.fillRect(QRectF(beatToX(cycle), y + 13, 1, h - 15), theme::withAlpha(theme::clipInk, int(120 * alpha)));
            });
        }
        p.restore();
        if (&a == m_sel && !m_autoMode) { p.setPen(QPen(theme::outline, 1)); p.setBrush(Qt::NoBrush); p.drawRect(QRectF(x + 0.5, y + 1.5, w - 2, h - 3)); }
        return;
    }

    // mini note preview, repeated for each loop of the clip, starting at the placement's offset
    const auto& notes = a.clip->notes;
    if (!notes.isEmpty() && L > 0) {
        int hi = -1, lo = 128;
        for (const Note& n : notes) { hi = std::max(hi, n.pitch); lo = std::min(lo, n.pitch); }
        const double nh = (h - 19.0) / (hi - lo + 1);
        const double end = a.start + a.length;
        p.setPen(Qt::NoPen);
        forClipPasses(a.offset, a.clip->loopStart, L, a.clip->loop, 0, a.length, [&](double at, double from, double to) {   // each pass: a seam, then its notes
            const double cycle = a.start + at;
            if (cycle > a.start + 1e-9 && cycle < end) p.fillRect(QRectF(beatToX(cycle), y + 13, 1, h - 15), theme::withAlpha(theme::clipInk, int(120 * alpha)));
            for (const Note& n : notes) {
                if (n.start < from || n.start >= to) continue;
                const double s = cycle + (n.start - from);
                if (s < a.start - 1e-9 || s >= end) continue;
                const double len = std::min({n.length, to - n.start, end - s});
                p.fillRect(QRectF(beatToX(s), y + 15 + (hi - n.pitch) * nh, std::max(1.0, len * m_zoom - 1), std::max(1.5, nh - 0.5)),
                           theme::withAlpha(theme::clipInk, int(200 * alpha)));
            }
        });
    }
    p.restore();
    if (&a == m_sel && !m_autoMode) { p.setPen(QPen(theme::outline, 1)); p.setBrush(Qt::NoBrush); p.drawRect(QRectF(x + 0.5, y + 1.5, w - 2, h - 3)); }
}

// A take still recording: a launched Session clip shows as that clip (looping from its launch),
// the input as the track's colour with the waveform so far, keys as the notes so far. A red
// frame says it's recording. Solid, like the clip it turns into when it ends.
void ArrangementView::drawTake(QPainter& p, const Transport::Take& take) {
    const int lane = m_project->indexOfTrack(take.trackId);
    if (lane < 0 || lane >= m_project->tracks.size() || !laneHeight(lane)) return;
    const Track& t = m_project->tracks[lane];
    const int y = laneTop(lane), h = m_trackH, lw = laneWidth();
    const double len = std::max(take.end - take.start, 2.0 / m_zoom);
    const double x = beatToX(take.start), w = len * m_zoom;
    if (x > lw || x + w < 0) return;
    if (take.source && !take.keys) {   // a Session clip on its way to the timeline
        ArrangementClip ghost;
        ghost.start = take.start;
        ghost.length = len;
        ghost.offset = std::max(0.0, take.source->positionAt(std::max(0.0, take.start - take.slotStart)));
        ghost.clip = take.source;
        drawClip(p, t, ghost, y, h, 1.0);
    } else {
        p.save();
        p.setClipRect(QRectF(std::max(0.0, x), y, std::min(w, lw - x), h));
        QColor c = t.color;
        p.fillRect(QRectF(x, y + 1, w - 1, 12), c);
        c.setAlphaF(0.5);
        p.fillRect(QRectF(x, y + 13, w - 1, h - 15), c);
        QFont f = font(); f.setBold(true); f.setPointSizeF(8); p.setFont(f);
        p.setPen(theme::ink);
        p.drawText(QPointF(x + 4, y + 10), take.keys && take.source ? take.source->name : t.name);
        const QColor ink = theme::withAlpha(theme::clipInk, 190);
        if (take.audio) {
            if (const Transport::LivePeaks* lp = m_transport->livePeaks(take.trackId)) {
                const double top = y + 14, half = (h - 16) / 2.0, mid = top + half;
                const double xs = std::max(0.0, x), xe = std::min(double(lw), x + w - 1);
                for (double px = std::floor(xs); px < xe; px += 1) {
                    const double b0 = xToBeat(px), b1 = xToBeat(px + 1);
                    const qint64 f0 = qint64(std::floor((b0 - lp->startBeat) * lp->framesPerBeat)), f1 = qint64(std::ceil((b1 - lp->startBeat) * lp->framesPerBeat));
                    if (f1 <= 0 || f0 >= lp->frames()) continue;
                    float lo, hi;
                    lp->range(f0, f1, lo, hi);
                    p.fillRect(QRectF(px, mid - hi * half, 1, std::max(1.0, double(hi - lo) * half)), ink);
                }
            }
        } else if (take.keys && take.source) {
            int hi = -1, lo = 128;
            for (const Note& n : take.source->notes) { hi = std::max(hi, n.pitch); lo = std::min(lo, n.pitch); }
            const double nh = hi >= lo ? (h - 19.0) / (hi - lo + 1) : 2;
            for (const Note& n : take.source->notes)
                p.fillRect(QRectF(beatToX(take.start + n.start), y + 15 + (hi - n.pitch) * nh, std::max(1.0, n.length * m_zoom - 1), std::max(1.5, nh - 0.5)), ink);
        }
        p.restore();
    }
    p.setPen(QPen(theme::record, 1)); p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(x + 0.5, y + 1.5, std::max(2.0, w - 2), h - 3));
}

void ArrangementView::drawEnvelope(QPainter& p, int lane) {
    const Track* t = m_project->trackAt(lane);
    if (!t) return;
    const QString tgt = autoTarget(lane);
    const Envelope* e = t->envelope(tgt);
    const QRect r = laneRect(lane);
    const bool off = t->overriddenTargets.contains(tgt);
    p.save();
    p.setClipRect(r);
    p.setRenderHint(QPainter::Antialiasing, true);
    if (!e || e->points.isEmpty()) {   // not automated yet: the static value as a dashed line
        const double y = valueToY(lane, m_project->targetValue(lane, tgt));
        p.setPen(QPen(kAutoOff, 1, Qt::DashLine));
        p.drawLine(QPointF(0, y), QPointF(r.width(), y));
        p.restore();
        return;
    }
    const QColor col = off ? kAutoOff : kAutoColor;
    QPainterPath line;
    line.moveTo(0, valueToY(lane, e->valueAt(xToBeat(0))));
    for (const AutoPoint& pt : e->points) {
        const double x = beatToX(pt.beat);
        if (x < 0 || x > r.width()) continue;
        line.lineTo(x, valueToY(lane, pt.value));
    }
    line.lineTo(r.width(), valueToY(lane, e->valueAt(xToBeat(r.width()))));
    QPainterPath area = line;
    area.lineTo(r.width(), r.bottom());
    area.lineTo(0, r.bottom());
    area.closeSubpath();
    QColor fill = col; fill.setAlpha(40);
    p.fillPath(area, fill);
    p.setPen(QPen(col, 1.5));
    p.drawPath(line);
    for (int i = 0; i < e->points.size(); ++i) {
        const QPointF pt(beatToX(e->points[i].beat), valueToY(lane, e->points[i].value));
        const bool sel = m_selPoint.lane == lane && m_selPoint.index == i;
        p.setBrush(sel ? theme::outline : col);
        p.setPen(QPen(theme::breakpointEdge, 1));
        p.drawEllipse(pt, 3.5, 3.5);
    }
    p.restore();
}

void ArrangementView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    const int lw = laneWidth(), nT = int(m_project->tracks.size()), lanes = m_project->trackCount();
    const int H = laneTop(lanes);
    p.fillRect(rect(), theme::editorBg);

    // lanes
    for (int i = 0; i < lanes; ++i) {
        if (!laneHeight(i)) continue;
        const bool group = i < nT && m_project->tracks[i].isGroup();
        const QColor c = i == m_selTrack ? theme::laneSelected : group ? theme::laneGroup : i < nT ? theme::lane : theme::laneReturn;
        p.fillRect(QRect(0, laneTop(i), lw, laneHeight(i) - 1), c);
    }

    // every second beat (or bar) a shade lighter, then the grid lines
    gridshade::paint(p, m_zoom, [this](double b) { return beatToX(b); }, m_offset, xToBeat(lw), QRectF(0, kTopH, lw, H - kTopH));
    const double b1 = xToBeat(lw);
    for (int b = std::max(0, int(std::floor(m_offset))); b <= b1; ++b) {
        bool bar = b % 4 == 0;
        if (!bar && m_zoom < 9) continue;
        int x = int(beatToX(b));
        p.fillRect(QRect(x, kTopH, 1, H - kTopH), bar ? theme::arrBar : theme::arrBeat);
    }

    // clips (dimmed while editing automation)
    for (int i = 0; i < nT; ++i) {
        const Track& t = m_project->tracks[i];
        if (!laneHeight(i)) continue;
        if (t.isGroup()) {   // a group lane sums up its tracks' clips
            const int y = laneTop(i);
            for (int m : m_project->membersOf(i))
                for (const ArrangementClip& a : m_project->tracks[m].arrangement) {
                    const double x0 = beatToX(a.start), x1 = beatToX(a.start + a.length);
                    if (x1 < 0 || x0 > lw) continue;
                    QColor c = m_project->tracks[m].color; c.setAlphaF(m_autoMode ? 0.25 : 0.45);
                    p.fillRect(QRectF(x0, y + 4, std::max(1.0, x1 - x0 - 1), m_trackH - 9), c);
                }
            continue;
        }
        const double alpha = (t.overridden ? 0.35 : 1.0) * (m_autoMode ? 0.45 : 1.0);
        for (const ArrangementClip& a : t.arrangement) drawClip(p, t, a, laneTop(i), m_trackH, alpha);
    }

    // recordings in progress: drawn as the clips they're becoming (0.22)
    for (const Transport::Take& take : m_transport->takes()) drawTake(p, take);

    // time selection, over the clips too (a part of a clip can be selected)
    if (m_timeSel.active) {
        const double x0 = beatToX(m_timeSel.start), x1 = beatToX(m_timeSel.end);
        for (int i = m_timeSel.lane0; i <= m_timeSel.lane1 && i < lanes; ++i) {
            if (!laneHeight(i)) continue;
            if (x1 - x0 >= 1) p.fillRect(QRectF(x0, laneTop(i), x1 - x0, laneHeight(i) - 1), theme::timeSelection);
            else p.fillRect(QRectF(x0, laneTop(i), 1, laneHeight(i) - 1), theme::insertMarker);   // insert marker
        }
    }

    if (m_autoMode) for (int i = 0; i < lanes; ++i) if (laneHeight(i)) drawEnvelope(p, i);

    // ---- top: overview, locators, loop brace, ruler
    {
        const QRect ov(0, 0, lw, kOverH - 1);
        p.fillRect(ov, theme::overview);
        p.fillRect(QRect(0, kOverH - 1, lw, 1), theme::line);
        const double k = (lw - 4) / overviewEnd();
        const double rowH = std::max(1.0, (kOverH - 6.0) / std::max(1, nT));
        for (int i = 0; i < nT; ++i) {
            const Track& t = m_project->tracks[i];
            for (const ArrangementClip& a : t.arrangement)
                p.fillRect(QRectF(2 + a.start * k, 3 + i * rowH, std::max(1.0, a.length * k), std::max(1.0, rowH - (rowH > 2 ? 1 : 0))), t.color);
        }
        const double vx0 = 2 + m_offset * k, vx1 = 2 + xToBeat(lw) * k;
        p.fillRect(QRectF(vx0, 1, vx1 - vx0, kOverH - 3), theme::overviewView);
        p.setPen(theme::textSoft); p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(vx0, 1, std::max(2.0, vx1 - vx0 - 1), kOverH - 4));
        p.fillRect(QRectF(2 + m_transport->beat() * k, 1, 1, kOverH - 3), theme::playhead);
    }
    p.fillRect(QRect(0, kLocY, lw, kLocH), theme::locatorStrip);
    p.fillRect(QRect(0, kBraceY, lw, kBraceH + kRulerH), theme::ruler);
    QFont small = font(); small.setPointSizeF(7.5); p.setFont(small);
    for (int i = 0; i < m_project->locators.size(); ++i) {
        const Locator& l = m_project->locators[i];
        const double x = beatToX(l.beat);
        if (x < -40 || x > lw) continue;
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(i == m_selLocator ? theme::accent : theme::text);
        QPolygonF tri; tri << QPointF(x - 5, kLocY + 2) << QPointF(x + 5, kLocY + 2) << QPointF(x, kLocY + kLocH - 2);
        p.drawPolygon(tri);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(theme::text);
        p.drawText(QPointF(x + 7, kLocY + kLocH - 3), l.name);
        p.fillRect(QRectF(x, kTopH, 1, H - kTopH), theme::locatorLine);
    }
    const double lx0 = beatToX(m_transport->loopStart()), lx1 = beatToX(m_transport->loopStart() + m_transport->loopLength());
    p.fillRect(QRectF(lx0, kBraceY + 1, lx1 - lx0, kBraceH - 2), m_transport->loopEnabled() ? theme::accent : theme::inactive);
    // punch-in / punch-out: red ends on the brace and a line down the lanes, where recording starts and stops
    for (const auto& [on, x] : {std::pair{m_transport->punchIn(), lx0}, std::pair{m_transport->punchOut(), lx1}}) {
        if (!on || x < -2 || x > lw) continue;
        p.fillRect(QRectF(x - 2, kBraceY, 4, kBraceH), theme::record);
        p.fillRect(QRectF(x, kTopH, 1, H - kTopH), theme::punchLine);
    }
    p.setPen(theme::rulerText);
    const int every = m_zoom * 4 < 22 ? 4 : m_zoom * 4 < 44 ? 2 : 1;
    for (int bar = std::max(0, int(std::floor(m_offset / 4))); bar * 4 <= b1; ++bar) {
        if (bar % every) continue;
        int x = int(beatToX(bar * 4));
        p.fillRect(QRect(x, kTopH - 5, 1, 5), theme::rulerText);
        p.drawText(x + 3, kTopH - 4, QString::number(bar + 1));
    }
    p.setFont(font());

    // ---- track headers: the name on the track's colour, then the mixer column
    QFont sf = font(); sf.setPointSizeF(7.5);
    auto button = [&](const QRect& r, bool on, const QColor& onColor, const QString& text) {
        p.fillRect(r, on ? onColor : theme::buttonOff);
        p.setPen(on ? theme::ink : theme::textSoft);
        p.drawText(r, Qt::AlignCenter, text);
    };
    auto field = [&](const Field& f) {
        const QRect r = fieldRect(f);
        if (r.isEmpty()) return;
        const Track* t = f.kind == Field::MainVolume ? nullptr : m_project->trackAt(f.lane);
        const QString tgt = f.kind == Field::Volume ? target::volume() : f.kind == Field::Pan ? target::pan() : target::send(f.send);
        const Envelope* e = t ? t->envelope(tgt) : nullptr;
        const bool automated = e && !e->points.isEmpty() && !t->overriddenTargets.contains(tgt);
        p.fillRect(r, theme::inset);
        p.setPen(automated ? kAutoColor : theme::readout);
        const QString prefix = f.kind == Field::Send ? QString(QChar('A' + f.send)) + " " : QString();
        p.drawText(r, Qt::AlignCenter, prefix + fieldText(f));
    };
    auto nameBlock = [&](const QRect& n, const QColor& colour, const QString& name, bool selected) {
        p.fillRect(n, colour);
        if (selected) { p.setPen(QPen(theme::outline, 1)); p.setBrush(Qt::NoBrush); p.drawRect(n.adjusted(0, 0, -1, -1)); }
        QFont bf = font(); bf.setBold(true); p.setFont(bf);
        p.setPen(colour.lightnessF() > 0.45 ? theme::ink : theme::inkLight);
        p.drawText(n.adjusted(6, 3, -4, 0), Qt::AlignTop | Qt::AlignLeft, p.fontMetrics().elidedText(name, Qt::ElideRight, n.width() - 10));
        p.setFont(font());
    };
    for (int i = 0; i < lanes; ++i) {
        if (!laneHeight(i)) continue;
        const Track& t = *m_project->trackAt(i);
        const bool isRet = i >= nT;
        const bool sel = i == m_selTrack || (!isRet && m_project->selectedTrackIds.size() > 1 && m_project->selectedTrackIds.contains(t.id));
        p.fillRect(headerRect(i), theme::editorBg);
        if (!isRet && t.isGroup()) {   // the fold arrow before the name
            const QRect n = nameRect(i);
            nameBlock(n.adjusted(0, 0, 0, 0), t.color, "     " + t.name, sel);
            p.setPen(t.color.lightnessF() > 0.45 ? theme::ink : theme::inkLight);
            p.drawText(QRect(n.left() + 4, n.top() + 3, 14, 14), Qt::AlignCenter, t.folded ? "▸" : "▾");
        } else if (!isRet && t.groupId) {   // a group's track: its group's colour down the left edge
            const int g = m_project->groupOf(i);
            const QRect n = nameRect(i);
            p.fillRect(QRect(n.left(), n.top(), 6, n.height()), g >= 0 ? m_project->tracks[g].color.darker(130) : theme::buttonOff);
            nameBlock(n.adjusted(7, 0, 0, 0), t.color, t.name, sel);
        } else {
            nameBlock(nameRect(i), t.color, t.name, sel);
        }
        p.fillRect(mixerRect(i), sel ? theme::headerMixerSel : isRet ? theme::panel : theme::headerMixer);
        p.setFont(sf);
        button(actRect(i), t.active, theme::mute, isRet ? QString(QChar('A' + i - nT)) : QString::number(i + 1));
        if (isRet) {
            button(prePostRect(i), !t.preFader, theme::accent, t.preFader ? "Pre" : "Post");
        } else {
            button(soloRect(i), t.solo, theme::solo, "S");
            if (!t.isGroup()) {
                button(armRect(i), t.armed, theme::record, "");
                icons::paint(p, icons::Glyph::Record, QRectF(armRect(i)).adjusted(1, 1, -1, -1), t.armed ? theme::ink : theme::textSoft);
            }
            for (int k = 0; k < m_project->returns.size(); ++k) field({Field::Send, i, k});
        }
        field({Field::Volume, i, -1});
        field({Field::Pan, i, -1});
        if (m_autoMode && !chooserRect(i).isEmpty()) {   // which parameter this lane shows
            const QRect chooser = chooserRect(i);
            p.fillRect(chooser, theme::chooserBg);
            p.setPen(t.envelope(autoTarget(i)) ? kAutoColor : theme::chooserText);
            p.drawText(chooser.adjusted(3, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft,
                       p.fontMetrics().elidedText("▾ " + m_project->targetName(i, autoTarget(i)), Qt::ElideRight, chooser.width() - 5));
        }
        p.setFont(font());
    }
    {   // the Main lane: main volume
        const int y = mainLaneTop();
        p.fillRect(QRect(0, y, lw, m_returnH - 1), theme::editorBg);
        p.fillRect(QRect(lw, y, m_headerW, m_returnH - 1), theme::editorBg);
        nameBlock(nameRect(lanes).adjusted(0, 0, 0, 0), theme::neutral, "Master", m_selTrack == lanes);
        p.fillRect(mixerRect(lanes), theme::panel);
        p.setFont(sf);
        field({Field::MainVolume, lanes, -1});
        p.setFont(font());
    }
    p.fillRect(QRect(lw, 0, m_headerW, kTopH), theme::ruler);
    p.fillRect(QRect(lw, 0, 1, height()), theme::line);

    if (m_drag == Drag::Header && m_moved) {   // where a dragged track will land
        const int target = std::clamp(trackAt(int(m_mousePos.y())), 0, nT - 1);
        const int y = target > m_dragTrack ? laneTop(target) + laneHeight(target) : laneTop(target);
        p.fillRect(QRect(0, y - 1, width(), 3), theme::accent);
    }

    // ---- bottom: the time ruler (minutes:seconds)
    {
        const int ty = height() - kTimeH;
        p.fillRect(QRect(0, ty, width(), kTimeH), theme::ruler);
        p.fillRect(QRect(0, ty, width(), 1), theme::line);
        const double secPerBeat = 60.0 / std::max(1.0, m_project->tempo);
        double step = 300;
        for (double st : {1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0}) if (st / secPerBeat * m_zoom >= 56) { step = st; break; }
        QFont tf = font(); tf.setPointSizeF(7.5); p.setFont(tf);
        p.setPen(theme::rulerText);
        for (double sec = std::floor(m_offset * secPerBeat / step) * step;; sec += step) {
            const double x = beatToX(sec / secPerBeat);
            if (x > lw) break;
            if (x < 0) continue;
            const int whole = int(std::lround(sec));
            p.fillRect(QRectF(x, ty + 1, 1, 4), theme::rulerText);
            p.drawText(QPointF(x + 3, ty + kTimeH - 4), QString("%1:%2").arg(whole / 60).arg(whole % 60, 2, 10, QChar('0')));
        }
        p.setFont(font());
    }

    // a device drag: the track it would go on, or a new one
    if (m_deviceLane >= 0) {
        p.setPen(QPen(theme::accent, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRect(0, laneTop(m_deviceLane), width(), laneHeight(m_deviceLane)).adjusted(1, 1, -1, -1));
    } else if (m_deviceLane == -1) {
        const int y0 = mainLaneTop();
        p.setPen(QPen(theme::accent, 2, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRect(0, y0, width(), std::max(m_returnH, height() - kTimeH - y0)).adjusted(1, 1, -1, -1));
        p.setPen(theme::accent);
        p.drawText(QPointF(10, y0 + 15), "New track");
    }

    // a file drag: where the audio will land
    if (m_dropLane >= -1) {
        const double dx = beatToX(m_dropBeat);
        const int y0 = m_dropLane >= 0 ? laneTop(m_dropLane) : laneTop(int(m_project->tracks.size()));
        const int hh = m_dropLane >= 0 ? laneHeight(m_dropLane) : 24;
        p.fillRect(QRectF(dx - 1, y0, 2, hh), theme::accent);
        if (m_dropLane < 0 || m_project->tracks[m_dropLane].type != TrackType::Audio) {
            p.setPen(theme::accent);
            p.drawText(QPointF(dx + 6, y0 + 14), "New audio track");
        }
    }

    // a Session clip dragged in through Tab: where it would land (the clip itself, faded, with the
    // snapped start in the accent), or why it can't land here
    if (m_clipDrag) {
        const int lane = clipDropLane(m_mousePos, m_clipDrag);
        if (lane >= 0) {
            const double at = snap(xToBeat(m_mousePos.x()), m_clipDragFine);
            ArrangementClip ghost;
            ghost.start = at; ghost.length = m_clipDrag->length; ghost.clip = m_clipDrag;
            drawClip(p, m_project->tracks[lane], ghost, laneTop(lane), m_trackH, 0.6);
            p.setPen(QPen(theme::dropGhost, 1, Qt::DashLine)); p.setBrush(Qt::NoBrush);
            p.drawRect(QRectF(beatToX(at) + 0.5, laneTop(lane) + 1.5, std::max(2.0, m_clipDrag->length * m_zoom - 2), m_trackH - 3));
            p.fillRect(QRectF(beatToX(at) - 1, laneTop(lane), 2, laneHeight(lane)), theme::accent);
        } else if (m_mousePos.x() >= 0 && m_mousePos.x() < lw && m_mousePos.y() >= kTopH && m_mousePos.y() < height() - kTimeH) {
            p.setPen(theme::dim);
            p.drawText(QPointF(10, kTopH + 15), m_clipDrag->isAudio() ? "Drop it on an audio track" : "Drop it on a MIDI track");
        }
    }

    // start marker: where Play starts (an outlined triangle in the ruler)
    const double sx = beatToX(m_transport->startMarker());
    if (sx >= 0 && sx < lw) {
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(Qt::NoBrush); p.setPen(QPen(theme::accent, 1.5));
        QPolygonF tri; tri << QPointF(sx - 4, kTopH - 6) << QPointF(sx + 4, kTopH - 6) << QPointF(sx, kTopH);
        p.drawPolygon(tri);
        p.setRenderHint(QPainter::Antialiasing, false);
    }

    // playhead
    const double px = beatToX(m_transport->beat());
    if (px >= 0 && px < lw) {
        p.fillRect(QRectF(px, kTopH, 1, H - kTopH), theme::playhead);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(theme::playhead); p.setPen(Qt::NoPen);
        QPolygonF tri; tri << QPointF(px - 4, kTopH - 6) << QPointF(px + 4, kTopH - 6) << QPointF(px, kTopH);
        p.drawPolygon(tri);
    }
}

// ---- mouse ----------------------------------------------------------------

void ArrangementView::mousePressEvent(QMouseEvent* e) {
    if (m_clipDrag) return;   // a Session clip is in flight: the button that holds it is already down
    if (e->button() != Qt::LeftButton) return;
    const QPointF pos = e->position();
    const bool fine = e->modifiers() & (Qt::AltModifier | Qt::MetaModifier);
    m_pressPos = pos;
    m_moved = false;
    const int lw = laneWidth(), nT = int(m_project->tracks.size());
    const double beat = xToBeat(pos.x());

    if (pos.x() >= lw) {                                        // ---- header
        const QPoint pt = pos.toPoint();
        if (const Field f = fieldAt(pt); f.kind != Field::None) {   // a value field: drag it
            if (f.kind != Field::MainVolume) { selectTrack(f.lane); emit trackSelected(f.lane); }
            const Track* ft = f.kind == Field::MainVolume ? nullptr : m_project->trackAt(f.lane);
            const int what = f.kind == Field::Volume || f.kind == Field::MainVolume ? 1 : f.kind == Field::Pan ? 2 : 10 + f.send;
            m_field = f;
            m_fieldV0 = fieldValue(f);
            m_drag = Drag::Field;
            m_project->beginEdit(f.kind == Field::Pan ? "Pan" : f.kind == Field::Send ? "Send " + QString(QChar('A' + f.send)) : f.kind == Field::MainVolume ? "Master Volume" : "Volume",
                                 mixerMergeKey(ft ? ft->id : 0, what));
            m_editOpen = true;
            update();
            return;
        }
        const int i = trackAt(int(pos.y()));
        if (i < 0) {
            if (pt.y() >= mainLaneTop() && pt.y() < mainLaneTop() + m_returnH) { m_selTrack = laneCount(); emit trackSelected(m_project->trackCount()); update(); }   // the Master
            return;
        }
        Track& t = *m_project->trackAt(i);
        if (i < nT && t.isGroup() && nameRect(i).contains(pt) && pt.x() - nameRect(i).left() < 18 && pt.y() - nameRect(i).top() < 18) {
            m_project->setFolded(i, !t.folded);   // the fold arrow
            return;
        }
        if (i < nT && nameRect(i).contains(pt) && (e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
            // Ctrl+click adds or removes a track, Shift+click a range: the tracks Group Tracks (Ctrl+G) takes
            QSet<int>& ids = m_project->selectedTrackIds;
            if (m_selTrack < nT && ids.isEmpty()) ids.insert(m_project->tracks[m_selTrack].id);
            if (e->modifiers() & Qt::ShiftModifier) {
                for (int k = std::min(m_selTrack, i); k <= std::max(m_selTrack, i) && k < nT; ++k)
                    if (laneHeight(k)) ids.insert(m_project->tracks[k].id);
            } else if (ids.contains(t.id) && ids.size() > 1) {
                ids.remove(t.id);
                const int still = m_project->selectedTrackIndexes().first();   // one that's still selected
                selectTrack(still);
                emit trackSelected(still);
                emit m_project->selectionChanged();
                return;
            } else {
                if (m_selTrack < nT) ids.insert(m_project->tracks[m_selTrack].id);
                ids.insert(t.id);
            }
            selectTrack(i);
            emit trackSelected(i);
            emit m_project->selectionChanged();
            return;
        }
        if (i < nT) m_project->selectedTrackIds = {t.id};
        selectTrack(i);
        emit trackSelected(i);
        if (m_autoMode && chooserRect(i).contains(pt)) { chooseTarget(i, e->globalPosition().toPoint()); return; }
        if (actRect(i).contains(pt)) {
            EditScope edit(m_project, t.active ? "Track Off" : "Track On");
            t.active = !t.active;
            emit m_project->mixerChanged();
        } else if (i >= nT && prePostRect(i).contains(pt)) {
            EditScope edit(m_project, t.preFader ? "Send Post-Fader" : "Send Pre-Fader");
            t.preFader = !t.preFader;
            emit m_project->mixerChanged();
        } else if (i < nT && soloRect(i).contains(pt)) {   // solo isn't undoable
            const bool on = !t.solo;
            if (!(e->modifiers() & Qt::ControlModifier)) for (Track& o : m_project->tracks) o.solo = false;
            t.solo = on;
            emit m_project->mixerChanged();
        } else if (i < nT && !t.isGroup() && armRect(i).contains(pt)) {
            t.armed = !t.armed;
            emit m_project->mixerChanged();
            emit m_project->structureChanged();
        } else if (i < nT && nameRect(i).contains(pt)) {
            m_drag = Drag::Header;
            m_dragTrack = i;
        }
        return;
    }
    if (pos.y() < kOverH) {                                     // ---- overview: drag sideways to scroll, up / down to zoom; click to go there
        m_drag = Drag::Overview;
        m_overK = (lw - 4) / overviewEnd();
        m_pressBeat = (pos.x() - 2) / m_overK;   // the song beat under the pointer
        m_zoom0 = m_zoom;
        m_offset0 = m_offset;
        return;
    }
    if (pos.y() < kLocY + kLocH) {                              // ---- locators
        m_selLocator = locatorAt(pos.x());
        if (m_selLocator >= 0) {
            jumpTo(m_project->locators[m_selLocator].beat);
            m_drag = Drag::Locator;
            m_project->beginEdit("Move Locator");
            m_editOpen = true;
        }
        update();
        return;
    }
    if (pos.y() < kBraceY + kBraceH) {                          // ---- loop brace
        const double xs = beatToX(m_transport->loopStart()), xe = beatToX(m_transport->loopStart() + m_transport->loopLength());
        m_loop0 = m_transport->loopStart();
        m_loopLen0 = m_transport->loopLength();
        m_pressBeat = beat;
        if (std::abs(pos.x() - xs) <= 5) m_drag = Drag::LoopStart;
        else if (std::abs(pos.x() - xe) <= 5) m_drag = Drag::LoopEnd;
        else if (pos.x() > xs && pos.x() < xe) m_drag = Drag::LoopMove;
        else { m_transport->setLoop(snap(beat, fine), grid()); m_loop0 = snap(beat, fine); m_drag = Drag::LoopEnd; }
        return;
    }
    if (pos.y() < kTopH) {                                      // ---- ruler
        m_drag = Drag::Ruler; m_pressBeat = beat; m_zoom0 = m_zoom;
        m_userZoomed = true;
        return;
    }

    const int i = trackAt(int(pos.y()));                        // ---- lanes
    if (i < 0) { m_sel = nullptr; m_timeSel.active = false; update(); return; }
    selectTrack(i);
    emit trackSelected(i);

    if (m_autoMode) {
        Track& t = *m_project->trackAt(i);
        const QString tgt = autoTarget(i);
        m_project->beginEdit("Edit Automation");
        m_editOpen = true;
        int pi = pointAt(i, pos);
        if (pi < 0) {   // a new breakpoint where you clicked
            Envelope* env = t.envelope(tgt);
            if (!env) { t.automation.push_back({tgt, {}}); env = &t.automation.last(); }
            const AutoPoint pt{snap(beat, fine), yToValue(i, pos.y())};
            auto it = std::upper_bound(env->points.begin(), env->points.end(), pt.beat, [](double b, const AutoPoint& q) { return b < q.beat; });
            pi = int(it - env->points.begin());
            env->points.insert(pi, pt);
            emit m_project->automationChanged();
        }
        m_selPoint = {i, pi};
        m_drag = Drag::Point;
        update();
        return;
    }

    ArrangementClip* a = clipAt(i, beat);
    m_sel = a;
    m_selPoint = {};
    m_copyDrag = false;
    m_copyOrig = -1;
    const bool ctrl = e->modifiers() & Qt::ControlModifier;
    const bool edge = a && (a->start + a->length - beat) * m_zoom < 6;
    const bool leftEdge = a && !edge && (beat - a->start) * m_zoom < 6;
    const bool title = a && pos.y() < laneTop(i) + 14;   // the clip's name bar moves it; its body selects time inside it
    const bool inRange = a && hasTimeSelection() && i >= m_timeSel.lane0 && i <= m_timeSel.lane1 && beat >= m_timeSel.start && beat < m_timeSel.end;
    if (a && !edge && !title && inRange) {   // drag the selected part of the clip(s) away; Ctrl: a copy of it
        emit clipSelected(i, a);
        beginRangeDrag(ctrl);
        update();
        return;
    }
    if (a && (edge || leftEdge || title)) {
        m_transport->setStartMarker(a->start);   // Play starts at the clip you clicked
        emit clipSelected(i, a);
        m_dragTrack = i;
        m_drag = edge ? Drag::Resize : leftEdge ? Drag::ResizeLeft : Drag::Move;
        m_project->beginEdit(edge || leftEdge ? "Resize Clip" : ctrl ? "Duplicate Clip" : "Move Clip");
        m_editOpen = true;
        m_clipOffset0 = a->offset;
        if (m_drag == Drag::Move && ctrl) {   // Ctrl+drag: a copy goes along, the original stays put
            Track& t = m_project->tracks[i];
            m_copyOrig = int(a - t.arrangement.constData());
            ArrangementClip copy = *a;
            copy.clip = m_project->copyClip(a->clip);
            t.arrangement.push_back(copy);
            m_sel = a = &t.arrangement.last();
            m_copyDrag = true;
        }
        m_start0 = a->start; m_len0 = a->length;
        m_timeSel.active = false;
    } else {
        if (a) {   // the body: the clip is selected (Play starts at it), and a time selection starts inside it
            m_transport->setStartMarker(a->start);
            emit clipSelected(i, a);
        }
        const double b = snap(beat, fine);
        if ((e->modifiers() & Qt::ShiftModifier) && m_timeSel.active) {
            m_timeSel.start = std::min(m_anchorBeat, b);
            m_timeSel.end = std::max(m_anchorBeat, b);
            m_timeSel.lane0 = std::min(m_anchorLane, i);
            m_timeSel.lane1 = std::max(m_anchorLane, i);
        } else {
            m_anchorBeat = b;
            m_anchorLane = i;
            m_timeSel = {true, b, b, i, i};
        }
        m_drag = Drag::Time;
    }
    update();
}

void ArrangementView::mouseMoveEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    m_mousePos = pos;
    const bool fine = e->modifiers() & (Qt::AltModifier | Qt::MetaModifier);
    if (m_clipDrag) {   // a Session clip in flight: follow it
        m_clipDragFine = fine;
        setCursor(clipDropLane(pos, m_clipDrag) >= 0 ? Qt::ClosedHandCursor : Qt::ForbiddenCursor);
        update();
        return;
    }
    const int lw = laneWidth();
    if (m_drag == Drag::None) {
        Qt::CursorShape cur = Qt::ArrowCursor;
        if (pos.x() >= lw) cur = fieldAt(pos.toPoint()).kind != Field::None ? Qt::SizeVerCursor : Qt::PointingHandCursor;
        else if (pos.y() < kOverH) cur = Qt::PointingHandCursor;
        else if (pos.y() < kLocY + kLocH) cur = locatorAt(pos.x()) >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor;
        else if (pos.y() < kBraceY + kBraceH) {
            const double xs = beatToX(m_transport->loopStart()), xe = beatToX(m_transport->loopStart() + m_transport->loopLength());
            cur = std::abs(pos.x() - xs) <= 5 || std::abs(pos.x() - xe) <= 5 ? Qt::SizeHorCursor : Qt::OpenHandCursor;
        } else if (pos.y() < kTopH) cur = Qt::SizeVerCursor;
        else if (m_autoMode) cur = pointAt(trackAt(int(pos.y())), pos) >= 0 ? Qt::SizeAllCursor : Qt::CrossCursor;
        else {
            const int i = trackAt(int(pos.y()));
            const double b = xToBeat(pos.x());
            const bool ctrl = e->modifiers() & Qt::ControlModifier;
            if (ArrangementClip* a = clipAt(i, b)) {
                const bool inRange = hasTimeSelection() && i >= m_timeSel.lane0 && i <= m_timeSel.lane1 && b >= m_timeSel.start && b < m_timeSel.end;
                if ((a->start + a->length - b) * m_zoom < 6 || (b - a->start) * m_zoom < 6) cur = Qt::SizeHorCursor;
                else if (pos.y() < laneTop(i) + 14 || inRange) cur = ctrl ? Qt::DragCopyCursor : Qt::OpenHandCursor;
                else cur = Qt::IBeamCursor;
            } else if (i >= 0) cur = Qt::IBeamCursor;
        }
        setCursor(cur);
        return;
    }
    if ((pos - m_pressPos).manhattanLength() > 2) m_moved = true;
    const double beat = xToBeat(pos.x());
    switch (m_drag) {
    case Drag::Overview: {
        // up / down zooms around the beat you pressed on (it stays where it was on screen), sideways scrolls
        const double z = std::clamp(m_zoom0 * PreferencesDialog::dragZoom(pos.y() - m_pressPos.y()), 1.5, 160.0);
        const double laneX = (m_pressBeat - m_offset0) * m_zoom0;
        m_zoom = z;
        m_offset = std::max(0.0, m_pressBeat - laneX / z + (pos.x() - m_pressPos.x()) / m_overK);
        m_userZoomed = true;
        break;
    }
    case Drag::Field: {   // like a DragValue: up raises, Shift for fine
        const double range = m_field.kind == Field::Pan ? 2.0 : 1.0;
        const double px = (e->modifiers() & Qt::ShiftModifier) ? 800.0 : 200.0;
        setFieldValue(m_field, m_fieldV0 + (m_pressPos.y() - pos.y()) / px * range);
        break;
    }
    case Drag::Ruler:
        m_zoom = std::clamp(m_zoom0 * PreferencesDialog::dragZoom(pos.y() - m_pressPos.y()), 1.5, 160.0);
        m_offset = std::max(0.0, m_pressBeat - pos.x() / m_zoom);
        break;
    case Drag::MoveRange: {   // the pieces move together, on the grid; the selection follows them
        const double g = std::max(grid(), m_zoom < 8 ? 4.0 : 1.0);
        double db = std::round((pos.x() - m_pressPos.x()) / m_zoom / g) * g;
        for (double s0 : m_rangeStart0) db = std::max(db, -s0);
        for (int k = 0; k < m_rangeDrag.size(); ++k) {
            const auto& [lane, idx] = m_rangeDrag[k];
            if (lane < m_project->tracks.size() && idx < m_project->tracks[lane].arrangement.size())
                m_project->tracks[lane].arrangement[idx].start = m_rangeStart0[k] + db;
        }
        m_timeSel.start = m_rangeSel0 + db;
        m_timeSel.end = m_rangeSel1 + db;
        break;
    }
    case Drag::ResizeLeft: {   // the start moves, the audio stays where it was
        if (!validSel() || !m_sel->clip) break;
        const double g = std::max(grid(), m_zoom < 8 ? 4.0 : 1.0);
        const double db = (pos.x() - m_pressPos.x()) / m_zoom;
        double ns = std::clamp(std::round((m_start0 + db) / g) * g, 0.0, m_start0 + m_len0 - g);
        double delta = ns - m_start0;
        const Clip& c = *m_sel->clip;
        if (!c.loop) delta = std::max(delta, -m_clipOffset0);   // a one-shot can't start before its audio does
        ns = m_start0 + delta;
        double off = m_clipOffset0 + delta;
        if (c.loop && c.length > 0 && (off < 0 || off >= c.loopEnd())) off = c.loopStart + std::fmod(std::fmod(off - c.loopStart, c.length) + c.length, c.length);   // round the brace
        m_sel->start = ns;
        m_sel->length = m_len0 - delta;
        m_sel->offset = std::max(0.0, off);
        break;
    }
    case Drag::Move:
    case Drag::Resize: {
        if (!validSel()) break;
        const double g = std::max(grid(), m_zoom < 8 ? 4.0 : 1.0);
        const double db = (pos.x() - m_pressPos.x()) / m_zoom;
        if (m_drag == Drag::Resize) {
            m_sel->length = std::max(g, std::round((m_len0 + db) / g) * g);
        } else {
            m_sel->start = std::max(0.0, std::round((m_start0 + db) / g) * g);
            int ni = trackAt(int(pos.y()));
            if (ni < 0) ni = pos.y() < kTopH ? 0 : int(m_project->tracks.size()) - 1;
            ni = std::min(ni, int(m_project->tracks.size()) - 1);   // clips never land on returns
            if (ni != m_dragTrack && m_project->tracks[ni].type == m_project->tracks[m_dragTrack].type) {
                // move the clip to another lane of the same kind: copy, erase, re-point selection
                ArrangementClip copy = *m_sel;
                auto& from = m_project->tracks[m_dragTrack].arrangement;
                from.erase(std::find_if(from.begin(), from.end(), [this](const ArrangementClip& x) { return &x == m_sel; }));
                m_project->tracks[ni].arrangement.push_back(copy);
                m_sel = &m_project->tracks[ni].arrangement.last();
                m_dragTrack = ni; m_selTrack = ni;
                emit trackSelected(ni);
            }
        }
        break;
    }
    case Drag::Time: {
        const double b = snap(beat, fine);
        int lane = trackAt(int(pos.y()));
        if (lane < 0) lane = pos.y() < kTopH ? 0 : m_project->trackCount() - 1;
        m_timeSel.start = std::min(m_anchorBeat, b);
        m_timeSel.end = std::max(m_anchorBeat, b);
        m_timeSel.lane0 = std::min(m_anchorLane, lane);
        m_timeSel.lane1 = std::max(m_anchorLane, lane);
        break;
    }
    case Drag::Point: {
        Track* t = m_project->trackAt(m_selPoint.lane);
        Envelope* env = t ? t->envelope(autoTarget(m_selPoint.lane)) : nullptr;
        if (!env || m_selPoint.index < 0 || m_selPoint.index >= env->points.size()) break;
        const int k = m_selPoint.index;
        const double lo = k > 0 ? env->points[k - 1].beat : 0.0;
        const double hi = k + 1 < env->points.size() ? env->points[k + 1].beat : 1e9;
        env->points[k].beat = std::clamp(snap(beat, fine), lo, hi);   // breakpoints keep their order
        env->points[k].value = yToValue(m_selPoint.lane, pos.y());
        emit m_project->automationChanged();
        break;
    }
    case Drag::Locator:
        if (m_selLocator >= 0 && m_selLocator < m_project->locators.size()) {
            m_project->locators[m_selLocator].beat = snap(beat, fine);
            emit m_project->locatorsChanged();
        }
        break;
    case Drag::LoopMove:
        m_transport->setLoop(std::max(0.0, m_loop0 + std::round((beat - m_pressBeat) / grid()) * grid()), m_loopLen0);
        break;
    case Drag::LoopStart: {
        const double end = m_loop0 + m_loopLen0;
        const double s = std::min(snap(beat, fine), end - grid());
        m_transport->setLoop(s, end - s);
        break;
    }
    case Drag::LoopEnd: {
        const double e2 = std::max(snap(beat, fine), m_loop0 + grid());
        m_transport->setLoop(m_loop0, e2 - m_loop0);
        break;
    }
    default: break;
    }
    update();
}

void ArrangementView::mouseReleaseEvent(QMouseEvent* e) {
    if (m_clipDrag) {   // the Session clip lands (or doesn't)
        const ClipPtr clip = endClipDrag();
        if (e->button() == Qt::LeftButton) placeDraggedClip(clip, e->position(), e->modifiers() & (Qt::AltModifier | Qt::MetaModifier));
        return;
    }
    const Drag drag = m_drag;
    m_drag = Drag::None;
    if (drag == Drag::Ruler && !m_moved) jumpTo(snap(m_pressBeat, false));
    if (drag == Drag::Overview && !m_moved) centerOn(m_pressBeat);   // a click: go there
    if (m_copyDrag && !m_moved) {   // a Ctrl+click that didn't drag: take the copy back
        if (drag == Drag::Move && m_dragTrack >= 0 && m_dragTrack < m_project->tracks.size()) {
            auto& arr = m_project->tracks[m_dragTrack].arrangement;
            if (!arr.isEmpty() && m_sel == &arr.last()) arr.removeLast();
            m_sel = m_copyOrig >= 0 && m_copyOrig < arr.size() ? &arr[m_copyOrig] : nullptr;
        } else if (drag == Drag::MoveRange) {
            for (int k = m_rangeDrag.size() - 1; k >= 0; --k) {   // the copies were appended: last first
                const auto& [lane, idx] = m_rangeDrag[k];
                if (lane < m_project->tracks.size() && idx < m_project->tracks[lane].arrangement.size()) m_project->tracks[lane].arrangement.removeAt(idx);
            }
        }
    }
    m_copyDrag = false;
    m_copyOrig = -1;
    if (drag == Drag::Header && m_moved) {
        int target = trackAt(int(e->position().y()));
        if (target < 0) target = e->position().y() < kTopH ? 0 : int(m_project->tracks.size()) - 1;
        target = std::clamp(target, 0, int(m_project->tracks.size()) - 1);
        if (m_project->moveTrack(m_dragTrack, target)) { m_selTrack = m_project->selectedTrack; emit trackSelected(m_selTrack); }
    }
    if (drag == Drag::Time && (m_moved || !m_sel)) m_transport->setStartMarker(m_timeSel.start);   // Play starts at the insert marker / selection (a click on a clip: at the clip)
    if (drag == Drag::Locator)
        std::sort(m_project->locators.begin(), m_project->locators.end(), [](const Locator& a, const Locator& b) { return a.beat < b.beat; });
    // what's placed is what plays: whatever lay under a moved or resized clip is cut away
    if ((drag == Drag::Move || drag == Drag::Resize || drag == Drag::ResizeLeft) && m_moved && validSel()) settleOverlaps(m_dragTrack, m_sel);
    if (drag == Drag::MoveRange && m_moved) {
        QVector<QPair<int, ArrangementClip>> pieces;
        for (const auto& [lane, idx] : m_rangeDrag)
            if (lane < m_project->tracks.size() && idx < m_project->tracks[lane].arrangement.size()) pieces.push_back({lane, m_project->tracks[lane].arrangement[idx]});
        for (int k = m_rangeDrag.size() - 1; k >= 0; --k) {   // take them out (appended, so last first), then put each back over what's there
            const auto& [lane, idx] = m_rangeDrag[k];
            if (lane < m_project->tracks.size() && idx < m_project->tracks[lane].arrangement.size()) m_project->tracks[lane].arrangement.removeAt(idx);
        }
        for (const auto& [lane, piece] : pieces) {
            arrangement::clearRange(*m_project, m_project->tracks[lane], piece.start, piece.start + piece.length);
            m_project->tracks[lane].arrangement.push_back(piece);
        }
    }
    if (m_editOpen) { m_editOpen = false; m_project->endEdit(); }
    if (drag == Drag::Move || drag == Drag::Resize || drag == Drag::ResizeLeft) emit m_project->clipChanged();
    if (drag == Drag::MoveRange || ((drag == Drag::Move || drag == Drag::Resize || drag == Drag::ResizeLeft) && m_sel && m_moved)) emit m_project->structureChanged();
    m_rangeDrag.clear();
    m_rangeStart0.clear();
    update();
}

void ArrangementView::settleOverlaps(int lane, ArrangementClip*& placed) {
    if (lane < 0 || lane >= m_project->tracks.size() || !placed) return;
    Track& t = m_project->tracks[lane];
    auto it = std::find_if(t.arrangement.begin(), t.arrangement.end(), [placed](const ArrangementClip& x) { return &x == placed; });
    if (it == t.arrangement.end()) return;
    const ArrangementClip keep = *it;
    t.arrangement.erase(it);
    arrangement::clearRange(*m_project, t, keep.start, keep.start + keep.length);
    t.arrangement.push_back(keep);
    std::sort(t.arrangement.begin(), t.arrangement.end(), [](const ArrangementClip& a, const ArrangementClip& b) { return a.start < b.start; });
    placed = nullptr;
    for (ArrangementClip& a : t.arrangement) if (a.clip == keep.clip && a.start == keep.start && a.length == keep.length) placed = &a;
}

// The selected part of the clips comes loose: split out of them (or, with Ctrl, copied while
// they stay whole) and dragged together as Drag::MoveRange, one undo step.
void ArrangementView::beginRangeDrag(bool copy) {
    const double s = m_timeSel.start, e = m_timeSel.end;
    m_project->beginEdit(copy ? "Copy Selection" : "Move Selection");
    m_editOpen = true;
    m_rangeDrag.clear();
    m_rangeStart0.clear();
    for (int lane = std::max(0, m_timeSel.lane0); lane <= m_timeSel.lane1 && lane < m_project->tracks.size(); ++lane) {
        Track& t = m_project->tracks[lane];
        if (copy) {
            QVector<ArrangementClip> copies;
            for (const ArrangementClip& a : t.arrangement) {
                const double cs = std::max(a.start, s), ce = std::min(a.start + a.length, e);
                if (ce <= cs + 1e-9) continue;
                copies.push_back({cs, ce - cs, a.offset + (cs - a.start), m_project->copyClip(a.clip)});
            }
            for (const ArrangementClip& c : copies) { t.arrangement.push_back(c); m_rangeDrag.push_back({lane, int(t.arrangement.size()) - 1}); }
        } else {
            arrangement::split(*m_project, t, s);
            arrangement::split(*m_project, t, e);
            for (int k = 0; k < t.arrangement.size(); ++k) {
                const ArrangementClip& a = t.arrangement[k];
                if (a.start >= s - 1e-9 && a.start + a.length <= e + 1e-9) m_rangeDrag.push_back({lane, k});
            }
        }
    }
    for (const auto& [lane, k] : m_rangeDrag) m_rangeStart0.push_back(m_project->tracks[lane].arrangement[k].start);
    m_rangeSel0 = s;
    m_rangeSel1 = e;
    m_sel = nullptr;
    m_copyDrag = copy;
    m_drag = Drag::MoveRange;
}

void ArrangementView::mouseDoubleClickEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    const double beat = xToBeat(pos.x());
    if (pos.x() >= laneWidth()) {
        if (const Field f = fieldAt(pos.toPoint()); f.kind != Field::None) {   // double-click a field: its default
            EditScope edit(m_project, "Reset " + QString(f.kind == Field::Pan ? "Pan" : f.kind == Field::Send ? "Send" : "Volume"));
            setFieldValue(f, f.kind == Field::Pan || f.kind == Field::Send ? 0.0 : 0.75);
            return;
        }
        int i = trackAt(int(pos.y()));
        if (i >= 0) emit openDeviceView(i);
        else if (pos.y() >= mainLaneTop() && pos.y() < mainLaneTop() + m_returnH) emit openDeviceView(m_project->trackCount());   // the Master's chain
        return;
    }
    if (pos.y() < kOverH) return;
    if (pos.y() < kLocY + kLocH) {
        const int li = locatorAt(pos.x());
        if (li >= 0) {
            inlineEdit(this, QRect(int(beatToX(m_project->locators[li].beat)) + 6, kLocY, 110, kLocH + 4), m_project->locators[li].name,
                       [this, li](const QString& s) {
                           if (li >= m_project->locators.size() || s.trimmed().isEmpty()) return;
                           EditScope edit(m_project, "Rename Locator");
                           m_project->locators[li].name = s.trimmed();
                           emit m_project->locatorsChanged();
                       });
        } else {
            m_timeSel = {true, snap(beat, false), snap(beat, false), m_selTrack, m_selTrack};
            addLocator();
        }
        return;
    }
    if (pos.y() < kTopH) return;
    const int i = trackAt(int(pos.y()));
    if (i < 0) return;
    if (m_autoMode) {
        Track& t = *m_project->trackAt(i);
        const QString tgt = autoTarget(i);
        const int pi = pointAt(i, pos);
        Envelope* env = t.envelope(tgt);
        if (pi < 0 || !env) return;
        EditScope edit(m_project, "Delete Breakpoint");
        env->points.removeAt(pi);
        if (env->points.isEmpty())
            t.automation.erase(std::remove_if(t.automation.begin(), t.automation.end(), [&](const Envelope& x) { return x.target == tgt; }), t.automation.end());
        m_selPoint = {};
        emit m_project->automationChanged();
        return;
    }
    if (i >= m_project->tracks.size()) return;
    if (ArrangementClip* a = clipAt(i, beat)) { emit clipSelected(i, a); return; }
    if (m_project->tracks[i].type != TrackType::Midi) return;   // audio lanes take dropped samples
    EditScope edit(m_project, "Create Clip");
    ArrangementClip a;
    a.start = std::floor(beat / 4) * 4;
    a.length = 4;
    a.clip = m_project->newClip("MIDI Clip", 4);
    m_project->tracks[i].arrangement.push_back(a);
    m_sel = &m_project->tracks[i].arrangement.last();
    m_selTrack = i;
    m_timeSel.active = false;
    emit m_project->structureChanged();
    emit trackSelected(i);
    emit clipSelected(i, m_sel);
}

void ArrangementView::contextMenuEvent(QContextMenuEvent* e) {
    const QPoint pos = e->pos();
    if (pos.x() >= laneWidth()) {
        const int i = trackAt(pos.y());
        if (i < 0) return;
        selectTrack(i);
        emit trackSelected(i);
        showTrackMenu(this, e->globalPos(), m_project, i, [this] { renameSelectedTrack(); });
        return;
    }
    QMenu menu(this);
    if (pos.y() < kOverH) return;
    if (pos.y() < kLocY + kLocH) {
        const int li = locatorAt(pos.x());
        if (li < 0) { menu.addAction("Add Locator", this, [this, pos] { m_timeSel = {true, snap(xToBeat(pos.x()), false), snap(xToBeat(pos.x()), false), m_selTrack, m_selTrack}; addLocator(); }); }
        else {
            m_selLocator = li;
            menu.addAction("Delete Locator", this, [this, li] {
                if (li >= m_project->locators.size()) return;
                EditScope edit(m_project, "Delete Locator");
                m_project->locators.removeAt(li);
                m_selLocator = -1;
                emit m_project->locatorsChanged();
            });
        }
        menu.exec(e->globalPos());
        return;
    }
    if (pos.y() < kTopH) return;
    const int i = trackAt(pos.y());
    if (i < 0) return;
    if (m_autoMode) {
        Track& t = *m_project->trackAt(i);
        const QString tgt = autoTarget(i);
        QAction* clear = menu.addAction("Clear Envelope: " + m_project->targetName(i, tgt), this, [this, i, tgt] {
            Track* tr = m_project->trackAt(i);
            if (!tr) return;
            EditScope edit(m_project, "Clear Envelope");
            tr->automation.erase(std::remove_if(tr->automation.begin(), tr->automation.end(), [&](const Envelope& x) { return x.target == tgt; }), tr->automation.end());
            tr->overriddenTargets.remove(tgt);
            emit m_project->automationChanged();
        });
        clear->setEnabled(t.envelope(tgt) != nullptr);
        menu.addAction("Choose Parameter…", this, [this, i, e] { chooseTarget(i, e->globalPos()); });
        menu.exec(e->globalPos());
        return;
    }
    const double hereBeat = xToBeat(pos.x());
    ArrangementClip* a = clipAt(i, hereBeat);
    const bool inRange = a && hasTimeSelection() && i >= m_timeSel.lane0 && i <= m_timeSel.lane1 && hereBeat >= m_timeSel.start && hereBeat < m_timeSel.end;
    if (a) {
        m_sel = a;
        emit clipSelected(i, a);
        if (!inRange) m_timeSel.active = false;   // the menu acts on the clip (or on the selected part of it)
        menu.addAction(inRange ? "Cut Selection\tCtrl+X" : "Cut\tCtrl+X", this, [this] { emit commandRequested("cut"); });
        menu.addAction(inRange ? "Copy Selection\tCtrl+C" : "Copy\tCtrl+C", this, [this] { emit commandRequested("copy"); });
        menu.addAction("Paste\tCtrl+V", this, [this] { emit commandRequested("paste"); });
        menu.addAction("Duplicate\tCtrl+D", this, [this] { duplicateSelection(); });
        menu.addAction(inRange ? "Delete Selection\tDel" : "Delete\tDel", this, [this] { deleteSelection(); });
        menu.addSeparator();
        menu.addAction("Rename\tCtrl+R", this, [this] { renameSelectedClip(); });
        if (const ClipPtr clip = a->clip) {
            QMenu* colors = menu.addMenu("Color");
            addColorGrid(colors, m_project->clipColor(clip), [this, clip](const QColor& c) { EditScope edit(m_project, "Clip Color"); clip->color = c; emit m_project->clipChanged(); });
            colors->addAction("Track Color", this, [this, clip] { EditScope edit(m_project, "Clip Color"); clip->color = QColor(); emit m_project->clipChanged(); })->setEnabled(clip->color.isValid());
            menu.addAction(clip->loop ? "Loop Off" : "Loop On", this, [this, clip] { EditScope edit(m_project, clip->loop ? "Loop Off" : "Loop On"); clip->loop = !clip->loop; emit m_project->clipChanged(); });
            if (clip->isAudio()) {
                menu.addAction(clip->reverse ? "Play Forwards" : "Reverse", this, [this, clip] { EditScope edit(m_project, "Reverse"); clip->reverse = !clip->reverse; emit m_project->clipChanged(); })->setEnabled(clip->takes.isEmpty());
                menu.addAction(clip->warp ? "Warp Off" : "Warp On", this, [this, clip] { EditScope edit(m_project, clip->warp ? "Warp Off" : "Warp On"); clip->warp = !clip->warp; emit m_project->clipChanged(); });
            }
        }
        menu.addAction("Loop Clip\tCtrl+L", this, [this] { loopSelection(); });
        menu.addSeparator();
        menu.addAction("Save Clip to User Library…", this, [this] { emit commandRequested("saveClip"); });
        if (a->clip && a->clip->isAudio()) menu.addAction("Show Sample in File Manager", this, [this] { emit commandRequested("showSample"); });
        menu.addSeparator();
    } else {
        // empty lane: the insert marker goes here, so Paste and Insert land where you clicked
        if (!hasTimeSelection()) { m_timeSel = {true, snap(hereBeat, false), snap(hereBeat, false), i, i}; m_anchorBeat = m_timeSel.start; m_anchorLane = i; }
        menu.addAction("Paste\tCtrl+V", this, [this] { emit commandRequested("paste"); });
        if (i < m_project->tracks.size() && m_project->tracks[i].type == TrackType::Midi && !m_project->tracks[i].isGroup()) {
            menu.addAction("Insert MIDI Clip", this, [this, i] {
                if (i >= m_project->tracks.size()) return;
                EditScope edit(m_project, "Create Clip");
                ArrangementClip c;
                c.start = m_timeSel.active ? m_timeSel.start : snap(m_transport->beat(), false);
                c.length = hasTimeSelection() ? m_timeSel.end - m_timeSel.start : 4;
                c.clip = m_project->newClip("MIDI Clip", c.length);
                arrangement::clearRange(*m_project, m_project->tracks[i], c.start, c.start + c.length);
                m_project->tracks[i].arrangement.push_back(c);
                m_sel = &m_project->tracks[i].arrangement.last();
                m_selTrack = i;
                emit m_project->structureChanged();
                emit clipSelected(i, m_sel);
            });
        }
        if (hasTimeSelection()) {
            menu.addAction("Cut Selection\tCtrl+X", this, [this] { emit commandRequested("cut"); });
            menu.addAction("Copy Selection\tCtrl+C", this, [this] { emit commandRequested("copy"); });
            menu.addAction("Delete Selection\tDel", this, [this] { deleteSelection(); });
        }
        menu.addSeparator();
    }
    const bool range = hasTimeSelection();
    menu.addAction("Split\tCtrl+E", this, [this] { splitAtSelection(); });
    menu.addAction("Consolidate\tCtrl+J", this, [this] { consolidateSelection(); })->setEnabled(range || m_sel);
    menu.addAction("Loop Selection\tCtrl+L", this, [this] { loopSelection(); })->setEnabled(range || m_sel);
    menu.addSeparator();
    menu.addAction("Insert Time", this, [this] { insertTime(); });
    menu.addAction("Delete Time", this, [this] { deleteTime(); })->setEnabled(range);
    menu.addAction("Duplicate Time", this, [this] { duplicateTime(); })->setEnabled(range);
    menu.addSeparator();
    menu.addAction("Add Locator", this, [this] { addLocator(); });
    update();
    menu.exec(e->globalPos());
}

void ArrangementView::wheelEvent(QWheelEvent* e) {
    QPointF pos = e->position();
    m_userZoomed = true;
    if (e->modifiers() & Qt::ControlModifier) {
        double b = xToBeat(pos.x());
        m_zoom = std::clamp(m_zoom * std::exp(e->angleDelta().y() * 0.002), 1.5, 160.0);
        m_offset = std::max(0.0, b - pos.x() / m_zoom);
    } else {
        int d = e->angleDelta().x() ? e->angleDelta().x() : e->angleDelta().y();
        m_offset = std::max(0.0, m_offset - d / m_zoom);
    }
    update();
    e->accept();
}

// ---- commands -------------------------------------------------------------

void ArrangementView::selectTrack(int lane) {
    if (lane < 0 || lane > m_project->trackCount()) return;   // trackCount(): the Master lane
    m_selTrack = lane;
    update();
}

void ArrangementView::setAutomationMode(bool on) {
    if (on == m_autoMode) return;
    m_autoMode = on;
    m_autoButton->setChecked(on);
    m_selPoint = {};
    fitLanes();
    update();
    emit automationModeChanged(on);
}

ClipPtr ArrangementView::selectedClip() const {
    ArrangementClip* a = const_cast<ArrangementView*>(this)->validSel();
    return a ? a->clip : nullptr;
}

// ---- a Session clip dragged in (Tab while dragging it in the Session View) -------------

void ArrangementView::beginClipDrag(const ClipPtr& clip, int fromTrack, int fromScene) {
    if (!clip) return;
    m_clipDrag = clip;
    m_clipDragTrack = fromTrack;
    m_clipDragScene = fromScene;
    m_clipDragFine = false;
    m_drag = Drag::None;
    m_moved = false;
    m_mousePos = QPointF(mapFromGlobal(QCursor::pos()));
    grabMouse();   // the press happened in the Session View: the moves and the release come here now
    setCursor(clipDropLane(m_mousePos, clip) >= 0 ? Qt::ClosedHandCursor : Qt::ForbiddenCursor);
    update();
}

ClipPtr ArrangementView::endClipDrag(int* fromTrack, int* fromScene) {
    const ClipPtr clip = m_clipDrag;
    if (fromTrack) *fromTrack = m_clipDragTrack;
    if (fromScene) *fromScene = m_clipDragScene;
    m_clipDrag = nullptr;
    m_clipDragTrack = m_clipDragScene = -1;
    if (QWidget::mouseGrabber() == this) releaseMouse();
    unsetCursor();
    update();
    return clip;
}

int ArrangementView::clipDropLane(const QPointF& pos, const ClipPtr& clip) const {
    if (!clip || pos.x() < 0 || pos.x() >= laneWidth() || pos.y() < kTopH) return -1;
    const int lane = trackAt(int(pos.y()));
    if (lane < 0 || lane >= m_project->tracks.size()) return -1;   // returns and the Main lane hold no clips
    const Track& t = m_project->tracks[lane];
    if (t.isGroup() || !laneHeight(lane)) return -1;
    return clip->isAudio() == (t.type == TrackType::Audio) ? lane : -1;   // clips stay on their kind of track
}

void ArrangementView::placeDraggedClip(const ClipPtr& clip, const QPointF& pos, bool fine) {
    const int lane = clipDropLane(pos, clip);
    if (lane < 0) return;
    const double at = snap(xToBeat(pos.x()), fine);
    EditScope edit(m_project, "Drag Clip to Arrangement");
    Track& t = m_project->tracks[lane];
    arrangement::clearRange(*m_project, t, at, at + clip->length);   // the drop replaces what was under it, like Paste
    t.arrangement.push_back({at, clip->length, 0, m_project->copyClip(clip)});
    m_sel = &t.arrangement.last();
    m_selTrack = lane;
    m_timeSel.active = false;
    m_project->selectedTrack = lane;
    emit m_project->structureChanged();
    emit trackSelected(lane);
    emit clipSelected(lane, m_sel);
}

void ArrangementView::pasteClip(const ClipPtr& clip) {
    if (!clip || m_selTrack >= m_project->tracks.size() || m_project->tracks[m_selTrack].isGroup()) return;
    if (clip->isAudio() != (m_project->tracks[m_selTrack].type == TrackType::Audio)) return;   // audio clips go on audio tracks
    const double at = m_timeSel.active ? m_timeSel.start : snap(m_transport->beat(), false);
    EditScope edit(m_project, "Paste Clip");
    Track& t = m_project->tracks[m_selTrack];
    arrangement::clearRange(*m_project, t, at, at + clip->length);
    t.arrangement.push_back({at, clip->length, 0, m_project->copyClip(clip)});
    m_sel = &t.arrangement.last();
    emit m_project->structureChanged();
    emit clipSelected(m_selTrack, m_sel);
}

void ArrangementView::selectTime(double start, double end, int lane0, int lane1) {
    m_timeSel = {true, std::min(start, end), std::max(start, end), std::min(lane0, lane1), std::max(lane0, lane1)};
    m_anchorBeat = m_timeSel.start;
    m_anchorLane = m_timeSel.lane0;
    update();
}

QVector<ArrangementView::ClipPiece> ArrangementView::copyRange() const {
    QVector<ClipPiece> out;
    if (!hasTimeSelection()) return out;
    const double s = m_timeSel.start, e = m_timeSel.end;
    for (int lane = std::max(0, m_timeSel.lane0); lane <= m_timeSel.lane1 && lane < m_project->tracks.size(); ++lane)
        for (const ArrangementClip& a : m_project->tracks[lane].arrangement) {
            const double cs = std::max(a.start, s), ce = std::min(a.start + a.length, e);
            if (ce <= cs + 1e-9 || !a.clip) continue;
            // the piece's clip is a copy of the whole clip (nothing is lost): the offset says where the piece starts in it
            out.push_back({lane - std::max(0, m_timeSel.lane0), cs - s, ce - cs, a.offset + (cs - a.start), std::make_shared<Clip>(*a.clip)});
        }
    return out;
}

void ArrangementView::cutRange() {
    if (!hasTimeSelection()) return;
    EditScope edit(m_project, "Cut");
    for (int i = std::max(0, m_timeSel.lane0); i <= m_timeSel.lane1 && i < m_project->tracks.size(); ++i)
        arrangement::clearRange(*m_project, m_project->tracks[i], m_timeSel.start, m_timeSel.end);
    m_sel = nullptr;
    emit m_project->structureChanged();
}

void ArrangementView::pastePieces(const QVector<ClipPiece>& pieces) {
    if (pieces.isEmpty() || m_selTrack >= m_project->tracks.size()) return;
    const double at = m_timeSel.active ? m_timeSel.start : snap(m_transport->beat(), false);
    EditScope edit(m_project, "Paste");
    double end = at;
    int lane1 = m_selTrack;
    ArrangementClip* first = nullptr;
    int firstLane = -1;
    for (const ClipPiece& pc : pieces) {
        const int lane = m_selTrack + pc.lane;
        if (lane >= m_project->tracks.size() || !pc.clip) continue;
        Track& t = m_project->tracks[lane];
        if (t.isGroup() || pc.clip->isAudio() != (t.type == TrackType::Audio)) continue;   // pieces land on their kind of track
        arrangement::clearRange(*m_project, t, at + pc.start, at + pc.start + pc.length);
        t.arrangement.push_back({at + pc.start, pc.length, pc.offset, m_project->copyClip(pc.clip)});
        if (!first) { first = &t.arrangement.last(); firstLane = lane; }
        end = std::max(end, at + pc.start + pc.length);
        lane1 = std::max(lane1, lane);
    }
    if (!first) return;
    m_sel = nullptr;
    for (ArrangementClip& a : m_project->tracks[firstLane].arrangement) if (&a == first) m_sel = &a;
    m_timeSel = {true, at, end, m_selTrack, lane1};
    emit m_project->structureChanged();
    if (m_sel) emit clipSelected(firstLane, m_sel);
}

void ArrangementView::deleteSelection() {
    if (m_autoMode && m_selPoint.lane >= 0) {
        Track* t = m_project->trackAt(m_selPoint.lane);
        const QString tgt = autoTarget(m_selPoint.lane);
        Envelope* env = t ? t->envelope(tgt) : nullptr;
        if (env && m_selPoint.index >= 0 && m_selPoint.index < env->points.size()) {
            EditScope edit(m_project, "Delete Breakpoint");
            env->points.removeAt(m_selPoint.index);
            if (env->points.isEmpty())
                t->automation.erase(std::remove_if(t->automation.begin(), t->automation.end(), [&](const Envelope& x) { return x.target == tgt; }), t->automation.end());
            emit m_project->automationChanged();
        }
        m_selPoint = {};
        return;
    }
    if (hasTimeSelection()) {   // a part of a clip, or a stretch of the song: that, before the whole clip
        EditScope edit(m_project, "Delete");
        for (int i = m_timeSel.lane0; i <= m_timeSel.lane1 && i < m_project->tracks.size(); ++i)
            arrangement::clearRange(*m_project, m_project->tracks[i], m_timeSel.start, m_timeSel.end);
        m_sel = nullptr;
        emit m_project->structureChanged();
        return;
    }
    if (validSel()) {
        EditScope edit(m_project, "Delete Clip");
        for (Track& t : m_project->tracks) {
            auto it = std::find_if(t.arrangement.begin(), t.arrangement.end(), [this](const ArrangementClip& x) { return &x == m_sel; });
            if (it != t.arrangement.end()) { t.arrangement.erase(it); break; }
        }
        m_sel = nullptr;
        emit m_project->structureChanged();
    }
}

void ArrangementView::duplicateSelection() {
    if (ArrangementClip* sel = hasTimeSelection() ? nullptr : validSel()) {   // a selected part of a clip goes first
        EditScope edit(m_project, "Duplicate Clip");
        for (Track& t : m_project->tracks) {
            auto it = std::find_if(t.arrangement.begin(), t.arrangement.end(), [sel](const ArrangementClip& x) { return &x == sel; });
            if (it == t.arrangement.end()) continue;
            ArrangementClip copy = *it;
            copy.start += copy.length;
            copy.clip = m_project->copyClip(copy.clip);
            arrangement::clearRange(*m_project, t, copy.start, copy.start + copy.length);
            t.arrangement.push_back(copy);
            m_sel = &t.arrangement.last();
            break;
        }
        emit m_project->structureChanged();
        return;
    }
    if (!hasTimeSelection()) return;
    EditScope edit(m_project, "Duplicate");
    const double s = m_timeSel.start, e = m_timeSel.end, len = e - s;
    for (int i = m_timeSel.lane0; i <= m_timeSel.lane1 && i < m_project->tracks.size(); ++i) {
        Track& t = m_project->tracks[i];
        QVector<ArrangementClip> copies;
        for (const ArrangementClip& a : t.arrangement) {
            const double cs = std::max(a.start, s), ce = std::min(a.start + a.length, e);
            if (ce <= cs) continue;
            copies.push_back({cs + len, ce - cs, a.offset + (cs - a.start), m_project->copyClip(a.clip)});
        }
        arrangement::clearRange(*m_project, t, e, e + len);
        t.arrangement += copies;
    }
    m_timeSel.start += len;
    m_timeSel.end += len;
    emit m_project->structureChanged();
}

void ArrangementView::loopSelection() {
    if (hasTimeSelection()) m_transport->setLoop(m_timeSel.start, m_timeSel.end - m_timeSel.start);
    else if (ArrangementClip* a = validSel()) m_transport->setLoop(a->start, a->length);
    else return;
    m_transport->setLoopEnabled(true);
    update();
}

void ArrangementView::splitAtSelection() {
    QVector<double> at;
    int lane0 = m_selTrack, lane1 = m_selTrack;
    if (m_timeSel.active) {
        at << m_timeSel.start;
        if (m_timeSel.end > m_timeSel.start) at << m_timeSel.end;
        lane0 = m_timeSel.lane0;
        lane1 = m_timeSel.lane1;
    } else if (ArrangementClip* a = validSel()) {
        const double b = m_transport->beat();
        if (b <= a->start || b >= a->start + a->length) return;
        at << b;
        lane0 = lane1 = m_project->trackOfClip(a->clip);
    } else {
        return;
    }
    EditScope edit(m_project, "Split");
    for (int i = std::max(0, lane0); i <= lane1 && i < m_project->tracks.size(); ++i)
        for (double b : at) arrangement::split(*m_project, m_project->tracks[i], b);
    m_sel = nullptr;
    emit m_project->structureChanged();
}

void ArrangementView::consolidateSelection() {
    double s = 0, e = 0;
    int lane0 = 0, lane1 = -1;
    if (hasTimeSelection()) { s = m_timeSel.start; e = m_timeSel.end; lane0 = m_timeSel.lane0; lane1 = m_timeSel.lane1; }
    else if (ArrangementClip* a = validSel()) { s = a->start; e = a->start + a->length; lane0 = lane1 = m_project->trackOfClip(a->clip); }
    else return;
    EditScope edit(m_project, "Consolidate");
    ClipPtr last;
    int lastLane = -1;
    for (int i = std::max(0, lane0); i <= lane1 && i < m_project->tracks.size(); ++i)
        if (ClipPtr c = arrangement::consolidate(*m_project, m_project->tracks[i], s, e)) { last = c; lastLane = i; }
    m_sel = nullptr;
    if (lastLane >= 0)
        for (ArrangementClip& a : m_project->tracks[lastLane].arrangement) if (a.clip == last) m_sel = &a;
    emit m_project->structureChanged();
    if (m_sel) emit clipSelected(lastLane, m_sel);
}

void ArrangementView::insertTime() {
    const double at = m_timeSel.active ? m_timeSel.start : snap(m_transport->beat(), false);
    const double len = hasTimeSelection() ? m_timeSel.end - m_timeSel.start : 4.0;   // a bar, unless you selected a length
    EditScope edit(m_project, "Insert Time");
    arrangement::insertTime(*m_project, at, len);
    m_sel = nullptr;
    emit m_project->structureChanged();
    emit m_project->automationChanged();
    emit m_project->locatorsChanged();
}

void ArrangementView::deleteTime() {
    if (!hasTimeSelection()) return;
    EditScope edit(m_project, "Delete Time");
    arrangement::deleteTime(*m_project, m_timeSel.start, m_timeSel.end);
    m_timeSel.end = m_timeSel.start;
    m_sel = nullptr;
    emit m_project->structureChanged();
    emit m_project->automationChanged();
    emit m_project->locatorsChanged();
}

void ArrangementView::duplicateTime() {
    if (!hasTimeSelection()) return;
    EditScope edit(m_project, "Duplicate Time");
    const double len = m_timeSel.end - m_timeSel.start;
    arrangement::duplicateTime(*m_project, m_timeSel.start, m_timeSel.end);
    m_timeSel.start += len;
    m_timeSel.end += len;
    m_sel = nullptr;
    emit m_project->structureChanged();
    emit m_project->automationChanged();
    emit m_project->locatorsChanged();
}

void ArrangementView::addLocator() {
    const double at = m_timeSel.active ? m_timeSel.start : snap(m_transport->beat(), false);
    for (const Locator& l : m_project->locators) if (std::abs(l.beat - at) < 1e-6) return;
    EditScope edit(m_project, "Add Locator");
    m_project->locators.push_back({at, QString::number(m_project->locators.size() + 1)});
    std::sort(m_project->locators.begin(), m_project->locators.end(), [](const Locator& a, const Locator& b) { return a.beat < b.beat; });
    emit m_project->locatorsChanged();
}

void ArrangementView::renameSelectedTrack() {
    const Track* t = m_project->trackAt(m_selTrack);
    if (!t) return;
    const int index = m_selTrack;
    const QRect n = nameRect(index);
    inlineEdit(this, QRect(n.left() + 3, n.top() + 2, n.width() - 6, 20), t->name, [this, index](const QString& s) { m_project->renameTrack(index, s); });
}

void ArrangementView::renameSelectedClip() {
    ArrangementClip* a = validSel();
    if (!a || !a->clip) return;
    const int lane = m_selTrack;
    const double x = std::max(0.0, beatToX(a->start));
    const int w = int(std::clamp(beatToX(a->start + a->length) - x, 80.0, 220.0));
    const ClipPtr clip = a->clip;
    inlineEdit(this, QRect(int(x) + 1, laneTop(lane) + 1, w, 16), clip->name, [this, clip](const QString& s) {
        if (s.trimmed().isEmpty() || s == clip->name) return;
        EditScope edit(m_project, "Rename Clip");
        clip->name = s.trimmed();
        emit m_project->clipChanged();
    });
}

int ArrangementView::placeClipFiles(const QStringList& paths, int lane, double beat) {
    QVector<ClipPtr> clips;
    QStringList errors;
    for (const QString& path : paths) {
        Clip c;
        QString error;
        if (presets::readClip(path, c, nullptr, &error)) clips.push_back(std::make_shared<Clip>(c));
        else errors << QFileInfo(path).fileName() + ": " + error;
    }
    if (!errors.isEmpty()) InfoView::notify("Can't load clip", errors.join("\n"));
    if (clips.isEmpty()) return -1;
    EditScope edit(m_project, clips.size() > 1 ? "Add Clips" : "Add Clip");
    const bool audio = clips.first()->isAudio();
    int t = lane;
    if (t < 0 || t >= m_project->tracks.size() || m_project->tracks[t].isGroup() || (m_project->tracks[t].type == TrackType::Audio) != audio) {
        const int at = t < 0 ? int(m_project->tracks.size()) : t + 1;
        t = m_project->insertTrack(at, audio ? TrackType::Audio : TrackType::Midi);
        if (t < 0) return -1;
        m_project->renameTrack(t, clips.first()->name);
    }
    Track& track = m_project->tracks[t];
    double at = std::max(0.0, beat);
    m_sel = nullptr;
    for (const ClipPtr& c : clips) {
        if (c->isAudio() != audio) continue;
        const ClipPtr placed = m_project->copyClip(c);
        arrangement::clearRange(*m_project, track, at, at + placed->length);
        track.arrangement.push_back({at, placed->length, 0, placed});
        at += placed->length;
    }
    for (ArrangementClip& pl : track.arrangement) if (pl.clip == track.arrangement.last().clip) m_sel = &pl;
    m_selTrack = t;
    m_timeSel.active = false;
    emit m_project->structureChanged();
    emit trackSelected(t);
    if (m_sel) emit clipSelected(t, m_sel);
    return t;
}

void ArrangementView::zoomToFit() {
    double end = 32;
    for (const Track& t : m_project->tracks) for (const auto& a : t.arrangement) end = std::max(end, a.start + a.length);
    m_offset = 0;
    m_zoom = std::clamp((laneWidth() - 20) / (end + 8), 1.5, 160.0);   // a couple of empty bars after the song
    update();
}

// ---- dropped audio files -------------------------------------------------------

void ArrangementView::dragEnterEvent(QDragEnterEvent* e) {
    if (!Browser::audioFilesIn(e->mimeData()).isEmpty() || !Browser::clipFilesIn(e->mimeData()).isEmpty() || grooves::indexIn(e->mimeData()) >= 0) e->acceptProposedAction();
    else if (devicedrag::parse(e->mimeData()).valid()) dragMoveEvent(e);
}

int ArrangementView::deviceLaneAt(int y) const {
    if (y < kTopH || y >= height() - kTimeH) return -2;
    const int lane = trackAt(y);
    if (lane >= 0) return lane;
    if (y >= mainLaneTop() && y < mainLaneTop() + m_returnH) return m_project->trackCount();   // the Master lane: its chain
    return y >= mainLaneTop() ? -1 : -2;   // below it: a new track
}

void ArrangementView::dragMoveEvent(QDragMoveEvent* e) {
    if (const devicedrag::Payload pl = devicedrag::parse(e->mimeData()); pl.valid()) {
        int lane = deviceLaneAt(int(e->position().y()));
        if (lane == m_project->trackCount() && !devicedrag::accepts(*m_project, pl, lane)) lane = -1;   // the Master takes effects only: anything else makes a new track
        if (lane >= 0 && !devicedrag::accepts(*m_project, pl, lane)) lane = -2;
        if (lane != m_deviceLane) { m_deviceLane = lane; update(); }
        if (lane == -2) { e->ignore(); return; }
        if (pl.source == devicedrag::Payload::Devices) { e->setDropAction(devicedrag::copying(e) ? Qt::CopyAction : Qt::MoveAction); e->accept(); }
        else e->acceptProposedAction();
        return;
    }
    const QPointF pos = e->position();
    if (pos.x() >= laneWidth() || pos.y() < kTopH) { m_dropLane = -2; e->ignore(); update(); return; }
    const int i = trackAt(int(pos.y()));
    m_dropLane = i >= 0 && i < m_project->tracks.size() ? i : -1;
    m_dropBeat = snap(xToBeat(pos.x()), e->modifiers() & Qt::AltModifier);
    e->acceptProposedAction();
    update();
}

void ArrangementView::dragLeaveEvent(QDragLeaveEvent*) { m_dropLane = m_deviceLane = -2; update(); }

void ArrangementView::dropEvent(QDropEvent* e) {
    if (const devicedrag::Payload pl = devicedrag::parse(e->mimeData()); pl.valid()) {
        m_deviceLane = -2;
        update();
        int lane = deviceLaneAt(int(e->position().y()));
        if (lane == m_project->trackCount() && !devicedrag::accepts(*m_project, pl, lane)) lane = -1;
        if (lane == -2) return;
        const bool copy = devicedrag::copying(e);
        const devicedrag::Result r = devicedrag::drop(*m_project, pl, lane, {}, copy);
        InfoView::notify(r.title, r.text);
        if (!r.ok) return;
        e->setDropAction(pl.source == devicedrag::Payload::Devices && !copy ? Qt::MoveAction : Qt::CopyAction);
        e->accept();
        m_selTrack = r.track;
        emit trackSelected(r.track);
        emit openDeviceView(r.track);
        return;
    }
    const QStringList files = Browser::audioFilesIn(e->mimeData());
    const int lane = m_dropLane;
    const double beat = std::max(0.0, m_dropBeat);
    m_dropLane = -2;
    if (const QStringList clipFiles = Browser::clipFilesIn(e->mimeData()); !clipFiles.isEmpty() && lane != -2) {   // clips from the User Library
        e->acceptProposedAction();
        placeClipFiles(clipFiles, lane, beat);
        update();
        return;
    }
    double humanVel = 0, humanTime = 0;
    if (const int gi = grooves::indexIn(e->mimeData(), &humanVel, &humanTime); gi >= 0 && lane != -2) {   // a Drum Studio groove: a MIDI clip
        e->acceptProposedAction();
        int t = -1;
        ClipPtr clip;
        {
            EditScope edit(m_project, "Add Groove");
            clip = grooves::makeClip(*m_project, grooves::all()[size_t(gi)], humanVel, humanTime);
            t = grooves::toArrangement(*m_project, lane, beat, clip);
        }
        m_sel = nullptr;
        if (t >= 0) {
            for (ArrangementClip& a : m_project->tracks[t].arrangement) if (a.clip == clip) m_sel = &a;
            m_selTrack = t;
            emit trackSelected(t);
            if (m_sel) emit clipSelected(t, m_sel);
        }
        update();
        return;
    }
    if (files.isEmpty() || lane == -2) return;
    e->acceptProposedAction();
    // the files load in the background; the clips go in when they're ready
    const int trackId = lane >= 0 ? m_project->tracks[lane].id : -1;
    QGuiApplication::setOverrideCursor(Qt::BusyCursor);
    samples::loadAsync(files, this, [this, files, trackId, beat](QVector<SamplePtr> loaded, QStringList errors) {
        QGuiApplication::restoreOverrideCursor();
        int t = -1;
        QVector<ClipPtr> clips;
        if (std::any_of(loaded.begin(), loaded.end(), [](const SamplePtr& s) { return s != nullptr; })) {
            EditScope edit(m_project, files.size() > 1 ? "Add Audio Clips" : "Add Audio Clip");
            clips = import::audioClips(*m_project, files, loaded);
            // a MIDI lane or the space below: a new audio track
            t = import::toArrangement(*m_project, m_project->indexOfTrack(trackId), beat, clips);
        }
        m_sel = nullptr;
        if (t >= 0 && !clips.isEmpty()) {
            for (ArrangementClip& a : m_project->tracks[t].arrangement) if (a.clip == clips.first()) m_sel = &a;
            m_selTrack = t;
            emit trackSelected(t);
            if (m_sel) emit clipSelected(t, m_sel);
        }
        if (!errors.isEmpty())
            QMessageBox::warning(this, "Add Audio", (t >= 0 ? "Some files couldn't be read:\n\n" : "Couldn't read:\n\n") + errors.join('\n'));
        update();
    });
    update();
}

// The ruler and locators: Play starts here from now on, and a playing song jumps here.
void ArrangementView::jumpTo(double beat) {
    m_transport->setStartMarker(beat);
    if (m_transport->playing()) m_transport->setBeat(beat);
}

void ArrangementView::fitSong() {
    m_userZoomed = false;
    zoomToFit();
}

} // namespace sessions
