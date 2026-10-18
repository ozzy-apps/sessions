#pragma once
// Knob: rotary control dragged vertically (never in a circle).
// Reuses DragValue's drag / wheel / reset behaviour, draws an arc instead.

#include "DragValue.h"

namespace sessions {

class Knob : public DragValue {
    Q_OBJECT
public:
    explicit Knob(QWidget* parent = nullptr, int diameter = 22);
    void setBipolar(bool b) { m_bipolar = b; update(); }   // arc from centre (pan)
    QSize sizeHint() const override { return QSize(m_d, m_d); }

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override {}  // knobs don't open an editor

private:
    int  m_d;
    bool m_bipolar = false;
};

} // namespace sessions
