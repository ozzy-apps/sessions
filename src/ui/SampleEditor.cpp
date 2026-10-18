#include "SampleEditor.h"
#include "InfoView.h"
#include "GridShade.h"
#include "Preferences.h"
#include "core/Model.h"
#include "core/Theme.h"
#include "core/Transport.h"
#include "core/SampleEdit.h"
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QFileInfo>
#include <QDir>
#include <QDesktopServices>
#include <QUrl>
#include <algorithm>
#include <cmath>

namespace sessions {

namespace {
const QColor& kMarker = theme::warpMarker;
constexpr double kMinGap = 1.0 / 64;   // beats between neighbouring markers
constexpr double kMaxZoom = 512;       // px per beat
constexpr int kHandle = 6;             // px either side of a start / end / brace handle
}

SampleEditor::SampleEditor(Project* project, Transport* transport, QWidget* parent)
    : QWidget(parent), m_project(project), m_transport(transport) {
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumHeight(120);
    InfoView::attach(this, "Sample Editor",
        "Drag across the waveform to select a stretch: Delete cuts it out, right-click for Crop and Loop Selection. A click plays from there. "
        "Drag the ▶ start marker along the audio, the ◀ end marker for the length, the loop brace to slide the loop. "
        "Warp markers (yellow) pin the audio to beats: drag one (or a grey transient tick), double-click to add or remove one. "
        "Shift+drag slides the audio under the grid, the middle button drags the view, drag the ruler up / down to zoom. Right-click for the warp commands.");
    connect(transport, &Transport::tick, this, [this] {
        followPlayhead();
        if (m_clip && (m_transport->playing() || m_transport->livePeaksFor(m_clip))) update();
    });
    connect(project, &Project::clipChanged, this, qOverload<>(&QWidget::update));
    connect(project, &Project::restored, this, [this] { m_sel = -1; m_drag = Drag::None; m_editOpen = false; m_rangeOn = false; update(); });
}

void SampleEditor::setClip(ClipPtr clip) {
    if (m_editOpen) { m_project->endEdit(); m_editOpen = false; }
    const bool same = clip && clip == m_clip;
    m_clip = clip && clip->isAudio() ? clip : nullptr;
    m_sel = -1;
    m_hoverTransient = -1;
    m_drag = Drag::None;
    if (!same) { m_rangeOn = false; m_userZoomed = false; fit(); }
    update();
}

void SampleEditor::resizeEvent(QResizeEvent*) { if (!m_userZoomed) fit(); else clampView(); }

void SampleEditor::extent(double& first, double& last) const {
    if (!m_clip) { first = 0; last = 4; return; }
    const double audioEnd = m_clip->sample ? m_clip->beatAt(m_clip->sample->seconds()) : m_clip->loopEnd();
    first = std::min(0.0, m_clip->beatAt(0));
    last = std::max({m_clip->loopEnd(), m_clip->start + 0.25, audioEnd, first + 0.25});
}

void SampleEditor::fit() {
    if (!m_clip) return;
    double first, last;
    extent(first, last);
    m_offset = first;
    m_zoom = std::max(0.5, (width() - kLeft - 12) / std::max(0.25, last - first + 0.25));
}

void SampleEditor::clampView() {
    if (!m_clip) return;
    double first, last;
    extent(first, last);
    const double span = last - first + 0.25;
    const double minZoom = std::max(0.05, (width() - kLeft - 12) / span);   // zooming out stops at the whole sample
    m_zoom = std::clamp(m_zoom, minZoom, kMaxZoom);
    const double visible = (width() - kLeft) / m_zoom;
    m_offset = std::clamp(m_offset, first, std::max(first, last + 0.25 - visible));
}

bool SampleEditor::viewport(double& first, double& last) const {
    if (!m_clip) return false;
    first = m_offset;
    last = xToBeat(width());
    return last > first;
}

void SampleEditor::setViewport(double first, double last) {
    if (!m_clip || !(last > first)) return;
    m_userZoomed = true;
    m_zoom = std::clamp((width() - kLeft) / (last - first), 0.05, kMaxZoom);
    m_offset = first;
    clampView();
    update();
}

void SampleEditor::setFollow(bool on) {
    m_follow = on;
    followPlayhead();
    update();
}

void SampleEditor::followPlayhead() {
    if (!m_follow || m_drag != Drag::None) return;
    const double beat = playheadBeat();
    if (beat < 0 || width() <= kLeft) return;
    const double span = (width() - kLeft) / m_zoom;
    // Leave a little room ahead of the cursor and retain context after a page turn.
    // Project::clipPosition also supplies the native audio clock when Warp is off.
    if (beat < m_offset || beat >= m_offset + span - std::min(12.0 / m_zoom, span * 0.1)) {
        double first, last;
        extent(first, last);
        // A loop return opens the whole first page instead of hiding its opening
        // fraction behind the generic page padding.
        m_offset = beat < m_offset && beat < first + span * 0.9 ? first : beat - span * 0.08;
        clampView();
    }
}

double SampleEditor::snap(double beat, bool free) const {
    if (free) return beat;
    const double grid = m_zoom > 200 ? 1.0 / 16 : m_zoom > 60 ? 0.25 : m_zoom > 15 ? 1.0 : 4.0;
    return std::round(beat / grid) * grid;
}

int SampleEditor::markerAt(const QPoint& p) const {
    if (!m_clip || p.y() < kStripY) return -1;
    int best = -1;
    double bestDist = p.y() < kTop ? 7 : 5;   // the strip's handles are a little easier to catch than the lines
    for (int i = 0; i < m_clip->warpMarkers.size(); ++i) {
        const double d = std::abs(beatToX(m_clip->warpMarkers[i].beat) - p.x());
        if (d <= bestDist) { best = i; bestDist = d; }
    }
    return best;
}

int SampleEditor::transientAt(const QPoint& p) const {
    if (!m_clip || !m_clip->sample || p.y() < kStripY || p.y() >= kTop) return -1;
    const std::vector<double>& tr = m_clip->sample->transients;
    int best = -1;
    double bestDist = 5;
    for (size_t i = 0; i < tr.size(); ++i) {
        const double d = std::abs(beatToX(m_clip->beatAt(tr[i])) - p.x());
        if (d <= bestDist) { best = int(i); bestDist = d; }
    }
    return best;
}

bool SampleEditor::selection(double& start, double& end) const {
    if (!hasSelection()) return false;
    start = m_rangeA; end = m_rangeB;
    return true;
}

void SampleEditor::select(double start, double end) {
    m_rangeOn = true;
    m_rangeA = std::min(start, end); m_rangeB = std::max(start, end);
    m_rangeAnchor = m_rangeA;
    emit selectionChanged();
    update();
}

void SampleEditor::selectAll() { if (m_clip) select(std::min(m_clip->start, m_clip->loopStart), m_clip->loopEnd()); }

double SampleEditor::playheadBeat() const {
    if (!m_clip || !m_transport->playing()) return -1;
    return m_project->clipPosition(m_clip, m_transport->beat());   // the start marker and the brace (0.23)
}

void SampleEditor::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const int W = width(), H = height();
    p.fillRect(rect(), theme::editorBg);
    if (!m_clip) {
        p.setPen(theme::dim);
        p.drawText(rect(), Qt::AlignCenter, "No audio clip");
        return;
    }
    const QColor marker = theme::text;   // neutral markers stay distinct from clip and warp colours
    const int top = kTop, h = H - kTop, mid = top + h / 2;
    const bool recording = !m_clip->sample && m_transport->livePeaksFor(m_clip);

    // ruler: bars and beats; then the brace row, the marker row and the warp strip
    p.fillRect(QRect(0, 0, W, kRulerH), theme::ruler);
    p.fillRect(QRect(0, kBraceY, W, kBraceH + kMarkH), theme::ruler);
    p.fillRect(QRect(0, kStripY, W, kStripH), theme::locatorStrip);
    p.fillRect(QRect(0, kTop - 1, W, 1), theme::line);
    p.fillRect(QRect(0, kStripY - 1, W, 1), theme::line);
    // the markers (0.23): ▶ at the start, the brace from loopStart, ◀ at its end; what plays is from the first of ▶ / the brace to ◀
    const double xs = beatToX(m_clip->start), xl = beatToX(m_clip->loopStart), xEnd = beatToX(m_clip->loopEnd());
    const double playFrom = std::min(m_clip->start, m_clip->loopStart), x0 = beatToX(playFrom);
    const double step = m_zoom > 200 ? 0.25 : m_zoom > 60 ? 1 : m_zoom > 15 ? 4 : m_zoom > 4 ? 16 : 64;
    p.setPen(theme::rulerText);
    QFont small = font(); small.setPointSizeF(7.5); p.setFont(small);
    gridshade::paint(p, m_zoom, [this](double b) { return beatToX(b); }, m_offset, xToBeat(W), QRectF(0, top, W, h));
    for (double b = std::ceil(m_offset / step) * step; beatToX(b) < W; b += step) {
        const double x = beatToX(b);
        const bool bar = std::abs(std::fmod(b, 4.0)) < 1e-9;
        p.fillRect(QRectF(x, kRulerH - 5, 1, 5), theme::rulerText);
        p.fillRect(QRectF(x, top, 1, h), bar ? theme::barLine : theme::beatLine);
        if (bar && (step >= 4 || std::abs(std::fmod(b, step * 4)) < 1e-9 || step <= 1)) p.drawText(QPointF(x + 3, kRulerH - 5), QString::number(int(std::floor(b / 4)) + 1));
        else if (step <= 1 && std::abs(b - std::round(b)) < 1e-9) p.drawText(QPointF(x + 3, kRulerH - 5), QString("%1.%2").arg(int(std::floor(b / 4)) + 1).arg(int(std::fmod(std::fmod(b, 4.0) + 4, 4.0)) + 1));
    }
    // the loop brace: the loop, with a handle at either end (faded with Loop off)
    {
        const QColor brace = m_clip->loop ? marker : theme::withAlpha(marker, 115);
        p.fillRect(QRectF(xl, kBraceY + 2, xEnd - xl, kBraceH - 4), brace);
        p.fillRect(QRectF(xl, kBraceY + 1, 2, kBraceH - 2), marker);
        p.fillRect(QRectF(xEnd - 2, kBraceY + 1, 2, kBraceH - 2), marker);
    }
    // the start (▶) and end (◀) markers
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen); p.setBrush(marker);
    {
        QPolygonF startTri; startTri << QPointF(xs, kMarkY + 1) << QPointF(xs + 7, kMarkY + kMarkH / 2.0) << QPointF(xs, kMarkY + kMarkH - 1);
        QPolygonF endTri; endTri << QPointF(xEnd, kMarkY + 1) << QPointF(xEnd - 7, kMarkY + kMarkH / 2.0) << QPointF(xEnd, kMarkY + kMarkH - 1);
        p.drawPolygon(startTri);
        p.drawPolygon(endTri);
    }
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(QRectF(xs, kMarkY, 1, kMarkH + kStripH), theme::withAlpha(marker, 140));
    p.fillRect(QRectF(xEnd, kMarkY, 1, kMarkH + kStripH), theme::withAlpha(marker, 140));
    if (xl != xs) p.fillRect(QRectF(xl, kBraceY, 1, kBraceH + kMarkH + kStripH), theme::withAlpha(marker, 70));

    // waveform through the warp map, one lane per channel; past the clip's end it's dimmed
    const SamplePtr smp = m_clip->reverse ? samples::reversed(m_clip->sample) : m_clip->sample;
    if (smp) {
        QColor wave = theme::waveform; wave.setAlphaF(0.92);
        QColor dim = theme::waveform; dim.setAlphaF(0.3);
        const int lanes = smp->channels >= 2 ? 2 : 1;
        const int laneH = (h - 4) / lanes;
        for (int c = 0; c < lanes; ++c) {
            const int cmid = top + 2 + c * laneH + laneH / 2, half = laneH / 2 - 3;
            p.fillRect(QRect(0, cmid, W, 1), theme::centreLine);
            if (c == 1) p.fillRect(QRect(0, top + 2 + laneH, W, 1), theme::barLine);
            for (int x = kLeft; x < W; ++x) {
                const SampleData* src = smp.get();
                double s0 = m_clip->secondsAt(xToBeat(x)), s1 = m_clip->secondsAt(xToBeat(x + 1));
                if (!m_clip->takes.isEmpty()) {   // a loop-recorded clip: the take the comp plays here
                    const int k = m_clip->takeAt(std::fmod(std::max(0.0, xToBeat(x)), std::max(1e-9, m_clip->length)));
                    src = k >= 0 ? m_clip->takes[k].sample.get() : nullptr;
                    s0 = m_clip->takeSeconds(k, xToBeat(x)); s1 = m_clip->takeSeconds(k, xToBeat(x + 1));
                }
                if (!src || s1 < 0 || s0 > src->seconds()) continue;
                float lo, hi;
                src->range(std::max(0.0, std::min(s0, s1)), std::max(s0, s1), lo, hi, lanes == 2 ? c : -1);
                const int y0 = cmid - int(hi * half), y1 = cmid - int(lo * half);
                p.fillRect(QRect(x, y0, 1, std::max(1, y1 - y0 + 1)), xToBeat(x) >= playFrom && xToBeat(x) < m_clip->loopEnd() ? wave : dim);
            }
        }
        // the transients, as grey ticks in the strip (the hovered one lit: drag it to warp there)
        if (m_clip->takes.isEmpty()) {
            const std::vector<double>& tr = m_clip->sample->transients;
            for (size_t i = 0; i < tr.size(); ++i) {
                const double x = beatToX(m_clip->beatAt(tr[i]));
                if (x < kLeft - 2 || x > W) continue;
                const bool hot = int(i) == m_hoverTransient;
                p.fillRect(QRectF(x, kStripY + (hot ? 2 : 5), 1, hot ? kStripH - 3 : kStripH - 6), hot ? theme::text : theme::faint);
                if (hot) p.fillRect(QRectF(x, top, 1, h), theme::withAlpha(theme::text, 60));
            }
        }
    } else if (const Transport::LivePeaks* lp = m_transport->livePeaksFor(m_clip)) {   // recording into this slot: the input so far
        QColor wave = theme::waveform; wave.setAlphaF(0.92);
        const int half = h / 2 - 3;
        p.fillRect(QRect(0, mid, W, 1), theme::centreLine);
        for (int x = kLeft; x < W; ++x) {
            const double b0 = xToBeat(x), b1 = xToBeat(x + 1);
            const qint64 f0 = qint64(std::floor(b0 * lp->framesPerBeat)), f1 = qint64(std::ceil(b1 * lp->framesPerBeat));
            if (f1 <= 0 || f0 >= lp->frames()) continue;
            float lo, hi;
            lp->range(f0, f1, lo, hi);
            const int y0 = mid - int(hi * half), y1 = mid - int(lo * half);
            p.fillRect(QRect(x, y0, 1, std::max(1, y1 - y0 + 1)), wave);
        }
        p.setPen(theme::record);
        p.drawText(QRect(kLeft, top + 2, W - 2 * kLeft, 16), Qt::AlignLeft | Qt::AlignTop, "● Recording");
    } else {
        p.fillRect(QRect(0, mid, W, 1), theme::centreLine);
        p.setPen(theme::record);
        p.drawText(QRect(kLeft, top, W - 2 * kLeft, h), Qt::AlignCenter, "Sample missing:\n" + QDir::toNativeSeparators(m_clip->samplePath));
    }
    if (xEnd < W) p.fillRect(QRectF(xEnd, top, W - xEnd, h), theme::outOfClip);
    if (x0 > kLeft) p.fillRect(QRectF(kLeft, top, x0 - kLeft, h), theme::outOfClip);

    // the time selection
    if (hasSelection()) {
        const double sx0 = beatToX(m_rangeA), sx1 = beatToX(m_rangeB);
        p.fillRect(QRectF(sx0, top, sx1 - sx0, h), theme::timeSelection);
        p.fillRect(QRectF(sx0, top, 1, h), theme::outline);
        p.fillRect(QRectF(sx1, top, 1, h), theme::outline);
        p.fillRect(QRectF(sx0, kRulerH - 3, sx1 - sx0, 3), theme::outline);
    }

    // warp markers: a handle in the strip, a line down the waveform
    p.setRenderHint(QPainter::Antialiasing, true);
    for (int i = 0; i < m_clip->warpMarkers.size(); ++i) {
        const double x = beatToX(m_clip->warpMarkers[i].beat);
        const QColor c = i == m_sel ? theme::outline : kMarker;
        p.fillRect(QRectF(x, top, 1, h), c);
        QPainterPath hd;
        hd.moveTo(x - 4, kStripY + 1); hd.lineTo(x + 4, kStripY + 1); hd.lineTo(x + 4, kTop - 5); hd.lineTo(x, kTop - 1); hd.lineTo(x - 4, kTop - 5); hd.closeSubpath();
        p.setPen(Qt::NoPen); p.setBrush(c); p.drawPath(hd);
    }
    p.setRenderHint(QPainter::Antialiasing, false);

    // playhead
    const double ph = playheadBeat();
    if (ph >= 0) p.fillRect(QRectF(beatToX(ph), top, 1, h), theme::playhead);

    // name and tempo, top right
    p.setFont(font());
    p.setPen(theme::textFaint);
    const QString source = m_clip->detectedBpm > 0 ? QString("Source %1 BPM").arg(m_clip->sampleBpm(), 0, 'f', 2) : QString("Source BPM unknown");
    const QString info = recording ? QString() : QString("%1   ·   %2   ·   %3")
        .arg(QFileInfo(m_clip->samplePath).fileName())
        .arg(source)
        .arg(!m_clip->warp ? "Warp Off" : warpModeName(m_clip->warpMode));
    p.drawText(QRect(kLeft, top + 2, W - 2 * kLeft, 16), Qt::AlignRight | Qt::AlignTop, info);
}

int SampleEditor::insertMarker(double beat, double seconds) {
    auto& m = m_clip->warpMarkers;
    const auto it = std::upper_bound(m.begin(), m.end(), beat, [](double b, const WarpMarker& x) { return b < x.beat; });
    const int i = int(it - m.begin());
    m.insert(i, {beat, seconds});
    return i;
}

void SampleEditor::mousePressEvent(QMouseEvent* e) {
    if (!m_clip) return;
    const QPoint pos = e->position().toPoint();
    m_pressPos = e->position();
    m_pressBeat = xToBeat(pos.x());
    m_zoom0 = m_zoom;
    m_offset0 = m_offset;
    m_len0 = m_clip->length;
    m_start0 = m_clip->start;
    m_loopStart0 = m_clip->loopStart;
    m_moved = false;
    if (e->button() == Qt::MiddleButton) { m_drag = Drag::Scroll; return; }   // drag the view
    if (e->button() != Qt::LeftButton) return;
    const double xs = beatToX(m_clip->start), xl = beatToX(m_clip->loopStart), xe = beatToX(m_clip->loopEnd());
    const bool free = e->modifiers() & (Qt::AltModifier | Qt::MetaModifier);
    if (pos.y() < kRulerH) {   // the ruler: drag up / down to zoom, sideways to scroll
        m_drag = Drag::Ruler;
        m_userZoomed = true;
        return;
    }
    if (pos.y() < kStripY) {   // the brace row: its ends, then its body; the marker row: ▶ start, ◀ end
        auto open = [this](Drag d, const char* label) { m_drag = d; m_project->beginEdit(label); m_editOpen = true; };
        if (pos.y() < kMarkY) {
            const bool nearL = std::abs(pos.x() - xl) <= kHandle, nearE = std::abs(pos.x() - xe) <= kHandle;
            if (nearE && !(nearL && pos.x() < xl)) open(Drag::LoopEnd, "Clip Length");
            else if (nearL) open(Drag::LoopStart, "Move Loop Start");
            else if (pos.x() > xl && pos.x() < xe) open(Drag::LoopMove, "Move Loop");
        } else {
            if (std::abs(pos.x() - xs) <= kHandle) open(Drag::Start, "Move Clip Start");
            else if (std::abs(pos.x() - xe) <= kHandle) open(Drag::LoopEnd, "Clip Length");
        }
        update();
        return;
    }
    m_sel = markerAt(pos);
    if (m_sel < 0 && m_clip->sample && m_clip->takes.isEmpty()) {   // a transient in the strip: it becomes a marker you're dragging
        if (const int ti = transientAt(pos); ti >= 0) {
            const double sec = m_clip->sample->transients[size_t(ti)];
            const double b = m_clip->beatAt(sec);
            bool have = false;
            for (const WarpMarker& w : m_clip->warpMarkers) if (std::abs(w.beat - b) < kMinGap) have = true;
            if (!have) {
                m_project->beginEdit("Move Warp Marker");
                m_editOpen = true;
                m_sel = insertMarker(b, sec);
                m_hoverTransient = -1;
                m_drag = Drag::Marker;
                emit m_project->clipChanged();
                update();
                return;
            }
        }
    }
    if (m_sel >= 0) {
        m_drag = Drag::Marker;
        m_project->beginEdit("Move Warp Marker");
        m_editOpen = true;
    } else if (pos.y() >= kTop && (e->modifiers() & Qt::ShiftModifier) && hasSelection()) {   // extend the selection
        const double b = snap(m_pressBeat, free);
        m_rangeA = std::min(m_rangeAnchor, b); m_rangeB = std::max(m_rangeAnchor, b);
        m_drag = Drag::Select;
        m_moved = true;
        emit selectionChanged();
    } else if (pos.y() >= kTop && (e->modifiers() & Qt::ShiftModifier)) {   // slide the audio under the grid
        m_drag = Drag::Slide;
        m_slide0.clear();
        for (const WarpMarker& w : m_clip->warpMarkers) m_slide0.push_back(w.beat);
        m_project->beginEdit("Slide Audio");
        m_editOpen = true;
    } else if (pos.y() >= kTop) {   // drag across the waveform: a selection; a click: play from there
        m_drag = Drag::Select;
        m_rangeAnchor = snap(m_pressBeat, free);
        m_rangeA = m_rangeB = m_rangeAnchor;
        m_rangeOn = false;
    }
    update();
}

void SampleEditor::mouseMoveEvent(QMouseEvent* e) {
    const QPoint pos = e->position().toPoint();
    const bool free = e->modifiers() & (Qt::AltModifier | Qt::MetaModifier);
    if (m_clip && m_drag == Drag::Ruler) {
        m_zoom = std::clamp(m_zoom0 * PreferencesDialog::dragZoom(e->position().y() - m_pressPos.y()), 0.05, kMaxZoom);
        m_offset = m_pressBeat - (e->position().x() - kLeft) / m_zoom;   // the beat under the pointer follows it
        clampView();
        update();
        return;
    }
    if (m_clip && m_drag == Drag::Scroll) {
        if (!m_moved && std::abs(e->position().x() - m_pressPos.x()) <= 3) return;
        m_moved = true;
        m_userZoomed = true;
        m_offset = m_offset0 - (e->position().x() - m_pressPos.x()) / m_zoom;
        clampView();
        update();
        return;
    }
    if (m_clip && m_drag == Drag::Select) {
        if (!m_moved && std::abs(e->position().x() - m_pressPos.x()) <= 3) return;
        m_moved = true;
        const double b = snap(xToBeat(pos.x()), free);
        m_rangeA = std::min(m_rangeAnchor, b); m_rangeB = std::max(m_rangeAnchor, b);
        m_rangeOn = true;
        emit selectionChanged();
        update();
        return;
    }
    if (m_clip && m_drag == Drag::Start) {   // ▶ follows the pointer (snapped); the warp map stays
        const double b = std::max(0.0, snap(xToBeat(pos.x()), free));
        if (b != m_clip->start) { m_clip->start = b; emit m_project->clipChanged(); }
        update();
        return;
    }
    if (m_clip && m_drag == Drag::LoopMove) {   // the brace moves along the clip, its length kept
        const double b = std::max(0.0, snap(m_loopStart0 + (xToBeat(pos.x()) - m_pressBeat), free));
        if (b != m_clip->loopStart) { m_clip->loopStart = b; emit m_project->clipChanged(); }
        update();
        return;
    }
    if (m_clip && m_drag == Drag::LoopStart) {   // the brace's left end: the end stays
        const double end = m_loopStart0 + m_len0;
        const double b = std::clamp(snap(xToBeat(pos.x()), free), 0.0, end - 0.25);
        if (b != m_clip->loopStart) { m_clip->loopStart = b; m_clip->length = end - b; emit m_project->clipChanged(); }
        update();
        return;
    }
    if (m_clip && m_drag == Drag::Slide && m_slide0.size() == m_clip->warpMarkers.size()) {
        const double db = snap(xToBeat(pos.x()) - m_pressBeat, free);
        bool changed = false;
        for (int i = 0; i < m_slide0.size(); ++i) {
            const double b = m_slide0[i] + db;
            if (b != m_clip->warpMarkers[i].beat) { m_clip->warpMarkers[i].beat = b; changed = true; }
        }
        if (changed) emit m_project->clipChanged();
        return;
    }
    if (m_clip && m_drag == Drag::Marker && m_sel >= 0 && m_sel < m_clip->warpMarkers.size()) {
        auto& m = m_clip->warpMarkers;
        double b = snap(xToBeat(pos.x()), free);
        if (m_sel > 0) b = std::max(b, m[m_sel - 1].beat + kMinGap);
        if (m_sel + 1 < m.size()) b = std::min(b, m[m_sel + 1].beat - kMinGap);
        if (b != m[m_sel].beat) { m[m_sel].beat = b; emit m_project->clipChanged(); }
        return;
    }
    if (m_clip && m_drag == Drag::LoopEnd) {   // ◀ (or the brace's right end): the length
        const double len = std::clamp(snap(xToBeat(pos.x()), free) - m_clip->loopStart, 0.25, 1024.0);
        if (len != m_clip->length) { m_clip->length = len; emit m_project->clipChanged(); }
        update();
        return;
    }
    if (m_drag != Drag::None) return;
    const int hover = markerAt(pos) >= 0 ? -1 : transientAt(pos);
    if (hover != m_hoverTransient) { m_hoverTransient = hover; update(); }
    Qt::CursorShape cur = Qt::ArrowCursor;
    if (m_clip) {
        const double xs = beatToX(m_clip->start), xl = beatToX(m_clip->loopStart), xe = beatToX(m_clip->loopEnd());
        if (pos.y() < kRulerH) cur = Qt::SizeVerCursor;
        else if (pos.y() < kMarkY) cur = std::abs(pos.x() - xl) <= kHandle || std::abs(pos.x() - xe) <= kHandle ? Qt::SizeHorCursor
                                       : pos.x() > xl && pos.x() < xe ? Qt::OpenHandCursor : Qt::ArrowCursor;
        else if (pos.y() < kStripY) cur = std::abs(pos.x() - xs) <= kHandle || std::abs(pos.x() - xe) <= kHandle ? Qt::SizeHorCursor : Qt::ArrowCursor;
        else if (markerAt(pos) >= 0 || hover >= 0) cur = Qt::SizeHorCursor;
        else if (pos.y() >= kTop) cur = (e->modifiers() & Qt::ShiftModifier) && !hasSelection() ? Qt::SizeAllCursor : Qt::IBeamCursor;
    }
    setCursor(cur);
}

void SampleEditor::mouseReleaseEvent(QMouseEvent* e) {
    const bool click = (m_drag == Drag::Scroll || m_drag == Drag::Select) && !m_moved && e->button() != Qt::MiddleButton;
    const Drag was = m_drag;
    m_drag = Drag::None;
    m_slide0.clear();
    if (m_editOpen) { m_editOpen = false; m_project->endEdit(); }
    if (click && m_clip) {   // play from where you clicked (the selection goes)
        if (m_rangeOn) { m_rangeOn = false; emit selectionChanged(); }
        m_transport->playClipFrom(m_clip, m_pressBeat);
    }
    if (was == Drag::Select && m_moved && m_rangeB - m_rangeA < 1e-9) m_rangeOn = false;
    update();
}

void SampleEditor::wheelEvent(QWheelEvent* e) {
    if (!m_clip) { e->ignore(); return; }
    m_userZoomed = true;
    if (e->modifiers() & Qt::ControlModifier) {   // zoom around the pointer
        const double b = xToBeat(e->position().x());
        m_zoom = std::clamp(m_zoom * std::exp(e->angleDelta().y() * 0.002), 0.05, kMaxZoom);
        m_offset = b - (e->position().x() - kLeft) / m_zoom;
    } else {
        const int d = e->angleDelta().x() ? e->angleDelta().x() : e->angleDelta().y();
        m_offset -= d / m_zoom;
    }
    clampView();
    update();
    e->accept();
}

void SampleEditor::addMarkerAt(double b) {
    if (!m_clip) return;
    for (const WarpMarker& w : m_clip->warpMarkers) if (std::abs(w.beat - b) < kMinGap) return;   // already one there
    EditScope edit(m_project, "Add Warp Marker");
    m_sel = insertMarker(b, m_clip->secondsAt(b));
    emit m_project->clipChanged();
}

void SampleEditor::mouseDoubleClickEvent(QMouseEvent* e) {
    if (!m_clip) return;
    const QPoint pos = e->position().toPoint();
    if (pos.y() < kStripY) return;
    if (const int i = markerAt(pos); i >= 0) { removeMarker(i); return; }
    const bool free = e->modifiers() & (Qt::AltModifier | Qt::MetaModifier);
    if (const int ti = transientAt(pos); ti >= 0 && m_clip->sample) {   // a transient: the marker goes exactly there
        const double sec = m_clip->sample->transients[size_t(ti)];
        const double b = m_clip->beatAt(sec);
        for (const WarpMarker& w : m_clip->warpMarkers) if (std::abs(w.beat - b) < kMinGap) return;
        EditScope edit(m_project, "Add Warp Marker");
        m_sel = insertMarker(b, sec);
        emit m_project->clipChanged();
        return;
    }
    addMarkerAt(snap(xToBeat(pos.x()), free));
}

void SampleEditor::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) { deleteSelection(); e->accept(); return; }
    if (e->key() == Qt::Key_Escape && m_rangeOn) { m_rangeOn = false; emit selectionChanged(); update(); e->accept(); return; }
    if (e->key() == Qt::Key_A && (e->modifiers() & Qt::ControlModifier)) { selectAll(); e->accept(); return; }
    QWidget::keyPressEvent(e);
}

void SampleEditor::removeMarker(int i) {
    if (!m_clip || i < 0 || i >= m_clip->warpMarkers.size() || m_clip->warpMarkers.size() <= 2) return;
    EditScope edit(m_project, "Delete Warp Marker");
    m_clip->warpMarkers.removeAt(i);
    m_sel = -1;
    emit m_project->clipChanged();
}

QString SampleEditor::editFolder() const { return m_transport->recordFolder(); }

// ---- the selection commands ----------------------------------------------------------------

void SampleEditor::deleteSelection() {
    if (!m_clip) return;
    if (!hasSelection()) { if (m_sel >= 0) removeMarker(m_sel); return; }
    QString error;
    EditScope edit(m_project, "Delete Audio");
    if (!sampleedit::cut(*m_clip, m_rangeA, m_rangeB, editFolder(), &error)) { InfoView::notify("Can't delete", error); return; }
    m_rangeOn = false;
    m_sel = -1;
    emit selectionChanged();
    emit m_project->clipChanged();
    InfoView::notify("Deleted", QString("The stretch is cut out; the audio after it moved earlier. The clip now plays %1 (the original file is untouched).").arg(QFileInfo(m_clip->samplePath).fileName()));
    clampView();
    update();
}

void SampleEditor::cropToSelection() {
    if (!m_clip || !hasSelection()) return;
    QString error;
    EditScope edit(m_project, "Crop Sample");
    if (!sampleedit::crop(*m_clip, m_rangeA, m_rangeB, editFolder(), &error)) { InfoView::notify("Can't crop", error); return; }
    m_rangeOn = false;
    m_sel = -1;
    emit selectionChanged();
    emit m_project->clipChanged();
    InfoView::notify("Cropped", QString("Only the selection is left; it starts at 1.1.1. The clip now plays %1.").arg(QFileInfo(m_clip->samplePath).fileName()));
    m_userZoomed = false;
    fit();
    update();
}

void SampleEditor::loopSelection() {
    if (!m_clip || !hasSelection()) return;
    EditScope edit(m_project, "Loop Selection");
    const double a = m_rangeA, len = std::clamp(m_rangeB - m_rangeA, 0.25, 1024.0);
    for (WarpMarker& w : m_clip->warpMarkers) w.beat -= a;   // the selection's start is 1.1.1
    m_clip->length = len;
    m_clip->loop = true;
    m_clip->start = m_clip->loopStart = 0;
    for (Envelope& env : m_clip->envelopes) for (AutoPoint& pt : env.points) pt.beat = std::max(0.0, pt.beat - a);
    m_offset -= a;
    m_rangeA = 0; m_rangeB = len; m_rangeAnchor = 0;
    emit selectionChanged();
    emit m_project->clipChanged();
    clampView();
    update();
}

// ---- the warp commands --------------------------------------------------------------------

void SampleEditor::setStartHere(double beat) {
    if (!m_clip || m_clip->warpMarkers.size() < 2) return;
    EditScope edit(m_project, "Set 1.1.1 Here");
    bool have = false;
    for (const WarpMarker& w : m_clip->warpMarkers) if (std::abs(w.beat - beat) < kMinGap) { have = true; beat = w.beat; }
    if (!have) insertMarker(beat, m_clip->secondsAt(beat));   // the map stays as it is, with a marker at the new 1.1.1
    for (WarpMarker& w : m_clip->warpMarkers) w.beat -= beat;
    m_sel = -1;
    m_rangeOn = false;
    emit m_project->clipChanged();
}

void SampleEditor::setEndHere(double beat) {
    if (!m_clip) return;
    const double len = std::clamp(beat - m_clip->loopStart, 0.25, 1024.0);
    if (len == m_clip->length) return;
    EditScope edit(m_project, "Set End Here");
    m_clip->length = len;
    emit m_project->clipChanged();
}

void SampleEditor::warpFromHere(double beat, double bpm, bool startHere) {
    if (!m_clip || !m_clip->sample || m_clip->warpMarkers.size() < 2 || bpm <= 0) return;
    EditScope edit(m_project, startHere ? "Warp From Here (Start at 1.1.1)" : "Warp From Here");
    auto& m = m_clip->warpMarkers;
    const double sec = m_clip->secondsAt(beat);
    // keep what's before `beat` (nothing, when it starts here), pin `beat` to its audio, and run straight to the end
    QVector<WarpMarker> kept;
    if (!startHere) for (const WarpMarker& w : m) if (w.beat < beat - kMinGap) kept.push_back(w);
    kept.push_back({beat, sec});
    const double dur = m_clip->sample->seconds();
    if (dur > sec + 1e-6) kept.push_back({beat + (dur - sec) * bpm / 60.0, dur});
    else kept.push_back({beat + 1, sec + 60.0 / bpm});
    m = kept;
    if (startHere) for (WarpMarker& w : m) w.beat -= beat;
    m_sel = -1;
    m_rangeOn = false;
    emit m_project->clipChanged();
}

void SampleEditor::contextMenuEvent(QContextMenuEvent* e) {
    if (!m_clip) return;
    const QPoint pos = e->pos();
    const bool free = e->modifiers() & (Qt::AltModifier | Qt::MetaModifier);
    const int marker = markerAt(pos);
    const double here = marker >= 0 ? m_clip->warpMarkers[marker].beat : snap(xToBeat(pos.x()), free);
    const double own = m_clip->sampleBpm();
    const double found = m_clip->detectedBpm > 0 ? m_clip->detectedBpm : own;
    const bool editable = m_clip->sample && m_clip->takes.isEmpty();
    QMenu menu(this);
    if (hasSelection()) {
        menu.addAction("Delete Selection\tDel", this, [this] { deleteSelection(); })->setEnabled(editable);
        menu.addAction("Crop to Selection", this, [this] { cropToSelection(); })->setEnabled(editable);
        menu.addAction("Loop Selection\tCtrl+L", this, [this] { loopSelection(); });
        menu.addAction("Deselect\tEsc", this, [this] { m_rangeOn = false; emit selectionChanged(); update(); });
    } else {
        menu.addAction("Select All\tCtrl+A", this, [this] { selectAll(); });
    }
    menu.addSeparator();
    if (marker >= 0) menu.addAction("Delete Warp Marker", this, [this, marker] { removeMarker(marker); })->setEnabled(m_clip->warpMarkers.size() > 2);
    else menu.addAction("Add Warp Marker", this, [this, here] { addMarkerAt(here); });
    menu.addSeparator();
    menu.addAction("Set 1.1.1 Here", this, [this, here] { setStartHere(here); });
    menu.addAction("Set End Here", this, [this, here] { setEndHere(here); })->setEnabled(here > m_clip->loopStart + 0.25 - 1e-9);
    menu.addAction("Warp From Here", this, [this, here, found] { warpFromHere(here, found, false); });
    menu.addAction("Warp From Here (Start at 1.1.1)", this, [this, here, found] { warpFromHere(here, found, true); });
    menu.addAction("Warp From Here (Straight)", this, [this, here, own] { warpFromHere(here, own, false); });
    menu.addAction(QString("Warp %1 BPM From Here").arg(m_project->tempo, 0, 'f', m_project->tempo == std::floor(m_project->tempo) ? 0 : 2), this,
                   [this, here] { warpFromHere(here, m_project->tempo, false); });
    menu.addSeparator();
    menu.addAction("Reset Warp Markers (Sync to Set)", this, [this] { m_project->syncClipToTempo(m_clip); m_userZoomed = false; fit(); update(); })
        ->setEnabled(m_clip->sample && m_clip->takes.isEmpty());
    menu.addAction(m_clip->reverse ? "Play Forwards" : "Reverse", this, [this] {
        EditScope edit(m_project, "Reverse");
        m_clip->reverse = !m_clip->reverse;
        emit m_project->clipChanged();
    })->setEnabled(m_clip->takes.isEmpty());
    menu.addAction(m_clip->loop ? "Loop Off" : "Loop On", this, [this] { EditScope edit(m_project, m_clip->loop ? "Loop Off" : "Loop On"); m_clip->loop = !m_clip->loop; emit m_project->clipChanged(); });
    menu.addSeparator();
    menu.addAction("Save Clip to User Library…", this, [this] { emit commandRequested("saveClip"); });
    menu.addAction("Show Sample in File Manager", this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_clip->samplePath).absolutePath())); })
        ->setEnabled(QFileInfo::exists(m_clip->samplePath));
    menu.exec(e->globalPos());
}

} // namespace sessions
