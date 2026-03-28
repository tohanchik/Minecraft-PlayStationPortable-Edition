#pragma once

// Very small pig mob renderer used for visual debugging/gameplay feedback.
// The mob is intentionally always visible (opaque solid mesh fallback).

bool PigMob_Init(float playerX, float playerY, float playerZ, float playerYawDeg);
void PigMob_Update(float dt, float playerX, float playerY, float playerZ, float playerYawDeg);
void PigMob_Render();
void PigMob_Shutdown();
