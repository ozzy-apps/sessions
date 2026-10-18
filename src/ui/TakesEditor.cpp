#include "TakesEditor.h"
#include "InfoView.h"
#include "GridShade.h"
#include "core/Model.h"
#include "core/Theme.h"
#include "core/Transport.h"
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace sessions {

TakesEditor::TakesEditor(Project* project, Transport* transport, QWidget* parent)
    : QWidget(parent), m_project(project), m_transport(transport) {
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumHeight(120);
    InfoView::attach(this, "Takes",
        "Every pass of a loop recording, newest on top; the Comp lane is what the clip plays. Drag across a take to use that stretch of it "
        "(Alt: off the grid), click for one grid step, double-click for the whole take. The joins crossfade; the recordings themselves stay as they are.");
    connect(transport, &Transport::tick, this, [this] { if (m_transport->playing() && m_clip) update(); });
    connect(project, &Project::clipChanged, this, qOverload<>(&QWidget::update));
    connect(project, &Project::restored, this, [this] { m_dragTake = -1; update(); });
}

void TakesEditor::setClip(ClipPtr clip) {
    m_clip = clip && clip->isAudio() ? clip : nullptr;
    m_dragTake = -1;
    m_firstLane = 0;
    fit();
    update();
}

void TakesEditor::resizeEvent(QResizeEvent*) { fit(); }

void TakesEditor::fit() {
    if (!m_clip) return;
    m_offset = 0;
    m_zoom = std::max(2.0, (width() - kLeft - 12) / std::max(0.25, m_clip->length + 0.25));
}

double TakesEditor::grid() const { return m_zoom > 60 ? 0.25 : m_zoom > 15 ? 1.0 : 4.0; }
double TakesEditor::snap(double beat) const { return m_free ? beat : std::round(beat / grid()) * grid(); }

int TakesEditor::laneCount() const { return m_clip ? 1 + int(m_clip->takes.size()) : 0; }

QRect TakesEditor::laneRect(int lane) const {
    if (!m_clip || lane < 0 || lane >= laneCount()) return {};
    if (lane == 0) return QRect(0, kRulerH, width(), kCompH);
    const int row = lane - 1 - m_firstLane;
    if (row < 0) return {};
    const int y = kRulerH + kCompH + 2 + row * (kLaneH + 1);
    if (y >= height()) return {};
    return QRect(0, y, width(), kLaneH);
}

int TakesEditor::laneAt(const QPoint& p) const {
    for (int lane = 0; lane < laneCount(); ++lane) if (laneRect(lane).contains(p)) return lane;
    return -1;
}

int TakesEditor::takeOfLane(int lane) const { return m_clip && lane >= 1 && lane < laneCount() ? int(m_clip->takes.size()) - lane : -1; }
int TakesEditor::laneOfTake(int take) const { return m_clip && take >= 0 && take < m_clip->takes.size() ? int(m_clip->takes.size()) - take : -1; }

QColor TakesEditor::takeColor(const QColor& track, int take) {
    QColor c = track.toHsv();
    const int hue = c.hsvHue() < 0 ? 30 : c.hsvHue();
    c.setHsv((hue + take * 47) % 360, std::clamp(c.hsvSaturation(), 120, 230), std::clamp(c.value(), 170, 240));
    return c;
}

double TakesEditor::playheadBeat() const {
    if (!m_clip || !m_transport->playing()) return -1;
    return m_project->clipPosition(m_clip, m_transport->beat());   // the start marker and the brace (0.23)
}

// One take's waveform between two clip beats, drawn into `lane` (its file begins at the take's own clip beat).
void TakesEditor::drawTake(QPainter& p, int take, const QRect& lane, double fromBeat, double toBeat, const QColor& color, bool bright) const {
    if (!m_clip || take < 0 || take >= m_clip->takes.size()) return;
    const SamplePtr smp = m_clip->takes[take].sample;
    if (!smp) return;
    const double dur = smp->seconds();
    const int mid = lane.center().y(), half = lane.height() / 2 - 4;
    QColor wave = color;
    wave.setAlphaF(bright ? 0.95 : 0.3);
    const int x0 = std::max(kLeft, int(std::floor(beatToX(fromBeat)))), x1 = std::min(width(), int(std::ceil(beatToX(toBeat))));
    for (int x = x0; x < x1; ++x) {
        const double s0 = m_clip->takeSeconds(take, xToBeat(x)), s1 = m_clip->takeSeconds(take, xToBeat(x + 1));
        if (s1 < 0 || s0 > dur) continue;
        float lo, hi;
        smp->range(std::max(0.0, std::min(s0, s1)), std::max(s0, s1), lo, hi);
        const int y0 = mid - int(hi * half), y1 = mid - int(lo * half);
        p.fillRect(QRect(x, y0, 1, std::max(1, y1 - y0 + 1)), wave);
    }
}

void TakesEditor::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const int W = width(), H = height();
    p.fillRect(rect(), theme::editorBg);
    if (!m_clip || m_clip->takes.isEmpty()) {
        p.setPen(theme::dim);
        p.drawText(rect(), Qt::AlignCenter, m_clip ? "No takes: record this clip in a loop with Arrangement Record on" : "No audio clip");
        return;
    }
    const int owner = m_project->trackOfClip(m_clip);
    const QColor col = owner >= 0 ? m_project->clipColor(m_clip) : theme::accent;
    const double xEnd = beatToX(m_clip->length);

    // ruler: bars and beats, the clip's length in the track colour
    p.fillRect(QRect(0, 0, W, kRulerH), theme::ruler);
    p.fillRect(QRectF(beatToX(0), 0, xEnd - beatToX(0), 3), col);
    const double step = m_zoom > 60 ? 1 : m_zoom > 15 ? 4 : 16;
    QFont small = font(); small.setPointSizeF(7.5); p.setFont(small);
    p.setPen(theme::rulerText);
    gridshade::paint(p, m_zoom, [this](double b) { return beatToX(b); }, m_offset, xToBeat(W), QRectF(0, kRulerH, W, H - kRulerH));
    for (double b = std::ceil(m_offset / step) * step; beatToX(b) < W; b += step) {
        const double x = beatToX(b);
        p.fillRect(QRectF(x, kRulerH - 5, 1, 5), theme::rulerText);
        p.fillRect(QRectF(x, kRulerH, 1, H - kRulerH), std::fmod(b, 4.0) == 0 ? theme::barLine : theme::beatLine);
        if (std::fmod(b, 4.0) == 0) p.drawText(QPointF(x + 3, kRulerH - 5), QString::number(int(b / 4) + 1));
        else if (step == 1) p.drawText(QPointF(x + 3, kRulerH - 5), QString("%1.%2").arg(int(std::floor(b / 4)) + 1).arg(int(std::fmod(b, 4.0)) + 1));
    }

    // the comp lane: each stretch from the take it plays
    const QRect comp = laneRect(0);
    p.fillRect(comp, theme::compLane);
    p.fillRect(QRect(0, comp.top(), kLeft - 1, comp.height()), theme::compHead);
    p.setPen(theme::text);
    p.drawText(QRect(6, comp.top(), kLeft - 10, comp.height()), Qt::AlignVCenter | Qt::AlignLeft, "Comp");
    p.fillRect(QRect(kLeft, comp.center().y(), W - kLeft, 1), theme::centreLine);
    {
        double at = 0;
        while (at < m_clip->length - 1e-9) {
            const int k = m_clip->takeAt(at);
            double end = m_clip->length;
            for (const CompRegion& r : m_clip->comp) {   // the end of this stretch: where the take changes
                if (r.start > at + 1e-9) end = std::min(end, r.start);
                if (r.start <= at + 1e-9 && r.end > at + 1e-9) end = std::min(end, r.end);
            }
            if (end <= at + 1e-9) break;
            const QColor tc = takeColor(col, k);
            QColor band = tc; band.setAlphaF(0.18);
            p.fillRect(QRectF(beatToX(at), comp.top() + 1, beatToX(end) - beatToX(at), comp.height() - 2), band);
            drawTake(p, k, comp, at, end, tc, true);
            p.fillRect(QRectF(beatToX(end), comp.top(), 1, comp.height()), theme::line);
            at = end;
        }
    }
    p.fillRect(QRect(0, comp.bottom() + 1, W, 2), theme::line);

    // the take lanes, newest first
    for (int lane = 1; lane < laneCount(); ++lane) {
        const QRect r = laneRect(lane);
        if (r.isEmpty()) continue;
        const int k = takeOfLane(lane);
        const Take& t = m_clip->takes[k];
        const QColor tc = takeColor(col, k);
        p.fillRect(r, lane % 2 ? theme::takeLaneA : theme::takeLaneB);
        p.fillRect(QRect(0, r.top(), kLeft - 1, r.height()), theme::takeHead);
        p.fillRect(QRect(0, r.top(), 3, r.height()), tc);
        p.setPen(theme::text);
        p.setFont(font());
        p.drawText(QRect(8, r.top(), kLeft - 12, r.height()), Qt::AlignVCenter | Qt::AlignLeft, t.name);
        p.fillRect(QRect(kLeft, r.center().y(), W - kLeft, 1), theme::centreLine);
        if (!t.sample) {
            p.setPen(theme::record);
            p.drawText(QRect(kLeft + 6, r.top(), W - kLeft - 12, r.height()), Qt::AlignVCenter | Qt::AlignLeft, "Missing: " + t.samplePath);
        } else {
            // the stretches the comp uses, bright; the rest dim
            double at = 0;
            while (at < m_clip->length - 1e-9) {
                const int using_ = m_clip->takeAt(at);
                double end = m_clip->length;
                for (const CompRegion& c : m_clip->comp) {
                    if (c.start > at + 1e-9) end = std::min(end, c.start);
                    if (c.start <= at + 1e-9 && c.end > at + 1e-9) end = std::min(end, c.end);
                }
                if (end <= at + 1e-9) break;
                if (using_ == k) { QColor band = tc; band.setAlphaF(0.14); p.fillRect(QRectF(beatToX(at), r.top() + 1, beatToX(end) - beatToX(at), r.height() - 2), band); }
                drawTake(p, k, r, at, end, tc, using_ == k);
                at = end;
            }
        }
        if (m_dragTake == k) {   // the stretch being chosen
            const double a = std::min(m_dragA, m_dragB), b = std::max(m_dragA, m_dragB);
            p.fillRect(QRectF(beatToX(a), r.top(), std::max(2.0, beatToX(b) - beatToX(a)), r.height()), theme::takeChosen);
        }
        p.fillRect(QRect(0, r.bottom() + 1, W, 1), theme::line);
    }
    if (xEnd < W) p.fillRect(QRectF(xEnd, kRulerH, W - xEnd, H - kRulerH), theme::outOfClip);

    // more lanes than fit: a slim position bar on the right
    const int takes = int(m_clip->takes.size());
    const int fit = std::max(1, (H - kRulerH - kCompH - 2) / (kLaneH + 1));
    if (takes > fit) {
        const QRect area(W - 5, kRulerH + kCompH + 2, 4, H - kRulerH - kCompH - 2);
        p.fillRect(area, theme::inset);
        const double frac = double(fit) / takes;
        p.fillRect(QRectF(area.x(), area.y() + area.height() * double(m_firstLane) / takes, 4, std::max(8.0, area.height() * frac)), theme::inactive);
    }

    // playhead
    const double ph = playheadBeat();
    if (ph >= 0) p.fillRect(QRectF(beatToX(ph), kRulerH, 1, H - kRulerH), theme::playhead);
}

void TakesEditor::useTake(double start, double end, int take, const QString& label) {
    if (!m_clip) return;
    EditScope edit(m_project, label);   // every stretch is its own undo step
    m_clip->useTake(start, end, take);
    emit m_project->clipChanged();
}

void TakesEditor::mousePressEvent(QMouseEvent* e) {
    if (!m_clip) return;
    const int lane = laneAt(e->pos());
    const int take = takeOfLane(lane);
    if (e->button() == Qt::RightButton) {
        QMenu menu(this);
        QAction* whole = menu.addAction(take >= 0 ? "Use Whole Take (" + m_clip->takes[take].name + ")" : "Use Whole Take");
        whole->setEnabled(take >= 0);
        QAction* newest = menu.addAction("Newest Take Everywhere");
        newest->setEnabled(!m_clip->comp.isEmpty());
        QAction* picked = menu.exec(e->globalPosition().toPoint());
        if (!picked || !m_clip) return;
        EditScope edit(m_project, picked == whole ? "Use Whole Take" : "Newest Take Everywhere");
        if (picked == whole) m_clip->useWholeTake(take); else m_clip->comp.clear();
        emit m_project->clipChanged();
        return;
    }
    if (e->button() != Qt::LeftButton || take < 0 || e->pos().x() < kLeft) return;
    m_free = e->modifiers() & Qt::AltModifier;
    m_dragTake = take;
    m_dragA = m_dragB = std::clamp(snap(xToBeat(e->pos().x())), 0.0, m_clip->length);
    update();
}

void TakesEditor::mouseMoveEvent(QMouseEvent* e) {
    if (m_dragTake < 0 || !(e->buttons() & Qt::LeftButton) || !m_clip) return;
    m_dragB = std::clamp(snap(xToBeat(e->pos().x())), 0.0, m_clip->length);
    update();
}

void TakesEditor::mouseReleaseEvent(QMouseEvent* e) {
    if (m_dragTake < 0 || e->button() != Qt::LeftButton || !m_clip) { m_dragTake = -1; return; }
    double a = std::min(m_dragA, m_dragB), b = std::max(m_dragA, m_dragB);
    if (b - a < 1e-9) {   // a click: one grid step from here
        a = std::clamp(std::floor(xToBeat(e->pos().x()) / grid()) * grid(), 0.0, m_clip->length);
        b = std::min(m_clip->length, a + grid());
    }
    const int take = m_dragTake;
    m_dragTake = -1;
    useTake(a, b, take, "Comp from " + m_clip->takes[take].name);
    update();
}

void TakesEditor::mouseDoubleClickEvent(QMouseEvent* e) {
    if (!m_clip || e->button() != Qt::LeftButton) return;
    const int take = takeOfLane(laneAt(e->pos()));
    if (take < 0) return;
    m_dragTake = -1;
    EditScope edit(m_project, "Use Whole Take");
    m_clip->useWholeTake(take);
    emit m_project->clipChanged();
    update();
}

void TakesEditor::wheelEvent(QWheelEvent* e) {
    if (!m_clip) return;
    const int takes = int(m_clip->takes.size());
    const int fit = std::max(1, (height() - kRulerH - kCompH - 2) / (kLaneH + 1));
    const int dir = e->angleDelta().y() < 0 ? 1 : e->angleDelta().y() > 0 ? -1 : 0;
    m_firstLane = std::clamp(m_firstLane + dir, 0, std::max(0, takes - fit));
    e->accept();
    update();
}

} // namespace sessions
