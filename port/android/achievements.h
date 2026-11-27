#pragma once
#include <cstdint>

namespace achievements {

void OnDataBaseConstructed(uint32_t db_va);
void OnDataBaseDestroyed(uint32_t db_va);

void Load();
void Save();
void ResetProgress();

// Called at ~2 Hz from main loop when guest is running; O(1) RAM reads, zero impact on FPS.
void Poll();

// Renders animated unlock toast banner and the dedicated Achievements tab.
void DrawToast(float view_w, float view_h);
void DrawTab();

int UnlockedCount();
int TotalCount();
int EarnedPoints();
int TotalPoints();

}  // namespace achievements
