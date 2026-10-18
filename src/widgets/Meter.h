#pragma once
// Meter: a simple level meter (green / yellow / red) with a peak-hold line.

#include <QWidget>

namespace sessions {

class Meter : public QWidget {
    Q_OBJECT
public:
    explicit Meter(QWidget* parent = nullptr);
    void setLevel(double v);   // 0..1
    QSize sizeHint() const override { return QSize(6, 80); }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    double m_level = 0, m_peak = 0;
    int    m_peakHold = 0;
};

} // namespace sessions
