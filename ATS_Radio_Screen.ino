// =============================================================================
//  ATS_Radio_Screen.ino
//  Standalone Tab5 sketch — ATS Mini V4 SDR controller + display
//
//  Hardware : M5Stack Tab5 (ESP32-P4, 1280x720 MIPI-DSI)
//  Connected: ATS Mini V4 SDR via USB-A to USB-C OTG cable
//
//  Architecture:
//    Core 0 — usb_host_task (in usb_host_cdc.cpp)
//             Enumerates ATS Mini, receives CSV stream, posts to ring buffer.
//             Callbacks fire from this core — they update g_radio under mutex.
//    Core 1 — Arduino loop()
//             Drives LVGL tick + handler. Reads g_radio under mutex every 250ms.
//             Updates UI from last-known state.
//
//  Files:
//    ATS_Radio_Screen.ino  <- this file
//    usb_host_cdc.h/.cpp   <- USB host CDC driver (from ATS_USB_Test, proven)
//    radio_state.h/.cpp    <- RadioState struct + CSV parser
//    radio_ui.h/.cpp       <- LVGL screen layout + update
// =============================================================================

#include <Arduino.h>
#include <M5Unified.h>       // MUST be first — Tab5 hardware init
#include "esp_log.h"
#include <lvgl.h>

#include "usb_host_cdc.h"
#include "radio_state.h"
#include "radio_ui.h"

// =============================================================================
//  GLOBALS (shared between USB task on core 0 and LVGL loop on core 1)
// =============================================================================

RadioState       g_radio        = {};
RadioHistory     g_history      = {};
SemaphoreHandle_t g_radio_mutex = nullptr;
volatile bool    g_radio_connected = false;
static unsigned long t_bmp_start = 0;   // BMP receive start time (for timeout)

// =============================================================================
//  LVGL BUFFERS
// =============================================================================

static lv_color_t *buf1 = nullptr;
static lv_color_t *buf2 = nullptr;

// =============================================================================
//  LVGL CALLBACKS (identical to WeatherDash — proven for Tab5)
// =============================================================================

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                           lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    M5.Display.setSwapBytes(true);
    M5.Display.pushImage(area->x1, area->y1, w, h, (uint16_t *)color_p);
    lv_disp_flush_ready(drv);
}

static void lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    lgfx::touch_point_t tp[3];
    uint8_t n = M5.Lcd.getTouchRaw(tp, 3);
    if (n > 0) {
        data->state   = LV_INDEV_STATE_PR;
        data->point.x = (1280 - 1) - tp[0].y;
        data->point.y = tp[0].x;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

// =============================================================================
//  LVGL INIT
// =============================================================================

static void lvgl_init() {
    lv_init();

    size_t buf_px = 1280 * 720 / 20;  // 1/20th screen
    buf1 = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    buf2 = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf1 || !buf2) {
        Serial.println("[LVGL] PSRAM alloc failed — falling back");
        heap_caps_free(buf1); heap_caps_free(buf2);
        buf_px = 1280 * 4;
        buf1   = (lv_color_t *)malloc(buf_px * sizeof(lv_color_t));
        buf2   = nullptr;
    }

    static lv_disp_draw_buf_t draw_buf;
    static lv_disp_drv_t      disp_drv;
    static lv_indev_drv_t     indev_drv;

    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_px);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = 1280;
    disp_drv.ver_res  = 720;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), LV_PART_MAIN);

    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_cb;
    lv_indev_drv_register(&indev_drv);

    Serial.println("[LVGL] Initialised 1280x720");
}

// =============================================================================
//  USB HOST CALLBACKS (called from core 0 USB task)
//  Keep these SHORT — just update shared state and return.
// =============================================================================

// Line receive buffer — must be large enough for BMP hex lines (~480 chars/line)
#define LINE_BUF_SIZE 512
static char  s_line_buf[LINE_BUF_SIZE];
static int   s_line_pos = 0;

// Flag set by USB task, consumed by loop() — avoids delay() on core 0
static volatile bool s_restart_stream = false;

void on_device_connected(void) {
    Serial.println("[USB] ATS Mini connected");

    usb_cdc_set_line_coding(115200, 8, 0, 1);
    delay(200);

    // Start CSV stream
    uint8_t cmd = 't';
    usb_cdc_write(&cmd, 1);

    xSemaphoreTake(g_radio_mutex, portMAX_DELAY);
    g_radio_connected = true;
    xSemaphoreGive(g_radio_mutex);

    // Signal UI (safe — LVGL runs on core 1 but label writes are atomic enough
    // for a status string; full state updates use the mutex in loop())
    radio_ui_set_connected(true);
}

void on_device_disconnected(void) {
    Serial.println("[USB] ATS Mini disconnected");
    s_line_pos = 0;

    xSemaphoreTake(g_radio_mutex, portMAX_DELAY);
    g_radio_connected = false;
    g_radio.valid     = false;
    xSemaphoreGive(g_radio_mutex);

    radio_ui_set_connected(false);
}

// ── Screenshot / BMP receive state machine ───────────────────────────────────
// ATS Mini sends 'C' response as a raw BMP file encoded as packed lowercase hex,
// NO spaces, NO separators, NO header line, NO end marker.
// Format: "424d3604000000000000..." (continuous hex stream, ~460KB ASCII)
// xxd -r -p converts it to binary. We do the same inline.
// The BMP is 240x240, either 16bpp RGB565 or 24bpp BGR.
// Transfer ends when the hex stream stops — we detect completion by byte count.

// 320x170 16bpp = 108,866 bytes; 240x240 24bpp = 172,854 bytes — allocate for worst case
#define BMP_MAX_BYTES   (54 + 320 * 320 * 3)    // ~307KB — covers any reasonable ATS BMP
#define BMP_PIXEL_COUNT (320 * 170)   // native ATS Mini framebuffer

enum BmpState { BMP_IDLE, BMP_RECEIVING };
static BmpState  s_bmp_state   = BMP_IDLE;
static uint8_t  *s_bmp_raw     = nullptr;   // PSRAM, allocated once
static size_t    s_bmp_raw_pos = 0;
static char      s_hex_carry   = 0;         // leftover nibble when odd chars arrive
static bool      s_hex_carry_valid = false;

// Hex char → nibble (returns -1 for non-hex)
static inline int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Feed raw bytes from USB directly into the hex decoder (no line buffering needed)
static void bmp_feed_raw(const uint8_t *data, size_t len) {
    if (!s_bmp_raw) return;
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        int n = hex_nibble(c);
        if (n < 0) continue;  // skip newlines, \r, spaces if any

        if (!s_hex_carry_valid) {
            s_hex_carry = (char)n;
            s_hex_carry_valid = true;
        } else {
            uint8_t byte_val = ((uint8_t)s_hex_carry << 4) | (uint8_t)n;
            if (s_bmp_raw_pos < BMP_MAX_BYTES) {
                s_bmp_raw[s_bmp_raw_pos++] = byte_val;
            }
            s_hex_carry_valid = false;

            // Check BMP magic at start
            if (s_bmp_raw_pos == 2) {
                if (s_bmp_raw[0] != 0x42 || s_bmp_raw[1] != 0x4D) {
                    Serial.printf("[BMP] Bad magic: %02X %02X — aborting\n",
                                  s_bmp_raw[0], s_bmp_raw[1]);
                    s_bmp_state = BMP_IDLE;
                    s_bmp_raw_pos = 0;
                    return;
                }
                Serial.println("[BMP] Magic OK (BM) — receiving...");
            }
        }
    }
}

// Check if received byte count matches expected BMP file size
static bool bmp_is_complete() {
    if (s_bmp_raw_pos < 6) return false;
    // BMP file size is at bytes 2-5 (little endian)
    uint32_t file_size = s_bmp_raw[2] | ((uint32_t)s_bmp_raw[3] << 8) |
                         ((uint32_t)s_bmp_raw[4] << 16) | ((uint32_t)s_bmp_raw[5] << 24);
    return (file_size > 54 && s_bmp_raw_pos >= file_size);
}

static void bmp_decode_and_render() {
    if (s_bmp_raw_pos < 54) {
        Serial.printf("[BMP] Too few bytes: %u\n", s_bmp_raw_pos);
        return;
    }

    uint32_t file_size = s_bmp_raw[2] | ((uint32_t)s_bmp_raw[3] << 8) |
                         ((uint32_t)s_bmp_raw[4] << 16) | ((uint32_t)s_bmp_raw[5] << 24);
    uint32_t px_offset = s_bmp_raw[10] | ((uint32_t)s_bmp_raw[11] << 8) |
                         ((uint32_t)s_bmp_raw[12] << 16) | ((uint32_t)s_bmp_raw[13] << 24);
    int bmp_w = (int)(s_bmp_raw[18] | ((uint32_t)s_bmp_raw[19] << 8));
    int bmp_h = (int)(s_bmp_raw[22] | ((uint32_t)s_bmp_raw[23] << 8));
    int bpp   =       s_bmp_raw[28] | ((uint32_t)s_bmp_raw[29] << 8);

    Serial.printf("[BMP] %ux%u %dbpp, file=%u bytes, got=%u bytes\n",
                  bmp_w, bmp_h, bpp, file_size, s_bmp_raw_pos);

    if (bmp_w < 1 || bmp_h < 1 || bpp < 16) {
        Serial.println("[BMP] Unexpected format");
        return;
    }

    uint16_t *rgb565 = (uint16_t *)heap_caps_malloc(
        BMP_PIXEL_COUNT * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgb565) { Serial.println("[BMP] RGB565 alloc failed"); return; }

    int bytes_per_pixel = bpp / 8;
    int row_bytes = ((bmp_w * bytes_per_pixel) + 3) & ~3;

    // Decode directly into 320×170 buffer — clamp if source differs
    int out_w = LV_MIN(bmp_w, 320);
    int out_h = LV_MIN(bmp_h, 170);

    for (int row = 0; row < out_h; row++) {
        int bmp_row = (bmp_h > 0) ? (bmp_h - 1 - row) : row;
        size_t row_off = px_offset + (size_t)bmp_row * row_bytes;

        for (int col = 0; col < out_w; col++) {
            size_t px = row_off + (size_t)col * bytes_per_pixel;
            if (px + (size_t)(bytes_per_pixel - 1) >= s_bmp_raw_pos) break;

            uint16_t pixel;
            if (bpp == 16) {
                pixel = s_bmp_raw[px] | ((uint16_t)s_bmp_raw[px + 1] << 8);
            } else {
                uint8_t b2 = s_bmp_raw[px];
                uint8_t g2 = s_bmp_raw[px + 1];
                uint8_t r2 = s_bmp_raw[px + 2];
                pixel = ((r2 >> 3) << 11) | ((g2 >> 2) << 5) | (b2 >> 3);
            }
            rgb565[row * 320 + col] = pixel;
        }
    }

    radio_ui_set_screenshot(rgb565);
    heap_caps_free(rgb565);
    Serial.println("[BMP] Screenshot rendered OK");
}

void on_data_received(const uint8_t *data, size_t len) {
    // If we're in BMP receive mode, feed bytes directly to the hex decoder
    // (bypasses line buffering — BMP stream has no meaningful line structure)
    if (s_bmp_state == BMP_RECEIVING) {
        bmp_feed_raw(data, len);
        if (bmp_is_complete()) {
            Serial.printf("[BMP] Complete: %u bytes\n", s_bmp_raw_pos);
            bmp_decode_and_render();
            radio_ui_screenshot_done(true);
            s_bmp_state       = BMP_IDLE;
            s_bmp_raw_pos     = 0;
            s_hex_carry_valid = false;
            s_restart_stream  = true;
        }
        return;
    }

    // Normal CSV mode — accumulate into line buffer
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];

        if (c == '\n' || c == '\r') {
            if (s_line_pos > 0) {
                s_line_buf[s_line_pos] = '\0';
                s_line_pos = 0;

                RadioState tmp = {};
                if (radio_parse_csv(s_line_buf, tmp)) {
                    xSemaphoreTake(g_radio_mutex, portMAX_DELAY);
                    g_radio = tmp;
                    g_history.rssi[g_history.head] = tmp.rssi;
                    g_history.snr[g_history.head]  = tmp.snr;
                    g_history.head = (g_history.head + 1) % RSSI_HIST_LEN;
                    if (g_history.count < RSSI_HIST_LEN) g_history.count++;
                    xSemaphoreGive(g_radio_mutex);
                }
                // Non-CSV lines are silently ignored in normal mode
            }
        } else {
            if (s_line_pos < LINE_BUF_SIZE - 1) {
                s_line_buf[s_line_pos++] = c;
            } else {
                s_line_pos = 0;
            }
        }
    }
}

// Called from btn_cmd_cb when 'C' is pressed — arms the BMP receiver
// before sending the command so we don't miss the first bytes
void radio_screenshot_request() {
    if (s_bmp_state == BMP_RECEIVING) return;  // already in progress

    // Allocate raw buffer in PSRAM once
    if (!s_bmp_raw) {
        s_bmp_raw = (uint8_t *)heap_caps_malloc(BMP_MAX_BYTES,
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_bmp_raw) {
            Serial.println("[BMP] PSRAM alloc failed");
            return;
        }
    }

    s_bmp_state       = BMP_RECEIVING;
    s_bmp_raw_pos     = 0;
    s_hex_carry_valid = false;
    t_bmp_start       = millis();
    radio_ui_screenshot_progress(0);   // show overlay immediately
    Serial.println("[BMP] Armed — sending C");

    uint8_t cmd = 'C';
    usb_cdc_write(&cmd, 1);
}

// =============================================================================
//  SETUP
// =============================================================================

SET_LOOP_TASK_STACK_SIZE(16 * 1024);

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n[ATS Radio] Booting...");

    // Suppress noisy I2C log spam
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    // M5Unified — MUST be first (Tab5 DSI power sequencing)
    auto cfg = M5.config();
    cfg.internal_imu = false;
    cfg.internal_mic = false;
    cfg.internal_spk = false;
    M5.begin(cfg);
    M5.Display.setRotation(3);

    // Create mutex before USB task starts
    g_radio_mutex = xSemaphoreCreateMutex();

    // LVGL
    lvgl_init();

    // Build radio UI screen
    radio_ui_init();
    Serial.println("[ATS Radio] UI ready");

    // USB host — starts background task on core 0
    usb_host_cdc_init(on_device_connected, on_device_disconnected, on_data_received);
    Serial.println("[ATS Radio] USB host started — plug in ATS Mini");
}

// =============================================================================
//  LOOP (core 1 — LVGL)
// =============================================================================

static unsigned long t_lvgl_tick = 0;
static unsigned long t_ui_update = 0;

void loop() {
    // LVGL tick — must call every 5ms
    unsigned long now = millis();
    if (now - t_lvgl_tick >= 5) {
        lv_tick_inc(5);
        t_lvgl_tick = now;
    }

    // LVGL handler
    lv_timer_handler();

    // UI update from latest radio state every 250ms
    if (now - t_ui_update >= 250) {
        t_ui_update = now;

        if (xSemaphoreTake(g_radio_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            bool conn  = g_radio_connected;
            bool valid = g_radio.valid;
            RadioState   snap_r = g_radio;
            RadioHistory snap_h = g_history;
            xSemaphoreGive(g_radio_mutex);

            if (conn && valid) {
                radio_ui_update(snap_r, snap_h);
            }
        }
    }

    // BMP receive timeout — recover if transfer stalls (should complete in ~25s)
    if (s_bmp_state == BMP_RECEIVING && (millis() - t_bmp_start) > 35000) {
        Serial.printf("[BMP] Timeout — got %u bytes, attempting decode\n", s_bmp_raw_pos);
        if (s_bmp_raw_pos > 54) bmp_decode_and_render();
        s_bmp_state       = BMP_IDLE;
        s_bmp_raw_pos     = 0;
        s_hex_carry_valid = false;
        s_restart_stream  = true;
        radio_ui_screenshot_done(false);
    }

    // Update progress bar while BMP is downloading
    if (s_bmp_state == BMP_RECEIVING && s_bmp_raw_pos >= 6) {
        uint32_t file_size = s_bmp_raw[2] | ((uint32_t)s_bmp_raw[3] << 8) |
                             ((uint32_t)s_bmp_raw[4] << 16) | ((uint32_t)s_bmp_raw[5] << 24);
        if (file_size > 0) {
            int pct = (int)((s_bmp_raw_pos * 100UL) / file_size);
            radio_ui_screenshot_progress(LV_CLAMP(0, pct, 99));  // 99 max — 100 set on done
        }
    }

    // Restart CSV stream after screenshot if flagged by USB task
    if (s_restart_stream) {
        s_restart_stream = false;
        delay(300);  // safe here — on core 1, USB task runs on core 0
        uint8_t t = 't';
        usb_cdc_write(&t, 1);
        Serial.println("[BMP] CSV stream restarted");
    }

    delay(5);
}
