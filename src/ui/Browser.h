#pragma once
// Browser: a sidebar of categories and places next to the items, live search. Items can be
// double-clicked (load onto the selected track) or dragged onto the
// Device View / tracks. Drag payload MIME: "application/x-sessions-item"
// with "Name|kind" where kind is i (instrument), f (audio fx), m (midi fx), s (sample:
// the name is the file's absolute path), u (your preset: the .sessionspreset file's path),
// n (a .nam capture: its path; listed in your sample folders and the User Library), r (an
// impulse response: a file's path, or a built-in "builtin:ir/…"; Categories → Impulse Responses).
// Sample and preset drags also carry file URLs, so drop targets treat them like files
// dragged in from the file manager (audioFilesIn, devicedrag::parse).
//
// Laid out: a sidebar (Categories, then Places: User Library (your presets), your
// sample folders, the current set's folder and Add Folder…) next to the content pane. Folders are
// remembered in QSettings "browser/folders"; right-click one to remove it. Clicking a
// sample previews it when the headphones Preview control is on. The sidebar divider is resizable;
// category/item decoration uses the embedded theme-colored SVGs in BrowserIcons.
//
// The content pane is a tree (0.22): a sample folder shows its subfolders as nodes you open and
// close (what you opened stays open while you browse), the User Library its presets, then a
// Clips node (clips saved from the views: kind c, the .sessionsclip file's path) and a Captures
// node; a search flattens everything that matches under a node per source. Right-click an item
// for what it offers (load it, preview, show it in the file manager, rename / delete your own).

#include <QWidget>

#include <QSet>

class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QTreeWidget;
class QTreeWidgetItem;
class QMimeData;
class QToolButton;
class QLabel;

namespace sessions {

class Browser : public QWidget {
    Q_OBJECT
public:
    explicit Browser(QWidget* parent = nullptr);
    static const char* mimeType() { return "application/x-sessions-item"; }
    // The audio files in a drag: browser samples, or files from the file manager.
    static QStringList audioFilesIn(const QMimeData* mime);
    // The .nam captures (Neural Amp Modeler) in a drag: browser items, or files from the file manager.
    static QStringList modelFilesIn(const QMimeData* mime);
    // The impulse responses in a drag: browser items from Impulse Responses (files, or built-in ones).
    static QStringList irFilesIn(const QMimeData* mime);
    // The library clips (.sessionsclip) in a drag: User Library items, or files from the file manager.
    static QStringList clipFilesIn(const QMimeData* mime);
    static const char* irCategory() { return "Impulse Responses"; }

    QStringList folders() const { return m_folders; }
    void addFolder(const QString& dir);      // no dialog; shows it
    void removeFolder(const QString& dir);
    void setProjectFolder(const QString& dir);   // "Current Project" in Places (empty: the set isn't saved)
    QStringList listedItems() const;         // the item list's payloads, for tests
    void showUserLibrary();                  // Places → User Library (your presets)
    void showCategory(const QString& name);  // e.g. irCategory()
    void addIrFolder(const QString& dir);    // an IR folder (import::addIrFolder); shows it

signals:
    void itemActivated(const QString& name, QChar kind);
    void previewRequested(const QString& path);   // a sample was clicked with Preview on

private:
    void rebuild();
    void saveFolders();
    void refreshAppearance();
    QStringList scan(const QString& dir) const;   // audio files below `dir`, relative paths
    QTreeWidgetItem* leaf(QTreeWidgetItem* parent, const QString& text, const QString& payload, const QString& tip);
    QTreeWidgetItem* node(QTreeWidgetItem* parent, const QString& text, const QString& key, const QString& path = {});   // a folder: open when it was
    void addFiles(QTreeWidgetItem* parent, const QString& dir, const QStringList& rel, const QString& filter);   // a folder's files as a tree
    void itemMenu(const QPoint& pos);

    QLineEdit*   m_search;
    QListWidget* m_categories;
    QTreeWidget* m_items;
    QSet<QString> m_expanded;   // the folder nodes you opened (by key), so a rebuild keeps them open
    QToolButton* m_preview;
    QLabel* m_location;
    QLabel* m_count;
    QLabel* m_previewName;
    QString      m_category = "Instruments";
    QStringList  m_folders;
    QString      m_projectFolder;
};

} // namespace sessions
