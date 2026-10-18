#include "InlineEdit.h"
#include "core/Theme.h"
#include "core/Model.h"
#include <QLineEdit>
#include <QKeyEvent>
#include <QMenu>
#include <QWidgetAction>
#include <QMouseEvent>
#include <QPixmap>
#include <QPainter>
#include <memory>

namespace sessions {

namespace {

class Editor : public QLineEdit {
public:
    using QLineEdit::QLineEdit;
    std::function<void()> onCancel;
protected:
    void keyPressEvent(QKeyEvent* e) override {
        if (e->key() == Qt::Key_Escape && onCancel) { onCancel(); return; }
        QLineEdit::keyPressEvent(e);
    }
};

// The colour grid: kPaletteColumns squares a row, the current colour outlined; hovering lifts a square.
class ColorGrid : public QWidget {
public:
    ColorGrid(QMenu* menu, const QColor& current, std::function<void(const QColor&)> pick)
        : QWidget(menu), m_menu(menu), m_current(current), m_pick(std::move(pick)) {
        const int cols = Project::kPaletteColumns, rows = (Project::palette().size() + cols - 1) / cols;
        setFixedSize(kPad * 2 + cols * kCell, kPad * 2 + rows * kCell);
        setMouseTracking(true);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), theme::panel);
        const QVector<QColor>& pal = Project::palette();
        for (int i = 0; i < pal.size(); ++i) {
            const QRect r = cell(i).adjusted(1, 1, -1, -1);
            p.fillRect(r, pal[i]);
            if (pal[i] == m_current) { p.setPen(QPen(theme::outline, 2)); p.setBrush(Qt::NoBrush); p.drawRect(r.adjusted(1, 1, -1, -1)); }
            else if (i == m_hover) { p.setPen(QPen(theme::textBright, 1)); p.setBrush(Qt::NoBrush); p.drawRect(r); }
        }
    }
    void mouseMoveEvent(QMouseEvent* e) override { const int h = at(e->pos()); if (h != m_hover) { m_hover = h; update(); } }
    void leaveEvent(QEvent*) override { m_hover = -1; update(); }
    void mouseReleaseEvent(QMouseEvent* e) override {
        const int i = at(e->pos());
        if (i < 0 || e->button() != Qt::LeftButton) return;
        const QColor c = Project::palette()[i];
        for (QWidget* w = m_menu; w; w = w->parentWidget()) if (auto* m = qobject_cast<QMenu*>(w)) m->close();   // the submenu and the menu it hangs from
        if (m_pick) m_pick(c);
    }
private:
    static constexpr int kCell = 18, kPad = 4;
    QRect cell(int i) const { return QRect(kPad + (i % Project::kPaletteColumns) * kCell, kPad + (i / Project::kPaletteColumns) * kCell, kCell, kCell); }
    int at(const QPoint& p) const { for (int i = 0; i < Project::palette().size(); ++i) if (cell(i).contains(p)) return i; return -1; }
    QMenu* m_menu;
    QColor m_current;
    std::function<void(const QColor&)> m_pick;
    int m_hover = -1;
};

QIcon swatch(const QColor& c) {
    QPixmap pm(14, 14);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setPen(theme::line);
    p.setBrush(c);
    p.drawRect(0, 0, 13, 13);
    return QIcon(pm);
}

} // namespace

void addColorGrid(QMenu* menu, const QColor& current, std::function<void(const QColor&)> pick) {
    auto* action = new QWidgetAction(menu);
    action->setDefaultWidget(new ColorGrid(menu, current, std::move(pick)));
    menu->addAction(action);
}

void inlineEdit(QWidget* parent, const QRect& rect, const QString& text, std::function<void(const QString&)> done) {
    auto* e = new Editor(parent);
    auto finished = std::make_shared<bool>(false);
    e->setGeometry(rect);
    e->setText(text);
    e->selectAll();
    e->show();
    e->setFocus();
    e->onCancel = [e, finished] { *finished = true; e->hide(); e->deleteLater(); };
    QObject::connect(e, &QLineEdit::editingFinished, e, [e, done, finished] {
        if (*finished) return;
        *finished = true;
        const QString t = e->text();
        e->hide();
        e->deleteLater();
        done(t);
    });
}

void showTrackMenu(QWidget* parent, const QPoint& globalPos, Project* p, int index, std::function<void()> rename) {
    if (!p->trackAt(index)) return;
    const bool isReturn = p->isReturn(index);
    QMenu menu(parent);
    menu.addAction("Rename\tCtrl+R", rename);
    QMenu* colors = menu.addMenu("Color");
    addColorGrid(colors, p->trackAt(index)->color, [p, index](const QColor& c) { p->setTrackColor(index, c); });
    menu.addSeparator();
    {   // the track's switches (0.22)
        Track* t = p->trackAt(index);
        menu.addAction(t->active ? "Track Off (Mute)" : "Track On", [p, index] {
            Track* tr = p->trackAt(index); if (!tr) return;
            EditScope edit(p, tr->active ? "Track Off" : "Track On");
            tr->active = !tr->active;
            emit p->mixerChanged();
        });
        if (!isReturn) {
            menu.addAction(t->solo ? "Unsolo" : "Solo", [p, index] { Track* tr = p->trackAt(index); if (!tr) return; tr->solo = !tr->solo; emit p->mixerChanged(); });
            if (!t->isGroup()) menu.addAction(t->armed ? "Disarm" : "Arm for Recording", [p, index] {
                Track* tr = p->trackAt(index); if (!tr) return;
                tr->armed = !tr->armed;
                emit p->mixerChanged();
                emit p->structureChanged();
            });
            if (t->type == TrackType::Audio) {
                QMenu* mon = menu.addMenu("Monitor");
                for (int m = 0; m < 3; ++m) {
                    QAction* a = mon->addAction(m == 0 ? "In" : m == 1 ? "Auto" : "Off", [p, index, m] {
                        Track* tr = p->trackAt(index); if (!tr) return;
                        EditScope edit(p, "Monitor");
                        tr->monitor = Monitor(m);
                        emit p->mixerChanged();
                    });
                    a->setCheckable(true); a->setChecked(int(t->monitor) == m);
                }
            }
            if (t->isGroup()) menu.addAction(t->folded ? "Unfold Group" : "Fold Group", [p, index] { p->setFolded(index, !p->trackAt(index)->folded); });
        }
        menu.addSeparator();
    }
    if (!isReturn) {
        menu.addAction("Duplicate\tCtrl+D", [p, index] { p->duplicateTrack(index); });
        QAction* left = menu.addAction("Move Left", [p, index] { p->moveTrack(index, index - 1); });
        left->setEnabled(index > 0);
        QAction* right = menu.addAction("Move Right", [p, index] { p->moveTrack(index, index + 1); });
        right->setEnabled(index + 1 < p->tracks.size());
    }
    QAction* del = menu.addAction(p->trackAt(index)->isGroup() ? "Delete Group and Its Tracks\tDel" : "Delete\tDel", [p, index] { p->deleteTrack(index); });
    del->setEnabled(isReturn || p->tracks.size() > 1);
    if (!isReturn) {
        menu.addSeparator();
        // the selected tracks when this is one of them, else just this one
        const int id = p->trackAt(index)->id;
        QList<int> picked = p->selectedTrackIds.contains(id) ? p->selectedTrackIndexes() : QList<int>{index};
        picked.removeIf([p](int i) { return p->tracks[i].isGroup(); });
        QAction* grp = menu.addAction(picked.size() > 1 ? QString("Group %1 Tracks\tCtrl+G").arg(picked.size()) : QString("Group Track\tCtrl+G"),
                                      [p, picked] { p->groupTracks(picked); });
        grp->setEnabled(!picked.isEmpty());
        const int g = p->trackAt(index)->isGroup() ? index : p->groupOf(index);
        if (g >= 0) menu.addAction("Ungroup Tracks\tCtrl+Shift+G", [p, g] { p->ungroup(g); });
    }
    menu.addSeparator();
    menu.addAction("Insert MIDI Track\tCtrl+T", [p, index, isReturn] { p->insertTrack(isReturn ? int(p->tracks.size()) : index + 1); });
    menu.addAction("Insert Audio Track\tCtrl+Shift+T", [p, index, isReturn] { p->insertTrack(isReturn ? int(p->tracks.size()) : index + 1, TrackType::Audio); });
    QAction* ret = menu.addAction("Insert Return Track\tCtrl+Alt+T", [p] { p->insertReturn(); });
    ret->setEnabled(p->returns.size() < 8);
    menu.exec(globalPos);
}

} // namespace sessions
