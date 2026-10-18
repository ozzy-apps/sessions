#pragma once
// LevelFader: the channel volume. A level meter with the fader as a ◁ marker on its
// right edge and a dB scale beside it (0 near the top, 60 at the bottom). The marker sits
// on the meter's own scale, so a fader at -12 points at the meter's 12. Dragging works
// like every DragValue (up / down, Shift for fine, Delete resets to 0 dB).

#include "DragValue.h"

namespace sessions {

class LevelFader : public DragValue {
    Q_OBJECT
public:
    explicit LevelFader(QWidget* parent = nullptr);
    void  setLevel(double meter);            // 0..1 on the meter scale (Track::meter)
    QSize sizeHint() const override { return QSize(36, 120); }
    static double meterPos(double db);       // dB -> 0..1, the same law as the meters

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override {}   // no text editor on a fader

private:
    double m_level = 0, m_peak = 0;
    int    m_peakHold = 0;
};

} // namespace sessions
