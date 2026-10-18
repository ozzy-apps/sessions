#include "PianoRoll.h"
#include <QHash>
#include "InfoView.h"
#include "GridShade.h"
#include "Preferences.h"
#include "core/Model.h"
#include "core/DeviceSpecs.h"
#include "core/Transport.h"
#include "core/Theme.h"
#include "core/Scales.h"
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <algorithm>
#include <cmath>

namespace sessions {

static bool isBlack(int p) { int k = ((p % 12) + 12) % 12; return k == 1 || k == 3 || k == 6 || k == 8 || k == 10; }

PianoRoll::PianoRoll(Project* project, Transport* transport, QWidget* parent)
    : QWidget(parent), m_project(project), m_transport(transport)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumHeight(120);
    InfoView::attach(this, "MIDI Note Editor", "Double-click to add or delete notes. Drag to move, drag a note's right edge to lengthen, drag empty space to box-select. Alt ignores the grid. Drag the ruler to zoom, wheel scrolls pitch. Drag stems in the Vel lane.");
    connect(transport, &Transport::tick, this, [this] {
        followPlayhead();
        if (m_transport->playing() && m_clip) update();
    });
    connect(project, &Project::restored, this, [this] {   // undo replaced the notes: indices are stale
        m_selected.clear();
        m_orig.clear();
        m_drag = Drag::None;
        rebuildRows();
        update();
    });
    connect(project, &Project::clipChanged, this, [this] { if (m_fold && m_drag == Drag::None) rebuildRows(); });
    rebuildRows();
}

void PianoRoll::rebuildRows() {
    const int keep = pitchAtRow(0);
    m_rowPitches.clear();
    if (m_fold && m_clip) {
        if (m_clip->scaleOn) {
            for (int p = 127; p >= 0; --p) if (inScale(p, m_clip->scaleRoot, m_clip->scale)) m_rowPitches.push_back(p);
        } else {
            QSet<int> used;
            for (const Note& n : m_clip->notes) used.insert(n.pitch);
            for (int p = 127; p >= 0; --p) if (used.contains(p)) m_rowPitches.push_back(p);
        }
    }
    if (m_rowPitches.isEmpty()) for (int p = 127; p >= 0; --p) m_rowPitches.push_back(p);
    // keep the same pitch at the top if it still has a row
    int idx = keep >= 0 ? rowOfPitch(keep) : -1;
    m_topRow = std::clamp(idx >= 0 ? idx : m_topRow, 0, std::max(0, int(m_rowPitches.size()) - rows()));
    update();
}

int PianoRoll::rowOfPitch(int pitch) const { return int(m_rowPitches.indexOf(pitch)); }

int PianoRoll::pitchAtRow(int r) const {
    const int i = m_topRow + r;
    return r >= 0 && i >= 0 && i < m_rowPitches.size() ? m_rowPitches[i] : -1;
}

void PianoRoll::setFold(bool on) {
    if (on == m_fold) return;
    m_fold = on;
    rebuildRows();
    m_needCenter = true;
    update();
}

void PianoRoll::refreshRows() { rebuildRows(); update(); }

QList<int> PianoRoll::selectedNotes() const {
    QList<int> out = m_selected.values();
    std::sort(out.begin(), out.end());
    return out;
}

int PianoRoll::ownerTrack() const {
    int t = m_project->trackOfClip(m_clip);
    if (t < 0 && m_project->selectedTrack < m_project->tracks.size()) t = m_project->selectedTrack;
    return t;
}

void PianoRoll::preview(int pitch, double velocity) {
    m_lastPreviewPitch = pitch;
    m_transport->previewNote(ownerTrack(), pitch, velocity);
}

void PianoRoll::openEdit(const QString& label) {
    if (m_editOpen) return;
    m_project->beginEdit(label);
    m_editOpen = true;
}

void PianoRoll::closeEdit() {
    if (!m_editOpen) return;
    m_editOpen = false;
    m_project->endEdit();
}

void PianoRoll::setClip(ClipPtr clip) {
    if (clip == m_clip) { update(); return; }
    m_clip = clip;
    m_selected.clear();
    m_offset = 0;
    m_needCenter = true;
    m_userZoomed = false;
    rebuildRows();
    fitToClip();
    update();
}

double PianoRoll::clipExtent() const { return m_clip ? std::max({m_clip->length, m_clip->loopEnd(), m_clip->start + 1}) : 4.0; }

void PianoRoll::fitToClip() {
    if (!m_clip || m_userZoomed || width() <= m_keyW + 40) return;
    m_offset = 0;
    m_zoom = std::clamp((width() - m_keyW - 12) / std::max(1.0, clipExtent()), 1.0, 400.0);
}

void PianoRoll::resizeEvent(QResizeEvent*) {
    fitToClip();
    m_topRow = std::clamp(m_topRow, 0, std::max(0, int(m_rowPitches.size()) - rows()));
}

bool PianoRoll::viewport(double& first, double& last) const {
    if (!m_clip) return false;
    first = m_offset;
    last = xToBeat(width());
    return last > first;
}

void PianoRoll::setViewport(double first, double last) {
    if (!m_clip || !(last > first)) return;
    m_userZoomed = true;
    m_zoom = std::clamp((width() - m_keyW) / (last - first), 1.0, 400.0);
    m_offset = first;
    clampView();
    update();
}

void PianoRoll::setFollow(bool on) {
    m_follow = on;
    followPlayhead();
    update();
}

void PianoRoll::followPlayhead() {
    if (!m_follow || !m_clip || !m_transport->playing() || m_drag != Drag::None || width() <= m_keyW) return;
    const double beat = m_project->clipPosition(m_clip, m_transport->beat());
    if (beat < 0) return;
    const double span = (width() - m_keyW) / m_zoom;
    if (beat < m_offset || beat >= m_offset + span - std::min(12.0 / m_zoom, span * 0.1)) {
        // Keep the start of the pattern in view after a loop returns to its first page.
        m_offset = beat < m_offset && beat < span * 0.9 ? 0.0 : beat - span * 0.08;
        clampView();
    }
}

void PianoRoll::clampView() {
    if (!m_clip) return;
    const double visible = std::max(0.0, (width() - m_keyW) / m_zoom);
    const double end = std::max(clipExtent(), 1.0) + 1;   // a beat past the clip's end
    const double minZoom = std::clamp((width() - m_keyW) / (end + 2), 1.0, 6.0);   // zooming out stops when the whole clip is on screen
    if (m_zoom < minZoom) m_zoom = minZoom;
    m_offset = std::clamp(m_offset, 0.0, std::max(0.0, end - visible));
}

void PianoRoll::centerOnNotes() {
    m_needCenter = false;
    int mid = 60;
    if (m_clip && !m_clip->notes.isEmpty()) {
        int lo = 127, hi = 0;
        for (const Note& n : m_clip->notes) { lo = std::min(lo, n.pitch); hi = std::max(hi, n.pitch); }
        mid = (lo + hi) / 2;
    }
    int idx = 0;   // the row nearest to the middle pitch
    for (int i = 0; i < m_rowPitches.size(); ++i) if (m_rowPitches[i] >= mid) idx = i;
    m_topRow = std::clamp(idx - rows() / 2, 0, std::max(0, int(m_rowPitches.size()) - rows()));
}

void PianoRoll::setGridSize(double g) { m_grid = std::clamp(g, 1.0 / 32, 4.0); emit gridChanged(m_grid); update(); }

int PianoRoll::rows() const { return std::max(1, (height() - m_rulerH - m_velH - 2) / m_rowH); }
double PianoRoll::pitchToY(int p) const {
    const int idx = rowOfPitch(p);
    return idx < 0 ? -10000.0 : m_rulerH + (idx - m_topRow) * m_rowH;
}
double PianoRoll::snap(double b, bool fine) const { return fine ? b : std::round(b / m_grid) * m_grid; }

PianoRoll::Hit PianoRoll::hitTest(const QPointF& p) const {
    Hit h;
    h.beat = xToBeat(p.x());
    if (p.y() < m_rulerH) { h.area = Hit::Ruler; return h; }
    if (p.y() >= height() - m_velH) { h.area = Hit::Velocity; return h; }
    h.pitch = pitchAtRow(int((p.y() - m_rulerH) / m_rowH));
    if (p.x() < m_keyW) { h.area = Hit::Keys; return h; }
    h.area = Hit::Grid;
    if (m_clip && h.pitch >= 0)
        for (int i = m_clip->notes.size() - 1; i >= 0; --i) {
            const Note& n = m_clip->notes[i];
            if (n.pitch == h.pitch && h.beat >= n.start && h.beat < n.start + n.length) {
                h.noteIndex = i;
                h.edge = beatToX(n.start + n.length) - p.x() < 6;
                break;
            }
        }
    return h;
}

// The ruler's top 5 px hold the brace (its ends within 5 px, else its body), the 8 px under them the
// ▶ start and ◀ end markers (within 6 px); anywhere else on the ruler zooms and scrolls.
PianoRoll::RulerHit PianoRoll::rulerHit(const QPointF& p) const {
    if (!m_clip || p.y() >= m_rulerH || p.x() < m_keyW) return RulerHit::None;
    const double xs = beatToX(m_clip->start), xl = beatToX(m_clip->loopStart), xe = beatToX(m_clip->loopEnd());
    if (p.y() < 5) {
        const bool nearL = std::abs(p.x() - xl) <= 5, nearE = std::abs(p.x() - xe) <= 5;
        if (nearE && !(nearL && p.x() < xl)) return RulerHit::LoopEnd;
        if (nearL) return RulerHit::LoopStart;
        if (p.x() > xl && p.x() < xe) return RulerHit::LoopBody;
        return RulerHit::None;
    }
    if (p.y() < 13) {
        if (std::abs(p.x() - xs) <= 6) return RulerHit::Start;
        if (std::abs(p.x() - xe) <= 6) return RulerHit::LoopEnd;
    }
    return RulerHit::None;
}

void PianoRoll::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    const int W = width(), H = height();
    p.fillRect(rect(), theme::rollBg);
    if (!m_clip) {
        p.setPen(theme::faint);
        p.drawText(rect(), Qt::AlignCenter, "Select a clip, or double-click an empty slot to create one");
        return;
    }
    if (m_needCenter) centerOnNotes();
    const int R = rows();
    const int gridH = R * m_rowH;
    const int owner = ownerTrack();
    const QColor col = owner >= 0 ? m_project->clipColor(m_clip) : theme::accent;
    const QColor marker = theme::text;
    QFont small = font(); small.setPointSizeF(7);

    // rows + keys; with a scale, in-scale rows are lit and the root is marked
    const bool scale = m_clip->scaleOn;
    const Device* rack = nullptr;   // a Drum Rack on the track: its pads' names on their rows (a pad's instrument's, when it has one)
    if (owner >= 0) for (const Device& d : m_project->tracks[owner].devices) if (!rack && deviceType(d.name) == DeviceType::DrumRack) rack = &d;
    QHash<int, QString> padInstruments;
    if (rack) for (const Device& d : m_project->tracks[owner].devices) if (d.rack == rack->id && d.kind == DeviceKind::Instrument) padInstruments.insert(d.chain, d.name);
    for (int r = 0; r < R; ++r) {
        const int pitch = pitchAtRow(r);
        const int y = m_rulerH + r * m_rowH;
        if (pitch < 0) { p.fillRect(QRect(0, y, W, m_rowH), theme::rollEmpty); continue; }
        const bool bk = isBlack(pitch);
        QColor row = bk ? theme::gridDark : theme::grid;
        if (scale) {
            const bool in = inScale(pitch, m_clip->scaleRoot, m_clip->scale);
            row = in ? theme::rollInScale : theme::rollOutScale;
            if (in && ((pitch - m_clip->scaleRoot) % 12 + 12) % 12 == 0) row = theme::rollRoot;
        }
        p.fillRect(QRect(m_keyW, y, W, m_rowH), row);
        p.fillRect(QRect(0, y, m_keyW - 1, m_rowH - 1), bk ? theme::blackKey : theme::whiteKey);
        const int pad = pitch - drumrack::kFirstNote;
        const bool padRow = rack && pad >= 0 && pad < rack->pads.size() && (!rack->pads[pad].samplePath.isEmpty() || padInstruments.contains(pad));
        const bool label = m_fold ? true : pitch % 12 == 0 || padRow;
        if (label) {
            p.setFont(small);
            p.setPen(bk ? theme::blackKeyText : theme::whiteKeyText);
            p.drawText(2, y + m_rowH - 2, QString(noteName(pitch)) + QString::number(pitch / 12 - 2));
        }
        if (padRow) {
            p.setFont(small);
            p.setPen(theme::rollPadText);
            p.drawText(m_keyW + 4, y + m_rowH - 2, padInstruments.value(pad, rack->pads[pad].name));
        }
    }
    // every second beat a shade lighter, then past the clip's end
    gridshade::paint(p, m_zoom, [this](double b) { return beatToX(b); }, m_offset, xToBeat(W), QRectF(m_keyW, m_rulerH, W - m_keyW, gridH));
    const double endX = beatToX(m_clip->loopEnd());   // past the end marker, and before whichever of the start marker and the loop comes first
    if (endX < W) p.fillRect(QRectF(std::max(endX, double(m_keyW)), m_rulerH, W, gridH), theme::rollBeyond);
    const double firstX = beatToX(std::min(m_clip->start, m_clip->loopStart));
    if (firstX > m_keyW) p.fillRect(QRectF(m_keyW, m_rulerH, firstX - m_keyW, gridH), theme::rollBeyond);

    // grid lines
    double step = m_grid * m_zoom < 5 ? 1 : m_grid;
    double b1 = xToBeat(W);
    for (double b = std::ceil(m_offset / step) * step; b <= b1; b += step) {
        int x = int(beatToX(b));
        bool bar = std::abs(std::fmod(b, 4.0)) < 1e-6, beat = std::abs(std::fmod(b, 1.0)) < 1e-6;
        p.fillRect(QRect(x, m_rulerH, 1, gridH), bar ? theme::rollBar : beat ? theme::rollBeat : theme::rollSub);
    }

    // notes
    p.save();
    p.setClipRect(QRect(m_keyW, m_rulerH, W - m_keyW, gridH));
    for (int i = 0; i < m_clip->notes.size(); ++i) {
        const Note& n = m_clip->notes[i];
        double x = beatToX(n.start), w = std::max(3.0, n.length * m_zoom - 1);
        double y = pitchToY(n.pitch);
        if (y < -1000) continue;   // folded away
        bool sel = m_selected.contains(i);
        QColor c = sel ? theme::noteSelected : col;
        c.setAlphaF(0.4 + 0.6 * n.velocity);
        p.fillRect(QRectF(x, y, w, m_rowH - 1), c);
        p.setPen(sel ? theme::outline : theme::noteEdge);
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(x + 0.5, y + 0.5, w - 1, m_rowH - 2));
    }
    // the start marker and the end marker, as lines down the grid
    const double xs = beatToX(m_clip->start), xl = beatToX(m_clip->loopStart);
    p.fillRect(QRectF(xs, m_rulerH, 1, gridH), theme::withAlpha(marker, 140));
    p.fillRect(QRectF(endX, m_rulerH, 1, gridH), theme::withAlpha(marker, 140));
    // playhead inside the clip (the start marker and the brace: Project::clipPosition)
    if (owner >= 0 && m_transport->playing()) {
        const double pos = m_project->clipPosition(m_clip, m_transport->beat());
        if (pos >= 0) p.fillRect(QRectF(beatToX(pos), m_rulerH, 1, gridH), theme::playhead);
    }
    if (m_drag == Drag::Box) { p.setPen(QPen(theme::boxSelect, 1, Qt::DashLine)); p.setBrush(Qt::NoBrush); p.drawRect(m_box.normalized()); }
    p.restore();

    // ruler: the neutral loop brace on top (handles at its ends; faded with Loop off), the bar numbers,
    // then the ▶ start and ◀ end markers over them
    p.fillRect(QRect(0, 0, W, m_rulerH), theme::ruler);
    p.setPen(theme::rollRulerText); p.setFont(small);
    for (int b = int(std::ceil(m_offset / 4)) * 4; b <= b1; b += 4) {
        int x = int(beatToX(b));
        p.fillRect(QRect(x, m_rulerH - 4, 1, 4), theme::rollRulerText);
        p.drawText(x + 3, 11, QString::number(b / 4 + 1));
    }
    {
        const QColor brace = m_clip->loop ? marker : theme::withAlpha(marker, 115);
        const double l0 = std::max(double(m_keyW), xl);
        p.fillRect(QRectF(l0, 0, std::max(0.0, endX - l0), 3), brace);
        p.fillRect(QRectF(xl, 0, 2, 5), marker);
        p.fillRect(QRectF(endX - 2, 0, 2, 5), marker);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen); p.setBrush(marker);
        QPolygonF startTri; startTri << QPointF(xs, 5) << QPointF(xs + 7, 9) << QPointF(xs, 13);
        QPolygonF endTri; endTri << QPointF(endX, 5) << QPointF(endX - 7, 9) << QPointF(endX, 13);
        p.drawPolygon(startTri);
        p.drawPolygon(endTri);
        p.setRenderHint(QPainter::Antialiasing, false);
    }
    p.fillRect(QRect(0, 0, m_keyW, m_rulerH), theme::ruler);

    // velocity lane
    int vy = H - m_velH;
    p.fillRect(QRect(0, vy - 2, W, m_velH + 2), theme::velLane);
    p.fillRect(QRect(0, vy - 2, W, 1), theme::velLine);
    p.setPen(theme::faint); p.drawText(4, vy + 11, "Vel");
    p.setRenderHint(QPainter::Antialiasing, true);
    for (int i = 0; i < m_clip->notes.size(); ++i) {
        const Note& n = m_clip->notes[i];
        double x = std::round(beatToX(n.start)) + 0.5;
        if (x < m_keyW) continue;
        double y = vy + m_velH - 4 - n.velocity * (m_velH - 12);
        QColor c = m_selected.contains(i) ? theme::noteSelected : col;
        p.setPen(c); p.setBrush(c);
        p.drawLine(QPointF(x, vy + m_velH - 2), QPointF(x, y));
        p.drawEllipse(QPointF(x, y), 2.5, 2.5);
    }
}

void PianoRoll::addNote(int pitch, double beat) {
    Note n;
    n.pitch = std::clamp(pitch, 0, 127);
    n.start = std::max(0.0, std::floor(beat / m_grid) * m_grid);
    n.length = m_grid;
    n.velocity = 0.8;
    m_clip->notes.push_back(n);
    m_selected = {int(m_clip->notes.size() - 1)};
    preview(n.pitch, n.velocity);
}

void PianoRoll::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || !m_clip) return;
    setFocus();
    QPointF pos = e->position();
    Hit h = hitTest(pos);
    m_pressPos = pos; m_pressBeat = h.beat; m_pressPitch = h.pitch;

    switch (h.area) {
    case Hit::Ruler: {   // the markers and the brace first, else zoom / scroll
        const RulerHit rh = rulerHit(pos);
        m_start0 = m_clip->start; m_loopStart0 = m_clip->loopStart; m_len0 = m_clip->length;
        if (rh == RulerHit::Start) { m_drag = Drag::Start; openEdit("Move Clip Start"); }
        else if (rh == RulerHit::LoopEnd) { m_drag = Drag::LoopEnd; openEdit("Clip Length"); }
        else if (rh == RulerHit::LoopStart) { m_drag = Drag::LoopStart; openEdit("Move Loop Start"); }
        else if (rh == RulerHit::LoopBody) { m_drag = Drag::LoopMove; openEdit("Move Loop"); }
        else { m_drag = Drag::Ruler; m_zoom0 = m_zoom; m_userZoomed = true; }
        break;
    }
    case Hit::Keys:
        if (h.pitch < 0) break;
        m_selected.clear();
        for (int i = 0; i < m_clip->notes.size(); ++i) if (m_clip->notes[i].pitch == h.pitch) m_selected.insert(i);
        preview(h.pitch);
        update();
        break;
    case Hit::Velocity: {
        // nearest note by start beat
        int best = -1; double bd = 6.0 / m_zoom;
        for (int i = 0; i < m_clip->notes.size(); ++i) { double d = std::abs(m_clip->notes[i].start - h.beat); if (d < bd) { bd = d; best = i; } }
        if (best < 0) break;
        if (!m_selected.contains(best)) m_selected = {best};
        m_velAnchor = best;
        openEdit("Change Velocity");
        m_orig.clear();
        for (int i : m_selected) m_orig[i] = {m_clip->notes[i].pitch, m_clip->notes[i].start, m_clip->notes[i].length, m_clip->notes[i].velocity};
        m_drag = Drag::Velocity;
        mouseMoveEvent(e);
        break;
    }
    case Hit::Grid:
        if (h.pitch < 0) break;
        if (m_draw) {
            openEdit("Draw Notes");
            if (h.noteIndex >= 0) { m_clip->notes.removeAt(h.noteIndex); m_selected.clear(); }
            else { addNote(h.pitch, h.beat); m_lastPaintPitch = h.pitch; m_lastPaintBeat = std::floor(h.beat / m_grid) * m_grid; m_drag = Drag::Paint; }
            changed();
        } else if (h.noteIndex >= 0) {
            if (e->modifiers() & Qt::ShiftModifier) { if (m_selected.contains(h.noteIndex)) m_selected.remove(h.noteIndex); else m_selected.insert(h.noteIndex); }
            else if (!m_selected.contains(h.noteIndex)) m_selected = {h.noteIndex};
            m_orig.clear();
            for (int i : m_selected) m_orig[i] = {m_clip->notes[i].pitch, m_clip->notes[i].start, m_clip->notes[i].length, m_clip->notes[i].velocity};
            m_drag = h.edge ? Drag::Resize : Drag::Move;
            openEdit(h.edge ? "Resize Notes" : "Move Notes");
            preview(m_clip->notes[h.noteIndex].pitch, m_clip->notes[h.noteIndex].velocity);
            update();
        } else {
            if (!(e->modifiers() & Qt::ShiftModifier)) m_selected.clear();
            m_drag = Drag::Box; m_box = QRectF(pos, pos);
            update();
        }
        break;
    }
}

void PianoRoll::mouseMoveEvent(QMouseEvent* e) {
    QPointF pos = e->position();
    if (m_drag == Drag::None) {
        if (!m_clip) return;
        Hit h = hitTest(pos);
        Qt::CursorShape c = Qt::ArrowCursor;
        if (h.area == Hit::Ruler) { const RulerHit rh = rulerHit(pos); c = rh == RulerHit::None ? Qt::SizeVerCursor : rh == RulerHit::LoopBody ? Qt::OpenHandCursor : Qt::SizeHorCursor; }
        else if (h.area == Hit::Velocity) c = Qt::SizeVerCursor;
        else if (h.area == Hit::Keys) c = Qt::PointingHandCursor;
        else if (m_draw) c = Qt::CrossCursor;
        else if (h.noteIndex >= 0) c = h.edge ? Qt::SizeHorCursor : Qt::SizeAllCursor;
        setCursor(c);
        return;
    }
    bool fine = e->modifiers() & (Qt::AltModifier | Qt::MetaModifier);
    switch (m_drag) {
    case Drag::Ruler:
        m_zoom = std::clamp(m_zoom0 * PreferencesDialog::dragZoom(pos.y() - m_pressPos.y()), 1.0, 400.0);
        m_offset = std::max(0.0, m_pressBeat - (pos.x() - m_keyW) / m_zoom);
        clampView();
        break;
    case Drag::Move: {
        double db = (pos.x() - m_pressPos.x()) / m_zoom;
        int dRow = int(std::round((m_pressPos.y() - pos.y()) / m_rowH));   // rows, so Fold moves along the scale
        for (auto it = m_orig.begin(); it != m_orig.end(); ++it) {
            Note& n = m_clip->notes[it.key()];
            n.start = std::max(0.0, snap(it->start + db, fine));
            const int row = rowOfPitch(it->pitch);
            n.pitch = row >= 0 ? m_rowPitches[std::clamp(row - dRow, 0, int(m_rowPitches.size()) - 1)] : std::clamp(it->pitch + dRow, 0, 127);
        }
        if (m_orig.size() == 1 && m_clip->notes[m_orig.begin().key()].pitch != m_lastPreviewPitch)
            preview(m_clip->notes[m_orig.begin().key()].pitch);   // hear the new pitch while dragging
        emit notesChanged();
        break;
    }
    case Drag::Resize: {
        double db = (pos.x() - m_pressPos.x()) / m_zoom;
        for (auto it = m_orig.begin(); it != m_orig.end(); ++it)
            m_clip->notes[it.key()].length = std::max(fine ? 0.05 : m_grid, snap(it->length + db, fine));
        emit notesChanged();
        break;
    }
    case Drag::Box: m_box.setBottomRight(pos); break;
    case Drag::Velocity: {
        double v = std::clamp((height() - 4 - pos.y()) / (m_velH - 12.0), 0.05, 1.0);
        double d = v - m_orig[m_velAnchor].vel;
        for (auto it = m_orig.begin(); it != m_orig.end(); ++it) m_clip->notes[it.key()].velocity = std::clamp(it->vel + d, 0.05, 1.0);
        emit notesChanged();
        break;
    }
    case Drag::Paint: {
        Hit h = hitTest(pos);
        double b = std::floor(h.beat / m_grid) * m_grid;
        if (h.area == Hit::Grid && h.pitch >= 0 && (b != m_lastPaintBeat || h.pitch != m_lastPaintPitch) && h.noteIndex < 0) {
            addNote(h.pitch, b); m_lastPaintBeat = b; m_lastPaintPitch = h.pitch; emit notesChanged();
        }
        break;
    }
    // the markers and the brace (Clip::start / loopStart / length), on the grid; Alt free
    case Drag::Start: {
        const double b = std::max(0.0, snap(m_start0 + (pos.x() - m_pressPos.x()) / m_zoom, fine));
        if (b != m_clip->start) { m_clip->start = b; emit m_project->clipChanged(); }
        break;
    }
    case Drag::LoopMove: {
        const double b = std::max(0.0, snap(m_loopStart0 + (pos.x() - m_pressPos.x()) / m_zoom, fine));
        if (b != m_clip->loopStart) { m_clip->loopStart = b; emit m_project->clipChanged(); }
        break;
    }
    case Drag::LoopStart: {   // the end stays
        const double end = m_loopStart0 + m_len0;
        const double b = std::clamp(snap(m_loopStart0 + (pos.x() - m_pressPos.x()) / m_zoom, fine), 0.0, end - std::max(m_grid, 0.25));
        if (b != m_clip->loopStart) { m_clip->loopStart = b; m_clip->length = end - b; emit m_project->clipChanged(); }
        break;
    }
    case Drag::LoopEnd: {
        const double len = std::clamp(snap(m_loopStart0 + m_len0 + (pos.x() - m_pressPos.x()) / m_zoom, fine) - m_clip->loopStart, std::max(m_grid, 0.25), 256.0);
        if (len != m_clip->length) { m_clip->length = len; emit m_project->clipChanged(); }
        break;
    }
    default: break;
    }
    update();
}

void PianoRoll::mouseReleaseEvent(QMouseEvent*) {
    if (m_drag == Drag::Box && m_clip) {
        QRectF b = m_box.normalized();
        double b0 = xToBeat(b.left()), b1 = xToBeat(b.right());
        QSet<int> pitches;
        for (int r = int((b.top() - m_rulerH) / m_rowH); r <= int((b.bottom() - m_rulerH) / m_rowH); ++r)
            if (pitchAtRow(r) >= 0) pitches.insert(pitchAtRow(r));
        for (int i = 0; i < m_clip->notes.size(); ++i) {
            const Note& n = m_clip->notes[i];
            if (pitches.contains(n.pitch) && n.start < b1 && n.start + n.length > b0) m_selected.insert(i);
        }
    }
    m_drag = Drag::None;
    closeEdit();
    update();
}

void PianoRoll::mouseDoubleClickEvent(QMouseEvent* e) {
    if (!m_clip || m_draw) return;
    Hit h = hitTest(e->position());
    if (h.area != Hit::Grid || h.pitch < 0) return;
    closeEdit();
    EditScope edit(m_project, h.noteIndex >= 0 ? "Delete Note" : "Add Note");
    if (h.noteIndex >= 0) { m_clip->notes.removeAt(h.noteIndex); m_selected.clear(); }
    else addNote(h.pitch, h.beat);
    m_drag = Drag::None;
    changed();
}

void PianoRoll::wheelEvent(QWheelEvent* e) {
    if (!m_clip) return;
    QPointF pos = e->position();
    if (e->modifiers() & Qt::ControlModifier) {
        m_userZoomed = true;
        double b = xToBeat(pos.x());
        m_zoom = std::clamp(m_zoom * std::exp(e->angleDelta().y() * 0.002), 1.0, 400.0);
        m_offset = std::max(0.0, b - (pos.x() - m_keyW) / m_zoom);   // zooms around the pointer
        clampView();
    } else if ((e->modifiers() & Qt::ShiftModifier) || std::abs(e->angleDelta().x()) > std::abs(e->angleDelta().y())) {
        int d = e->angleDelta().x() ? e->angleDelta().x() : e->angleDelta().y();
        m_offset = std::max(0.0, m_offset - d / m_zoom);
        clampView();
    } else {
        m_topRow = std::clamp(m_topRow + (e->angleDelta().y() > 0 ? -2 : 2), 0, std::max(0, int(m_rowPitches.size()) - rows()));
    }
    update();
    e->accept();
}

void PianoRoll::deleteSelected() {
    if (!m_clip || m_selected.isEmpty()) return;
    EditScope edit(m_project, "Delete Notes");
    QList<int> idx = m_selected.values();
    std::sort(idx.begin(), idx.end(), std::greater<int>());
    for (int i : idx) m_clip->notes.removeAt(i);
    m_selected.clear();
    changed();
}

void PianoRoll::duplicateSelected() {
    if (!m_clip || m_selected.isEmpty()) return;
    EditScope edit(m_project, "Duplicate Notes");
    double a0 = 1e9, a1 = 0;
    for (int i : m_selected) { a0 = std::min(a0, m_clip->notes[i].start); a1 = std::max(a1, m_clip->notes[i].start + m_clip->notes[i].length); }
    QSet<int> fresh;
    for (int i : m_selected) { Note n = m_clip->notes[i]; n.start += a1 - a0; m_clip->notes.push_back(n); fresh.insert(m_clip->notes.size() - 1); }
    m_selected = fresh;
    changed();
}

void PianoRoll::selectAll() {
    if (!m_clip) return;
    for (int i = 0; i < m_clip->notes.size(); ++i) m_selected.insert(i);
    update();
}

void PianoRoll::nudge(int dPitch, double dBeat, bool length) {
    if (!m_clip || m_selected.isEmpty()) return;
    EditScope edit(m_project, length ? "Change Note Length" : "Move Notes");
    for (int i : m_selected) {
        Note& n = m_clip->notes[i];
        n.pitch = std::clamp(n.pitch + dPitch, 0, 127);
        if (length) n.length = std::max(m_grid, n.length + dBeat);
        else n.start = std::max(0.0, n.start + dBeat);
    }
    changed();
}

// Right-click (0.22): the note commands on the selection (or the note under the pointer), and the clip's.
void PianoRoll::contextMenuEvent(QContextMenuEvent* e) {
    if (!m_clip) return;
    const Hit h = hitTest(e->pos());
    if (h.noteIndex >= 0 && !m_selected.contains(h.noteIndex)) { m_selected = {h.noteIndex}; update(); }
    const bool any = !m_selected.isEmpty();
    QMenu menu(this);
    if (h.area == Hit::Grid && h.noteIndex < 0) {
        menu.addAction("Add Note Here", this, [this, h] { addNote(h.pitch, snap(h.beat, false)); });
        menu.addSeparator();
    }
    menu.addAction("Cut\tCtrl+X", this, [this] { emit commandRequested("cut"); })->setEnabled(any);
    menu.addAction("Copy\tCtrl+C", this, [this] { emit commandRequested("copy"); })->setEnabled(any);
    menu.addAction("Paste\tCtrl+V", this, [this] { emit commandRequested("paste"); });
    menu.addAction("Duplicate\tCtrl+D", this, [this] { duplicateSelected(); })->setEnabled(any);
    menu.addAction("Delete\tDel", this, [this] { deleteSelected(); })->setEnabled(any);
    menu.addSeparator();
    menu.addAction("Select All\tCtrl+A", this, [this] { selectAll(); });
    if (h.area == Hit::Keys) menu.addAction(QString("Select All Notes of This Key"), this, [this, h] {
        m_selected.clear();
        for (int i = 0; i < m_clip->notes.size(); ++i) if (m_clip->notes[i].pitch == h.pitch) m_selected.insert(i);
        update();
    });
    menu.addSeparator();
    menu.addAction("Octave Up\tShift+↑", this, [this] { nudge(12, 0, false); })->setEnabled(any);
    menu.addAction("Octave Down\tShift+↓", this, [this] { nudge(-12, 0, false); })->setEnabled(any);
    menu.addAction("Semitone Up\t↑", this, [this] { nudge(1, 0, false); })->setEnabled(any);
    menu.addAction("Semitone Down\t↓", this, [this] { nudge(-1, 0, false); })->setEnabled(any);
    menu.addAction("Nudge Later\t→", this, [this] { nudge(0, m_grid, false); })->setEnabled(any);
    menu.addAction("Nudge Earlier\t←", this, [this] { nudge(0, -m_grid, false); })->setEnabled(any);
    menu.addSeparator();
    QMenu* grid = menu.addMenu(QString("Grid: 1/%1").arg(int(std::round(4.0 / m_grid))));
    for (double g : {1.0, 0.5, 0.25, 0.125, 1.0 / 16, 1.0 / 32}) {
        QAction* a = grid->addAction(QString("1/%1").arg(int(std::round(4.0 / g))), this, [this, g] { setGridSize(g); });
        a->setCheckable(true); a->setChecked(std::abs(g - m_grid) < 1e-9);
    }
    menu.addAction(m_fold ? "Unfold" : "Fold", this, [this] { setFold(!m_fold); });
    menu.addSeparator();
    menu.addAction("Save Clip to User Library…", this, [this] { emit commandRequested("saveClip"); });
    menu.exec(e->globalPos());
}

void PianoRoll::keyPressEvent(QKeyEvent* e) {
    bool shift = e->modifiers() & Qt::ShiftModifier;
    switch (e->key()) {
    case Qt::Key_Up:    nudge(shift ? 12 : 1, 0, false); break;
    case Qt::Key_Down:  nudge(shift ? -12 : -1, 0, false); break;
    case Qt::Key_Left:  nudge(0, -m_grid, shift); break;
    case Qt::Key_Right: nudge(0, m_grid, shift); break;
    case Qt::Key_Delete: case Qt::Key_Backspace: deleteSelected(); break;
    default: QWidget::keyPressEvent(e); return;
    }
    e->accept();
}

} // namespace sessions
