#include "ThemeEditor.h"
#include "InfoView.h"
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>
#include <QSignalBlocker>
#include <cmath>

namespace sessions {

namespace {

// A swatch: the colour over a checkerboard (so alpha shows), with its hex as the tooltip.
void paintSwatch(QToolButton* b, const QColor& c) {
    QPixmap pm(34, 14);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    for (int y = 0; y < 14; y += 7) for (int x = 0; x < 34; x += 7) p.fillRect(x, y, 7, 7, ((x + y) / 7) % 2 ? QColor("#9a9a9a") : QColor("#e6e6e6"));
    p.fillRect(pm.rect(), c);
    p.end();
    b->setIcon(QIcon(pm));
    b->setIconSize(pm.size());
    b->setToolTip(c.name(c.alpha() < 255 ? QColor::HexArgb : QColor::HexRgb));
}

} // namespace

ThemeEditor::ThemeEditor(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Customize Theme");
    setObjectName("themeEditor");
    setMinimumSize(560, 520);
    resize(620, 640);
    auto* lay = new QVBoxLayout(this);
    lay->setSpacing(8);

    // ---- the theme: which one, its name, save / delete / revert
    auto* top = new QHBoxLayout;
    top->setSpacing(6);
    m_presets = new QComboBox(this);
    m_presets->setObjectName("themePresets");
    m_presets->setMinimumWidth(150);
    InfoView::attach(m_presets, "Theme", "Start from one of the built-in themes or one of your own.");
    top->addWidget(new QLabel("Start from", this));
    top->addWidget(m_presets);
    m_name = new QLineEdit(this);
    m_name->setObjectName("themeName");
    m_name->setPlaceholderText("Name");
    InfoView::attach(m_name, "Name", "What your theme is called in View > Theme.");
    top->addWidget(new QLabel("Name", this));
    top->addWidget(m_name, 1);
    m_save = new QPushButton("Save", this);
    m_save->setObjectName("themeSave");
    InfoView::attach(m_save, "Save Theme", "Keeps the theme as yours (a .sessionstheme file) and lists it in View > Theme.");
    m_remove = new QPushButton("Delete", this);
    m_remove->setObjectName("themeDelete");
    InfoView::attach(m_remove, "Delete Theme", "Deletes this theme of yours (the built-in ones can't be deleted).");
    m_revert = new QPushButton("Revert", this);
    InfoView::attach(m_revert, "Revert", "Back to the theme you started from, as it was.");
    top->addWidget(m_save);
    top->addWidget(m_remove);
    top->addWidget(m_revert);
    lay->addLayout(top);

    // ---- tint
    auto* tintBox = new QWidget(this);
    auto* tf = new QFormLayout(tintBox);
    tf->setContentsMargins(0, 0, 0, 0);
    tf->setHorizontalSpacing(10);
    tf->setVerticalSpacing(4);
    m_base = new QComboBox(tintBox);
    m_base->setObjectName("themeBase");
    m_base->addItems({"Dark", "Mid", "Light"});
    InfoView::attach(m_base, "Base", "The palette the theme is built on: the dark, the mid grey or the light one.");
    tf->addRow("Base", m_base);
    auto* hueRow = new QHBoxLayout;
    m_tintOn = new QCheckBox("Tint the greys", tintBox);
    m_tintOn->setObjectName("themeTintOn");
    InfoView::attach(m_tintOn, "Tint", "Gives every grey a hue: blue for a cool look, amber for a warm one.");
    m_hue = new QSlider(Qt::Horizontal, tintBox);
    m_hue->setObjectName("themeHue");
    m_hue->setRange(0, 359);
    InfoView::attach(m_hue, "Hue", "Which hue the greys take, round the colour wheel.");
    m_hueText = new QLabel(tintBox);
    m_hueText->setMinimumWidth(32);
    hueRow->addWidget(m_tintOn);
    hueRow->addWidget(m_hue, 1);
    hueRow->addWidget(m_hueText);
    tf->addRow("Hue", hueRow);
    m_tint = new QSlider(Qt::Horizontal, tintBox);
    m_tint->setObjectName("themeTint");
    m_tint->setRange(0, 100);
    InfoView::attach(m_tint, "Amount", "How much of the hue the greys take: a hint, or a strong colour cast.");
    tf->addRow("Amount", m_tint);
    m_contrast = new QSlider(Qt::Horizontal, tintBox);
    m_contrast->setObjectName("themeContrast");
    m_contrast->setRange(-100, 100);
    InfoView::attach(m_contrast, "Contrast", "Pushes the greys apart from the background (right) or closer together (left).");
    tf->addRow("Contrast", m_contrast);
    auto* accentRow = new QHBoxLayout;
    m_accent = new QToolButton(tintBox);
    m_accent->setObjectName("themeAccent");
    InfoView::attach(m_accent, "Accent", "The highlight colour: lit buttons, the selected tab, curves and readouts. Text on it goes dark or light by itself.");
    m_accentReset = new QToolButton(tintBox);
    m_accentReset->setText("↺");
    InfoView::attach(m_accentReset, "Base Accent", "Back to the base palette's own accent.");
    accentRow->addWidget(m_accent);
    accentRow->addWidget(m_accentReset);
    accentRow->addStretch(1);
    tf->addRow("Accent", accentRow);
    lay->addWidget(tintBox);

    // ---- every colour, grouped
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* host = new QWidget(scroll);
    auto* grid = new QGridLayout(host);
    grid->setContentsMargins(0, 0, 6, 0);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(2);
    const QStringList order = {"Window", "Transport", "Session View", "Arrangement", "Editors", "Piano Roll", "Widgets", "Device View", "Device Panels"};
    const QStringList names = theme::colourNames();
    int row = 0;
    for (const QString& g : order) {
        auto* head = new QLabel(g, host);
        head->setObjectName("infoTitle");
        grid->addWidget(head, row++, 0, 1, 6);
        int col = 0;
        for (const QString& n : names) {
            if (theme::group(n) != g) continue;
            auto* label = new QLabel(n, host);
            label->setObjectName("dimLabel");
            auto* swatch = new QToolButton(host);
            swatch->setObjectName("swatch");
            swatch->setProperty("colourName", n);
            swatch->setFocusPolicy(Qt::NoFocus);
            InfoView::attach(swatch, n, "Click to pick this colour (alpha too).");
            auto* reset = new QToolButton(host);
            reset->setText("↺");
            reset->setFocusPolicy(Qt::NoFocus);
            reset->setStyleSheet("QToolButton{padding:0 3px;min-height:12px;font-size:9px;}");
            InfoView::attach(reset, "Theme's Value", "Back to what the theme makes of this colour.");
            connect(swatch, &QToolButton::clicked, this, [this, n] {
                const QColor c = QColorDialog::getColor(theme::resolve(m_theme, n), this, n, QColorDialog::ShowAlphaChannel);
                if (c.isValid()) setColour(n, c);
            });
            connect(reset, &QToolButton::clicked, this, [this, n] { setColour(n, QColor()); });
            grid->addWidget(label, row, col * 3);
            grid->addWidget(swatch, row, col * 3 + 1);
            grid->addWidget(reset, row, col * 3 + 2);
            m_rows.insert(n, {swatch, reset});
            if (++col == 2) { col = 0; ++row; }
        }
        if (col) ++row;
    }
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(3, 1);
    scroll->setWidget(host);
    lay->addWidget(scroll, 1);

    // ---- wiring
    connect(m_presets, &QComboBox::activated, this, [this](int i) {
        bool found = false;
        const theme::Theme t = theme::themeNamed(m_presets->itemData(i).toString(), &found);
        if (found) load(t);
    });
    connect(m_name, &QLineEdit::textEdited, this, [this](const QString& t) { m_theme.name = t; m_theme.builtin = false; m_remove->setEnabled(false); });
    connect(m_save, &QPushButton::clicked, this, [this] {
        theme::Theme t = m_theme;
        t.name = m_name->text().trimmed();
        for (const theme::Theme& b : theme::builtinThemes())
            if (b.name.compare(t.name, Qt::CaseInsensitive) == 0) t.name = uniqueName(t.name + " (mine)");
        if (t.name.isEmpty()) t.name = uniqueName("My Theme");
        QString error;
        if (!theme::saveCustom(t, &error)) { QMessageBox::warning(this, "Save Theme", error); return; }
        t.builtin = false;
        m_theme = t;
        m_start = t;
        theme::apply(t);
        theme::save(t);
        fillPresets();
        m_name->setText(t.name);
        m_remove->setEnabled(true);
    });
    connect(m_remove, &QPushButton::clicked, this, [this] {
        if (m_theme.builtin) return;
        if (QMessageBox::question(this, "Delete Theme", "Delete \"" + m_theme.name + "\"?") != QMessageBox::Yes) return;
        theme::removeCustom(m_theme.name);
        load(theme::builtinThemes().first());
        theme::save(theme::Kind::Dark);
        fillPresets();
    });
    connect(m_revert, &QPushButton::clicked, this, [this] { load(m_start); });
    connect(m_base, &QComboBox::activated, this, [this](int i) { m_theme.base = theme::Kind(i); applyNow(); });
    connect(m_tintOn, &QCheckBox::toggled, this, [this](bool on) {
        if (m_loading) return;
        if (on && m_theme.hue < 0) m_theme.hue = m_hue->value();
        if (!on) m_theme.hue = -1;
        if (on && m_theme.tint < 0.001) { m_theme.tint = 0.4; }
        applyNow(); refreshTint();
    });
    connect(m_hue, &QSlider::valueChanged, this, [this](int v) { if (m_loading) return; m_theme.hue = v; if (!m_tintOn->isChecked()) { QSignalBlocker b(m_tintOn); m_tintOn->setChecked(true); } if (m_theme.tint < 0.001) m_theme.tint = 0.4; applyNow(); refreshTint(); });
    connect(m_tint, &QSlider::valueChanged, this, [this](int v) { if (m_loading) return; m_theme.tint = v / 100.0; if (m_theme.hue < 0) m_theme.hue = m_hue->value(); applyNow(); refreshTint(); });
    connect(m_contrast, &QSlider::valueChanged, this, [this](int v) { if (m_loading) return; m_theme.contrast = v / 100.0; applyNow(); });
    connect(m_accent, &QToolButton::clicked, this, [this] {
        const QColor c = QColorDialog::getColor(theme::resolve(m_theme, "accent"), this, "Accent");
        if (!c.isValid()) return;
        m_theme.accent = c;
        applyNow(); refreshTint();
    });
    connect(m_accentReset, &QToolButton::clicked, this, [this] { m_theme.accent = QColor(); applyNow(); refreshTint(); });
    connect(theme::notifier(), &theme::Notifier::listChanged, this, &ThemeEditor::fillPresets);

    fillPresets();
    load(theme::current());
}

void ThemeEditor::fillPresets() {
    QSignalBlocker b(m_presets);
    m_presets->clear();
    for (const theme::Theme& t : theme::builtinThemes()) m_presets->addItem(t.name, t.name);
    const QVector<theme::Theme> mine = theme::customThemes();
    if (!mine.isEmpty()) m_presets->insertSeparator(m_presets->count());
    for (const theme::Theme& t : mine) m_presets->addItem(t.name, t.name);
    const int at = m_presets->findData(m_theme.name);
    if (at >= 0) m_presets->setCurrentIndex(at);
}

QString ThemeEditor::uniqueName(const QString& wanted) const {
    QString name = wanted;
    int n = 2;
    auto taken = [&](const QString& x) {
        for (const theme::Theme& t : theme::builtinThemes()) if (t.name.compare(x, Qt::CaseInsensitive) == 0) return true;
        return false;
    };
    while (taken(name)) name = QString("%1 %2").arg(wanted).arg(n++);
    return name;
}

void ThemeEditor::load(const theme::Theme& t) {
    m_loading = true;
    m_theme = t;
    m_start = t;
    m_name->setText(t.name);
    m_remove->setEnabled(!t.builtin);
    { QSignalBlocker b(m_base); m_base->setCurrentIndex(int(t.base)); }
    refreshTint();
    const int at = m_presets->findData(t.name);
    { QSignalBlocker b(m_presets); if (at >= 0) m_presets->setCurrentIndex(at); }
    m_loading = false;
    applyNow();
}

void ThemeEditor::refreshTint() {
    m_loading = true;
    QSignalBlocker b1(m_tintOn), b2(m_hue), b3(m_tint), b4(m_contrast);
    m_tintOn->setChecked(m_theme.hue >= 0);
    m_hue->setValue(m_theme.hue >= 0 ? int(std::lround(m_theme.hue)) % 360 : 220);
    m_hue->setEnabled(true);
    m_hueText->setText(m_theme.hue >= 0 ? QString("%1°").arg(int(std::lround(m_theme.hue)) % 360) : QString("off"));
    m_tint->setValue(int(std::lround(m_theme.tint * 100)));
    m_contrast->setValue(int(std::lround(m_theme.contrast * 100)));
    paintSwatch(m_accent, theme::resolve(m_theme, "accent"));
    m_accentReset->setEnabled(m_theme.accent.isValid());
    m_loading = false;
}

void ThemeEditor::setColour(const QString& name, const QColor& c) {
    if (c.isValid()) m_theme.overrides.insert(name, c); else m_theme.overrides.remove(name);
    applyNow();
}

void ThemeEditor::applyNow() {
    if (m_loading) return;
    theme::apply(m_theme);
    refreshSwatches();
}

void ThemeEditor::refreshSwatches() {
    for (auto it = m_rows.begin(); it != m_rows.end(); ++it) {
        paintSwatch(it->swatch, theme::resolve(m_theme, it.key()));
        it->reset->setVisible(m_theme.overrides.contains(it.key()));
    }
}

} // namespace sessions
