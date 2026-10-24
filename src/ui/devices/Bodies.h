#pragma once
// Private to the device panels: the per-family body factories and layout helpers.

#include "DeviceBody.h"
#include <QBoxLayout>
#include <QFrame>
#include <QLabel>

namespace sessions::devui {

DeviceBody* createInstrumentBody(const DeviceContext& ctx, QWidget* parent);   // nullptr: none of its own
DeviceBody* createEffectBody(const DeviceContext& ctx, QWidget* parent);
DeviceBody* createProductionBody(const DeviceContext& ctx, QWidget* parent);
DeviceBody* createStudioBody(const DeviceContext& ctx, QWidget* parent);
QHBoxLayout* createSidechainSection(DeviceBody* body, DeviceType type);
DeviceBody* createMidiBody(const DeviceContext& ctx, QWidget* parent);
DeviceBody* createRackBody(const DeviceContext& ctx, QWidget* parent);   // Instrument / Audio Effect Rack
DeviceBody* createGuitarBody(const DeviceContext& ctx, QWidget* parent);  // Tuner, Guitar Amp, pedals
DeviceBody* createDrumStudioEditor(const DeviceContext& ctx, QWidget* parent);
DeviceBody* createDrumStudioBody(const DeviceContext& ctx, QWidget* parent);   // DrumStudioBody.cpp
DeviceBody* createOsmiumBody(const DeviceContext& ctx, QWidget* parent);

// A thin vertical line between a device's sections.
inline QFrame* vline(QWidget* parent) {
    auto* f = new QFrame(parent);
    f->setFixedWidth(1);
    f->setStyleSheet("background:#202020;border:0;");
    return f;
}

inline QHBoxLayout* hbox(int spacing = 4) { auto* l = new QHBoxLayout; l->setContentsMargins(0, 0, 0, 0); l->setSpacing(spacing); return l; }
inline QVBoxLayout* vbox(int spacing = 3) { auto* l = new QVBoxLayout; l->setContentsMargins(0, 0, 0, 0); l->setSpacing(spacing); return l; }

// The body's own top-level row, with the panel margins.
inline QHBoxLayout* rootRow(QWidget* body, int spacing = 6) {
    auto* l = new QHBoxLayout(body);
    l->setContentsMargins(8, 6, 8, 6);
    l->setSpacing(spacing);
    return l;
}

// Adds widgets to a box in order, then a stretch so they stay packed at the start.
inline void pack(QBoxLayout* l, std::initializer_list<QWidget*> ws, bool stretch = true) {
    for (QWidget* w : ws) l->addWidget(w);
    if (stretch) l->addStretch(1);
}

} // namespace sessions::devui
