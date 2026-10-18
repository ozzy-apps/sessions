#include "Meter.h"
#include "core/Theme.h"
#include <QPainter>
#include <QLinearGradient>
#include <algorithm>

namespace sessions {

Meter::Meter(QWidget* parent) : QWidget(parent) {
    setFixedWidth(6);
    setMinimumHeight(40);
}

void Meter::setLevel(double v) {
    v = std::clamp(v, 0.0, 1.0);
    if (std::abs(v - m_level) < 0.004 && m_peakHold <= 0) return;
    m_level = v;
    if (v >= m_peak) { m_peak = v; m_peakHold = 40; }
    else if (--m_peakHold < 0) m_peak = std::max(0.0, m_peak - 0.01);
    update();
}

void Meter::paintEvent(QPaintEvent*) {
    QPainter p(this);
    int h = height();
    p.fillRect(rect(), theme::meterBg2);
    QLinearGradient g(0, h, 0, 0);
    g.setColorAt(0.0, QColor("#58c45a"));
    g.setColorAt(0.65, QColor("#58c45a"));
    g.setColorAt(0.82, QColor("#e6d34a"));
    g.setColorAt(1.0, QColor("#f0603e"));
    int lh = int(m_level * h);
    p.fillRect(QRect(0, h - lh, width(), lh), g);
    if (m_peak > 0.01) {
        int py = h - int(m_peak * h);
        p.fillRect(QRect(0, py, width(), 1), m_peak > 0.95 ? theme::peakClip : theme::peakHold);
    }
}

} // namespace sessions
