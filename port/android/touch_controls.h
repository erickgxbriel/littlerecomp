#pragma once
#include <SDL3/SDL.h>

#include "core/input.h"

namespace touch {

// Handles SDL finger events in normalized [0..1] window coordinates.
// Returns true if the event was consumed by the virtual controls / menu pill.
bool HandleEvent(const SDL_Event& e, float view_w, float view_h, bool overlay_open, bool enabled);

// Clears all active fingers (e.g. when opening the overlay or losing focus).
void Clear();

// Draws translucent touch controls onto ImGui's background draw list.
void DrawOverlay(float view_w, float view_h, float opacity, bool enabled, bool overlay_open);

// Thread-safe snapshot of virtual pad state for guest threads.
input::Pad GetPad();

// True if any touch input is currently active.
bool IsActive();

// Touch layout persistence & interactive on-screen editor.
void LoadLayout();
void SaveLayout();
void ResetLayout();
void SetEditing(bool editing);
bool IsEditing();

}  // namespace touch
