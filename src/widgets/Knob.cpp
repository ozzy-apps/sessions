#include "Knob.h"
#include "core/Theme.h"
#include <QPainter>
#include <cmath>

namespace sessions {

Knob::Knob(QWidget* parent, int diameter) : DragValue(parent), m_d(diameter) {
    setFixedSize(m_d, m_d);
    setDragSensitivity(140);
}

void Knob::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QRectF r = rect().adjusted(2, 2, -2, -2);
    const double span = 270.0;
    const double startAngle = -135.0;   // degrees, 0 = up, clockwise positive

    // Qt arcs: 0° at 3 o'clock, counter-clockwise, in 1/16 degree.
    auto qtAngle = [](double clockDeg) { return int((90 - clockDeg) * 16); };

    p.setPen(QPen(theme::knobArc, 2.5, Qt::SolidLine, Qt::FlatCap));
    p.drawArc(r, qtAngle(startAngle), -int(span * 16));

    double n = normalized();
    p.setPen(QPen(theme::accent, 2.5, Qt::SolidLine, Qt::FlatCap));
    if (m_bipolar) {
        double from = 0.0, to = (n - 0.5) * span;
        p.drawArc(r, qtAngle(from), -int(to * 16));
    } else {
        p.drawArc(r, qtAngle(startAngle), -int(n * span * 16));
    }

    p.setPen(Qt::NoPen);
    p.setBrush(theme::knobFace);
    p.drawEllipse(r.adjusted(3, 3, -3, -3));

    // pointer
    double ang = (startAngle + n * span) * M_PI / 180.0;
    QPointF c = r.center();
    double R = r.width() / 2 - 3;
    p.setPen(QPen(hasFocus() ? theme::outline : theme::knobPointer, 2, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(c + QPointF(std::sin(ang) * R * 0.35, -std::cos(ang) * R * 0.35),
               c + QPointF(std::sin(ang) * R, -std::cos(ang) * R));
}

} // namespace sessions
