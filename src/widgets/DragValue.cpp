#include "DragValue.h"
#include "core/Theme.h"
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QFontMetrics>
#include <algorithm>
#include <cmath>

namespace sessions {

DragValue::DragValue(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::SizeVerCursor);
    setFocusPolicy(Qt::ClickFocus);
    setFixedHeight(18);
}

QString DragValue::text() const {
    return m_fmt ? m_fmt(m_value) : QString::number(m_value, 'f', 2);
}

QSize DragValue::sizeHint() const {
    QFontMetrics fm(font());
    return QSize(std::max(fm.horizontalAdvance(QString(m_minChars, '0')) + 10, 30), 18);
}

void DragValue::setValue(double v, bool emitSignal) {
    v = std::clamp(v, m_min, m_max);
    if (m_step > 0) v = std::round(v / m_step) * m_step;
    if (std::abs(v - m_value) < 1e-9) return;
    m_value = v;
    update();
    if (emitSignal) emit valueChanged(m_value);
}

void DragValue::applyUserValue(double v) {
    double before = m_value;
    setValue(v);
    if (std::abs(before - m_value) > 1e-9) emit valueEdited(m_value);
}

void DragValue::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QRect r = rect().adjusted(0, 0, -1, -1);
    p.setPen(theme::line);
    p.setBrush(theme::field);
    p.drawRect(r);
    if (hasFocus()) { p.setPen(QPen(theme::text, 1)); p.setBrush(Qt::NoBrush); p.drawRect(r.adjusted(1, 1, -1, -1)); }
    p.setPen(m_accent ? theme::readout : theme::text);   // readout: the accent in the dark, the text colour on light surfaces
    p.drawText(rect(), Qt::AlignCenter, text());
}

void DragValue::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || !m_editable) return;
    m_lastY = e->position().toPoint().y();
    m_dragValue = m_value;
    m_dragging = true;
    setFocus();
}

void DragValue::mouseMoveEvent(QMouseEvent* e) {
    if (!m_dragging) return;
    int y = e->position().toPoint().y();
    double range = m_max - m_min;
    double sens = (e->modifiers() & Qt::ShiftModifier) ? m_sens * 10 : m_sens;
    m_dragValue += (m_lastY - y) * range / sens;
    m_dragValue = std::clamp(m_dragValue, m_min, m_max);
    m_lastY = y;
    applyUserValue(m_dragValue);
}

void DragValue::mouseReleaseEvent(QMouseEvent*) { m_dragging = false; }

void DragValue::mouseDoubleClickEvent(QMouseEvent*) {
    if (!m_editable) return;
    if (!m_editor) {
        m_editor = new QLineEdit(this);
        m_editor->setAlignment(Qt::AlignCenter);
        connect(m_editor, &QLineEdit::editingFinished, this, [this] {
            bool ok = false;
            double v = m_editor->text().toDouble(&ok);
            if (ok) applyUserValue(v);
            m_editor->hide();
            setFocus();
        });
    }
    m_editor->setGeometry(rect());
    m_editor->setText(QString::number(m_value, 'f', m_step >= 1 ? 0 : 2));
    m_editor->selectAll();
    m_editor->show();
    m_editor->setFocus();
}

void DragValue::wheelEvent(QWheelEvent* e) {
    if (!m_editable) { e->ignore(); return; }
    double step = m_step > 0 ? m_step : (m_max - m_min) / 40.0;
    if (e->modifiers() & Qt::ShiftModifier) step /= 10;
    applyUserValue(m_value + (e->angleDelta().y() > 0 ? step : -step));
    e->accept();
}

void DragValue::keyPressEvent(QKeyEvent* e) {
    double step = m_step > 0 ? m_step : (m_max - m_min) / 40.0;
    switch (e->key()) {
    case Qt::Key_Delete: case Qt::Key_Backspace: applyUserValue(m_default); break;
    case Qt::Key_Up:   applyUserValue(m_value + step); break;
    case Qt::Key_Down: applyUserValue(m_value - step); break;
    default: QWidget::keyPressEvent(e); return;
    }
    e->accept();
}

void DragValue::focusInEvent(QFocusEvent*)  { update(); }
void DragValue::focusOutEvent(QFocusEvent*) { update(); }

} // namespace sessions
