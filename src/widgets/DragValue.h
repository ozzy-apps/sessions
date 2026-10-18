#pragma once
// DragValue: the signature draggable number field.
//  - drag up/down to change (Shift = fine)
//  - mouse wheel steps
//  - double-click to type a value, Enter to commit
//  - click to select, then Delete/Backspace resets to the default
//  - an endless drag (0.28): at the screen's top or bottom edge the pointer jumps to the other
//    edge and the drag goes on, so a field near an edge (the tempo) isn't stuck after a few pixels
// Also the base class for Knob and Fader, which share the drag logic.

#include <QWidget>
#include <QPoint>
#include <functional>
class QLineEdit;

namespace sessions {

// The endless drag, for every widget that drags a value: call it with the move's global position
// after using it; when the pointer is at the screen's edge it is warped to the opposite edge,
// `landed` says where, and the caller measures the next move from there. False when nothing
// happened (away from the edge, or where the pointer can't be warped: the drag then stops there).
namespace dragwrap {
bool vertical(const QWidget* w, const QPoint& global, QPoint* landed);
bool horizontal(const QWidget* w, const QPoint& global, QPoint* landed);
}

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
    // Typing: the editor shows and reads value × scale (a 0..1 amount shown as a percentage types as
    // 35, not 0.35); a unit after the number is ignored when reading.
    void   setEditScale(double s) { m_editScale = s; }
    bool   editing() const;

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
    double m_editScale = 1;
    std::function<QString(double)> m_fmt;

private:
    int    m_lastY = 0;   // the last move's global y (the endless drag warps the pointer)
    double m_dragValue = 0;
    bool   m_dragging = false;
    ::QLineEdit* m_editor = nullptr;
};

} // namespace sessions
