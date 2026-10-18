#pragma once
// ThemeEditor (View > Theme > Customize…): make the interface look the way you like, live.
//  - Start from any theme (the built-in ones or your own), give it a name, Save it as yours
//    (a .sessionstheme file in theme::folder(); it then shows in View > Theme), Delete it.
//  - Tint: the base palette (Dark / Mid / Light), the hue the greys take and how much,
//    a contrast stretch, and the accent colour (the orange: lit buttons, curves, readouts).
//  - Colours: every colour by name, grouped (Window, Session View, Arrangement, …); click a
//    swatch to pick one (alpha included), ↺ puts the theme's own value back.
// Everything applies as you change it; Revert goes back to the theme you started from.

#include "core/Theme.h"
#include <QDialog>
#include <QHash>

class QComboBox;
class QLineEdit;
class QSlider;
class QLabel;
class QToolButton;
class QPushButton;
class QCheckBox;

namespace sessions {

class ThemeEditor : public QDialog {
    Q_OBJECT
public:
    explicit ThemeEditor(QWidget* parent = nullptr);
    const theme::Theme& theme() const { return m_theme; }
    void load(const theme::Theme& t);   // shows and applies it
    void setColour(const QString& name, const QColor& c);   // an override (invalid: back to the theme's value); applies

private:
    void fillPresets();
    void applyNow();
    void refreshSwatches();
    void refreshTint();
    QString uniqueName(const QString& wanted) const;

    theme::Theme m_theme, m_start;
    bool m_loading = false;
    QComboBox*   m_presets;
    QLineEdit*   m_name;
    QPushButton* m_save;
    QPushButton* m_remove;
    QPushButton* m_revert;
    QComboBox*   m_base;
    QCheckBox*   m_tintOn;
    QSlider*     m_hue;
    QSlider*     m_tint;
    QSlider*     m_contrast;
    QToolButton* m_accent;
    QToolButton* m_accentReset;
    QLabel*      m_hueText;
    struct Row { QToolButton* swatch; QToolButton* reset; };
    QHash<QString, Row> m_rows;
};

} // namespace sessions
