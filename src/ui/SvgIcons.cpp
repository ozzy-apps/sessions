#include "SvgIcons.h"
#include "core/Theme.h"
#include <QFile>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QSvgRenderer>

namespace sessions::svgicons {

namespace {

QByteArray source(const QString& path) {
    static QHash<QString, QByteArray> files;   // read once per resource
    auto it = files.constFind(path);
    if (it != files.constEnd()) return *it;
    QFile f(path);
    f.open(QIODevice::ReadOnly);
    const QByteArray data = f.readAll();
    files.insert(path, data);
    return data;
}

class Engine : public QIconEngine {
public:
    explicit Engine(QString path) : m_path(std::move(path)) {}
    QIconEngine* clone() const override { return new Engine(m_path); }
    QString key() const override { return "SessionsSvg"; }
    void paint(QPainter* p, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
        const QColor colour = mode == QIcon::Disabled ? theme::disabled : state == QIcon::On ? theme::onAccent : theme::text;
        QByteArray data = source(m_path);
        data.replace("#000000", colour.name().toUtf8());
        QSvgRenderer r(data);
        p->setRenderHint(QPainter::Antialiasing, true);
        r.render(p, QRectF(rect));
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        QPixmap pm(size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        paint(&p, QRect(QPoint(), size), mode, state);
        return pm;
    }
private:
    QString m_path;
};

} // namespace

QIcon themed(const QString& folder, const QString& name) { return QIcon(new Engine(QString(":/%1/%2.svg").arg(folder, name))); }

} // namespace sessions::svgicons
