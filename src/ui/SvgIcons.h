#pragma once
// Themed SVG icons for buttons (0.35): an SVG from the resources, drawn in the theme's colours
// at paint time, so View > Theme recolours it and it stays crisp at any size and pixel ratio.
// The files are black strokes / fills (#000000) on nothing, 24 × 24, 1.7 px round strokes, like
// the browser's (assets/browser); the colour stands in for the black when it's painted: the
// button's text colour when up, the dark on-accent colour when checked (the stylesheet lights
// the button then), the disabled colour when disabled.

#include <QIcon>
#include <QString>

namespace sessions::svgicons {

// The icon for ":/<folder>/<name>.svg", e.g. themed("controlbar", "play").
QIcon themed(const QString& folder, const QString& name);

} // namespace sessions::svgicons
