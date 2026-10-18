#include "SessionView.h"
#include "Icons.h"
#include "Browser.h"
#include "core/Import.h"
#include "MixerStrip.h"
#include "InfoView.h"
#include "InlineEdit.h"
#include "core/Model.h"
#include "core/Transport.h"
#include "core/Theme.h"
#include <QPainter>
#include <QMouseEvent>
#include <QCursor>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QApplication>
#include <QMenu>
#include <QSplitter>
#include <QSettings>
#include <QMessageBox>
#include <QMimeData>
#include <QDragEnterEvent>
#include "DeviceDrag.h"
#include "core/Grooves.h"
#include "core/Presets.h"
#include "InfoView.h"
#include <QFileInfo>
#include <cmath>

namespace sessions {

// ===========================================================================
// ClipGrid
// ===========================================================================

ClipGrid::ClipGrid(Project* project, Transport* transport, QWidget* parent)
    : QWidget(parent), m_project(project), m_transport(transport)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setAcceptDrops(true);
    InfoView::attach(this, "Session View",
        "Click ▶ to launch a clip, ■ to stop a track. Number-row keys 1–9 and 0 launch the first ten scenes. "
        "Drag clips between slots (Ctrl copies), drag track titles to reorder. "
        "Arm a track and click ● in an empty slot to record from the computer keyboard (M). Right-click anything for more.");
    connect(project, &Project::structureChanged, this, [this] {
        m_selTrack = std::clamp(m_selTrack, 0, std::max(0, m_project->trackCount() - 1));
        m_selScene = std::clamp(m_selScene, 0, std::max(0, int(m_project->scenes.size()) - 1));
        updateGeometry();
        update();
    });
    connect(project, &Project::clipChanged, this, qOverload<>(&QWidget::update));
    connect(project, &Project::selectionChanged, this, qOverload<>(&QWidget::update));   // the selected tracks
    connect(transport, &Transport::sessionChanged, this, qOverload<>(&QWidget::update));
    connect(transport, &Transport::recordingChanged, this, qOverload<>(&QWidget::update));
    connect(transport, &Transport::tick, this, [this] {
        bool any = m_transport->sessionRecording();
        for (const Track& t : m_project->tracks) if (t.playingSlot >= 0 || t.queuedSlot != -2) any = true;
        if (any) update();
    });
}

int ClipGrid::fixedHeight(int rowH) const {
    return m_headerH + m_gap + int(m_project->scenes.size()) * (rowH + m_gap) + m_stopH;
}

QSize ClipGrid::sizeHint() const {
    return QSize(contentWidth(), fixedHeight(m_rowH));
}

QSize ClipGrid::minimumSizeHint() const {
    return QSize(contentWidth(), m_headerH + 2 * (m_rowH + m_gap) + m_stopH);
}

// ---- rows: the header row stays at the top, the stop / status row at the bottom, and the
// scene rows in between scroll; below the last scene, empty cells fill the space.

int ClipGrid::stopTop() const { return height() - m_stopH; }
int ClipGrid::rowTop(int row) const { return m_headerH + m_gap + (row - m_firstRow) * (m_rowH + m_gap); }
int ClipGrid::visibleRows() const { return std::max(1, (stopTop() - m_headerH - m_gap) / (m_rowH + m_gap)); }

void ClipGrid::scrollToRow(int row) {
    const int maxFirst = std::max(0, int(m_project->scenes.size()) - visibleRows());
    const int first = std::clamp(row, 0, maxFirst);
    if (first == m_firstRow) return;
    m_firstRow = first;
    update();
}

void ClipGrid::ensureVisible(int row) {
    if (row < m_firstRow) scrollToRow(row);
    else if (row >= m_firstRow + visibleRows()) scrollToRow(row - visibleRows() + 1);
}

void ClipGrid::resizeEvent(QResizeEvent*) { scrollToRow(m_firstRow); }

void ClipGrid::wheelEvent(QWheelEvent* e) {
    if (m_project->scenes.size() <= visibleRows() || (e->modifiers() & Qt::ShiftModifier)) { e->ignore(); return; }
    const int steps = e->angleDelta().y() / 40;
    scrollToRow(m_firstRow - (steps == 0 ? (e->angleDelta().y() > 0 ? 1 : -1) : steps));
    e->accept();
}

int ClipGrid::contentWidth() const {
    int w = m_mainW;
    for (int c = 0; c < m_project->trackCount(); ++c) if (const int cw = columnWidth(c)) w += cw + m_gap;
    return w;
}

int ClipGrid::freeSpace() const { return std::max(0, width() - contentWidth()); }

int ClipGrid::columnWidth(int col) const {
    if (col >= m_project->trackCount()) return m_mainW;
    if (col < m_project->tracks.size() && m_project->hiddenByFold(col)) return 0;   // inside a folded group
    const Track* t = m_project->trackAt(col);
    return t && t->width > 0 ? t->width : kDefaultColumnWidth;
}

int ClipGrid::columnLeft(int col) const {
    int x = col >= int(m_project->tracks.size()) ? freeSpace() : 0;   // returns and Main: after the free space
    for (int c = 0; c < col; ++c) if (const int cw = columnWidth(c)) x += cw + m_gap;
    return x;
}

int ClipGrid::columnAt(int x) const {
    for (int c = 0; c <= m_project->trackCount(); ++c) {
        const int left = columnLeft(c), w = columnWidth(c);
        if (w > 0 && x >= left && x < left + w + m_gap) return c;
    }
    return -1;
}

int ClipGrid::dividerAt(const QPoint& p) const {
    if (p.y() < 0 || p.y() >= height()) return -1;
    for (int c = 0; c < m_project->trackCount(); ++c)
        if (columnWidth(c) > 0 && std::abs(p.x() - (columnLeft(c) + columnWidth(c))) <= 3) return c;
    return -1;
}

void ClipGrid::setColumnWidth(int col, int w) {
    Track* t = m_project->trackAt(col);
    if (!t) return;
    w = w <= 0 ? 0 : std::clamp(w, int(kMinColumnWidth), int(kMaxColumnWidth));
    if (t->width == w) return;
    t->width = w;
    setMinimumWidth(contentWidth());
    updateGeometry();
    update();
    emit columnsResized();
}

QRect ClipGrid::slotRect(int col, int row) const {
    int x = columnLeft(col);
    int w = columnWidth(col);
    if (row == kHeaderRow) return QRect(x, 0, w, m_headerH);
    if (row == kStopRow) return QRect(x, stopTop(), w, m_stopH);
    return QRect(x, rowTop(row), w, m_rowH);
}

ClipGrid::Hit ClipGrid::hitTest(const QPoint& p) const {
    Hit h;
    const int nT = int(m_project->tracks.size()), cols = m_project->trackCount(), rows = int(m_project->scenes.size());
    const int col = columnAt(p.x());
    if (col < 0 || col > cols) return h;   // includes the free space before the returns
    if (p.y() < m_headerH) { h.what = col == cols ? Hit::MainHeader : Hit::Header; h.track = col; return h; }
    if (p.y() >= stopTop()) { h.what = col == cols ? Hit::StopAll : col < nT ? Hit::Stop : Hit::ReturnBody; h.track = col; return h; }
    if (col >= nT && col < cols) { h.what = Hit::ReturnBody; h.track = col; return h; }   // returns hold no clips
    if (p.y() < m_headerH + m_gap) return h;
    const int row = rowAt(p.y());
    h.track = col; h.scene = row;
    if (row >= rows) { h.what = Hit::Empty; return h; }   // a filler cell below the last scene
    h.what = col == cols ? Hit::SceneSlot : Hit::Slot;
    h.launchButton = (p.x() - columnLeft(col)) < 18;
    return h;
}

void ClipGrid::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    QFont bold = font(); bold.setBold(true);
    const int nT = int(m_project->tracks.size()), cols = m_project->trackCount(), rows = int(m_project->scenes.size());
    const bool blink = int(m_transport->beat() * 6) % 2;

    // ---- headers (tracks, returns, Main)
    for (int c = 0; c <= cols; ++c) {
        QRect r = slotRect(c, -1);
        if (r.width() <= 0) continue;   // folded away
        const bool isMain = c == cols;
        const Track* t = isMain ? nullptr : m_project->trackAt(c);
        const bool sel = isMain ? c == m_selTrack : (c == m_selTrack || (c < nT && m_project->selectedTrackIds.size() > 1 && m_project->selectedTrackIds.contains(t->id)));
        p.fillRect(r, sel ? theme::selection : t && t->isGroup() ? theme::panel2.lighter(112) : theme::panel2);
        p.fillRect(QRect(r.left(), r.top(), r.width(), 3), isMain ? theme::neutral : t->color);
        if (c < nT && t->groupId) {   // a track in a group: a line in the group's colour along the bottom
            const int g = m_project->groupOf(c);
            if (g >= 0) p.fillRect(QRect(r.left(), r.bottom() - 1, r.width(), 2), m_project->tracks[g].color);
        }
        if (sel && m_selKind == Sel::Track) { p.setPen(theme::outline); p.setBrush(Qt::NoBrush); p.drawRect(r.adjusted(0, 0, -1, -1)); }
        int textLeft = 6;
        if (t && t->isGroup()) {   // the fold arrow
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(sel ? theme::selectionText : theme::text);
            const QPointF o(r.left() + 8, r.center().y() + 1);
            QPolygonF arrow;
            if (t->folded) arrow << o + QPointF(-3, -4) << o + QPointF(-3, 4) << o + QPointF(3, 0);
            else arrow << o + QPointF(-4, -3) << o + QPointF(4, -3) << o + QPointF(0, 3);
            p.drawPolygon(arrow);
            p.setRenderHint(QPainter::Antialiasing, false);
            textLeft = 16;
        }
        p.setPen(sel ? theme::selectionText : theme::text);
        p.setFont(bold);
        p.drawText(r.adjusted(textLeft, 3, -4, 0), Qt::AlignVCenter | Qt::AlignLeft,
                   p.fontMetrics().elidedText(isMain ? QString("Master") : t->name, Qt::ElideRight, r.width() - textLeft - 4));
    }
    p.setFont(font());

    // ---- slots: the scene rows that fit, then empty filler cells down to the stop row
    p.save();
    p.setClipRect(QRect(0, m_headerH, width(), stopTop() - m_headerH));
    for (int row = m_firstRow; rowTop(row) < stopTop(); ++row) {
        if (row >= rows) {   // filler: empty cells (double-click or drop to fill them; scenes are added)
            for (int c = 0; c <= cols; ++c) {
                const QRect r = slotRect(c, row);
                p.fillRect(r, c >= nT && c < cols ? theme::returnColumn : c == cols ? theme::slotScene.darker(115) : theme::slotEmpty.darker(108));
                if (c < nT && c == m_dropTrack && row == m_dropScene) { p.setPen(QPen(theme::accent, 2, Qt::DashLine)); p.setBrush(Qt::NoBrush); p.drawRect(r.adjusted(1, 1, -1, -1)); }
            }
            continue;
        }
        for (int c = nT; c < cols; ++c) p.fillRect(slotRect(c, row), theme::returnColumn);
        for (int c = 0; c < nT; ++c) {
            const Track& t = m_project->tracks[c];
            const QRect r = slotRect(c, row);
            if (r.width() <= 0) continue;   // folded away
            if (t.isGroup()) {   // a group slot: lit when its tracks have clips in this scene; ▶ launches them all
                bool any = false, playing = false, queued = false;
                for (int m : m_project->membersOf(c)) {
                    const Track& mt = m_project->tracks[m];
                    any |= row < mt.clipSlots.size() && mt.clipSlots[row];
                    playing |= mt.playingSlot == row;
                    queued |= mt.queuedSlot == row;
                }
                p.fillRect(r, any ? t.color.darker(150) : theme::slotEmpty);
                if (any) {
                    const QColor tri = playing ? theme::groupPlaying : theme::groupGlyph;
                    if (!queued || blink) icons::paint(p, icons::Glyph::Play, QRectF(r.left() + 2, r.center().y() - 7, 14, 14), tri);
                } else {
                    p.fillRect(QRect(r.left() + 6, r.center().y() - 3, 6, 6), theme::slotStop);
                }
                if (m_selKind == Sel::Slot && c == m_selTrack && row == m_selScene) { p.setPen(QPen(theme::outline, 1)); p.setBrush(Qt::NoBrush); p.drawRect(r.adjusted(0, 0, -1, -1)); }
                continue;
            }
            const ClipPtr clip = row < t.clipSlots.size() ? t.clipSlots[row] : nullptr;
            const bool playing = t.playingSlot == row;
            const bool queued  = t.queuedSlot == row;
            const bool recording = m_transport->isRecordingSlot(c, row);
            const bool selected = m_selKind == Sel::Slot && c == m_selTrack && row == m_selScene;
            if (clip) {
                const QColor clipCol = m_project->clipColor(clip);
                p.fillRect(r, recording ? theme::record : clipCol);
                if (m_dragging && m_drag == Drag::Clip && c == m_dragTrack && row == m_dragScene)   // the clip being dragged
                    p.fillRect(r, theme::shade);
                const QColor tri = playing ? theme::clipPlaying : theme::clipGlyph;
                const QRectF glyph(r.left() + 2, r.center().y() - 7, 14, 14);
                if (recording) icons::paint(p, icons::Glyph::Record, glyph, tri);
                else if (!queued || blink) icons::paint(p, icons::Glyph::Play, glyph, tri);
                const bool running = playing && m_transport->playing() && clip->length > 0;
                p.setPen(theme::ink);
                p.drawText(r.adjusted(18, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(clip->name, Qt::ElideRight, r.width() - (running ? 38 : 24)));
                if (running) {   // where the clip is in its loop: a bar along the bottom and a small clock at the right, in the clip's own shade
                    const double pos = loopFraction(t, *clip);
                    const QColor shade = recording ? theme::ink : clipCol.darker(230);
                    p.setRenderHint(QPainter::Antialiasing, true);
                    p.setPen(Qt::NoPen);
                    p.setBrush(shade);
                    p.drawRoundedRect(QRectF(r.left() + 1, r.bottom() - 2.5, std::max(2.0, (r.width() - 2) * pos), 2.5), 1, 1);
                    const QRectF clock(r.right() - 14, r.center().y() - 5, 10, 10);
                    QColor ring = shade; ring.setAlphaF(0.45);
                    p.setBrush(ring);
                    p.drawEllipse(clock);
                    p.setBrush(shade);
                    p.drawPie(clock.adjusted(1, 1, -1, -1), 90 * 16, -int(std::lround(pos * 360 * 16)));
                    p.setRenderHint(QPainter::Antialiasing, false);
                }
            } else {
                p.fillRect(r, theme::slotEmpty);
                if (t.armed) {   // record button (MIDI from the keyboards, audio from the input)
                    icons::paint(p, icons::Glyph::Record, QRectF(r.left() + 2, r.center().y() - 7, 14, 14), theme::slotArmed);
                } else {         // stop button
                    p.fillRect(QRect(r.left() + 6, r.center().y() - 3, 6, 6), theme::slotStop);
                }
            }
            if (selected) { p.setPen(QPen(theme::outline, 1)); p.setBrush(Qt::NoBrush); p.drawRect(r.adjusted(0, 0, -1, -1)); }
            if (c == m_dropTrack && row == m_dropScene) { p.setPen(QPen(theme::accent, 2, Qt::DashLine)); p.setBrush(Qt::NoBrush); p.drawRect(r.adjusted(1, 1, -1, -1)); }
        }
        // scene slot: launch triangle and name, the scene number in a narrow column on the right
        const QRect r = slotRect(cols, row);
        const bool selScene = m_selKind == Sel::Scene && row == m_selScene;
        p.fillRect(r, selScene ? theme::selection : theme::slotScene);
        const QRect num(r.right() - kSceneNumberW + 1, r.top(), kSceneNumberW, r.height());
        p.fillRect(num, selScene ? theme::selection.darker(115) : theme::sceneNumber);
        icons::paint(p, icons::Glyph::Play, QRectF(r.left() + 2, r.center().y() - 7, 14, 14), theme::textFaint);
        p.setPen(m_project->scenes[row].isEmpty() ? theme::dim : theme::text);
        p.drawText(r.adjusted(18, 0, -kSceneNumberW - 4, 0), Qt::AlignVCenter | Qt::AlignLeft,
                   p.fontMetrics().elidedText(m_project->scenes[row], Qt::ElideRight, r.width() - 22 - kSceneNumberW));
        p.setPen(theme::dim);
        p.drawText(num.adjusted(0, 0, -3, 0), Qt::AlignVCenter | Qt::AlignRight, row == 9 ? QString("0") : QString::number(row + 1));
        if (selScene) { p.setPen(theme::outline); p.setBrush(Qt::NoBrush); p.drawRect(r.adjusted(0, 0, -1, -1)); }
    }
    if (rows > visibleRows()) {   // where the visible scenes are, in a slim bar at the right edge
        const int areaTop = m_headerH + m_gap, areaH = stopTop() - areaTop;
        const int barH = std::max(12, areaH * visibleRows() / rows), barY = areaTop + (areaH - barH) * m_firstRow / std::max(1, rows - visibleRows());
        p.fillRect(QRect(width() - 4, barY, 3, barH), theme::neutral);
    }
    p.restore();

    // ---- stop / status row: ■ stops the track; a playing clip shows how often it has looped,
    // a clock that fills once per loop and the loop's length in beats
    p.setRenderHint(QPainter::Antialiasing, true);
    for (int c = 0; c <= cols; ++c) {
        const QRect r = slotRect(c, kStopRow);
        if (r.width() <= 0) continue;
        if (c >= nT && c < cols) { p.fillRect(r, theme::returnColumn); continue; }
        p.fillRect(r, theme::stopRow);
        if (c == cols) {
            p.fillRect(QRect(r.center().x() - 28, r.center().y() - 3, 6, 6), theme::faint);
            p.setPen(theme::faint);
            p.drawText(r.adjusted(0, 0, -4, 0), Qt::AlignCenter, "    Stop All");
            continue;
        }
        p.fillRect(QRect(r.left() + 6, r.center().y() - 3, 6, 6), theme::faint);
        const TrackStatus st = trackStatus(c);
        if (!st.playing) continue;
        // the clock, in the track's colour: a dim ring that fills once per loop, the loop's length beside it
        const QColor tone = m_project->tracks[c].color;
        const double frac = st.fraction;
        const QString len = std::abs(st.length - std::round(st.length)) < 1e-6 ? QString::number(int(std::round(st.length)))
                                                                               : QString::number(st.length, 'f', 2);
        QFont sf = font(); sf.setPointSizeF(7.5); p.setFont(sf);
        const int lenW = p.fontMetrics().horizontalAdvance(len);
        const QRectF clock(r.right() - lenW - 6 - 13, r.center().y() - 6.5, 13, 13);
        p.setPen(theme::textSoft);
        p.drawText(QRect(int(clock.right()) + 3, r.top(), lenW + 3, r.height()), Qt::AlignVCenter | Qt::AlignLeft, len);
        QColor dim = tone; dim.setAlphaF(0.3);
        p.setPen(Qt::NoPen);
        p.setBrush(dim);
        p.drawEllipse(clock);
        p.setBrush(tone);
        p.drawPie(clock.adjusted(1.5, 1.5, -1.5, -1.5), 90 * 16, -int(std::lround(frac * 360 * 16)));
        p.setPen(QPen(tone, 1));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(clock);
        if (st.pass > 0) {   // the pass the loop is on
            const QString pass = QString::number(st.pass);
            p.setPen(tone);
            p.drawText(QRect(r.left() + 14, r.top(), int(clock.left()) - r.left() - 17, r.height()), Qt::AlignVCenter | Qt::AlignRight, pass);
        }
        p.setFont(font());
    }
    p.setRenderHint(QPainter::Antialiasing, false);

    // ---- the free space before the returns: where dropped samples start a new audio track
    if (freeSpace() > 70) {
        const QRect gap(columnLeft(nT - 1) + columnWidth(nT - 1) + m_gap, m_headerH, freeSpace() - m_gap, stopTop() - m_headerH);
        p.setPen(m_dropHere ? theme::accent : theme::hint);
        p.drawText(gap.adjusted(6, 0, -6, 0), Qt::AlignCenter | Qt::TextWordWrap, "Drop Files, Samples and Devices Here");
        if (m_dropHere) { p.setBrush(Qt::NoBrush); p.drawRect(gap.adjusted(2, 2, -3, -3)); }
    }

    // ---- a device drag over a track that can't take it: a new track right after it
    if (m_dropAfter >= 0 && m_dropAfter < cols) {
        const int x = columnLeft(m_dropAfter) + columnWidth(m_dropAfter) + m_gap / 2;
        p.setPen(QPen(theme::accent, 3));
        p.drawLine(x, m_headerH + 2, x, stopTop() - 2);
        p.setPen(theme::accent);
        p.save();
        p.translate(x + 6, m_headerH + 8);
        p.rotate(90);
        p.drawText(0, 0, "New track");
        p.restore();
    }
    // ---- a device drag: the column it would go on
    if (m_dropColumn >= 0 && m_dropColumn < cols) {
        const QRect head = slotRect(m_dropColumn, kHeaderRow), stop = slotRect(m_dropColumn, kStopRow);
        p.setPen(QPen(theme::accent, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRect(head.topLeft(), stop.bottomRight()).adjusted(1, 1, -1, -1));
    }

    // ---- drag feedback
    if (m_dragging && m_drag == Drag::Scene) {   // the line the scene would land on
        const int target = std::clamp(rowAt(m_mousePos.y()), 0, rows - 1);
        const QRect r = slotRect(cols, target);
        const int y = target > m_dragScene ? r.bottom() : r.top();
        p.fillRect(QRect(0, y - 1, width(), 3), theme::accent);
    }
    if (m_dragging && m_drag == Drag::Clip) {
        Hit h = hitTest(m_mousePos);
        if (h.what == Hit::Slot) {
            QRect r = slotRect(h.track, h.scene);
            p.setPen(QPen(theme::outline, 2, Qt::DashLine)); p.setBrush(Qt::NoBrush);
            p.drawRect(r.adjusted(1, 1, -1, -1));
        }
    } else if (m_dragging && m_drag == Drag::Track) {
        const int at = columnAt(m_mousePos.x());
        const int target = std::clamp(at < 0 ? nT - 1 : at, 0, nT - 1);
        const int x = target > m_dragTrack ? slotRect(target, kHeaderRow).right() : slotRect(target, kHeaderRow).left();
        p.fillRect(QRect(x - 1, 0, 3, height()), theme::accent);
    }
}

QVector<QPair<QRect, QString>> ClipGrid::mapCells() const {
    QVector<QPair<QRect, QString>> out;
    const int nT = int(m_project->tracks.size()), cols = m_project->trackCount(), rows = int(m_project->scenes.size());
    for (int row = m_firstRow; row < rows && rowTop(row) < stopTop(); ++row) {
        for (int c = 0; c < nT; ++c) out.push_back({slotRect(c, row), QString("slot:%1:%2").arg(m_project->tracks[c].id).arg(row)});
        out.push_back({slotRect(cols, row), QString("scene:%1").arg(row)});
    }
    for (int c = 0; c < nT; ++c) out.push_back({slotRect(c, kStopRow), QString("stop:%1").arg(m_project->tracks[c].id)});
    out.push_back({slotRect(cols, kStopRow), QStringLiteral("stopall")});
    return out;
}

QString ClipGrid::mapTargetAt(const QPoint& p) const {
    const Hit h = hitTest(p);
    const int nT = int(m_project->tracks.size());
    switch (h.what) {
    case Hit::Slot:      return QString("slot:%1:%2").arg(m_project->tracks[h.track].id).arg(h.scene);
    case Hit::SceneSlot: return QString("scene:%1").arg(h.scene);
    case Hit::Stop:      return h.track < nT ? QString("stop:%1").arg(m_project->tracks[h.track].id) : QString();
    case Hit::StopAll:   return QStringLiteral("stopall");
    default:             return {};
    }
}

// How far a playing clip is through its loop (0 .. 1), from the beat you hear. The start may
// sit a little ahead of the heard beat right after a launch (output latency): that counts as 0.
double ClipGrid::loopFraction(const Track& t, const Clip& clip) const {
    if (clip.length <= 0) return 0;
    if (!clip.warp && t.playingClipBeat >= 0) {
        const double start = clip.loop ? clip.loopStart : clip.start;
        return std::clamp((t.playingClipBeat - start) / std::max(1e-9, clip.loopEnd() - start), 0.0, 1.0);
    }
    const double elapsed = std::max(0.0, m_transport->beat() - t.slotStart);
    const double firstPass = std::max(1e-9, clip.loopEnd() - clip.start);   // from the start marker to the end marker (0.23)
    if (!clip.loop) return std::min(1.0, elapsed / firstPass);
    const double pos = clip.positionAt(elapsed);
    if (pos < clip.loopStart) return std::clamp(elapsed / firstPass, 0.0, 1.0);   // running into the brace
    return std::clamp((pos - clip.loopStart) / clip.length, 0.0, 1.0);
}

ClipGrid::TrackStatus ClipGrid::trackStatus(int track) const {
    TrackStatus st;
    const Track* t = track >= 0 && track < m_project->tracks.size() ? &m_project->tracks[track] : nullptr;
    const ClipPtr clip = t && t->playingSlot >= 0 ? t->clipSlots.value(t->playingSlot) : nullptr;
    if (!clip || clip->length <= 0) return st;
    const double elapsed = std::max(0.0, m_transport->beat() - t->slotStart);
    st.playing = true;
    st.length = clip->length;
    st.fraction = loopFraction(*t, *clip);
    st.pass = clip->loop ? (!clip->warp && t->playingClipPass > 0 ? t->playingClipPass : int(elapsed / clip->length) + 1) : 0;
    return st;
}

void ClipGrid::select(Sel kind, int track, int scene) {
    m_selKind = kind;
    if (track >= 0) m_selTrack = track;
    if (scene >= 0) { m_selScene = scene; ensureVisible(scene); }
    if (track >= 0) emit trackSelected(track);
    update();
}

void ClipGrid::selectTrack(int track) {
    if (track == m_selTrack) return;
    m_selTrack = track;
    update();
}

void ClipGrid::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    const Hit h = hitTest(e->position().toPoint());
    m_pressPos = m_mousePos = e->position().toPoint();
    m_drag = Drag::None;
    m_dragging = false;
    if (const int div = dividerAt(m_pressPos); div >= 0) {   // resize that column
        m_drag = Drag::Resize;
        m_dragTrack = div;
        m_resizeWidth0 = columnWidth(div);
        return;
    }
    switch (h.what) {
    case Hit::Header:
    case Hit::ReturnBody: {
        const int nT = int(m_project->tracks.size());
        const Track* t = m_project->trackAt(h.track);
        if (h.what == Hit::Header && t && t->isGroup() && m_pressPos.x() - columnLeft(h.track) < 16) {   // the fold arrow
            m_project->setFolded(h.track, !t->folded);
            return;
        }
        // Ctrl+click adds or removes a track from the selection, Shift+click selects a range (for Group Tracks)
        if (h.track < nT && (e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
            if (e->modifiers() & Qt::ShiftModifier) {
                const int a = std::min(m_selTrack, h.track), b = std::max(m_selTrack, h.track);
                for (int i = std::max(0, a); i <= b && i < nT; ++i) m_project->selectedTrackIds.insert(m_project->tracks[i].id);
            } else if (m_project->selectedTrackIds.contains(t->id) && m_project->selectedTrackIds.size() > 1) {
                m_project->selectedTrackIds.remove(t->id);
                select(Sel::Track, m_project->selectedTrackIndexes().first(), -1);   // one that's still selected
                return;
            } else {
                if (m_selTrack < nT) m_project->selectedTrackIds.insert(m_project->tracks[m_selTrack].id);
                m_project->selectedTrackIds.insert(t->id);
            }
            select(Sel::Track, h.track, -1);
            return;
        }
        m_project->selectedTrackIds = {t->id};
        select(Sel::Track, h.track, -1);
        if (h.what == Hit::Header && h.track < nT) { m_drag = Drag::Track; m_dragTrack = h.track; }
        break;
    }
    case Hit::MainHeader:   // the Master: its devices in the Device View
        select(Sel::Track, m_project->trackCount(), -1);
        break;
    case Hit::Slot: {
        Track& t = m_project->tracks[h.track];
        m_project->selectedTrackIds = {t.id};
        if (t.isGroup()) {   // a group slot launches its tracks' clips in that scene (and stops the others)
            select(Sel::Slot, h.track, h.scene);
            if (h.launchButton) for (int m : m_project->membersOf(h.track)) {
                if (h.scene < m_project->tracks[m].clipSlots.size() && m_project->tracks[m].clipSlots[h.scene]) m_transport->launchSlot(m, h.scene);
                else m_transport->stopTrack(m);
            }
            break;
        }
        const ClipPtr clip = t.clipSlots[h.scene];
        select(Sel::Slot, h.track, h.scene);
        if (h.launchButton) {
            if (clip) { m_transport->launchSlot(h.track, h.scene); m_launchTrack = h.track; m_launchScene = h.scene; }
            else if (t.armed) m_transport->recordSlot(h.track, h.scene);
            else m_transport->stopTrack(h.track);
        } else if (clip) {
            m_drag = Drag::Clip; m_dragTrack = h.track; m_dragScene = h.scene;
        }
        if (t.clipSlots[h.scene]) emit clipSelected(h.track, h.scene);
        break;
    }
    case Hit::SceneSlot:
        select(Sel::Scene, -1, h.scene);
        if (h.launchButton) { m_transport->launchScene(h.scene); break; }
        m_drag = Drag::Scene;   // the name: launches on release, unless it's dragged to another row
        m_dragScene = h.scene;
        m_sceneClick = true;
        break;
    case Hit::Empty:     if (h.track < m_project->trackCount()) { m_selTrack = h.track; emit trackSelected(h.track); update(); } break;
    case Hit::Stop:
        if (h.track < m_project->tracks.size() && m_project->tracks[h.track].isGroup()) for (int m : m_project->membersOf(h.track)) m_transport->stopTrack(m);
        else m_transport->stopTrack(h.track);
        break;
    case Hit::StopAll:   m_transport->stopAll(); break;
    default: break;
    }
}

void ClipGrid::mouseMoveEvent(QMouseEvent* e) {
    m_mousePos = e->position().toPoint();
    if (m_drag == Drag::Resize) {
        setColumnWidth(m_dragTrack, m_resizeWidth0 + m_mousePos.x() - m_pressPos.x());
        return;
    }
    if (m_drag != Drag::None) {
        if (!m_dragging && (m_mousePos - m_pressPos).manhattanLength() > QApplication::startDragDistance()) m_dragging = true;
        if (m_dragging) { setCursor(m_drag == Drag::Track ? Qt::SizeHorCursor : m_drag == Drag::Scene ? Qt::SizeVerCursor : Qt::ClosedHandCursor); update(); }
        return;
    }
    if (dividerAt(m_mousePos) >= 0) { setCursor(Qt::SplitHCursor); return; }
    Hit h = hitTest(m_mousePos);
    setCursor(h.what == Hit::None || h.what == Hit::ReturnBody ? Qt::ArrowCursor : Qt::PointingHandCursor);
}

void ClipGrid::mouseReleaseEvent(QMouseEvent* e) {
    if (m_launchTrack >= 0) { m_transport->releaseSlot(m_launchTrack, m_launchScene); m_launchTrack = m_launchScene = -1; }
    if (m_dragging && m_drag == Drag::Clip) {
        const Hit h = hitTest(e->position().toPoint());
        if (h.what == Hit::Slot && !(h.track == m_dragTrack && h.scene == m_dragScene)
            && m_project->tracks[h.track].type == m_project->tracks[m_dragTrack].type) {   // clips stay on their kind of track
            const bool copy = e->modifiers() & Qt::ControlModifier;
            EditScope edit(m_project, copy ? "Copy Clip" : "Move Clip");
            ClipPtr src = m_project->tracks[m_dragTrack].clipSlots[m_dragScene];
            m_project->tracks[h.track].clipSlots[h.scene] = copy ? m_project->copyClip(src) : src;
            if (!copy) m_project->tracks[m_dragTrack].clipSlots[m_dragScene] = nullptr;
            emit m_project->structureChanged();
            select(Sel::Slot, h.track, h.scene);
            emit clipSelected(h.track, h.scene);
        }
    } else if (m_dragging && m_drag == Drag::Track) {
        const int at = columnAt(int(e->position().x()));
        const int target = std::clamp(at < 0 ? int(m_project->tracks.size()) - 1 : at, 0, int(m_project->tracks.size()) - 1);
        if (m_project->moveTrack(m_dragTrack, target)) select(Sel::Track, target, -1);
    } else if (m_dragging && m_drag == Drag::Scene) {
        const int target = std::clamp(rowAt(int(e->position().y())), 0, int(m_project->scenes.size()) - 1);
        if (m_transport->sessionRecording()) InfoView::notify("Move Scene", "Scenes stay put while a clip is recording.");
        else if (m_project->moveScene(m_dragScene, target)) select(Sel::Scene, -1, target);
    } else if (m_drag == Drag::Scene && m_sceneClick) {
        m_transport->launchScene(m_dragScene);   // a click on its name
    }
    m_sceneClick = false;
    m_drag = Drag::None;
    m_dragging = false;
    if (QWidget::mouseGrabber() == this) releaseMouse();   // a drag resumed from the Arrangement grabbed it
    setCursor(Qt::ArrowCursor);
    update();
}

ClipPtr ClipGrid::takeClipDrag(int* track, int* scene) {
    if (!clipDragActive()) return nullptr;
    ClipPtr clip;
    if (m_dragTrack >= 0 && m_dragTrack < m_project->tracks.size() && m_dragScene >= 0 && m_dragScene < m_project->tracks[m_dragTrack].clipSlots.size())
        clip = m_project->tracks[m_dragTrack].clipSlots[m_dragScene];
    if (track) *track = m_dragTrack;
    if (scene) *scene = m_dragScene;
    m_drag = Drag::None;
    m_dragging = false;
    if (QWidget::mouseGrabber() == this) releaseMouse();
    setCursor(Qt::ArrowCursor);
    update();
    return clip;
}

void ClipGrid::resumeClipDrag(int track, int scene) {
    if (track < 0 || track >= m_project->tracks.size() || scene < 0 || scene >= m_project->tracks[track].clipSlots.size() || !m_project->tracks[track].clipSlots[scene]) return;
    m_drag = Drag::Clip;
    m_dragging = true;
    m_dragTrack = track;
    m_dragScene = scene;
    m_mousePos = m_pressPos = mapFromGlobal(QCursor::pos());
    grabMouse();   // the press happened before the view switched away: make sure the moves and the release come here
    setCursor(Qt::ClosedHandCursor);
    update();
}

void ClipGrid::mouseDoubleClickEvent(QMouseEvent* e) {
    if (const int div = dividerAt(e->position().toPoint()); div >= 0) { setColumnWidth(div, 0); return; }   // default width
    const Hit h = hitTest(e->position().toPoint());
    if (h.what == Hit::Header || h.what == Hit::ReturnBody) { emit openDeviceView(h.track); return; }
    if (h.what == Hit::SceneSlot) { select(Sel::Scene, -1, h.scene); renameSelection(); return; }
    if (h.what == Hit::Slot && !h.launchButton && !m_project->tracks[h.track].clipSlots[h.scene]
        && m_project->tracks[h.track].type == TrackType::Midi) createClip(h.track, h.scene);
    if (h.what == Hit::Empty && h.track < m_project->tracks.size() && m_project->tracks[h.track].type == TrackType::Midi) {
        EditScope edit(m_project, "Create Clip");   // the scenes down to that row, and the clip
        addScenesThrough(h.scene);
        createClip(h.track, h.scene);
    }
}

void ClipGrid::contextMenuEvent(QContextMenuEvent* e) {
    const Hit h = hitTest(e->pos());
    if (h.what == Hit::Header || h.what == Hit::ReturnBody) {
        select(Sel::Track, h.track, -1);
        showTrackMenu(this, e->globalPos(), m_project, h.track, [this] { renameSelection(); });
        return;
    }
    QMenu menu(this);
    if (h.what == Hit::SceneSlot) {
        select(Sel::Scene, -1, h.scene);
        menu.addAction("Rename\tCtrl+R", this, [this] { renameSelection(); });
        menu.addAction("Insert Scene\tCtrl+I", this, [this] { insertScene(); });
        menu.addAction("Capture and Insert Scene\tCtrl+Shift+I", this, [this] { captureScene(); });
        menu.addAction("Duplicate\tCtrl+D", this, [this] { duplicateSelection(); });
        QAction* del = menu.addAction("Delete\tDel", this, [this] { deleteSelection(); });
        del->setEnabled(m_project->scenes.size() > 1);
    } else if (h.what == Hit::Empty && h.track < m_project->tracks.size() && m_project->tracks[h.track].type == TrackType::Midi) {
        menu.addAction("Insert MIDI Clip", this, [this, h] {
            EditScope edit(m_project, "Create Clip");
            addScenesThrough(h.scene);
            createClip(h.track, h.scene);
        });
    } else if (h.what == Hit::Slot && m_project->tracks[h.track].isGroup()) {
        return;
    } else if (h.what == Hit::Slot) {
        select(Sel::Slot, h.track, h.scene);
        if (const ClipPtr clip = m_project->tracks[h.track].clipSlots[h.scene]) {
            emit clipSelected(h.track, h.scene);
            const Track& t = m_project->tracks[h.track];
            const bool playing = t.playingSlot == h.scene;
            menu.addAction(playing ? "Stop Clip" : "Launch Clip", this, [this, h, playing] {
                if (playing) m_transport->stopTrack(h.track); else { m_transport->launchSlot(h.track, h.scene); m_transport->releaseSlot(h.track, h.scene); }
            });
            menu.addSeparator();
            menu.addAction("Cut\tCtrl+X", this, [this] { emit commandRequested("cut"); });
            menu.addAction("Copy\tCtrl+C", this, [this] { emit commandRequested("copy"); });
            menu.addAction("Paste\tCtrl+V", this, [this] { emit commandRequested("paste"); });
            menu.addAction("Duplicate\tCtrl+D", this, [this] { duplicateSelection(); });
            menu.addAction("Delete\tDel", this, [this] { deleteSelection(); });
            menu.addSeparator();
            menu.addAction("Rename\tCtrl+R", this, [this] { renameSelection(); });
            QMenu* colors = menu.addMenu("Color");
            addColorGrid(colors, m_project->clipColor(clip), [this, clip](const QColor& c) { EditScope edit(m_project, "Clip Color"); clip->color = c; emit m_project->clipChanged(); });
            colors->addAction("Track Color", this, [this, clip] { EditScope edit(m_project, "Clip Color"); clip->color = QColor(); emit m_project->clipChanged(); })->setEnabled(clip->color.isValid());
            menu.addAction(clip->loop ? "Loop Off" : "Loop On", this, [this, clip] { EditScope edit(m_project, clip->loop ? "Loop Off" : "Loop On"); clip->loop = !clip->loop; emit m_project->clipChanged(); });
            if (clip->isAudio()) {
                menu.addAction(clip->reverse ? "Play Forwards" : "Reverse", this, [this, clip] { EditScope edit(m_project, "Reverse"); clip->reverse = !clip->reverse; emit m_project->clipChanged(); })->setEnabled(clip->takes.isEmpty());
                menu.addAction(clip->warp ? "Warp Off" : "Warp On", this, [this, clip] { EditScope edit(m_project, clip->warp ? "Warp Off" : "Warp On"); clip->warp = !clip->warp; emit m_project->clipChanged(); });
            }
            menu.addSeparator();
            menu.addAction("Save Clip to User Library…", this, [this] { emit commandRequested("saveClip"); });
            if (clip->isAudio()) menu.addAction("Show Sample in File Manager", this, [this] { emit commandRequested("showSample"); });
        } else if (m_project->tracks[h.track].type == TrackType::Midi) {
            menu.addAction("Insert MIDI Clip", this, [this, h] { createClip(h.track, h.scene); });
            menu.addAction("Paste\tCtrl+V", this, [this] { emit commandRequested("paste"); });
            if (m_project->tracks[h.track].armed) menu.addAction("Record into Slot", this, [this, h] { m_transport->recordSlot(h.track, h.scene); });
        } else {
            menu.addAction("Paste\tCtrl+V", this, [this] { emit commandRequested("paste"); });
            if (m_project->tracks[h.track].armed) menu.addAction("Record into Slot", this, [this, h] { m_transport->recordSlot(h.track, h.scene); });
            menu.addAction("Drag samples or clips here from the browser or the file manager")->setEnabled(false);
        }
    } else if (h.what == Hit::Stop && h.track < m_project->tracks.size()) {
        menu.addAction("Stop Track", this, [this, h] { m_transport->stopTrack(h.track); });
        menu.addAction("Stop All Clips", this, [this] { m_transport->stopAll(); });
    } else if (h.what == Hit::StopAll || h.what == Hit::MainHeader) {
        menu.addAction("Stop All Clips", this, [this] { m_transport->stopAll(); });
        menu.addAction("Back to Arrangement", this, [this] { m_transport->backToArrangement(); });
    } else {
        return;
    }
    menu.exec(e->globalPos());
}

void ClipGrid::leaveEvent(QEvent*) { if (m_drag == Drag::None) setCursor(Qt::ArrowCursor); }

int ClipGrid::rowAt(int y) const {   // scene row under y (past the last scene for filler cells)
    return m_firstRow + std::max(0, (y - m_headerH - m_gap) / (m_rowH + m_gap));
}

void ClipGrid::dragEnterEvent(QDragEnterEvent* e) {
    if (!Browser::audioFilesIn(e->mimeData()).isEmpty() || !Browser::clipFilesIn(e->mimeData()).isEmpty() || grooves::indexIn(e->mimeData()) >= 0) e->acceptProposedAction();
    else if (devicedrag::parse(e->mimeData()).valid()) dragMoveEvent(e);
}

bool ClipGrid::deviceDragOver(const QMimeData* mime, int col) {
    const devicedrag::Payload pl = devicedrag::parse(mime);
    const int oldColumn = m_dropColumn, oldAfter = m_dropAfter;
    const bool oldHere = m_dropHere;
    m_dropColumn = -1;
    m_dropAfter = -1;
    m_dropHere = false;
    bool ok = pl.valid();
    if (ok && col >= 0 && col < m_project->trackCount()) {
        ok = devicedrag::accepts(*m_project, pl, col);
        if (ok) m_dropColumn = col;
        else if (devicedrag::newTrackAfter(*m_project, pl, col)) { ok = true; m_dropAfter = col; }   // an instrument over an audio track with clips: a new MIDI track beside it
    } else if (ok && col == m_project->trackCount() && devicedrag::accepts(*m_project, pl, col)) {   // the Master column: its chain takes audio effects
        m_dropColumn = col;
    } else if (ok) {   // the free space (or an instrument over the Master column): a new track
        m_dropHere = true;   // the free space: a new track
    }
    if (m_dropColumn != oldColumn || m_dropHere != oldHere || m_dropAfter != oldAfter) update();
    return ok;
}

bool ClipGrid::deviceDrop(const QMimeData* mime, int col, bool copy) {
    clearDeviceDrop();
    const devicedrag::Payload pl = devicedrag::parse(mime);
    if (!pl.valid()) return false;
    const int after = devicedrag::newTrackAfter(*m_project, pl, col) ? col : -1;
    const int target = after >= 0 || col > m_project->trackCount() || (col == m_project->trackCount() && !devicedrag::accepts(*m_project, pl, col)) ? -1 : col;
    const devicedrag::Result r = devicedrag::drop(*m_project, pl, target, {}, copy, after);
    InfoView::notify(r.title, r.text);
    if (!r.ok) return false;
    select(Sel::Track, r.track, -1);
    emit openDeviceView(r.track);
    return true;
}

void ClipGrid::clearDeviceDrop() {
    if (m_dropColumn < 0 && m_dropAfter < 0 && !m_dropHere) return;
    m_dropColumn = -1;
    m_dropAfter = -1;
    m_dropHere = false;
    update();
}

void ClipGrid::addScenesThrough(int row) {
    while (m_project->scenes.size() <= row) m_project->insertScene(int(m_project->scenes.size()));
}

void ClipGrid::dragMoveEvent(QDragMoveEvent* e) {
    if (const devicedrag::Payload pl = devicedrag::parse(e->mimeData()); pl.valid()) {
        if (!deviceDragOver(e->mimeData(), columnAt(int(e->position().x())))) { e->ignore(); return; }
        if (pl.source == devicedrag::Payload::Devices) { e->setDropAction(devicedrag::copying(e) ? Qt::CopyAction : Qt::MoveAction); e->accept(); }
        else e->acceptProposedAction();
        return;
    }
    const Hit h = hitTest(e->position().toPoint());
    const bool groove = grooves::indexIn(e->mimeData()) >= 0;   // a groove wants a MIDI track, audio files an audio track
    const QStringList clipFiles = Browser::clipFilesIn(e->mimeData());
    bool clipAudio = false;   // a library clip wants a track of its kind (read once per file, not per move)
    if (!clipFiles.isEmpty()) {
        static QHash<QString, bool> kinds;
        auto k = kinds.constFind(clipFiles.first());
        if (k == kinds.constEnd()) { Clip c; k = kinds.insert(clipFiles.first(), presets::readClip(clipFiles.first(), c) && c.isAudio()); }
        clipAudio = *k;
    }
    const bool takes = (h.what == Hit::Slot || (h.what == Hit::Empty && h.track < m_project->tracks.size()))
                           && m_project->tracks[h.track].type == (groove || (!clipFiles.isEmpty() && !clipAudio) ? TrackType::Midi : TrackType::Audio);
    m_dropTrack = takes ? h.track : -1;
    m_dropScene = takes ? h.scene : -1;
    m_dropHere = !takes;   // anywhere else: a new track
    e->acceptProposedAction();
    update();
}

void ClipGrid::dragLeaveEvent(QDragLeaveEvent*) {
    m_dropTrack = m_dropScene = m_dropColumn = m_dropAfter = -1;
    m_dropHere = false;
    update();
}

void ClipGrid::dropEvent(QDropEvent* e) {
    if (const devicedrag::Payload pl = devicedrag::parse(e->mimeData()); pl.valid()) {
        const bool copy = devicedrag::copying(e);
        if (deviceDrop(e->mimeData(), columnAt(int(e->position().x())), copy)) {
            e->setDropAction(pl.source == devicedrag::Payload::Devices && !copy ? Qt::MoveAction : Qt::CopyAction);
            e->accept();
        }
        return;
    }
    const QStringList files = Browser::audioFilesIn(e->mimeData());
    const QPoint pos = e->position().toPoint();
    m_dropTrack = m_dropScene = -1;
    m_dropHere = false;
    if (const QStringList clipFiles = Browser::clipFilesIn(e->mimeData()); !clipFiles.isEmpty()) {   // clips from the User Library
        const Hit h = hitTest(pos);
        const int track = (h.what == Hit::Slot || h.what == Hit::Header || h.what == Hit::Empty) && h.track < m_project->tracks.size() ? h.track : -1;
        const int scene = h.what == Hit::Slot || h.what == Hit::Empty ? h.scene : std::min(rowAt(pos.y()), int(m_project->scenes.size()));
        e->acceptProposedAction();
        placeClipFiles(clipFiles, track, scene);
        update();
        return;
    }
    double humanVel = 0, humanTime = 0;
    if (const int gi = grooves::indexIn(e->mimeData(), &humanVel, &humanTime); gi >= 0) {   // a Drum Studio groove: a MIDI clip
        const Hit h = hitTest(pos);
        const int track = (h.what == Hit::Slot || h.what == Hit::Header || h.what == Hit::Empty) && h.track < m_project->tracks.size() ? h.track : -1;
        const int scene = h.what == Hit::Slot || h.what == Hit::Empty ? h.scene : std::min(rowAt(pos.y()), int(m_project->scenes.size()));
        e->acceptProposedAction();
        int t = -1;
        {
            EditScope edit(m_project, "Add Groove");
            if (pos.y() < stopTop()) addScenesThrough(scene - 1);
            t = grooves::toSession(*m_project, track, scene, grooves::makeClip(*m_project, grooves::all()[size_t(gi)], humanVel, humanTime));
        }
        if (t >= 0) { select(Sel::Slot, t, scene); emit clipSelected(t, scene); }
        update();
        return;
    }
    if (files.isEmpty()) return;
    const Hit h = hitTest(pos);
    // An audio track's slot takes the clips; a MIDI track gets a new audio track next to it;
    // the free space, the returns and Main add one after the last track.
    const int track = (h.what == Hit::Slot || h.what == Hit::Header || h.what == Hit::Empty) && h.track < m_project->tracks.size() ? h.track : -1;
    const int scene = h.what == Hit::Slot || h.what == Hit::Empty ? h.scene : std::min(rowAt(pos.y()), int(m_project->scenes.size()));
    e->acceptProposedAction();
    // the files load in the background; the clips go in when they're ready (by track id, in
    // case the tracks move meanwhile)
    const int trackId = track >= 0 ? m_project->tracks[track].id : -1;
    const bool filler = pos.y() < stopTop();
    QGuiApplication::setOverrideCursor(Qt::BusyCursor);
    samples::loadAsync(files, this, [this, files, trackId, scene, filler](QVector<SamplePtr> loaded, QStringList errors) {
        QGuiApplication::restoreOverrideCursor();
        int t = -1;
        if (std::any_of(loaded.begin(), loaded.end(), [](const SamplePtr& s) { return s != nullptr; })) {
            EditScope edit(m_project, files.size() > 1 ? "Add Audio Clips" : "Add Audio Clip");
            if (filler) addScenesThrough(scene - 1);   // a filler row: the scenes above it first
            t = import::toSession(*m_project, m_project->indexOfTrack(trackId), scene, import::audioClips(*m_project, files, loaded));
        }
        if (t >= 0) { select(Sel::Slot, t, scene); emit clipSelected(t, scene); }
        if (!errors.isEmpty())
            QMessageBox::warning(this, "Add Audio", (t >= 0 ? "Some files couldn't be read:\n\n" : "Couldn't read:\n\n") + errors.join('\n'));
        update();
    });
    update();
}

void ClipGrid::createClip(int track, int scene) {
    EditScope edit(m_project, "Create Clip");
    Track& t = m_project->tracks[track];
    t.clipSlots[scene] = m_project->newClip("MIDI Clip", 4);
    select(Sel::Slot, track, scene);
    emit m_project->structureChanged();
    emit clipSelected(track, scene);
}

ClipPtr ClipGrid::selectedClip() const {
    if (m_selKind != Sel::Slot || m_selTrack >= m_project->tracks.size()) return nullptr;
    return m_project->tracks[m_selTrack].clipSlots.value(m_selScene);
}

void ClipGrid::pasteClip(const ClipPtr& clip) {
    if (!clip || m_selKind != Sel::Slot || m_selTrack >= m_project->tracks.size() || m_selScene >= m_project->scenes.size()) return;
    if (clip->isAudio() != (m_project->tracks[m_selTrack].type == TrackType::Audio)) return;   // audio clips go on audio tracks
    EditScope edit(m_project, "Paste Clip");
    m_project->tracks[m_selTrack].clipSlots[m_selScene] = m_project->copyClip(clip);
    emit m_project->structureChanged();
    emit clipSelected(m_selTrack, m_selScene);
}

int ClipGrid::placeClipFiles(const QStringList& paths, int track, int scene) {
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
    int t = track;
    if (t < 0 || t >= m_project->tracks.size() || m_project->tracks[t].isGroup() || (m_project->tracks[t].type == TrackType::Audio) != audio) {
        const int at = t < 0 ? int(m_project->tracks.size()) : t + 1;
        t = m_project->insertTrack(at, audio ? TrackType::Audio : TrackType::Midi);
        if (t < 0) return -1;
        m_project->renameTrack(t, clips.first()->name);
    }
    scene = std::max(0, scene);
    addScenesThrough(scene + clips.size() - 1);
    int s = scene;
    for (const ClipPtr& c : clips) {
        if (c->isAudio() != audio) continue;
        while (s < m_project->scenes.size() && m_project->tracks[t].clipSlots.value(s)) ++s;   // the next free slot
        if (s >= m_project->scenes.size()) addScenesThrough(s);
        m_project->tracks[t].clipSlots[s] = m_project->copyClip(c);
        ++s;
    }
    select(Sel::Slot, t, scene);
    emit m_project->structureChanged();
    emit clipSelected(t, scene);
    return t;
}

void ClipGrid::deleteSelection() {
    switch (m_selKind) {
    case Sel::Track: m_project->deleteTrack(m_selTrack); return;
    case Sel::Scene: m_project->deleteScene(m_selScene); return;
    case Sel::Slot: break;
    }
    if (m_selTrack < 0 || m_selTrack >= m_project->tracks.size()) return;
    Track& t = m_project->tracks[m_selTrack];
    if (m_selScene < 0 || m_selScene >= t.clipSlots.size() || !t.clipSlots[m_selScene]) return;
    EditScope edit(m_project, "Delete Clip");
    if (m_project->selectedClip == t.clipSlots[m_selScene] && m_project->trackOfClip(t.clipSlots[m_selScene]) == m_selTrack) {
        bool onTimeline = false;
        for (const auto& a : t.arrangement) onTimeline |= a.clip == t.clipSlots[m_selScene];
        if (!onTimeline) { m_project->selectedClip = nullptr; emit m_project->selectionChanged(); }
    }
    t.clipSlots[m_selScene] = nullptr;
    emit m_project->structureChanged();
}

void ClipGrid::duplicateSelection() {
    switch (m_selKind) {
    case Sel::Track: { int i = m_project->duplicateTrack(m_selTrack); if (i >= 0) select(Sel::Track, i, -1); return; }
    case Sel::Scene: { int s = m_project->duplicateScene(m_selScene); if (s >= 0) select(Sel::Scene, -1, s); return; }
    case Sel::Slot: break;
    }
    if (m_selTrack < 0 || m_selTrack >= m_project->tracks.size()) return;
    Track& t = m_project->tracks[m_selTrack];
    if (m_selScene < 0 || m_selScene >= t.clipSlots.size() || !t.clipSlots[m_selScene]) return;
    EditScope edit(m_project, "Duplicate Clip");
    if (m_selScene + 1 >= t.clipSlots.size() || t.clipSlots[m_selScene + 1]) m_project->insertScene(m_selScene + 1);   // make room
    Track& tt = m_project->tracks[m_selTrack];
    tt.clipSlots[m_selScene + 1] = m_project->copyClip(tt.clipSlots[m_selScene]);
    m_selScene += 1;
    emit m_project->structureChanged();
    emit clipSelected(m_selTrack, m_selScene);
}

void ClipGrid::renameSelection() {
    if (m_selKind == Sel::Track) {
        const Track* t = m_project->trackAt(m_selTrack);
        if (!t) return;
        const int index = m_selTrack;
        inlineEdit(this, slotRect(index, kHeaderRow), t->name, [this, index](const QString& s) { m_project->renameTrack(index, s); });
    } else if (m_selKind == Sel::Scene) {
        const int scene = m_selScene;
        ensureVisible(scene);
        inlineEdit(this, slotRect(m_project->trackCount(), scene), m_project->scenes.value(scene), [this, scene](const QString& s) { m_project->renameScene(scene, s); });
    } else if (ClipPtr clip = selectedClip()) {
        ensureVisible(m_selScene);
        inlineEdit(this, slotRect(m_selTrack, m_selScene), clip->name, [this, clip](const QString& s) {
            if (s.trimmed().isEmpty() || s == clip->name) return;
            EditScope edit(m_project, "Rename Clip");
            clip->name = s.trimmed();
            emit m_project->clipChanged();
        });
    }
}

void ClipGrid::insertScene() {
    const int s = m_project->insertScene(m_selScene + 1);
    select(Sel::Scene, -1, s);
}

void ClipGrid::captureScene() {
    const int s = m_project->captureScene(m_selScene);
    select(Sel::Scene, -1, s);
}

// ===========================================================================
// SessionView
// ===========================================================================

SessionView::SessionView(Project* project, Transport* transport, QWidget* parent)
    : QWidget(parent), m_project(project), m_transport(transport)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(6);

    // The clips above, the mixer below, and a divider you can drag between them.
    // The grid fills whatever height it gets with empty cells; a bigger window goes to the mixer.
    m_split = new QSplitter(Qt::Vertical, this);
    m_split->setObjectName("sessionSplit");
    m_split->setChildrenCollapsible(false);
    m_split->setHandleWidth(5);
    m_grid = new ClipGrid(project, transport, m_split);
    m_grid->setRowHeight(ClipGrid::kRowHeight);
    m_stripsHost = new QWidget(m_split);
    m_stripsHost->setMinimumHeight(262);
    m_split->addWidget(m_grid);
    m_split->addWidget(m_stripsHost);
    m_split->setStretchFactor(0, 0);
    m_split->setStretchFactor(1, 1);
    InfoView::attach(m_split->handle(1), "Clips / Mixer Divider", "Drag it to show more clip slots or more mixer. Sessions remembers where you leave it.");
    connect(m_split, &QSplitter::splitterMoved, this, [this] {
        m_gridHeight = m_grid->height();
        QSettings("Sessions", "Sessions").setValue("session/gridHeight", m_gridHeight);
    });
    m_gridHeight = QSettings("Sessions", "Sessions").value("session/gridHeight", 0).toInt();
    m_stripsHost->installEventFilter(this);
    m_stripsHost->setAcceptDrops(true);   // devices dropped on a mixer strip go on its track
    m_grid->installEventFilter(this);
    lay->addWidget(m_split, 1);

    connect(m_grid, &ClipGrid::trackSelected, this, &SessionView::trackSelected);
    connect(m_grid, &ClipGrid::clipSelected, this, &SessionView::clipSelected);
    connect(m_grid, &ClipGrid::openDeviceView, this, &SessionView::openDeviceView);
    connect(m_grid, &ClipGrid::columnsResized, this, &SessionView::layoutColumns);
    connect(m_grid, &ClipGrid::columnsResized, this, &SessionView::columnsResized);
    // Tracks or returns added, removed or moved: rebuild the strips (they're bound to an index).
    connect(project, &Project::structureChanged, this, [this] {
        QVector<int> ids;
        for (int i = 0; i < m_project->trackCount(); ++i) ids.push_back(m_project->trackAt(i)->id);
        if (ids != m_stripIds) rebuildStrips(); else layoutColumns();
    });
    rebuildStrips();
}

void SessionView::rebuildStrips() {
    for (auto* s : m_strips) { s->hide(); s->deleteLater(); }
    m_strips.clear();
    if (m_main) { m_main->hide(); m_main->deleteLater(); m_main = nullptr; }
    m_stripIds.clear();
    for (int i = 0; i < m_project->trackCount(); ++i) m_stripIds.push_back(m_project->trackAt(i)->id);
    for (int i = 0; i < m_project->trackCount(); ++i) {
        auto* s = new MixerStrip(m_project, m_transport, i, m_stripsHost);
        s->show();
        m_strips.push_back(s);
    }
    m_main = new MainStrip(m_project, m_transport, m_stripsHost);
    m_main->show();
    layoutColumns();
}

bool SessionView::eventFilter(QObject* obj, QEvent* ev) {
    if ((obj == m_stripsHost || obj == m_grid) && ev->type() == QEvent::Resize) placeStrips();
    if (obj == m_stripsHost) {
        // devices dragged over the mixer: the strip's track (the grid shows which)
        const int dx = m_grid->x() - m_stripsHost->x();
        switch (ev->type()) {
        case QEvent::DragEnter:
        case QEvent::DragMove: {
            auto* e = static_cast<QDragMoveEvent*>(ev);
            const devicedrag::Payload pl = devicedrag::parse(e->mimeData());
            if (!pl.valid() || !m_grid->deviceDragOver(e->mimeData(), m_grid->columnAt(int(e->position().x()) - dx))) { e->ignore(); return true; }
            if (pl.source == devicedrag::Payload::Devices) { e->setDropAction(devicedrag::copying(e) ? Qt::CopyAction : Qt::MoveAction); e->accept(); }
            else e->acceptProposedAction();
            return true;
        }
        case QEvent::DragLeave: m_grid->clearDeviceDrop(); return true;
        case QEvent::Drop: {
            auto* e = static_cast<QDropEvent*>(ev);
            const devicedrag::Payload pl = devicedrag::parse(e->mimeData());
            const bool copy = devicedrag::copying(e);
            if (pl.valid() && m_grid->deviceDrop(e->mimeData(), m_grid->columnAt(int(e->position().x()) - dx), copy)) {
                e->setDropAction(pl.source == devicedrag::Payload::Devices && !copy ? Qt::MoveAction : Qt::CopyAction);
                e->accept();
            }
            return true;
        }
        default: break;
        }
    }
    return QWidget::eventFilter(obj, ev);
}

void SessionView::placeStrips() {
    const int h = m_stripsHost->height();
    const int dx = m_grid->x() - m_stripsHost->x();   // both are full-width in the splitter
    for (int i = 0; i < m_strips.size(); ++i) {
        const int w = m_grid->columnWidth(i);
        m_strips[i]->setVisible(w > 0);   // folded into its group
        if (w > 0) m_strips[i]->setGeometry(dx + m_grid->columnLeft(i), 0, w, h);
    }
    if (m_main) m_main->setGeometry(dx + m_grid->columnLeft(int(m_strips.size())), 0, m_grid->columnWidth(int(m_strips.size())), h);
}

void SessionView::resizeEvent(QResizeEvent*) { layoutColumns(); }

void SessionView::layoutColumns() {
    int n = m_project->trackCount();
    if (!n) return;
    m_grid->setMinimumWidth(m_grid->contentWidth());   // narrower windows scroll instead of squeezing
    m_stripsHost->setMinimumWidth(m_grid->contentWidth());
    m_grid->updateGeometry();
    // The divider: where you left it, else enough for every scene (as far as the mixer allows).
    const int total = m_split->height() - m_split->handleWidth();
    if (total > 0) {
        const int want = m_gridHeight > 0 ? m_gridHeight : m_grid->fixedHeight(ClipGrid::kRowHeight);
        const int g = std::clamp(want, m_grid->minimumSizeHint().height(), std::max(m_grid->minimumSizeHint().height(), total - m_stripsHost->minimumHeight()));
        if (g != m_grid->height()) m_split->setSizes({g, total - g});
    }
    placeStrips();
}

} // namespace sessions
