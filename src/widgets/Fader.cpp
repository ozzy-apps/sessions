#include "Fader.h"
#include "core/Theme.h"
#include <QPainter>
#include <QMouseEvent>
#include <cmath>

namespace sessions {

QString dbText(double v) {
    if (v < 0.002) return "-inf";
    double db = 40.0 * std::log10(v / 0.75);
    return QString::number(db, 'f', 1);
}

Fader::Fader(QWidget* parent) : DragValue(parent) {
    setFixedWidth(20);
    setMinimumHeight(60);
    setMaximumHeight(16777215);
    setDefault(0.75);
    setDragSensitivity(200);
}

void Fader::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    int w = width(), h = height();
    int cx = w / 2;
    p.setPen(Qt::NoPen);
    p.setBrush(theme::meterBg2);
    p.drawRect(cx - 3, 4, 6, h - 8);

    // 0 dB tick
    int zeroY = int(4 + (1 - 0.75) * (h - 8 - 8)) + 4;
    p.setBrush(theme::faderZero);
    p.drawRect(cx - 8, zeroY, 16, 1);

    double n = normalized();
    int handleH = 8;
    int y = int(4 + (1 - n) * (h - 8 - handleH));
    p.setBrush(hasFocus() ? theme::outline : theme::faderHandle);
    p.drawRoundedRect(QRect(cx - 9, y, 18, handleH), 1.5, 1.5);
    p.setBrush(theme::faderLine);
    p.drawRect(cx - 9, y + handleH / 2, 18, 1);
}

void Fader::mousePressEvent(QMouseEvent* e) {
    DragValue::mousePressEvent(e);
}

} // namespace sessions
