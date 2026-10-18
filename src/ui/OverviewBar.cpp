#include "OverviewBar.h"
#include "InfoView.h"
#include "Preferences.h"
#include "core/Model.h"
#include "core/Transport.h"
#include "core/Theme.h"
#include "core/DeviceSpecs.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QToolTip>
#include <QHelpEvent>
#include <algorithm>
#include <cmath>

namespace sessions {

namespace {
constexpr int kClipWidth = 300;
constexpr int kChainWidth = 520;
}

// ---- the clip ----------------------------------------------------------------------------

ClipOverview::ClipOverview(Project* project, Transport* transport, QWidget* parent)
    : QWidget(parent), m_project(project), m_transport(transport) {
    setCursor(Qt::PointingHandCursor);
    InfoView::attach(this, "Clip Overview", "The selected clip in miniature: its notes or its waveform, where it's playing, and (outlined) what the Clip View shows. "
                                            "Drag sideways to scroll the editor, up or down to zoom it; click to open the Clip View.");
    // repaint for another clip, an edit, or the playhead moving (the strip over the Clip View has no timer of its own)
    connect(project, &Project::selectionChanged, this, qOverload<>(&QWidget::update));
    connect(project, &Project::clipChanged, this, qOverload<>(&QWidget::update));
    connect(project, &Project::structureChanged, this, qOverload<>(&QWidget::update));
    connect(project, &Project::restored, this, qOverload<>(&QWidget::update));
    connect(transport, &Transport::tick, this, [this] { if (m_transport->playing()) update(); });
}

void ClipOverview::setViewport(std::function<bool(double&, double&)> get, std::function<void(double, double)> set) {
    m_getView = std::move(get);
    m_setView = std::move(set);
}

double ClipOverview::spanBeats() const {   // 1.1.1 to the end marker (or past the start marker, when that's later)
    const ClipPtr clip = m_project->selectedClip;
    return clip ? std::max({clip->length, clip->loopEnd(), clip->start + 1, 1.0}) : 4.0;
}

double ClipOverview::beatAt(double x) const { return (x - 1) / std::max(1.0, width() - 2.0) * spanBeats(); }

void ClipOverview::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    m_pressPos = e->position();
    m_moved = false;
    m_dragging = false;
    double f, l;
    if (m_project->selectedClip && m_getView && m_getView(f, l) && l > f) {
        m_first0 = f; m_last0 = l;
        m_pressBeat = beatAt(e->position().x());
        m_dragging = true;
    }
}

void ClipOverview::mouseMoveEvent(QMouseEvent* e) {
    if (!m_dragging || !(e->buttons() & Qt::LeftButton)) return;
    const QPointF d = e->position() - m_pressPos;
    if (!m_moved && d.manhattanLength() <= 2) return;
    m_moved = true;
    // up / down zooms around the beat you pressed on, sideways scrolls: the outline follows the pointer
    const double span0 = m_last0 - m_first0;
    const double span = std::clamp(span0 / PreferencesDialog::dragZoom(d.y()), 0.25, spanBeats() * 4);
    const double frac = (m_pressBeat - m_first0) / span0;
    const double first = m_pressBeat - frac * span + d.x() / std::max(1.0, width() - 2.0) * spanBeats();
    if (m_setView) m_setView(first, first + span);
    update();
}

void ClipOverview::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    const bool wasDrag = m_dragging && m_moved;
    m_dragging = false;
    if (!wasDrag) emit clicked();
}

void ClipOverview::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.fillRect(rect(), theme::inset);
    p.setPen(theme::line);
    p.drawRect(r);
    const ClipPtr clip = m_project->selectedClip;
    if (!clip) {
        p.setPen(theme::faint);
        QFont f = p.font(); f.setPointSizeF(7); p.setFont(f);
        p.drawText(rect().adjusted(6, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft, "No clip selected");
        return;
    }
    const int owner = m_project->trackOfClip(clip);
    const Track* t = m_project->trackAt(owner);
    const QColor ink = t ? m_project->clipColor(clip) : theme::accent;
    const QRectF in = r.adjusted(1, 1, -1, -1);
    const double len = spanBeats();
    if (clip->isAudio()) {
        if (const SamplePtr s = clip->sample) {
            const double mid = in.center().y(), half = in.height() / 2 - 1;
            const int w = int(in.width());
            for (int x = 0; x < w; ++x) {
                float lo, hi;
                s->range(clip->secondsAt(len * x / w), clip->secondsAt(len * (x + 1) / w), lo, hi);
                p.fillRect(QRectF(in.left() + x, mid - hi * half, 1, std::max(1.0, double(hi - lo) * half)), ink);
            }
        } else {
            p.setPen(theme::missingText);
            p.drawText(rect().adjusted(6, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft, "Missing: " + clip->samplePath.section('/', -1));
        }
    } else {
        int lo = 127, hi = 0;
        for (const Note& n : clip->notes) { lo = std::min(lo, n.pitch); hi = std::max(hi, n.pitch); }
        if (hi < lo) { lo = 60; hi = 72; }
        if (hi - lo < 12) { const int c = (hi + lo) / 2; lo = c - 6; hi = c + 6; }
        const double rowH = in.height() / (hi - lo + 1);
        for (const Note& n : clip->notes) {
            if (n.start >= len) continue;
            const double x0 = in.left() + n.start / len * in.width(), x1 = in.left() + std::min(len, n.start + n.length) / len * in.width();
            const double y = in.bottom() - (n.pitch - lo + 1) * rowH;
            p.fillRect(QRectF(x0, y, std::max(1.0, x1 - x0), std::max(1.0, rowH - 0.5)), ink);
        }
    }
    // the loop bars, faint; the brace along the top and the start marker (0.23)
    p.setPen(theme::withAlpha(theme::text, 40));
    for (double b = 4; b < len; b += 4) { const double x = in.left() + b / len * in.width(); p.drawLine(QPointF(x, in.top()), QPointF(x, in.bottom())); }
    {
        const double bx0 = in.left() + std::clamp(clip->loopStart / len, 0.0, 1.0) * in.width(), bx1 = in.left() + std::clamp(clip->loopEnd() / len, 0.0, 1.0) * in.width();
        p.fillRect(QRectF(bx0, in.top(), std::max(1.0, bx1 - bx0), 2), theme::withAlpha(ink, clip->loop ? 220 : 110));
        const double sx = in.left() + std::clamp(clip->start / len, 0.0, 1.0) * in.width();
        p.fillRect(QRectF(sx, in.top(), 1, in.height()), theme::withAlpha(ink, 160));
    }
    // what the editor shows
    double f, l;
    if (m_getView && m_getView(f, l) && l > f) {
        const double x0 = in.left() + std::clamp(f / len, 0.0, 1.0) * in.width(), x1 = in.left() + std::clamp(l / len, 0.0, 1.0) * in.width();
        if (x1 - x0 < in.width() - 1) {
            p.fillRect(QRectF(x0, in.top(), std::max(2.0, x1 - x0), in.height()), theme::overviewView);
            p.setPen(theme::textSoft); p.setBrush(Qt::NoBrush);
            p.drawRect(QRectF(x0, in.top(), std::max(2.0, x1 - x0 - 1), in.height() - 1));
        }
    }
    // the playhead, when this clip plays on its track
    double local = 0;
    if (owner >= 0 && m_project->playingClip(owner, m_transport->beat(), &local) == clip) {
        const double x = in.left() + std::clamp(local / len, 0.0, 1.0) * in.width();
        p.setPen(QPen(theme::playhead, 1));
        p.drawLine(QPointF(x, in.top()), QPointF(x, in.bottom()));
    }
}

// ---- the chain ---------------------------------------------------------------------------

ChainOverview::ChainOverview(Project* project, QWidget* parent) : QWidget(parent), m_project(project) {
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
    InfoView::attach(this, "Device Chain Overview", "The selected track's devices in miniature, in order (a dim one is switched off). "
                                                    "Drag sideways to scroll along the chain; click a device to open the Device View with it selected.");
}

void ChainOverview::refreshThumbs() {
    m_pictures.clear();
    if (!m_thumbs) { update(); return; }
    const int h = height() - 4;
    for (const auto& [id, pm] : m_thumbs()) {
        if (pm.isNull() || pm.height() <= 0 || h <= 0) continue;
        const double k = double(h) / pm.height();
        m_pictures.append({id, pm.scaled(std::max(8, int(std::lround(pm.width() * k))), h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)});
    }
    m_scroll = std::clamp(m_scroll, 0, std::max(0, contentWidth() - width()));
    update();
}

int ChainOverview::contentWidth() const {
    int w = 4;
    const QList<QPair<QRect, int>> bs = blocks();
    for (const auto& b : bs) w = std::max(w, b.first.right() + m_scroll + 3);
    return w;
}

QList<QPair<QRect, int>> ChainOverview::blocks() const {
    QList<QPair<QRect, int>> out;
    const Track* t = m_project->trackAt(m_project->selectedTrack);
    if (!t) return out;
    const int gap = 2;
    if (!m_pictures.isEmpty()) {   // the panels' pictures, at their own widths, along the strip
        int x = 2 - m_scroll;
        for (const auto& [id, pm] : m_pictures) {
            out << qMakePair(QRect(x, 2, pm.width(), height() - 4), id);
            x += pm.width() + gap;
        }
        return out;
    }
    QList<const Device*> top;   // no pictures (no Device View): labelled blocks
    for (int i = 0; i < t->devices.size(); i = deviceBlockEnd(t->devices, i)) top << &t->devices[i];
    if (top.isEmpty()) return out;
    const int avail = width() - 4 - gap * (int(top.size()) - 1);
    const int w = std::clamp(avail / int(top.size()), 24, 96);
    int x = 2 - m_scroll;
    for (const Device* d : top) {
        out << qMakePair(QRect(x, 2, w, height() - 4), d->id);
        x += w + gap;
    }
    return out;
}

int ChainOverview::deviceAt(const QPoint& pos) const {
    for (const auto& b : blocks()) if (b.first.contains(pos)) return b.second;
    return 0;
}

void ChainOverview::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    m_pressPos = e->position();
    m_scroll0 = m_scroll;
    m_moved = false;
    m_dragging = true;
}

void ChainOverview::mouseMoveEvent(QMouseEvent* e) {
    if (!m_dragging || !(e->buttons() & Qt::LeftButton)) return;
    const double dx = e->position().x() - m_pressPos.x();
    if (!m_moved && std::abs(dx) <= 2) return;
    m_moved = true;
    m_scroll = std::clamp(m_scroll0 - int(std::lround(dx)), 0, std::max(0, contentWidth() - width()));   // the chain follows the pointer
    update();
}

void ChainOverview::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    const bool wasDrag = m_dragging && m_moved;
    m_dragging = false;
    if (!wasDrag) emit clicked(deviceAt(e->position().toPoint()));
}

void ChainOverview::wheelEvent(QWheelEvent* e) {
    const int d = e->angleDelta().x() ? e->angleDelta().x() : e->angleDelta().y();
    m_scroll = std::clamp(m_scroll - d / 2, 0, std::max(0, contentWidth() - width()));
    update();
    e->accept();
}

bool ChainOverview::event(QEvent* e) {
    if (e->type() == QEvent::ToolTip) {
        auto* he = static_cast<QHelpEvent*>(e);
        const int id = deviceAt(he->pos());
        const Track* t = m_project->trackAt(m_project->selectedTrack);
        if (id && t) for (const Device& d : t->devices) if (d.id == id) { QToolTip::showText(he->globalPos(), d.name + (d.enabled ? "" : " (off)"), this); return true; }
        QToolTip::hideText();
        return true;
    }
    return QWidget::event(e);
}

void ChainOverview::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.fillRect(rect(), theme::inset);
    p.setPen(theme::line);
    p.drawRect(r);
    const Track* t = m_project->trackAt(m_project->selectedTrack);
    QFont f = p.font(); f.setPointSizeF(6.5); p.setFont(f);
    const QList<QPair<QRect, int>> bs = blocks();
    if (!t || bs.isEmpty()) {
        p.setPen(theme::faint);
        p.drawText(rect().adjusted(6, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft, t ? "No devices" : "");
        return;
    }
    p.setClipRect(rect().adjusted(1, 1, -1, -1));
    const QList<int> selected = m_selected ? m_selected() : QList<int>();
    for (int k = 0; k < bs.size(); ++k) {
        const auto& b = bs[k];
        const Device* d = nullptr;
        for (const Device& x : t->devices) if (x.id == b.second) d = &x;
        if (!d) continue;
        const bool sel = selected.contains(d->id);
        if (!m_pictures.isEmpty()) {   // the panel's picture, dimmed when the device is off
            const QPixmap& pm = m_pictures[k].second;
            p.drawPixmap(b.first.topLeft(), pm);
            if (!d->enabled) p.fillRect(b.first, theme::shade);
            if (sel) { p.setPen(QPen(theme::outline, 1)); p.setBrush(Qt::NoBrush); p.drawRect(QRectF(b.first).adjusted(0.5, 0.5, -0.5, -0.5)); }
            continue;
        }
        QColor fill = d->kind == DeviceKind::Instrument ? theme::withAlpha(theme::accent, 170)
                    : d->kind == DeviceKind::MidiEffect ? theme::devCyanDark : theme::deviceTitle;
        if (!d->enabled) fill = theme::devicePanelOff;
        p.setPen(sel ? theme::outline : theme::line);
        p.setBrush(fill);
        p.drawRoundedRect(QRectF(b.first).adjusted(0.5, 0.5, -0.5, -0.5), 2, 2);
        p.setPen(!d->enabled ? theme::disabled : d->kind == DeviceKind::Instrument ? theme::ink : theme::text);
        QString name = d->name;
        if (isRack(*d)) name += QString(" (%1)").arg(d->chains.size());
        p.drawText(b.first.adjusted(3, 0, -3, 0), Qt::AlignCenter, p.fontMetrics().elidedText(name, Qt::ElideRight, b.first.width() - 6));
    }
    // more to the left or right: a little shadow at the edge
    if (m_scroll > 0) p.fillRect(QRect(1, 1, 6, height() - 2), theme::shade);
    if (contentWidth() - m_scroll > width()) p.fillRect(QRect(width() - 7, 1, 6, height() - 2), theme::shade);
}

// ---- the bar -----------------------------------------------------------------------------

OverviewBar::OverviewBar(Project* project, Transport* transport, QWidget* parent) : QWidget(parent), m_project(project) {
    setObjectName("overviewBar");
    setFixedHeight(kBarHeight);
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 3, 8, 3);
    lay->setSpacing(8);
    m_status = new QLabel(this);
    m_status->setObjectName("overviewStatus");
    lay->addWidget(m_status, 1);
    m_clip = new ClipOverview(project, transport, this);
    m_clip->setFixedSize(kClipWidth, kBarHeight - 6);
    lay->addWidget(m_clip);
    m_chain = new ChainOverview(project, this);
    m_chain->setFixedSize(kChainWidth, kBarHeight - 6);
    lay->addWidget(m_chain);
    connect(project, &Project::structureChanged, this, &OverviewBar::refresh);
    connect(project, &Project::clipChanged, this, &OverviewBar::refresh);
    connect(project, &Project::deviceChanged, this, &OverviewBar::refresh);
    connect(project, &Project::selectionChanged, this, &OverviewBar::refresh);
    connect(project, &Project::restored, this, &OverviewBar::refresh);
    m_timer.setInterval(50);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        const QString key = QString("%1/%2").arg(m_project->selectedTrack).arg(m_project->selectedClip ? m_project->selectedClip->id : 0);
        if (key != m_key) { m_key = key; refresh(); return; }
        if (++m_ticks % 10 == 0 && isVisible()) m_chain->refreshThumbs();   // the panels' pictures, twice a second (knobs move)
        m_clip->update(); m_chain->update();
    });
    m_timer.start();
    refresh();
}

void OverviewBar::refresh() {
    const Track* t = m_project->trackAt(m_project->selectedTrack);
    QString s = t ? t->name : QString();
    if (const ClipPtr c = m_project->selectedClip) {
        const int bars = int(std::ceil(c->length / 4 - 1e-9));
        s += QString("  ·  %1  ·  %2 %3").arg(c->name).arg(bars).arg(bars == 1 ? "bar" : "bars");
        if (!c->isAudio()) s += QString("  ·  %1 %2").arg(c->notes.size()).arg(c->notes.size() == 1 ? "note" : "notes");
    }
    m_status->setText(s);
    m_clip->update();
    m_chain->refreshThumbs();
}

} // namespace sessions
