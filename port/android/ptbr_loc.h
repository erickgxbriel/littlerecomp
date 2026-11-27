#pragma once
#include <cstdint>

namespace ptbr_loc {

void Load();
void ReloadExternal();

// Called from guest TextBible lookup hook (sub_0024D900). Returns guest VA of translated UTF-16LE string or 0.
uint32_t TranslateTextBible(uint8_t* mem_base, uint32_t token_va, uint32_t orig_wide_va);

// Called from HUDSubtitle hooks (sub_00190C40 / sub_001911D0).
void OnSubtitleActivated(uint32_t wide_va);
void OnSubtitleDeactivated();

// Renders PT-BR HD subtitle overlay at bottom center when enabled.
void DrawSubtitleOverlay(float view_w, float view_h);

int BuiltinCount();
int ExternalCount();
int CapturedCount();

}  // namespace ptbr_loc
