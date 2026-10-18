#pragma once
// Small shared UI helpers: in-place renaming and the track context menu
// (used by Session headers and Arrangement headers alike).

#include <QRect>
#include <QString>
#include <QColor>
#include <functional>

class QWidget;
class QPoint;
class QMenu;

namespace sessions {

class Project;

// Shows a line edit over `rect`; Enter or clicking away commits (calls `done`), Escape cancels.
void inlineEdit(QWidget* parent, const QRect& rect, const QString& text, std::function<void(const QString&)> done);

// Right-click menu for a track or return track. `rename` starts an inline rename in the caller.
void showTrackMenu(QWidget* parent, const QPoint& globalPos, Project* project, int trackIndex, std::function<void()> rename);
// The colour grid (0.21): Project::palette() as rows of squares inside `menu`; a click calls `pick`
// and closes the menu. `current` is outlined.
void addColorGrid(QMenu* menu, const QColor& current, std::function<void(const QColor&)> pick);

} // namespace sessions
