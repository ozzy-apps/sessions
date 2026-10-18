#include "EnvelopeEditor.h"
#include "InfoView.h"
#include "GridShade.h"
#include "core/Model.h"
#include "core/Theme.h"
#include "core/Transport.h"
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <algorithm>
#include <cmath>

namespace sessions {

namespace {
const QColor& kEnv = theme::envelope;      // clip envelopes: red
const QColor& kEnvOff = theme::envelopeOff;
}

EnvelopeEditor::EnvelopeEditor(Project* project, Transport* transport, QWidget* parent)
    : QWidget(parent), m_project(project), m_transport(transport), m_target(target::volume()) {
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumHeight(120);
    InfoView::attach(this, "Clip Envelope",
        "While the clip plays, this line sets the parameter. Click to add a breakpoint (Alt: off the grid) and drag it; "
        "double-click one or press Delete to remove it. The envelope loops with the clip.");
    connect(transport, &Transport::tick, this, [this] { if (m_transport->playing() && m_clip && isVisible()) update(); });
    connect(project, &Project::clipChanged, this, [this] { fit(); update(); });
    connect(project, &Project::restored, this, [this] { m_sel = -1; m_dragging = false; m_editOpen = false; update(); });
}

void EnvelopeEditor::setClip(ClipPtr clip) {
    if (m_editOpen) { m_project->endEdit(); m_editOpen = false; }
    m_clip = clip;
    m_sel = -1;
    m_dragging = false;
    fit();
    update();
}

void EnvelopeEditor::setTarget(const QString& target) {
    if (target == m_target) return;
    if (m_editOpen) { m_project->endEdit(); m_editOpen = false; }
    m_target = target;
    m_sel = -1;
    update();
}

void EnvelopeEditor::resizeEvent(QResizeEvent*) { fit(); }

void EnvelopeEditor::fit() {
    if (!m_clip) return;
    m_zoom = std::max(2.0, (width() - kLeft - 12) / std::max(0.25, m_clip->length));
}

double EnvelopeEditor::grid() const { return m_zoom > 60 ? 0.25 : m_zoom > 15 ? 1.0 : 4.0; }

double EnvelopeEditor::snap(double beat, bool free) const {
    if (free) return beat;
    return std::round(beat / grid()) * grid();
}

double EnvelopeEditor::valueToY(double v) const {
    const int top = kRulerH + 8, h = height() - kRulerH - 16;
    return top + (1.0 - std::clamp(v, 0.0, 1.0)) * h;
}

double EnvelopeEditor::yToValue(double y) const {
    const int top = kRulerH + 8, h = height() - kRulerH - 16;
    return std::clamp(1.0 - (y - top) / std::max(1, h), 0.0, 1.0);
}

const Envelope* EnvelopeEditor::envelope() const { return m_clip ? std::as_const(*m_clip).envelope(m_target) : nullptr; }

int EnvelopeEditor::pointAt(const QPointF& p) const {
    const Envelope* e = envelope();
    if (!e) return -1;
    for (int i = 0; i < e->points.size(); ++i)
        if (std::abs(beatToX(e->points[i].beat) - p.x()) <= 5 && std::abs(valueToY(e->points[i].value) - p.y()) <= 5) return i;
    return -1;
}

double EnvelopeEditor::playheadBeat() const {
    if (!m_clip || !m_transport->playing()) return -1;
    return m_project->clipPosition(m_clip, m_transport->beat());   // the start marker and the brace (0.23)
}

void EnvelopeEditor::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const int W = width(), H = height();
    p.fillRect(rect(), theme::editorBg);
    if (!m_clip) {
        p.setPen(theme::dim);
        p.drawText(rect(), Qt::AlignCenter, "No clip");
        return;
    }
    const int owner = m_project->trackOfClip(m_clip);
    const QColor col = owner >= 0 ? m_project->clipColor(m_clip) : theme::accent;
    const int top = kRulerH;

    // ruler and beat grid; past the clip's end is shaded
    p.fillRect(QRect(0, 0, W, kRulerH), theme::ruler);
    const double xEnd = beatToX(m_clip->length);
    p.fillRect(QRectF(beatToX(0), 0, xEnd - beatToX(0), 3), col);
    const double step = m_zoom > 60 ? 1 : m_zoom > 15 ? 4 : 16;
    QFont small = font(); small.setPointSizeF(7.5); p.setFont(small);
    p.setPen(theme::rulerText);
    gridshade::paint(p, m_zoom, [this](double b) { return beatToX(b); }, 0, xToBeat(W), QRectF(0, top, W, H - top));
    for (double b = 0; beatToX(b) < W; b += step) {
        const double x = beatToX(b);
        p.fillRect(QRectF(x, kRulerH - 5, 1, 5), theme::rulerText);
        p.fillRect(QRectF(x, top, 1, H - top), std::fmod(b, 4.0) == 0 ? theme::barLine : theme::beatLine);
        if (std::fmod(b, 4.0) == 0) p.drawText(QPointF(x + 3, kRulerH - 5), QString::number(int(b / 4) + 1));
        else if (step == 1) p.drawText(QPointF(x + 3, kRulerH - 5), QString("%1.%2").arg(int(std::floor(b / 4)) + 1).arg(int(std::fmod(b, 4.0)) + 1));
    }
    for (double v : {0.0, 0.5, 1.0}) p.fillRect(QRectF(0, valueToY(v), W, 1), theme::envGrid);
    if (xEnd < W) p.fillRect(QRectF(xEnd, top, W - xEnd, H - top), theme::outOfClip);

    // the envelope, or the track's own value as a dashed line when there's none yet
    const Envelope* e = envelope();
    p.setRenderHint(QPainter::Antialiasing, true);
    if (!e || e->points.isEmpty()) {
        const double y = valueToY(owner >= 0 ? m_project->targetValue(owner, m_target) : 0.5);
        p.setPen(QPen(kEnvOff, 1, Qt::DashLine));
        p.drawLine(QPointF(0, y), QPointF(W, y));
        p.setPen(theme::dim);
        p.drawText(QRect(kLeft, top + 4, W - 2 * kLeft, 16), Qt::AlignLeft | Qt::AlignTop,
                   "No envelope yet: click to add a breakpoint.");
    } else {
        QPainterPath line;
        line.moveTo(0, valueToY(e->valueAt(xToBeat(0))));
        for (const AutoPoint& pt : e->points) line.lineTo(beatToX(pt.beat), valueToY(pt.value));
        line.lineTo(W, valueToY(e->valueAt(xToBeat(W))));
        QPainterPath area = line;
        area.lineTo(W, H);
        area.lineTo(0, H);
        area.closeSubpath();
        QColor fill = kEnv; fill.setAlpha(36);
        p.fillPath(area, fill);
        p.setPen(QPen(kEnv, 1.5));
        p.drawPath(line);
        for (int i = 0; i < e->points.size(); ++i) {
            p.setBrush(i == m_sel ? theme::outline : kEnv);
            p.setPen(QPen(theme::breakpointEdge, 1));
            p.drawEllipse(QPointF(beatToX(e->points[i].beat), valueToY(e->points[i].value)), 3.5, 3.5);
        }
    }
    p.setRenderHint(QPainter::Antialiasing, false);

    const double ph = playheadBeat();
    if (ph >= 0) p.fillRect(QRectF(beatToX(ph), top, 1, H - top), theme::playhead);

    // which parameter, top right
    p.setFont(font());
    p.setPen(theme::textFaint);
    p.drawText(QRect(kLeft, top + 2, W - 2 * kLeft, 16), Qt::AlignRight | Qt::AlignTop,
               owner >= 0 ? m_project->targetName(owner, m_target) : QString());
}

void EnvelopeEditor::mousePressEvent(QMouseEvent* e) {
    if (!m_clip || e->button() != Qt::LeftButton || e->position().y() < kRulerH) return;
    const QPointF pos = e->position();
    const bool free = e->modifiers() & (Qt::AltModifier | Qt::MetaModifier);
    m_project->beginEdit("Edit Clip Envelope");
    m_editOpen = true;
    m_sel = pointAt(pos);
    if (m_sel < 0) {   // a new breakpoint where you clicked
        Envelope* env = m_clip->envelope(m_target);
        if (!env) { m_clip->envelopes.push_back({m_target, {}}); env = &m_clip->envelopes.last(); }
        const AutoPoint pt{std::clamp(snap(xToBeat(pos.x()), free), 0.0, m_clip->length), yToValue(pos.y())};
        auto it = std::upper_bound(env->points.begin(), env->points.end(), pt.beat, [](double b, const AutoPoint& q) { return b < q.beat; });
        m_sel = int(it - env->points.begin());
        env->points.insert(m_sel, pt);
        emit m_project->clipChanged();
    }
    m_dragging = true;
    update();
}

void EnvelopeEditor::mouseMoveEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    if (!m_dragging) { setCursor(pointAt(pos) >= 0 ? Qt::SizeAllCursor : Qt::CrossCursor); return; }
    Envelope* env = m_clip ? m_clip->envelope(m_target) : nullptr;
    if (!env || m_sel < 0 || m_sel >= env->points.size()) return;
    const bool free = e->modifiers() & (Qt::AltModifier | Qt::MetaModifier);
    const double lo = m_sel > 0 ? env->points[m_sel - 1].beat : 0.0;
    const double hi = m_sel + 1 < env->points.size() ? env->points[m_sel + 1].beat : m_clip->length;
    env->points[m_sel].beat = std::clamp(snap(xToBeat(pos.x()), free), lo, hi);   // breakpoints keep their order
    env->points[m_sel].value = yToValue(pos.y());
    emit m_project->clipChanged();
}

void EnvelopeEditor::mouseReleaseEvent(QMouseEvent*) {
    m_dragging = false;
    if (m_editOpen) { m_editOpen = false; m_project->endEdit(); }
}

void EnvelopeEditor::mouseDoubleClickEvent(QMouseEvent* e) {
    if (const int i = pointAt(e->position()); i >= 0) removePoint(i);
}

// Right-click (0.22): the breakpoint under the pointer, or the whole envelope.
void EnvelopeEditor::contextMenuEvent(QContextMenuEvent* e) {
    if (!m_clip || m_target.isEmpty()) return;
    const int i = pointAt(e->pos());
    const Envelope* env = envelope();
    QMenu menu(this);
    if (i >= 0) {
        m_sel = i; update();
        menu.addAction("Delete Breakpoint\tDel", this, [this, i] { removePoint(i); });
    } else {
        menu.addAction("Add Breakpoint Here", this, [this, e] {
            const double b = std::clamp(snap(xToBeat(e->pos().x()), e->modifiers() & Qt::AltModifier), 0.0, m_clip->length);
            EditScope edit(m_project, "Add Breakpoint");
            Envelope* ev = m_clip->envelope(m_target);
            if (!ev) { m_clip->envelopes.push_back({m_target, {}}); ev = &m_clip->envelopes.last(); }
            const AutoPoint pt{b, std::clamp(yToValue(e->pos().y()), 0.0, 1.0)};
            auto it = std::upper_bound(ev->points.begin(), ev->points.end(), pt.beat, [](double x, const AutoPoint& q) { return x < q.beat; });
            m_sel = int(it - ev->points.begin());
            ev->points.insert(m_sel, pt);
            emit m_project->clipChanged();
            update();
        });
    }
    menu.addSeparator();
    menu.addAction("Clear Envelope", this, [this] {
        EditScope edit(m_project, "Clear Envelope");
        m_clip->envelopes.erase(std::remove_if(m_clip->envelopes.begin(), m_clip->envelopes.end(), [&](const Envelope& x) { return x.target == m_target; }), m_clip->envelopes.end());
        m_sel = -1;
        emit m_project->clipChanged();
        update();
    })->setEnabled(env && !env->points.isEmpty());
    menu.exec(e->globalPos());
}

void EnvelopeEditor::keyPressEvent(QKeyEvent* e) {
    if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && m_sel >= 0) { removePoint(m_sel); return; }
    QWidget::keyPressEvent(e);
}

void EnvelopeEditor::removePoint(int i) {
    Envelope* env = m_clip ? m_clip->envelope(m_target) : nullptr;
    if (!env || i < 0 || i >= env->points.size()) return;
    EditScope edit(m_project, "Delete Envelope Point");
    env->points.removeAt(i);
    if (env->points.isEmpty())   // the last one: the parameter goes back to the track's value
        m_clip->envelopes.removeIf([this](const Envelope& x) { return x.target == m_target; });
    m_sel = -1;
    emit m_project->clipChanged();
}

} // namespace sessions
