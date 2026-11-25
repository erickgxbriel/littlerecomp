#pragma once

// Rewrites cached HUD text objects after the active input device changes.
void ApplyHudDeviceText(bool key_prompts);

namespace hud_prompts {

// Effective prompt mode: bracketed key labels or the original pad glyphs.
bool UseKeyLabels();

// Rebuilds the bracketed key labels after the keyboard-to-pad bindings change.
void RefreshKeyLabels();

// The music library snapshots the input device when its page is built and keeps those prompts until another page
// is loaded. Other screens keep following the active device immediately.
bool Frozen();

// Called by the page loader. `key_labels` is the mode to hold while the music library is open.
void SetMusicLibraryPage(bool active, bool key_labels);

}  // namespace hud_prompts
