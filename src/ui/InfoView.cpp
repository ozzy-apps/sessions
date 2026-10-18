#include "InfoView.h"
#include <QLabel>
#include <QVBoxLayout>
#include <QEvent>
#include <QApplication>

namespace sessions {

namespace { InfoView* s_main = nullptr; }   // the newest one: the window's

InfoView::InfoView(QWidget* parent) : QWidget(parent) {
    s_main = this;
    setObjectName("infoView");
    setFixedWidth(150);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(4);
    m_title = new QLabel(this);
    m_title->setObjectName("infoTitle");
    m_title->setWordWrap(true);
    m_body = new QLabel(this);
    m_body->setObjectName("infoBody");
    m_body->setWordWrap(true);
    m_body->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    lay->addWidget(m_title);
    lay->addWidget(m_body, 1);
    showDefault();
    qApp->installEventFilter(this);
}

InfoView::~InfoView() { if (s_main == this) s_main = nullptr; }

void InfoView::notify(const QString& title, const QString& body) { if (s_main) s_main->show(title, body); }

void InfoView::show(const QString& title, const QString& body) {
    m_title->setText(title);
    m_body->setText(body);
}

void InfoView::showDefault() {
    show("Info View", "Hover anything to see what it does.\n\nTab switches Session and Arrangement View. Shift+Tab switches Clip and Device View. Space plays and stops. Ctrl+Z undoes. F11 is full screen.");
}

void InfoView::attach(QWidget* w, const QString& title, const QString& body) {
    w->setProperty("info", title + "|" + body);
}

bool InfoView::eventFilter(QObject* obj, QEvent* ev) {
    if (ev->type() == QEvent::Enter) {
        // Walk up to the nearest ancestor carrying hover help.
        for (QObject* o = obj; o; o = o->parent()) {
            QVariant v = o->property("info");
            if (v.isValid()) {
                QStringList parts = v.toString().split('|');
                show(parts.value(0), parts.value(1));
                break;
            }
        }
    }
    return false;
}

} // namespace sessions
