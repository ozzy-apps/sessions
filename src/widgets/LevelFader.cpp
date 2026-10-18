#include "LevelFader.h"
#include "core/Theme.h"
#include <QPainter>
#include <QLinearGradient>
#include <algorithm>
#include <cmath>

namespace sessions {

namespace {
constexpr int kTop = 6, kBottom = 6;     // room for the scale's first and last labels
constexpr int kMeterW = 7, kMarkerW = 7;
}

LevelFader::LevelFader(QWidget* parent) : DragValue(parent) {
    setFixedWidth(36);
    setMinimumHeight(70);
    setMaximumHeight(16777215);
    setRange(0, 1);
    setDefault(0.75);
    setDragSensitivity(200);
}

double LevelFader::meterPos(double db) { return std::clamp((db + 60.0) / 63.15, 0.0, 1.0); }

void LevelFader::setLevel(double v) {
    v = std::clamp(v, 0.0, 1.0);
    if (std::abs(v - m_level) < 0.004 && m_peakHold <= 0) return;
    m_level = v;
    if (v >= m_peak) { m_peak = v; m_peakHold = 40; }
    else if (--m_peakHold < 0) m_peak = std::max(0.0, m_peak - 0.01);
    update();
}

void LevelFader::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const int h = height() - kTop - kBottom;
    auto yAt = [&](double pos) { return kTop + int(std::round((1.0 - pos) * h)); };

    // meter
    const QRect m(0, kTop, kMeterW, h);
    p.fillRect(m, theme::meterBg);
    QLinearGradient g(0, m.bottom(), 0, m.top());
    g.setColorAt(0.0, QColor("#4fc04f"));
    g.setColorAt(0.70, QColor("#5fd35a"));
    g.setColorAt(0.86, QColor("#e6d34a"));
    g.setColorAt(1.0, QColor("#f0603e"));
    const int lh = int(m_level * h);
    p.fillRect(QRect(m.left() + 1, m.bottom() + 1 - lh, kMeterW - 2, lh), g);
    if (m_peak > 0.01) p.fillRect(QRect(m.left() + 1, yAt(m_peak), kMeterW - 2, 1), m_peak > 0.95 ? theme::peakClip : theme::peakHold);

    // scale: every 12 dB, written without the minus (0 at the top)
    QFont f = font(); f.setPointSizeF(6.5); p.setFont(f);
    p.setPen(theme::textFaint);
    const int sx = kMeterW + kMarkerW + 3;
    for (int db = 0; db >= -60; db -= 12) {
        const int y = yAt(meterPos(db));
        p.drawText(QRect(sx, y - 6, width() - sx, 12), Qt::AlignLeft | Qt::AlignVCenter, QString::number(-db));
    }

    // the fader: a marker pointing at the meter
    const double v = value();
    const double db = v < 0.002 ? -100.0 : 40.0 * std::log10(v / 0.75);
    const int y = v < 0.002 ? m.bottom() : yAt(meterPos(db));
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(hasFocus() ? theme::outline : theme::levelMarker);
    QPolygonF tri;
    tri << QPointF(kMeterW + 0.5, y + 0.5) << QPointF(kMeterW + kMarkerW, y - 4.5) << QPointF(kMeterW + kMarkerW, y + 5.5);
    p.drawPolygon(tri);
}

} // namespace sessions
