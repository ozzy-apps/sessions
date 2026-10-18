#pragma once
// DragValue: the signature draggable number field.
//  - drag up/down to change (Shift = fine)
//  - mouse wheel steps
//  - double-click to type a value, Enter to commit
//  - click to select, then Delete/Backspace resets to the default
// Also the base class for Knob and Fader, which share the drag logic.

#include <QWidget>
#include <functional>
class QLineEdit;

namespace sessions {

class DragValue : public QWidget {
    Q_OBJECT
public:
    explicit DragValue(QWidget* parent = nullptr);

    void   setRange(double min, double max) { m_min = min; m_max = max; update(); }
    void   setValue(double v, bool emitSignal = true);
    double value() const { return m_value; }
    void   setDefault(double d) { m_default = d; }
    void   setStep(double s) { m_step = s; }          // 0 = continuous
    void   setDragSensitivity(double pixelsForFullRange) { m_sens = pixelsForFullRange; }
    void   setFormatter(std::function<QString(double)> f) { m_fmt = std::move(f); update(); }
    void   setEditable(bool e) { m_editable = e; }
    void   setAccentText(bool a) { m_accent = a; update(); }
    void   setMinimumChars(int n) { m_minChars = n; updateGeometry(); }

    QString text() const;
    QSize   sizeHint() const override;

signals:
    void valueChanged(double v);
    void valueEdited(double v);   // changed by the user (not programmatically)

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void focusInEvent(QFocusEvent*) override;
    void focusOutEvent(QFocusEvent*) override;

    double normalized() const { return (m_value - m_min) / (m_max - m_min); }
    void   applyUserValue(double v);

    double m_min = 0, m_max = 1, m_value = 0, m_default = 0, m_step = 0;
    double m_sens = 140;
    bool   m_editable = true, m_accent = true;
    int    m_minChars = 6;
    std::function<QString(double)> m_fmt;

private:
    int    m_lastY = 0;
    double m_dragValue = 0;
    bool   m_dragging = false;
    ::QLineEdit* m_editor = nullptr;
};

} // namespace sessions
