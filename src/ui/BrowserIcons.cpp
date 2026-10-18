#include "BrowserIcons.h"
#include "core/Theme.h"
#include <QFile>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QSvgRenderer>

namespace sessions::browsericons {
namespace {
const char* name(Symbol s) {
    static const char* names[] = {"sounds", "drums", "instrument", "audio-effect", "guitar", "midi-effect", "impulse", "library",
        "folder", "folder-open", "add-folder", "project", "sample", "capture", "clip", "preset", "rack", "sampler", "wavetable", "fm", "oscillator",
        "search", "headphones", "chevron-right", "chevron-down", "close"};
    return names[int(s)];
}
QByteArray svg(Symbol symbol) {
    static QHash<int, QByteArray> originals;
    auto it = originals.constFind(int(symbol));
    if (it != originals.constEnd()) return *it;
    QFile file(QString(":/browser/%1.svg").arg(name(symbol)));
    file.open(QIODevice::ReadOnly); const auto data = file.readAll(); originals.insert(int(symbol), data); return data;
}
class SvgIcon : public QIconEngine {
public:
    SvgIcon(Symbol symbol, bool toggle, bool folder) : m_symbol(symbol), m_toggle(toggle), m_folder(folder) {}
    QIconEngine* clone() const override { return new SvgIcon(*this); }
    QString key() const override { return "SessionsBrowserSvg"; }
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
        QColor color = mode == QIcon::Disabled ? theme::textFaint : m_toggle && state == QIcon::On ? theme::onAccent
                     : mode == QIcon::Selected ? theme::textBright : theme::textSoft;
        auto data = svg(m_folder && state == QIcon::On ? Symbol::FolderOpen : m_symbol);
        data.replace("#000000", color.name().toUtf8());
        QSvgRenderer renderer(data); renderer.render(painter, QRectF(rect));
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        QPixmap pm(size); pm.fill(Qt::transparent); QPainter p(&pm); paint(&p, QRect(QPoint(), size), mode, state); return pm;
    }
private:
    Symbol m_symbol; bool m_toggle, m_folder;
};
}
QIcon icon(Symbol symbol, bool toggle) { return QIcon(new SvgIcon(symbol, toggle, false)); }
QIcon folder() { return QIcon(new SvgIcon(Symbol::Folder, false, true)); }
}
