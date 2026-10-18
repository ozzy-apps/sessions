#include "DragValue.h"
#include "core/Theme.h"
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QFontMetrics>
#include <QCursor>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QScreen>
#include <algorithm>
#include <cmath>

namespace sessions {

namespace dragwrap {
namespace {
bool wrap(const QWidget* w, const QPoint& global, QPoint* landed, bool verticalAxis) {
    QScreen* screen = QGuiApplication::screenAt(global);
    if (!screen && w) screen = w->screen();
    if (!screen) return false;
    const QRect g = screen->geometry();
    constexpr int kEdge = 1, kInset = 4;   // at the edge: land `kInset` px inside the other edge
    QPoint to = global;
    if (verticalAxis) {
        if (global.y() <= g.top() + kEdge) to.setY(g.bottom() - kInset);
        else if (global.y() >= g.bottom() - kEdge) to.setY(g.top() + kInset);
        else return false;
    } else {
        if (global.x() <= g.left() + kEdge) to.setX(g.right() - kInset);
        else if (global.x() >= g.right() - kEdge) to.setX(g.left() + kInset);
        else return false;
    }
    QCursor::setPos(screen, to);
    const QPoint now = QCursor::pos();
    if ((now - global).manhattanLength() < kInset) return false;   // the platform didn't move it (Wayland): the drag stops at the edge
    if (landed) *landed = now;
    return true;
}
}
bool vertical(const QWidget* w, const QPoint& global, QPoint* landed) { return wrap(w, global, landed, true); }
bool horizontal(const QWidget* w, const QPoint& global, QPoint* landed) { return wrap(w, global, landed, false); }
}

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

bool DragValue::editing() const { return m_editor && m_editor->isVisible(); }

void DragValue::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QRect r = rect().adjusted(0, 0, -1, -1);
    p.setPen(theme::line);
    p.setBrush(theme::field);
    p.drawRect(r);
    if (hasFocus()) { p.setPen(QPen(theme::text, 1)); p.setBrush(Qt::NoBrush); p.drawRect(r.adjusted(1, 1, -1, -1)); }
    if (editing()) return;   // typing: the editor is the text (it used to show through a transparent editor)
    p.setPen(m_accent ? theme::readout : theme::text);   // readout: the accent in the dark, the text colour on light surfaces
    p.drawText(rect(), Qt::AlignCenter, text());
}

void DragValue::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || !m_editable) return;
    m_lastY = e->globalPosition().toPoint().y();
    m_dragValue = m_value;
    m_dragging = true;
    setFocus();
}

void DragValue::mouseMoveEvent(QMouseEvent* e) {
    if (!m_dragging) return;
    const QPoint global = e->globalPosition().toPoint();
    double range = m_max - m_min;
    double sens = (e->modifiers() & Qt::ShiftModifier) ? m_sens * 10 : m_sens;
    m_dragValue += (m_lastY - global.y()) * range / sens;
    m_dragValue = std::clamp(m_dragValue, m_min, m_max);
    m_lastY = global.y();
    applyUserValue(m_dragValue);
    QPoint landed;   // the endless drag: measure the next move from where the pointer landed
    if (dragwrap::vertical(this, global, &landed)) m_lastY = landed.y();
}

void DragValue::mouseReleaseEvent(QMouseEvent*) { m_dragging = false; }

void DragValue::mouseDoubleClickEvent(QMouseEvent*) {
    if (!m_editable) return;
    if (!m_editor) {
        m_editor = new QLineEdit(this);
        m_editor->setObjectName("dragValueEditor");
        m_editor->setAlignment(Qt::AlignCenter);
        m_editor->setFrame(false);
        m_editor->setAutoFillBackground(true);   // opaque whatever the panel's stylesheet says (device bodies make line edits transparent)
        connect(m_editor, &QLineEdit::editingFinished, this, [this] {
            // the first number in the text: "35 %", "-12.5 dB", "+3" all read
            static const QRegularExpression number("[-+]?\\d*\\.?\\d+");
            const QRegularExpressionMatch m = number.match(m_editor->text());
            if (m.hasMatch()) applyUserValue(m.captured(0).toDouble() / m_editScale);
            m_editor->hide();
            setFocus();
            update();
        });
    }
    m_editor->setStyleSheet(QString("QLineEdit { background: %1; color: %2; border: 0; padding: 0; selection-background-color: %3; selection-color: %4; }")
                                .arg(theme::field.name(), theme::text.name(), theme::accent.name(), theme::onAccent.name()));
    m_editor->setGeometry(rect().adjusted(1, 1, -1, -1));
    const double shown = m_value * m_editScale;
    const bool whole = m_step * m_editScale >= 1 || std::abs(shown - std::round(shown)) < 1e-6;
    m_editor->setText(QString::number(shown, 'f', whole ? 0 : 2));
    m_editor->selectAll();
    m_editor->show();
    m_editor->setFocus();
    update();
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
