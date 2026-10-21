#include "Browser.h"
#include "BrowserIcons.h"
#include "core/Theme.h"
#include "InfoView.h"
#include "core/Sample.h"
#include "core/DeviceSpecs.h"
#include "audio/Nam.h"
#include "core/Presets.h"
#include "core/Import.h"
#include "audio/Builtin.h"
#include <algorithm>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QTreeWidget>
#include <functional>
#include <QMimeData>
#include <QDrag>
#include <QApplication>
#include <QMouseEvent>
#include <QToolButton>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QDirIterator>
#include <QSettings>
#include <QMenu>
#include <QUrl>
#include <QHash>
#include <QLabel>
#include <QDesktopServices>
#include <QInputDialog>
#include <QMessageBox>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QAction>
#include <QPainter>
#include <QHeaderView>
#include <QScrollBar>
#include <QKeyEvent>
#include <QTimer>
#include <QStyle>

namespace sessions {

namespace {

struct Item { const char* name; char kind; };
struct Category { const char* name; QVector<Item> items; };

// Built from the device table: presets are the Sounds, drum kits the Drums, the rest by kind,
// each list in alphabetical order. Everything for the guitar lives under Guitar: the
// Tuner, the Guitar Amp and NAM, the pedals, the Cabinet and the ready-made rigs (0.21: they left
// Audio Effects and Sounds, so the effect list is the studio effects only).
const QVector<Category>& catalogue() {
    static const QVector<Category> c = [] {
        QVector<Category> cats = {{"Sounds", {}}, {"Drums", {}}, {"Instruments", {}}, {"Audio Effects", {}}, {"Guitar", {}}, {"MIDI Effects", {}}};
        for (const DeviceSpec& s : deviceSpecs()) {
            const bool preset = specOf(s.type) != &s;
            const bool drums = s.type == DeviceType::DrumRack || s.type == DeviceType::DrumKit || s.type == DeviceType::Drum808
                            || s.type == DeviceType::DrumAcoustic || s.type == DeviceType::DrumStudio;
            const char kind = s.kind == DeviceKind::Instrument ? 'i' : s.kind == DeviceKind::AudioEffect ? 'f' : 'm';
            const RackRecipe* rig = preset ? rackRecipe(s.name) : nullptr;
            const bool guitar = (!preset && isGuitarType(s.type)) || (rig && rig->guitar);
            const int cat = guitar ? 4 : preset && s.type != DeviceType::DrumStudio ? 0 : drums ? 1 : s.kind == DeviceKind::Instrument ? 2 : s.kind == DeviceKind::AudioEffect ? 3 : 5;
            cats[cat].items.push_back({s.name, kind});
        }
        for (int k : {0, 2, 3, 5})
            std::sort(cats[k].items.begin(), cats[k].items.end(), [](const Item& a, const Item& b) { return QString(a.name).compare(b.name, Qt::CaseInsensitive) < 0; });
        // Guitar: the Tuner, the amp and NAM first, then the pedals, then the rigs
        auto rank = [](const Item& it) {
            const DeviceType t = deviceType(it.name);
            const bool rig = specOf(t) != deviceSpec(it.name);
            return t == DeviceType::Tuner ? 0 : (t == DeviceType::GuitarAmp || t == DeviceType::Nam) && !rig ? 1 : rig ? 3 : 2;
        };
        std::stable_sort(cats[4].items.begin(), cats[4].items.end(), [&](const Item& a, const Item& b) {
            return rank(a) != rank(b) ? rank(a) < rank(b) : QString(a.name).compare(b.name, Qt::CaseInsensitive) < 0;
        });
        return cats;
    }();
    return c;
}

constexpr int kMaxFiles = 5000;   // per folder, so a huge drive can't stall the browser
const char* kAddFolder = "add-folder";
const char* kUserLibrary = "user-library";   // your presets
const char* kHeader = "header";
const char* kAddIrFolder = "add-ir-folder";
const QString kFolderPrefix = QStringLiteral("folder:");
const QString kIrFolderPrefix = QStringLiteral("irfolder:");

using Symbol = browsericons::Symbol;
Symbol categoryIcon(const QString& name) {
    if (name == "Sounds") return Symbol::Sounds;
    if (name == "Drums") return Symbol::Drums;
    if (name == "Instruments") return Symbol::Instrument;
    if (name == "Audio Effects") return Symbol::AudioEffect;
    if (name == "Guitar") return Symbol::Guitar;
    if (name == Browser::irCategory()) return Symbol::Impulse;
    return Symbol::MidiEffect;
}
Symbol payloadIcon(const QString& payload) {
    const QChar kind = payload.isEmpty() ? QChar() : payload.back();
    if (kind == 's') return Symbol::Sample;
    if (kind == 'n') return Symbol::Capture;
    if (kind == 'r') return Symbol::Impulse;
    if (kind == 'c') return Symbol::Clip;
    if (kind == 'u') return Symbol::Preset;
    const DeviceType type = deviceType(payload.chopped(2));
    if (isRackType(type)) return Symbol::Rack;
    if (isGuitarType(type)) return type == DeviceType::Nam ? Symbol::Capture : Symbol::Guitar;
    switch (type) {
    case DeviceType::Sampler: return Symbol::Sampler;
    case DeviceType::WavetableSynth: return Symbol::Wavetable;
    case DeviceType::FMSynth: return Symbol::Fm;
    case DeviceType::Monolith: case DeviceType::Osmium: return Symbol::Oscillator;
    case DeviceType::DrumKit: case DeviceType::Drum808: case DeviceType::DrumAcoustic: case DeviceType::DrumStudio: return Symbol::Drums;
    default: return kind == 'i' ? Symbol::Instrument : kind == 'm' ? Symbol::MidiEffect : Symbol::AudioEffect;
    }
}

class BrowserLabel : public QLabel {
public:
    using QLabel::QLabel;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.setFont(font()); p.setPen(palette().color(QPalette::WindowText));
        p.drawText(contentsRect(), Qt::AlignVCenter | Qt::AlignLeft, fontMetrics().elidedText(text(), Qt::ElideMiddle, contentsRect().width()));
    }
};

// Original SVGs and one row rhythm for both lists. Colors are read while painting,
// so switching themes doesn't rebuild the library or lose its current selection.
class BrowserRows : public QStyledItemDelegate {
public:
    BrowserRows(bool sidebar, QObject* parent) : QStyledItemDelegate(parent), m_sidebar(sidebar) {}
    QSize sizeHint(const QStyleOptionViewItem& o, const QModelIndex&) const override {
        return {100, std::max(28, QFontMetrics(o.font).height() + 11)};
    }
    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyleOptionViewItem o(option); initStyleOption(&o, index);
        const bool header = m_sidebar && index.data(Qt::UserRole).toString() == "header";
        const bool selected = o.state & QStyle::State_Selected, hover = o.state & QStyle::State_MouseOver;
        const bool folder = !m_sidebar && !index.data(Qt::UserRole + 1).toString().isEmpty();
        p->save(); p->fillRect(o.rect, selected ? theme::panel2 : hover && !header ? theme::itemHover : m_sidebar ? theme::sidebar : theme::panel);
        if (header) {
            auto f = o.font; f.setBold(true); f.setPointSizeF(std::max(7.0, f.pointSizeF() - 1)); p->setFont(f); p->setPen(theme::textFaint);
            p->drawText(o.rect.adjusted(11, 4, -4, 0), Qt::AlignVCenter, o.text); p->restore(); return;
        }
        if (m_sidebar && selected) p->fillRect(QRect(o.rect.left(), o.rect.top() + 3, 2, o.rect.height() - 6), theme::accent);
        const int iconX = o.rect.left() + (m_sidebar ? 10 : 4), iconY = o.rect.center().y() - 8;
        if (!o.icon.isNull()) o.icon.paint(p, QRect(iconX, iconY, 18, 18), Qt::AlignCenter, selected ? QIcon::Selected : QIcon::Normal,
                                         folder && (o.state & QStyle::State_Open) ? QIcon::On : QIcon::Off);
        QRect textRect = o.rect.adjusted(m_sidebar ? 36 : 29, 0, -8, 0);
        if (folder && o.rect.width() > 150) {
            const int rows = index.model()->rowCount(index);
            const int count = rows == 1 && !(index.model()->index(0, 0, index).flags() & Qt::ItemIsEnabled) ? 0 : rows; auto f = o.font; f.setPointSizeF(std::max(7.0, f.pointSizeF() - 1));
            p->setFont(f); p->setPen(theme::textFaint); p->drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, QString::number(count)); textRect.adjust(0, 0, -27, 0);
        }
        auto f = o.font; f.setBold(folder); p->setFont(f); p->setPen(selected ? theme::textBright : index.flags() & Qt::ItemIsEnabled ? theme::text : theme::textFaint);
        p->drawText(textRect, Qt::AlignVCenter, QFontMetrics(f).elidedText(o.text, Qt::ElideRight, std::max(0, textRect.width())));
        if ((o.state & QStyle::State_HasFocus) && !m_sidebar) { p->setPen(QPen(theme::withAlpha(theme::accent, 120), 1)); p->setBrush(Qt::NoBrush); p->drawRect(o.rect.adjusted(0, 1, -1, -2)); }
        p->restore();
    }
private:
    bool m_sidebar;
};

QHash<QString, QStringList>& scanCache() { static QHash<QString, QStringList> c; return c; }

constexpr int kPayloadRole = Qt::UserRole;        // leaves: "name|kind"
constexpr int kKeyRole = Qt::UserRole + 1;        // folder nodes: a key for m_expanded
constexpr int kPathRole = Qt::UserRole + 2;       // folder nodes: the folder on disk (empty for a grouping)

// Tree that starts a drag with our MIME payload (and file URLs for samples, presets and clips).
class ItemTree : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
protected:
    void drawBranches(QPainter* p, const QRect& rect, const QModelIndex& index) const override {
        p->fillRect(rect, selectionModel()->isSelected(index) ? theme::panel2 : theme::panel);
        if (model()->hasChildren(index)) browsericons::icon(isExpanded(index) ? Symbol::ChevronDown : Symbol::ChevronRight)
            .paint(p, QRect(rect.right() - indentation() + 3, rect.center().y() - 6, 13, 13));
        if (index.parent().isValid()) {
            p->setPen(QPen(theme::withAlpha(theme::textFaint, 38), 1));
            const int x = rect.right() - indentation() + 9;
            p->drawLine(x, rect.top(), x, rect.bottom());
        }
    }
    void mousePressEvent(QMouseEvent* e) override { m_pressPos = e->pos(); QTreeWidget::mousePressEvent(e); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton)) { QTreeWidget::mouseMoveEvent(e); return; }
        if ((e->pos() - m_pressPos).manhattanLength() < QApplication::startDragDistance()) return;
        QTreeWidgetItem* it = currentItem();
        if (!it || it->data(0, kPayloadRole).toString().isEmpty()) return;
        const QString payload = it->data(0, kPayloadRole).toString();
        auto* mime = new QMimeData;
        mime->setData(Browser::mimeType(), payload.toUtf8());
        const bool file = payload.endsWith("|s") || payload.endsWith("|u") || payload.endsWith("|c") || payload.endsWith("|n")
                       || (payload.endsWith("|r") && !audio::isBuiltin(payload.chopped(2)));
        if (file) mime->setUrls({QUrl::fromLocalFile(payload.chopped(2))});
        else mime->setText(it->text(0));
        auto* drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->exec(Qt::CopyAction);
    }
private:
    QPoint m_pressPos;
};

void collectLeaves(QTreeWidgetItem* it, QStringList& out) {
    const QString payload = it->data(0, kPayloadRole).toString();
    if (!payload.isEmpty()) out << payload;
    for (int i = 0; i < it->childCount(); ++i) collectLeaves(it->child(i), out);
}

} // namespace

QStringList Browser::audioFilesIn(const QMimeData* mime) {
    QStringList out;
    if (!mime) return out;
    if (mime->hasFormat(mimeType())) {
        const QString payload = QString::fromUtf8(mime->data(mimeType()));
        if (payload.endsWith("|s")) out << payload.chopped(2);
        return out;
    }
    for (const QUrl& u : mime->urls())
        if (u.isLocalFile() && samples::isAudioFile(u.toLocalFile())) out << QFileInfo(u.toLocalFile()).absoluteFilePath();
    return out;
}

QStringList Browser::modelFilesIn(const QMimeData* mime) {
    QStringList out;
    if (!mime) return out;
    if (mime->hasFormat(mimeType())) {
        const QString payload = QString::fromUtf8(mime->data(mimeType()));
        if (payload.endsWith("|n")) out << payload.chopped(2);
        return out;
    }
    for (const QUrl& u : mime->urls())
        if (u.isLocalFile() && nam::isModelFile(u.toLocalFile())) out << QFileInfo(u.toLocalFile()).absoluteFilePath();
    return out;
}

QStringList Browser::irFilesIn(const QMimeData* mime) {
    QStringList out;
    if (!mime || !mime->hasFormat(mimeType())) return out;
    const QString payload = QString::fromUtf8(mime->data(mimeType()));
    if (payload.endsWith("|r")) out << payload.chopped(2);
    return out;
}

QStringList Browser::clipFilesIn(const QMimeData* mime) {
    QStringList out;
    if (!mime) return out;
    if (mime->hasFormat(mimeType())) {
        const QString payload = QString::fromUtf8(mime->data(mimeType()));
        if (payload.endsWith("|c")) out << payload.chopped(2);
        return out;
    }
    for (const QUrl& u : mime->urls())
        if (u.isLocalFile() && presets::isClipFile(u.toLocalFile())) out << QFileInfo(u.toLocalFile()).absoluteFilePath();
    return out;
}

Browser::Browser(QWidget* parent) : QWidget(parent) {
    setObjectName("browser");
    setMinimumWidth(280);   // the splitter next to it sets the width (View > Browser hides it)
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(4);

    m_search = new QLineEdit(this);
    m_search->setObjectName("browserSearch"); m_search->setAccessibleName("Search library");
    m_search->setPlaceholderText("Search"); m_search->setFixedHeight(30);
    auto* searchIcon = m_search->addAction(browsericons::icon(Symbol::Search), QLineEdit::LeadingPosition); searchIcon->setObjectName("browserSearchIcon");
    auto* clear = m_search->addAction(browsericons::icon(Symbol::Close), QLineEdit::TrailingPosition); clear->setObjectName("browserClearSearch");
    clear->setToolTip("Clear search"); clear->setVisible(false); connect(clear, &QAction::triggered, m_search, &QLineEdit::clear);
    connect(m_search, &QLineEdit::textChanged, clear, [clear](const QString& text) { clear->setVisible(!text.isEmpty()); });
    InfoView::attach(m_search, "Search", "Filters items, and the samples in your folders, as you type.");
    connect(m_search, &QLineEdit::textChanged, this, &Browser::rebuild);
    lay->addWidget(m_search);

    auto* panes = new QSplitter(Qt::Horizontal, this); panes->setObjectName("browserPanes");
    panes->setChildrenCollapsible(false); panes->setHandleWidth(4);
    m_categories = new QListWidget(this);
    m_categories->setObjectName("browserSidebar");
    m_categories->setMinimumWidth(142); m_categories->setMaximumWidth(232);
    m_categories->setItemDelegate(new BrowserRows(true, m_categories));
    m_categories->setIconSize(QSize(18, 18)); m_categories->setMouseTracking(true);
    m_categories->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_categories->setFocusPolicy(Qt::NoFocus);
    m_categories->setContextMenuPolicy(Qt::CustomContextMenu);
    InfoView::attach(m_categories, "Library", "Click a category to show what it holds. Add Folder… adds a folder of samples; right-click a folder to remove it.");
    connect(m_categories, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
        const QString key = it->data(Qt::UserRole).toString();
        if (key == kHeader) return;
        if (key == kAddFolder) {
            const QString dir = QFileDialog::getExistingDirectory(this, "Add a Folder of Samples", QDir::homePath());
            if (!dir.isEmpty()) addFolder(dir);
            return;
        }
        if (key == kAddIrFolder) {
            const QString dir = QFileDialog::getExistingDirectory(this, "Add a Folder of Impulse Responses", QDir::homePath());
            if (!dir.isEmpty()) addIrFolder(dir);
            return;
        }
        if (key.startsWith(kFolderPrefix)) scanCache().remove(key.mid(kFolderPrefix.size()));   // pick up new files
        m_category = key;
        m_search->clear();
        rebuild();
    });
    connect(m_categories, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* it = m_categories->itemAt(pos);
        const QString key = it ? it->data(Qt::UserRole).toString() : QString();
        QMenu menu(this);
        if (key == irCategory()) {   // the Impulse Responses folder: open it, add a folder of your own
            QAction* open = menu.addAction("Show Impulse Responses Folder");
            QAction* add = menu.addAction("Add IR Folder…");
            QAction* chosen = menu.exec(m_categories->viewport()->mapToGlobal(pos));
            if (chosen == open) { QDir().mkpath(import::irFolder()); QDesktopServices::openUrl(QUrl::fromLocalFile(import::irFolder())); }
            else if (chosen == add) {
                const QString dir = QFileDialog::getExistingDirectory(this, "Add a Folder of Impulse Responses", QDir::homePath());
                if (!dir.isEmpty()) addIrFolder(dir);
            }
            return;
        }
        if (key.startsWith(kIrFolderPrefix)) {
            const QString dir = key.mid(kIrFolderPrefix.size());
            QAction* remove = menu.addAction("Remove IR Folder");
            if (menu.exec(m_categories->viewport()->mapToGlobal(pos)) == remove) {
                import::removeIrFolder(dir);
                if (m_category == key) m_category = irCategory();
                rebuild();
            }
            return;
        }
        if (!key.startsWith(kFolderPrefix)) return;
        const QString dir = key.mid(kFolderPrefix.size());
        QAction* rescan = menu.addAction("Rescan");
        QAction* remove = menu.addAction("Remove Folder");
        QAction* chosen = menu.exec(m_categories->viewport()->mapToGlobal(pos));
        if (chosen == remove) removeFolder(dir);
        else if (chosen == rescan) { scanCache().remove(dir); rebuild(); }
    });
    panes->addWidget(m_categories);

    auto* results = new QWidget(panes); auto* content = new QVBoxLayout(results);
    content->setContentsMargins(0, 0, 0, 0); content->setSpacing(0);
    auto* heading = new QWidget(results); heading->setObjectName("browserContentHeader"); heading->setFixedHeight(28);
    auto* header = new QHBoxLayout(heading); header->setContentsMargins(9, 0, 9, 0); header->setSpacing(6);
    m_location = new BrowserLabel(heading); m_location->setObjectName("browserLocation"); m_location->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_count = new QLabel(heading); m_count->setObjectName("browserCount"); header->addWidget(m_location, 1); header->addWidget(m_count);
    content->addWidget(heading);
    m_items = new ItemTree(this);
    m_items->setObjectName("browserItems");
    m_items->setHeaderHidden(true);
    m_items->setColumnCount(1);
    m_items->setRootIsDecorated(true);
    m_items->setIndentation(18);
    m_items->setItemDelegate(new BrowserRows(false, m_items));
    m_items->setIconSize(QSize(18, 18));
    m_items->setMouseTracking(true);
    m_items->header()->setStretchLastSection(true);
    m_items->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_items->setUniformRowHeights(true);
    m_items->setExpandsOnDoubleClick(false);
    m_items->setContextMenuPolicy(Qt::CustomContextMenu);
    InfoView::attach(m_items, "Browser", "Double-click an item to add it to the selected track, or drag it: onto a track (its column, mixer strip or Arrangement lane), "
                                         "a place in the Device View, or the empty space for a new track. Samples go onto audio tracks or into a Sampler. "
                                         "Folders open and close with their arrows; right-click an item for more.");
    connect(m_items, &QTreeWidget::customContextMenuRequested, this, &Browser::itemMenu);
    connect(m_items, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* it) { const auto key = it->data(0, kKeyRole).toString(); if (!key.isEmpty()) m_expanded.insert(key); });
    connect(m_items, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* it) { m_expanded.remove(it->data(0, kKeyRole).toString()); });
    m_expanded = {"ir:builtin", "ir:mine", "user:clips", "user:captures"};   // open to begin with; close one and it stays closed
    connect(presets::Library::instance(), &presets::Library::changed, this, &Browser::rebuild);
    connect(m_items, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it) {
        QStringList parts = it->data(0, kPayloadRole).toString().split('|');
        if (parts.size() < 2) return;
        const QString kind = parts.takeLast();
        stopPreview();   // it's chosen: the preview doesn't play on over what was just loaded
        emit itemActivated(parts.join('|'), kind.isEmpty() ? QChar('i') : kind.at(0));
    });
    connect(m_items, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* it) {
        const QString payload = it->data(0, kPayloadRole).toString();
        if (payload.isEmpty() && !it->data(0, kKeyRole).toString().isEmpty()) { it->setExpanded(!it->isExpanded()); return; }   // a folder: a click opens or closes it
        if (payload.endsWith("|s") && m_preview->isChecked()) { m_previewPending = true; emit previewRequested(payload.chopped(2)); }
        else if (!payload.endsWith("|s")) stopPreview();   // another kind of item: the sample stops
    });
    m_items->installEventFilter(this);   // Esc stops the preview
    content->addWidget(m_items, 1);
    panes->addWidget(results); panes->setStretchFactor(0, 0); panes->setStretchFactor(1, 1);
    panes->setSizes({156, 180});
    connect(panes, &QSplitter::splitterMoved, this, [panes] { QSettings("Sessions", "Sessions").setValue("browser/sidebarWidth", panes->sizes().value(0)); });
    const int sidebarWidth = QSettings("Sessions", "Sessions").value("browser/sidebarWidth", 156).toInt();
    panes->setSizes({std::clamp(sidebarWidth, 142, 232), 200});
    lay->addWidget(panes, 1);

    auto* footer = new QWidget(this); footer->setObjectName("browserPreviewBar");
    auto* previewRow = new QHBoxLayout(footer); previewRow->setContentsMargins(0, 4, 0, 0); previewRow->setSpacing(9);
    m_preview = new QToolButton(footer); m_preview->setObjectName("browserPreview"); m_preview->setAccessibleName("Preview samples");
    m_preview->setText("Preview"); m_preview->setIcon(browsericons::icon(Symbol::Headphones, true)); m_preview->setIconSize(QSize(18, 18));
    m_preview->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); m_preview->setCheckable(true); m_preview->setFocusPolicy(Qt::NoFocus);
    m_preview->setFixedHeight(28); m_preview->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_preview->setChecked(QSettings("Sessions", "Sessions").value("browser/preview", true).toBool());
    InfoView::attach(m_preview, "Preview samples", "On: clicking a sample plays it at the Cue volume. The headphones button toggles preview.");
    connect(m_preview, &QToolButton::toggled, this, [this](bool on) {
        QSettings("Sessions", "Sessions").setValue("browser/preview", on);
        if (!on) stopPreview();
    });
    m_previewName = new BrowserLabel(footer); m_previewName->setObjectName("browserPreviewName"); m_previewName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_previewStop = new QToolButton(footer); m_previewStop->setObjectName("browserPreviewStop"); m_previewStop->setAccessibleName("Stop preview");
    m_previewStop->setIcon(browsericons::icon(Symbol::Stop)); m_previewStop->setIconSize(QSize(16, 16));
    m_previewStop->setFixedSize(28, 28); m_previewStop->setFocusPolicy(Qt::NoFocus); m_previewStop->setToolTip("Stop preview (Esc)");
    m_previewStop->hide();
    InfoView::attach(m_previewStop, "Stop preview", "Stops the sample playing in the preview. Esc in the list, turning Preview off or loading an item stops it too.");
    connect(m_previewStop, &QToolButton::clicked, this, &Browser::stopPreview);
    previewRow->addWidget(m_preview); previewRow->addWidget(m_previewName, 1); previewRow->addWidget(m_previewStop); lay->addWidget(footer);
    m_previewTimer = new QTimer(this); m_previewTimer->setInterval(100);
    connect(m_previewTimer, &QTimer::timeout, this, [this] { if (!m_previewProbe || !m_previewProbe()) previewEnded(); });
    connect(m_items, &QTreeWidget::currentItemChanged, this, [this] { showPreviewName(); });
    connect(theme::notifier(), &theme::Notifier::changed, this, &Browser::refreshAppearance);

    for (const QString& dir : QSettings("Sessions", "Sessions").value("browser/folders").toStringList())
        if (QDir(dir).exists()) m_folders << dir;
    rebuild();
}

// The bar names the preview while it plays (in the text colour), else the selected sample.
void Browser::showPreviewName() {
    QString path = m_previewing;
    if (path.isEmpty()) {
        const QTreeWidgetItem* it = m_items->currentItem();
        const QString payload = it ? it->data(0, kPayloadRole).toString() : QString();
        if (payload.endsWith("|s")) path = payload.chopped(2);
    }
    m_previewName->setText(path.isEmpty() ? QString() : QFileInfo(path).fileName());
    m_previewName->setToolTip(path.isEmpty() ? QString() : QDir::toNativeSeparators(path));
    m_previewName->setProperty("playing", !m_previewing.isEmpty());
    m_previewName->style()->unpolish(m_previewName); m_previewName->style()->polish(m_previewName);
}

void Browser::previewStarted(const QString& path) {
    m_previewPending = false;
    m_previewing = path;
    m_previewStop->show();
    showPreviewName();
    m_previewTimer->start();
}

void Browser::previewEnded() {
    m_previewPending = false;
    m_previewTimer->stop();
    if (m_previewing.isEmpty() && m_previewStop->isHidden()) return;
    m_previewing.clear();
    m_previewStop->hide();
    showPreviewName();
}

void Browser::stopPreview() {
    if (m_previewing.isEmpty() && !m_previewPending) return;
    emit previewStopRequested();
    previewEnded();
}

bool Browser::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_items && event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape
        && (!m_previewing.isEmpty() || m_previewPending)) {
        stopPreview();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void Browser::refreshAppearance() {
    m_search->findChild<QAction*>("browserSearchIcon")->setIcon(browsericons::icon(Symbol::Search));
    m_search->findChild<QAction*>("browserClearSearch")->setIcon(browsericons::icon(Symbol::Close));
    m_preview->setIcon(browsericons::icon(Symbol::Headphones, true));
    m_previewStop->setIcon(browsericons::icon(Symbol::Stop));
    m_categories->viewport()->update(); m_items->viewport()->update();
}

void Browser::saveFolders() { QSettings("Sessions", "Sessions").setValue("browser/folders", m_folders); }

void Browser::setProjectFolder(const QString& dir) {
    const QString abs = dir.isEmpty() ? QString() : QDir(dir).absolutePath();
    if (abs == m_projectFolder) return;
    if (!m_projectFolder.isEmpty() && m_category == kFolderPrefix + m_projectFolder) m_category = "Instruments";
    m_projectFolder = abs;
    if (!abs.isEmpty()) scanCache().remove(abs);
    rebuild();
}

void Browser::addFolder(const QString& dir) {
    const QString abs = QDir(dir).absolutePath();
    if (!m_folders.contains(abs)) { m_folders << abs; saveFolders(); }
    scanCache().remove(abs);
    m_category = kFolderPrefix + abs;
    m_search->clear();
    rebuild();
}

void Browser::removeFolder(const QString& dir) {
    m_folders.removeAll(dir);
    saveFolders();
    if (m_category == kFolderPrefix + dir) m_category = "Instruments";
    rebuild();
}

QStringList Browser::scan(const QString& dir) const {
    auto it = scanCache().constFind(dir);
    if (it != scanCache().constEnd()) return *it;
    QStringList files;
    QDirIterator walk(dir, samples::nameFilters() + nam::nameFilters(), QDir::Files | QDir::Readable, QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
    const QDir base(dir);
    while (walk.hasNext() && files.size() < kMaxFiles) files << base.relativeFilePath(walk.next());
    files.sort(Qt::CaseInsensitive);
    scanCache().insert(dir, files);
    return files;
}

void Browser::showUserLibrary() {
    m_category = kUserLibrary;
    m_search->clear();
    rebuild();
}

void Browser::showCategory(const QString& name) {
    m_category = name;
    m_search->clear();
    rebuild();
}

void Browser::addIrFolder(const QString& dir) {
    import::addIrFolder(dir);
    m_category = kIrFolderPrefix + QDir(dir).absolutePath();
    m_search->clear();
    rebuild();
}

QStringList Browser::listedItems() const {
    QStringList out;
    for (int i = 0; i < m_items->topLevelItemCount(); ++i) collectLeaves(m_items->topLevelItem(i), out);
    return out;
}

QTreeWidgetItem* Browser::leaf(QTreeWidgetItem* parent, const QString& text, const QString& payload, const QString& tip) {
    auto* it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_items);
    it->setText(0, text);
    if (!payload.isEmpty()) it->setIcon(0, browsericons::icon(payloadIcon(payload)));
    it->setData(0, kPayloadRole, payload);
    if (!tip.isEmpty()) it->setToolTip(0, tip);
    if (payload.isEmpty()) it->setFlags(Qt::NoItemFlags);   // a note ("No audio files here")
    else it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemNeverHasChildren);
    return it;
}

QTreeWidgetItem* Browser::node(QTreeWidgetItem* parent, const QString& text, const QString& key, const QString& path) {
    auto* it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_items);
    it->setText(0, text);
    it->setIcon(0, key == "cat:user" ? browsericons::icon(Symbol::Library) : key == "user:clips" ? browsericons::icon(Symbol::Clip)
        : key == "user:captures" ? browsericons::icon(Symbol::Capture) : key.startsWith("ir:") ? browsericons::icon(Symbol::Impulse)
        : key.startsWith("cat:") ? browsericons::icon(categoryIcon(text)) : browsericons::folder());
    it->setData(0, kKeyRole, key);
    it->setData(0, kPathRole, path);
    it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    it->setForeground(0, theme::textSoft);
    if (!path.isEmpty()) it->setToolTip(0, QDir::toNativeSeparators(path));
    it->setExpanded(m_expanded.contains(key));
    return it;
}

// A folder's files (relative paths, sorted) as nested nodes: subfolders first, then the files.
// `filter` (lower case): only files whose relative path holds it, flat, with the file's name.
void Browser::addFiles(QTreeWidgetItem* parent, const QString& dir, const QStringList& rel, const QString& filter) {
    struct Dir { QMap<QString, Dir> dirs; QStringList files; };
    Dir root;
    for (const QString& r : rel) {
        if (!filter.isEmpty()) { if (r.toLower().contains(filter)) root.files << r; continue; }
        Dir* d = &root;
        const QStringList parts = r.split('/');
        for (int i = 0; i + 1 < parts.size(); ++i) d = &d->dirs[parts[i]];
        d->files << r;
    }
    std::function<void(QTreeWidgetItem*, const Dir&, const QString&)> emitDir = [&](QTreeWidgetItem* into, const Dir& d, const QString& relDir) {
        for (auto it = d.dirs.constBegin(); it != d.dirs.constEnd(); ++it) {
            const QString sub = relDir.isEmpty() ? it.key() : relDir + "/" + it.key();
            emitDir(node(into, it.key(), dir + "/" + sub, QDir(dir).filePath(sub)), it.value(), sub);
        }
        for (const QString& r : d.files) {
            const QString abs = QDir(dir).filePath(r);
            const bool capture = nam::isModelFile(abs);   // a Neural Amp Modeler capture among the samples
            leaf(into, QFileInfo(r).fileName(), abs + (capture ? "|n" : "|s"),
                 QDir::toNativeSeparators(abs) + (capture ? "\nNeural Amp Modeler capture: drag it onto an audio track (or a NAM device), or double-click to load it onto the selected track."
                                                          : "\nClick to preview, drag onto a track, or double-click to load."));
        }
    };
    emitDir(parent, root, QString());
}

// The right-click menu of the content pane: what the item under the pointer offers.
void Browser::itemMenu(const QPoint& pos) {
    QTreeWidgetItem* it = m_items->itemAt(pos);
    if (!it) return;
    const QString payload = it->data(0, kPayloadRole).toString();
    const QString nodePath = it->data(0, kPathRole).toString();
    QMenu menu(this);
    if (payload.isEmpty()) {   // a folder node
        if (it->childCount()) menu.addAction(it->isExpanded() ? "Collapse" : "Expand", this, [it] { it->setExpanded(!it->isExpanded()); });
        if (!nodePath.isEmpty()) {
            menu.addAction("Show in File Manager", this, [nodePath] { QDesktopServices::openUrl(QUrl::fromLocalFile(nodePath)); });
            menu.addAction("Rescan", this, [this, nodePath] {
                for (auto k = scanCache().keyBegin(); k != scanCache().keyEnd(); ++k) if (nodePath.startsWith(*k)) { scanCache().remove(*k); break; }
                rebuild();
            });
        }
        if (!menu.isEmpty()) menu.exec(m_items->viewport()->mapToGlobal(pos));
        return;
    }
    const QChar kind = payload.at(payload.size() - 1);
    const QString path = payload.chopped(2);
    const QString name = payload.chopped(2);
    menu.addAction(kind == 'c' ? "Load Clip onto Selected Track" : kind == 's' ? "Load onto Selected Track" : "Add to Selected Track", this, [this, name, kind] { emit itemActivated(name, kind); });
    if (kind == 's') menu.addAction("Preview", this, [this, path] { emit previewRequested(path); });
    const bool file = kind == 's' || kind == 'u' || kind == 'c' || kind == 'n' || (kind == 'r' && !audio::isBuiltin(path));
    if (file) menu.addAction("Show in File Manager", this, [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath())); });
    if (kind == 'u' || kind == 'c') {   // yours: rename, delete
        menu.addSeparator();
        menu.addAction("Rename…", this, [this, path, kind] {
            bool ok = false;
            const QString current = kind == 'c' ? presets::clipNameOf(path) : presets::nameOf(path);
            const QString n = QInputDialog::getText(this, kind == 'c' ? "Rename Clip" : "Rename Preset", "Name:", QLineEdit::Normal, current, &ok).trimmed();
            QString error;
            if (!ok || n.isEmpty()) return;
            const bool done = kind == 'c' ? presets::renameClip(path, n, nullptr, &error) : presets::rename(path, n, nullptr, &error);
            if (!done) QMessageBox::warning(this, "Rename", error);
        });
        menu.addAction("Delete", this, [this, path, kind] {
            const QString current = kind == 'c' ? presets::clipNameOf(path) : presets::nameOf(path);
            if (QMessageBox::question(this, kind == 'c' ? "Delete Clip" : "Delete Preset", "Delete \"" + current + "\"? It goes to the trash.") == QMessageBox::Yes) {
                if (kind == 'c') presets::removeClip(path); else presets::remove(path);
            }
        });
    }
    menu.exec(m_items->viewport()->mapToGlobal(pos));
}

void Browser::rebuild() {
    // categories: the built-in ones, your folders, Add Folder…
    {
        QSignalBlocker block(m_categories);
        m_categories->clear();
        auto add = [this](const QString& text, const QString& key, const QString& tip = {}) {
            auto* it = new QListWidgetItem(text, m_categories);
            it->setData(Qt::UserRole, key);
            it->setIcon(key == kUserLibrary ? browsericons::icon(Symbol::Library) : key == kAddFolder || key == kAddIrFolder ? browsericons::icon(Symbol::AddFolder)
                : key == kFolderPrefix + m_projectFolder ? browsericons::icon(Symbol::Project) : key.startsWith(kFolderPrefix) ? browsericons::folder()
                : key.startsWith(kIrFolderPrefix) ? browsericons::icon(Symbol::Impulse) : browsericons::icon(categoryIcon(key)));
            if (!tip.isEmpty()) it->setToolTip(tip);
            if (key == m_category) m_categories->setCurrentItem(it);
        };
        auto section = [this](const QString& title) {
            auto* it = new QListWidgetItem(title, m_categories);
            it->setData(Qt::UserRole, QString(kHeader));
            it->setFlags(Qt::NoItemFlags);
            QFont f = it->font(); f.setPointSizeF(7.5); it->setFont(f);
            it->setForeground(theme::neutral);
        };
        section("Categories");
        for (const Category& c : catalogue()) add(QString::fromUtf8(c.name), c.name);
        add(irCategory(), irCategory(), "Cabinet and room impulse responses: the amp's own cabinets, and the audio files in your Impulse Responses folder "
                                                                     "(right-click to open it, or to add a folder of your own). Drag one onto a Guitar Amp, a NAM or a Cabinet device, or onto a track.");
        section("Places");
        add("User Library", kUserLibrary, "Your presets: right-click a device's title and choose Save as Preset.");
        if (!m_projectFolder.isEmpty())
            add("Current Project", kFolderPrefix + m_projectFolder, "The saved set's folder: " + QDir::toNativeSeparators(m_projectFolder));
        for (const QString& dir : m_folders) add(QDir(dir).dirName(), kFolderPrefix + dir, QDir::toNativeSeparators(dir));
        add("Add Folder…", kAddFolder, "Browse a folder of samples here.");
        for (const QString& dir : import::irFolders()) add(QDir(dir).dirName(), kIrFolderPrefix + dir, "Impulse responses: " + QDir::toNativeSeparators(dir) + "\nRight-click to remove the folder.");
        add("Add IR Folder…", kAddIrFolder, "Browse a folder of impulse responses here (they're listed under Impulse Responses too).");
    }

    m_items->clear();
    const QString q = m_search->text().trimmed().toLower();
    const bool searching = !q.isEmpty();
    // while searching, every source that matches gets a node of its own
    auto group = [&](const QString& title, const QString& key, const QString& path = QString()) -> QTreeWidgetItem* {
        if (!searching) return nullptr;
        m_expanded.insert(key);   // a search's groups start open
        return node(nullptr, title, key, path);
    };
    auto dropEmpty = [](QTreeWidgetItem* n) { if (n && n->childCount() == 0) delete n; };
    for (const Category& c : catalogue()) {
        if (!searching && QString(c.name) != m_category) continue;
        QTreeWidgetItem* parent = group(QString::fromUtf8(c.name), QString("cat:") + c.name);
        for (const Item& it : c.items) {
            if (searching && !QString(it.name).toLower().contains(q)) continue;
            const DeviceSpec* spec = deviceSpec(it.name);
            leaf(parent, QString::fromUtf8(it.name), QString("%1|%2").arg(it.name).arg(it.kind),
                 spec && *spec->help ? QString("%1\nDouble-click to load.").arg(QString::fromUtf8(spec->help)) : QString("%1 — double-click to load").arg(it.name));
        }
        dropEmpty(parent);
    }
    // impulse responses: the built-in cabinets, then the Impulse Responses folder and your IR folders
    if (searching || m_category == irCategory() || m_category.startsWith(kIrFolderPrefix)) {
        QTreeWidgetItem* parent = group(irCategory(), "cat:ir");
        auto addIr = [&](QTreeWidgetItem* into, const QString& path, bool builtin) {
            const QString name = builtin ? audio::builtinName(path) : QFileInfo(path).fileName();
            if (searching && !name.toLower().contains(q)) return;
            leaf(into, name, path + "|r",
                 (builtin ? "Built-in cabinet impulse response: " + name : QDir::toNativeSeparators(path))
                 + "\nDrag it onto a Guitar Amp, NAM or Cabinet device (or onto a track: it goes into the track's amp, or a new Cabinet), or double-click to load it onto the selected track.");
        };
        if (m_category.startsWith(kIrFolderPrefix) && !searching) {
            for (const QString& path : import::irsIn(m_category.mid(kIrFolderPrefix.size()))) addIr(nullptr, path, false);
            if (m_items->topLevelItemCount() == 0) leaf(nullptr, "No audio files here", QString(), QString());
        } else {
            QTreeWidgetItem* cabs = searching ? parent : node(nullptr, "Built-in Cabinets", "ir:builtin");
            for (const QString& path : audio::builtinIrPaths()) addIr(cabs, path, true);
            QStringList files = import::irsIn(import::irFolder());
            for (const QString& dir : import::irFolders()) files += import::irsIn(dir);
            files.removeDuplicates();
            QTreeWidgetItem* mine = searching ? parent : files.isEmpty() ? nullptr : node(nullptr, "Your Impulse Responses", "ir:mine", import::irFolder());
            for (const QString& path : files) addIr(mine, path, false);
        }
        dropEmpty(parent);
    }
    // your presets, clips and captures
    if (searching || m_category == kUserLibrary) {
        QTreeWidgetItem* parent = group("User Library", "cat:user", presets::folder());
        const QStringList mine = presets::list();
        int shown = 0;
        for (const QString& path : mine) {
            const QString name = presets::nameOf(path);
            if (searching && !name.toLower().contains(q)) continue;
            Project::DeviceBlocks blocks;
            presets::read(path, blocks);
            QStringList names;
            char kind = 'f';
            for (const QVector<Device>& b : blocks) {
                names << b.first().name;
                if (b.first().kind == DeviceKind::Instrument) kind = 'i';
                else if (b.first().kind == DeviceKind::MidiEffect && kind != 'i') kind = 'm';
            }
            leaf(parent, name, path + "|u", QString("Your preset: %1.\nDrag it onto a track, or double-click to load. Right-click to rename or delete it.").arg(names.join(" → ")));
            ++shown;
        }
        // the clips you saved (Clips)
        const QStringList clips = presets::listClips();
        QTreeWidgetItem* clipNode = searching ? parent : node(nullptr, "Clips", "user:clips", presets::clipFolder());
        for (const QString& path : clips) {
            const QString name = presets::clipNameOf(path);
            if (searching && !name.toLower().contains(q)) continue;
            leaf(clipNode, name, path + "|c", QString("Your clip: %1\nDrag it onto a clip slot or an Arrangement lane, or double-click to put it on the selected track. Right-click to rename or delete it.")
                                                     .arg(QDir::toNativeSeparators(path)));
            ++shown;
        }
        if (!searching && clipNode->childCount() == 0) leaf(clipNode, "No saved clips", QString(), "Right-click a clip and choose Save Clip to User Library.");
        // .nam captures kept in the User Library folder (any subfolder)
        QTreeWidgetItem* capNode = searching ? parent : nullptr;
        QDirIterator caps(presets::folder(), nam::nameFilters(), QDir::Files | QDir::Readable, QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
        while (caps.hasNext()) {
            const QString abs = caps.next();
            const QString name = QFileInfo(abs).completeBaseName();
            if (searching && !name.toLower().contains(q)) continue;
            if (!capNode) capNode = node(nullptr, "Captures", "user:captures", presets::folder());
            leaf(capNode, name, abs + "|n", QString("Neural Amp Modeler capture: %1\nDrag it onto an audio track (or a NAM device), or double-click to load it onto the selected track.").arg(QDir::toNativeSeparators(abs)));
            ++shown;
        }
        if (!searching && mine.isEmpty() && shown == 0)
            leaf(nullptr, "No saved presets", QString(), "Right-click a device title and choose Save as Preset. Captures stored here are also listed.");
        dropEmpty(parent);
    }
    QStringList places = m_folders;
    if (!m_projectFolder.isEmpty() && !places.contains(m_projectFolder)) places.prepend(m_projectFolder);
    for (const QString& dir : places) {
        if (!searching && m_category != kFolderPrefix + dir) continue;
        const QStringList rel = scan(dir);
        QTreeWidgetItem* parent = group(QDir(dir).dirName(), "folder:" + dir, dir);
        addFiles(parent, dir, rel, q);
        dropEmpty(parent);
    }
    if (!searching && m_category.startsWith(kFolderPrefix) && m_items->topLevelItemCount() == 0)
        leaf(nullptr, "No samples or captures", QString(), "Supported files: " + (samples::nameFilters() + nam::nameFilters()).join(' '));
    if (searching && m_items->topLevelItemCount() == 0) leaf(nullptr, "Nothing matches \"" + m_search->text().trimmed() + "\"", QString(), QString());
    bool hierarchy = false;
    for (int i = 0; i < m_items->topLevelItemCount(); ++i) hierarchy |= m_items->topLevelItem(i)->childCount() > 0;
    m_items->setRootIsDecorated(hierarchy); // Flat catalogues use the full column width.
    const int results = listedItems().size();
    const QString location = searching ? "Search" : m_category == kUserLibrary ? "User Library" : m_category.startsWith(kFolderPrefix) ? QDir(m_category.mid(kFolderPrefix.size())).dirName()
        : m_category.startsWith(kIrFolderPrefix) ? QDir(m_category.mid(kIrFolderPrefix.size())).dirName() : m_category;
    m_location->setText(location); m_location->setToolTip(location); m_count->setText(QString::number(results));
    m_count->setToolTip(QString("%1 item%2").arg(results).arg(results == 1 ? "" : "s"));

}

} // namespace sessions
