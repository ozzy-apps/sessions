#pragma once
// Fader: vertical volume fader. 0.75 = 0 dB, 1.0 = +6 dB.

#include "DragValue.h"

namespace sessions {

class Fader : public DragValue {
    Q_OBJECT
public:
    explicit Fader(QWidget* parent = nullptr);
    QSize sizeHint() const override { return QSize(20, 80); }

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override {}
};

QString dbText(double v);   // shared formatter: 0.75 -> "0.0", 0 -> "-inf"

} // namespace sessions
