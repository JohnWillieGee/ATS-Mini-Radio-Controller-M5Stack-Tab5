// =============================================================================
// radio_ui.h
// LVGL Screen — ATS Mini Radio Controller
// 1280 x 720, dark theme, two panels
// =============================================================================
#pragma once
#include <lvgl.h>
#include "radio_state.h"

// ── Init — call once after lvgl_init() ───────────────────────────────────────
void radio_ui_init();

// ── Update — call from loop() ~250ms when g_radio.valid ──────────────────────
void radio_ui_update(const RadioState &r, const RadioHistory &h);

// ── Connection status banner ──────────────────────────────────────────────────
void radio_ui_set_connected(bool connected);

// ── Screenshot canvas — call after BMP decode to render mirror ───────────────
// pixel_buf: RGB565 data, 320*170*2 bytes, row-major
void radio_ui_set_screenshot(const uint16_t *pixel_buf);

// ── Screenshot progress — call from loop() during BMP receive ────────────────
// pct: 0–100. Call radio_ui_screenshot_done() when complete or aborted.
void radio_ui_screenshot_progress(int pct);
void radio_ui_screenshot_done(bool success);
