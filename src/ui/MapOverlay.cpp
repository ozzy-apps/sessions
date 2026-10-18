#include "MapOverlay.h"
#include "SessionView.h"
#include "core/Mapping.h"
#include "core/Model.h"
#include <QPainter>
#include <QEvent>

namespace sessions {

MapOverlay::MapOverlay(Project* project, Mapper* mapper, ClipGrid* grid, QWidget* parent)
    : QWidget(parent), m_project(project), m_mapper(mapper), m_grid(grid) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    parent->installEventFilter(this);
    setGeometry(parent->rect());
    connect(mapper, &Mapper::changed, this, qOverload<>(&QWidget::update));
    m_refresh.setInterval(100);
    connect(&m_refresh, &QTimer::timeout, this, qOverload<>(&QWidget::update));
    hide();
}

bool MapOverlay::eventFilter(QObject* obj, QEvent* ev) {
    if (obj == parentWidget() && ev->type() == QEvent::Resize) setGeometry(parentWidget()->rect());
    return QWidget::eventFilter(obj, ev);
}

QVector<MapOverlay::Target> MapOverlay::targets() const {
    QVector<Target> out;
    QWidget* root = parentWidget();
    for (QWidget* w : root->findChildren<QWidget*>()) {
        const QString t = w->property("mapTarget").toString();
        if (t.isEmpty() || !w->isVisible()) continue;
        const QRect vis = w->visibleRegion().boundingRect();
        if (vis.isEmpty()) continue;
        out.push_back({QRect(w->mapTo(root, vis.topLeft()), vis.size()), t});
    }
    if (m_grid && m_grid->isVisible()) {
        const QRect gridVis = m_grid->visibleRegion().boundingRect();
        for (const auto& cell : m_grid->mapCells()) {
            const QRect r = cell.first.intersected(gridVis);
            if (!r.isEmpty()) out.push_back({QRect(m_grid->mapTo(root, r.topLeft()), r.size()), cell.second});
        }
    }
    return out;
}

void MapOverlay::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const bool keys = m_mapper->mode() == Mapper::Mode::Key;
    const QColor tint = keys ? QColor(255, 140, 40, 70) : QColor(60, 120, 240, 80);
    const QColor edge = keys ? QColor(255, 160, 70) : QColor(110, 160, 255);
    QFont f = font(); f.setPointSizeF(7); f.setBold(true); p.setFont(f);
    for (const Target& t : targets()) {
        const bool sel = t.target == m_mapper->selected();
        p.fillRect(t.rect, sel ? QColor(255, 255, 255, 70) : tint);
        p.setPen(QPen(sel ? Qt::white : edge, sel ? 2 : 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(t.rect.adjusted(0, 0, -1, -1));
        QStringList labels;
        for (const Mapping& m : m_mapper->mappingsFor(t.target))
            if ((m.source == Mapping::Key) == keys) labels << mapping::sourceText(m);
        if (labels.isEmpty()) continue;
        const QString text = labels.join(", ");
        const QRect label(t.rect.left() + 1, t.rect.top() + 1, std::min(t.rect.width() - 2, p.fontMetrics().horizontalAdvance(text) + 6), 12);
        p.fillRect(label, keys ? QColor(150, 70, 0) : QColor(20, 50, 130));
        p.setPen(Qt::white);
        p.drawText(label, Qt::AlignCenter, p.fontMetrics().elidedText(text, Qt::ElideRight, label.width() - 2));
    }
}

} // namespace sessions
