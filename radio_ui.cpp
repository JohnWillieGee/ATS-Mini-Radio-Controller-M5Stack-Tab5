// =============================================================================
// radio_ui.cpp
// LVGL Radio Screen — ATS Mini Controller
// Layout: left panel (live data + controls) | right panel (screen mirror)
// 1280 x 720, dark theme
// =============================================================================
#include "radio_ui.h"
#include "usb_host_cdc.h"
#include <Arduino.h>

// ── Fonts (Montserrat sizes compiled in lv_conf.h) ───────────────────────────
LV_FONT_DECLARE(lv_font_montserrat_14)
LV_FONT_DECLARE(lv_font_montserrat_16)
LV_FONT_DECLARE(lv_font_montserrat_20)
LV_FONT_DECLARE(lv_font_montserrat_28)
LV_FONT_DECLARE(lv_font_montserrat_40)
LV_FONT_DECLARE(lv_font_montserrat_48)

// ── Colour palette ────────────────────────────────────────────────────────────
#define COL_BG         lv_color_hex(0x181818)   // screen background
#define COL_CARD       lv_color_hex(0x242424)   // card surface
#define COL_CARD2      lv_color_hex(0x1e1e1e)   // inner card / metric bg
#define COL_BORDER     lv_color_hex(0x383838)
#define COL_TEXT       lv_color_hex(0xE0E0E0)
#define COL_MUTED      lv_color_hex(0x888888)
#define COL_DIM        lv_color_hex(0x555555)
#define COL_TEAL       lv_color_hex(0x1D9E75)   // RSSI bar / good signal
#define COL_BLUE       lv_color_hex(0x378ADD)   // SNR bar
#define COL_AMBER      lv_color_hex(0xEF9F27)   // mode / vol bar
#define COL_RED        lv_color_hex(0xE24B4A)   // weak signal
#define COL_WHITE      lv_color_hex(0xFFFFFF)
#define COL_BTN_ACCENT lv_color_hex(0x1D9E75)   // click/encoder btn

// ── Screen dimensions ────────────────────────────────────────────────────────
#define SCR_W  1280
#define SCR_H  720

#define LEFT_W  820   // left panel width (px)
#define RIGHT_W 440   // right panel width (right edge to right margin)
#define PAD      10   // outer padding

// ── LVGL objects ─────────────────────────────────────────────────────────────
static lv_obj_t *s_scr          = nullptr;

// Left panel widgets
static lv_obj_t *s_lbl_freq     = nullptr;
static lv_obj_t *s_lbl_band     = nullptr;
static lv_obj_t *s_lbl_mode     = nullptr;
static lv_obj_t *s_lbl_fw       = nullptr;

static lv_obj_t *s_bar_rssi     = nullptr;
static lv_obj_t *s_lbl_rssi     = nullptr;
static lv_obj_t *s_bar_snr      = nullptr;
static lv_obj_t *s_lbl_snr      = nullptr;

static lv_obj_t *s_lbl_vol      = nullptr;
static lv_obj_t *s_bar_vol      = nullptr;
static lv_obj_t *s_lbl_batt     = nullptr;
static lv_obj_t *s_lbl_step     = nullptr;
static lv_obj_t *s_lbl_bw       = nullptr;
static lv_obj_t *s_lbl_agc      = nullptr;

// Status / connection banner
static lv_obj_t *s_lbl_status   = nullptr;

// Right panel — screen mirror using lv_img (avoids canvas tiling bug)
static lv_obj_t    *s_mirror_img  = nullptr;
static uint16_t    *s_canvas_buf  = nullptr;  // PSRAM, 320*170 RGB565
static lv_img_dsc_t s_mirror_dsc  = {};       // image descriptor

// Sparkline canvas objects
static lv_obj_t *s_spark_rssi   = nullptr;
static lv_obj_t *s_spark_snr    = nullptr;
// Sparkline canvas pixel buffers — allocated in PSRAM at init time
static lv_color_t *s_spark_rssi_buf = nullptr;
static lv_color_t *s_spark_snr_buf  = nullptr;

// Screenshot progress overlay (shown over mirror area during BMP download)
static lv_obj_t *s_progress_card  = nullptr;
static lv_obj_t *s_progress_bar   = nullptr;
static lv_obj_t *s_progress_label = nullptr;

// ── Helper: make a dark card ─────────────────────────────────────────────────
static lv_obj_t* make_card(lv_obj_t *parent, int x, int y, int w, int h,
                             lv_color_t bg = COL_CARD) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

// ── Helper: label with font + colour ─────────────────────────────────────────
static lv_obj_t* make_label(lv_obj_t *parent, const char *txt,
                              int x, int y,
                              const lv_font_t *font = &lv_font_montserrat_14,
                              lv_color_t col = COL_TEXT) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, col, LV_PART_MAIN);
    lv_obj_set_pos(lbl, x, y);
    return lbl;
}

// ── Helper: slim horizontal bar ──────────────────────────────────────────────
static lv_obj_t* make_bar(lv_obj_t *parent, int x, int y, int w, int h,
                            lv_color_t ind_col) {
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, ind_col, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 127);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    return bar;
}

// Forward declaration — defined in ATS_Radio_Screen.ino
extern void radio_screenshot_request();

static void sync_btn_cb(lv_event_t *e) {
    radio_screenshot_request();
}

// ── Control button callback ───────────────────────────────────────────────────
static void btn_cmd_cb(lv_event_t *e) {
    char cmd = (char)(intptr_t)lv_event_get_user_data(e);
    uint8_t b = (uint8_t)cmd;
    usb_cdc_write(&b, 1);
}

// ── Helper: control button ───────────────────────────────────────────────────
static void make_ctrl_btn(lv_obj_t *parent, int x, int y, int w, int h,
                           const char *sym, const char *label_txt,
                           char cmd, bool accent = false) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1e1e1e), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn,
        accent ? COL_BTN_ACCENT : COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);

    // Symbol line
    lv_obj_t *sym_lbl = lv_label_create(btn);
    lv_label_set_text(sym_lbl, sym);
    lv_obj_set_style_text_font(sym_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(sym_lbl,
        accent ? COL_BTN_ACCENT : COL_TEXT, LV_PART_MAIN);
    lv_obj_align(sym_lbl, LV_ALIGN_TOP_MID, 0, 6);

    // Caption line
    lv_obj_t *cap_lbl = lv_label_create(btn);
    lv_label_set_text(cap_lbl, label_txt);
    lv_obj_set_style_text_font(cap_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(cap_lbl, COL_MUTED, LV_PART_MAIN);
    lv_obj_align(cap_lbl, LV_ALIGN_BOTTOM_MID, 0, -5);

    lv_obj_add_event_cb(btn, btn_cmd_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)cmd);
}

// ── Draw sparkline into canvas ────────────────────────────────────────────────
static void draw_sparkline(lv_obj_t *canvas, const int *data, int count,
                            int max_val, lv_color_t col) {
    if (!canvas || count < 2) return;
    int cw = lv_obj_get_width(canvas);
    int ch = lv_obj_get_height(canvas);

    lv_canvas_fill_bg(canvas, COL_CARD2, LV_OPA_COVER);

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = col;
    dsc.width = 2;
    dsc.opa   = LV_OPA_COVER;

    int n = LV_MIN(count, RSSI_HIST_LEN);

    // lv_canvas_draw_line(canvas, points[], point_cnt, dsc)
    // Draw each segment as a 2-point array
    for (int i = 0; i < n - 1; i++) {
        int x1 = (i       * (cw - 4)) / (n - 1) + 2;
        int x2 = ((i + 1) * (cw - 4)) / (n - 1) + 2;
        int v1 = LV_CLAMP(0, data[i],     max_val);
        int v2 = LV_CLAMP(0, data[i + 1], max_val);
        int y1 = ch - 2 - (v1 * (ch - 4)) / max_val;
        int y2 = ch - 2 - (v2 * (ch - 4)) / max_val;
        lv_point_t seg[2] = {
            {(lv_coord_t)x1, (lv_coord_t)y1},
            {(lv_coord_t)x2, (lv_coord_t)y2}
        };
        lv_canvas_draw_line(canvas, seg, 2, &dsc);
    }
}

// ── radio_ui_init ─────────────────────────────────────────────────────────────
void radio_ui_init() {
    // Allocate canvas pixel buffer in PSRAM — native ATS Mini 320×170 RGB565
    s_canvas_buf = (uint16_t *)heap_caps_malloc(
        320 * 170 * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_canvas_buf) {
        Serial.println("[RadioUI] WARNING: canvas PSRAM alloc failed");
    } else {
        memset(s_canvas_buf, 0, 320 * 170 * sizeof(uint16_t));
    }

    // Create screen
    s_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_scr, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, LV_PART_MAIN);

    // ── LEFT PANEL ────────────────────────────────────────────────────────────
    lv_obj_t *left = make_card(s_scr, PAD, PAD, LEFT_W - PAD, SCR_H - PAD * 2);

    // --- Frequency hero ---
    s_lbl_freq = make_label(left, "---.- MHz", 0, 14,
                             &lv_font_montserrat_48, COL_WHITE);
    lv_obj_set_width(s_lbl_freq, LEFT_W - PAD * 2);
    lv_obj_set_style_text_align(s_lbl_freq, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_pos(s_lbl_freq, 0, 14);

    // Band · Mode · FW row
    s_lbl_band = make_label(left, "---", 0, 80, &lv_font_montserrat_20, COL_BLUE);
    lv_obj_set_style_text_align(s_lbl_band, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(s_lbl_band, (LEFT_W - PAD * 2) / 3);
    lv_obj_set_pos(s_lbl_band, 0, 80);

    s_lbl_mode = make_label(left, "---", 0, 80, &lv_font_montserrat_20, COL_AMBER);
    lv_obj_set_style_text_align(s_lbl_mode, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(s_lbl_mode, (LEFT_W - PAD * 2) / 3);
    lv_obj_set_pos(s_lbl_mode, (LEFT_W - PAD * 2) / 3, 80);

    s_lbl_fw = make_label(left, "FW ---", 0, 80, &lv_font_montserrat_16, COL_DIM);
    lv_obj_set_style_text_align(s_lbl_fw, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(s_lbl_fw, (LEFT_W - PAD * 2) / 3);
    lv_obj_set_pos(s_lbl_fw, (LEFT_W - PAD * 2) * 2 / 3, 83);

    // Divider
    lv_obj_t *div1 = lv_obj_create(left);
    lv_obj_set_size(div1, LEFT_W - PAD * 4, 1);
    lv_obj_set_pos(div1, PAD, 112);
    lv_obj_set_style_bg_color(div1, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(div1, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(div1, 0, LV_PART_MAIN);

    // --- RSSI row ---
    int bar_y = 130;
    int bar_w = LEFT_W - PAD * 4 - 130;  // leave room for label
    make_label(left, "RSSI", PAD, bar_y + 2, &lv_font_montserrat_14, COL_DIM);
    s_lbl_rssi = make_label(left, "-- dBuV", PAD + 56, bar_y,
                             &lv_font_montserrat_20, COL_TEAL);
    s_bar_rssi = make_bar(left, PAD + 170, bar_y + 8, bar_w, 10, COL_TEAL);

    // --- SNR row ---
    bar_y = 168;
    make_label(left, "SNR", PAD, bar_y + 2, &lv_font_montserrat_14, COL_DIM);
    s_lbl_snr  = make_label(left, "-- dB", PAD + 56, bar_y,
                             &lv_font_montserrat_20, COL_BLUE);
    s_bar_snr  = make_bar(left, PAD + 170, bar_y + 8, bar_w, 10, COL_BLUE);

    // --- Info row: Vol / Step / BW / AGC / Batt ---
    int info_y = 210;
    int info_w = (LEFT_W - PAD * 4 - 40) / 5;

    // Plain helper — no lambda (lambda captures crash on ESP32-P4 toolchain)
    struct InfoHelper {
        static lv_obj_t* make(lv_obj_t *parent, int idx,
                               int base_x, int iy, int iw,
                               const char *lbl_txt) {
            int ix = base_x + idx * (iw + 8);
            lv_obj_t *ic = lv_obj_create(parent);
            lv_obj_set_pos(ic, ix, iy);
            lv_obj_set_size(ic, iw, 56);
            lv_obj_set_style_bg_color(ic, COL_CARD2, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_color(ic, COL_BORDER, LV_PART_MAIN);
            lv_obj_set_style_border_width(ic, 1, LV_PART_MAIN);
            lv_obj_set_style_radius(ic, 6, LV_PART_MAIN);
            lv_obj_set_style_pad_all(ic, 0, LV_PART_MAIN);
            lv_obj_clear_flag(ic, LV_OBJ_FLAG_SCROLLABLE);
            // Caption
            lv_obj_t *cap = lv_label_create(ic);
            lv_label_set_text(cap, lbl_txt);
            lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, LV_PART_MAIN);
            lv_obj_set_style_text_color(cap, COL_DIM, LV_PART_MAIN);
            lv_obj_set_pos(cap, 8, 6);
            // Value label
            lv_obj_t *val = lv_label_create(ic);
            lv_label_set_text(val, "-");
            lv_obj_set_style_text_font(val, &lv_font_montserrat_16, LV_PART_MAIN);
            lv_obj_set_style_text_color(val, COL_TEXT, LV_PART_MAIN);
            lv_obj_set_pos(val, 8, 28);
            return val;
        }
    };

    s_lbl_vol  = InfoHelper::make(left, 0, PAD, info_y, info_w, "Vol");
    s_lbl_step = InfoHelper::make(left, 1, PAD, info_y, info_w, "Step");
    s_lbl_bw   = InfoHelper::make(left, 2, PAD, info_y, info_w, "BW");
    s_lbl_agc  = InfoHelper::make(left, 3, PAD, info_y, info_w, "AGC");
    s_lbl_batt = InfoHelper::make(left, 4, PAD, info_y, info_w, "Batt");

    // Volume bar (thin, below info row)
    int vbar_y = 278;
    make_label(left, "Volume", PAD, vbar_y + 2, &lv_font_montserrat_14, COL_DIM);
    s_bar_vol = lv_bar_create(left);
    lv_obj_set_pos(s_bar_vol, PAD + 80, vbar_y + 6);
    lv_obj_set_size(s_bar_vol, LEFT_W - PAD * 4 - 80, 8);
    lv_obj_set_style_bg_color(s_bar_vol, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar_vol, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_vol, COL_AMBER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bar_vol, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar_vol, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar_vol, 4, LV_PART_INDICATOR);
    lv_bar_set_range(s_bar_vol, 0, 63);
    lv_bar_set_value(s_bar_vol, 0, LV_ANIM_OFF);

    // Divider 2
    lv_obj_t *div2 = lv_obj_create(left);
    lv_obj_set_size(div2, LEFT_W - PAD * 4, 1);
    lv_obj_set_pos(div2, PAD, 300);
    lv_obj_set_style_bg_color(div2, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(div2, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(div2, 0, LV_PART_MAIN);

    // --- Control buttons (2 rows × 5) ---
    make_label(left, "Controls", PAD, 314,
               &lv_font_montserrat_14, COL_DIM);

    int btn_y0  = 338;
    int btn_w   = (LEFT_W - PAD * 4 - 32) / 5;
    int btn_h   = 82;
    int btn_gap = 8;

    struct BtnDef { const char *sym; const char *cap; char cmd; bool accent; };
    BtnDef row1[5] = {
        { "<<", "Band-",  'b', false },
        { "v",  "Vol-",   'v', false },
        { "O",  "Click",  'e', true  },
        { "^",  "Vol+",   'V', false },
        { ">>", "Band+",  'B', false },
    };
    BtnDef row2[5] = {
        { "<",  "Tune-",  'r', false },
        { "M",  "Mode",   'M', false },
        { "Z",  "Sleep",  'O', false },
        { "W",  "BW",     'W', false },
        { ">",  "Tune+",  'R', false },
    };
    for (int i = 0; i < 5; i++) {
        make_ctrl_btn(left,
            PAD + i * (btn_w + btn_gap), btn_y0,
            btn_w, btn_h,
            row1[i].sym, row1[i].cap, row1[i].cmd, row1[i].accent);
        make_ctrl_btn(left,
            PAD + i * (btn_w + btn_gap), btn_y0 + btn_h + btn_gap,
            btn_w, btn_h,
            row2[i].sym, row2[i].cap, row2[i].cmd, row2[i].accent);
    }

    // Status banner (bottom of left panel)
    s_lbl_status = make_label(left, "Waiting for ATS Mini...",
                               PAD, SCR_H - PAD * 2 - 36,
                               &lv_font_montserrat_16, COL_MUTED);

    // ── RIGHT PANEL ───────────────────────────────────────────────────────────
    int rp_x = LEFT_W + PAD;
    int rp_w = SCR_W - LEFT_W - PAD * 3;

    lv_obj_t *right = make_card(s_scr, rp_x, PAD, rp_w, SCR_H - PAD * 2);

    // Screen mirror label
    make_label(right, "ATS Screen Mirror", PAD, PAD,
               &lv_font_montserrat_14, COL_DIM);

    // Mirror canvas — native ATS Mini resolution 320×170 (16bpp RGB565)
    // Displayed at full panel width with correct aspect ratio
    #define ATS_BMP_W  320
    #define ATS_BMP_H  170
    int canvas_w = rp_w - PAD * 2;
    int canvas_h = (canvas_w * ATS_BMP_H) / ATS_BMP_W;  // maintain 320:170 ratio

    if (s_canvas_buf) {
        // Build LVGL image descriptor pointing to PSRAM buffer
        s_mirror_dsc.header.cf          = LV_IMG_CF_TRUE_COLOR;
        s_mirror_dsc.header.always_zero = 0;
        s_mirror_dsc.header.reserved    = 0;
        s_mirror_dsc.header.w           = 320;
        s_mirror_dsc.header.h           = 170;
        s_mirror_dsc.data_size          = 320 * 170 * sizeof(uint16_t);
        s_mirror_dsc.data               = (const uint8_t *)s_canvas_buf;

        // Fill placeholder with dark grey
        memset(s_canvas_buf, 0x18, 320 * 170 * sizeof(uint16_t));

        s_mirror_img = lv_img_create(right);
        lv_img_set_src(s_mirror_img, &s_mirror_dsc);

        // Fit full 320px width into panel, maintaining aspect ratio
        int avail_w = rp_w - PAD * 2;
        uint16_t zoom = (uint16_t)(((uint32_t)avail_w * 256) / 320);
        lv_img_set_zoom(s_mirror_img, zoom);
        lv_img_set_antialias(s_mirror_img, false);
        // Pivot at image centre (LVGL default) — position accounts for zoom offset
        // Scaled width = avail_w, scaled height = canvas_h
        // Centre of scaled image: avail_w/2 from panel left + PAD
        lv_img_set_pivot(s_mirror_img, 160, 85);  // centre of 320x170 source image
        lv_obj_set_pos(s_mirror_img, PAD + avail_w / 2 - 160, 32);

        // Clip overflow so zoom doesn't bleed outside panel
        lv_obj_clear_flag(right, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    } else {
        lv_obj_t *ph = make_card(right, PAD, 32, canvas_w, canvas_h,
                                  lv_color_hex(0x111111));
        make_label(ph, "No PSRAM for canvas", PAD, canvas_h / 2 - 10,
                   &lv_font_montserrat_14, COL_DIM);
    }

    // Sync button (below canvas)
    int sync_y = 32 + canvas_h + 12;
    lv_obj_t *sync_btn = lv_btn_create(right);
    lv_obj_set_pos(sync_btn, PAD, sync_y);
    lv_obj_set_size(sync_btn, rp_w - PAD * 2, 44);
    lv_obj_set_style_bg_color(sync_btn, lv_color_hex(0x1e1e1e), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sync_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(sync_btn, COL_BLUE, LV_PART_MAIN);
    lv_obj_set_style_border_width(sync_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(sync_btn, 6, LV_PART_MAIN);
    lv_obj_t *sync_lbl = lv_label_create(sync_btn);
    lv_label_set_text(sync_lbl, "Sync Screen");
    lv_obj_set_style_text_font(sync_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(sync_lbl, COL_BLUE, LV_PART_MAIN);
    lv_obj_align(sync_lbl, LV_ALIGN_CENTER, 0, 0);
    // Command 'C' handled via radio_screenshot_request() to arm receiver first
    lv_obj_add_event_cb(sync_btn, sync_btn_cb, LV_EVENT_CLICKED, nullptr);

    // ── Screenshot progress overlay ───────────────────────────────────────────
    // Sits over the mirror image area, hidden by default, shown during BMP download
    s_progress_card = lv_obj_create(right);
    lv_obj_set_pos(s_progress_card, PAD, 32);
    lv_obj_set_size(s_progress_card, rp_w - PAD * 2, canvas_h);
    lv_obj_set_style_bg_color(s_progress_card, lv_color_hex(0x111111), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_progress_card, 220, LV_PART_MAIN);  // semi-transparent
    lv_obj_set_style_border_width(s_progress_card, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_progress_card, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_progress_card, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_progress_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_progress_card, LV_OBJ_FLAG_HIDDEN);  // hidden until download starts

    // "Syncing..." label
    s_progress_label = lv_label_create(s_progress_card);
    lv_label_set_text(s_progress_label, "Syncing screen...");
    lv_obj_set_style_text_font(s_progress_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_progress_label, COL_BLUE, LV_PART_MAIN);
    lv_obj_align(s_progress_label, LV_ALIGN_CENTER, 0, -20);

    // Progress bar
    int pb_w = rp_w - PAD * 6;
    s_progress_bar = lv_bar_create(s_progress_card);
    lv_obj_set_size(s_progress_bar, pb_w, 14);
    lv_obj_align(s_progress_bar, LV_ALIGN_CENTER, 0, 14);
    lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_progress_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_progress_bar, COL_BLUE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_progress_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_progress_bar, 7, LV_PART_MAIN);
    lv_obj_set_style_radius(s_progress_bar, 7, LV_PART_INDICATOR);
    lv_bar_set_range(s_progress_bar, 0, 100);
    lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);

    // Sparklines (RSSI + SNR history) below sync button
    int spark_y = sync_y + 44 + 12;
    int spark_h = 36;
    int spark_w = rp_w - PAD * 2;

    // Allocate sparkline buffers in PSRAM
    size_t spark_bytes = (size_t)spark_w * spark_h * sizeof(lv_color_t);
    s_spark_rssi_buf = (lv_color_t *)heap_caps_malloc(spark_bytes,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_spark_snr_buf  = (lv_color_t *)heap_caps_malloc(spark_bytes,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    make_label(right, "RSSI history", PAD, spark_y,
               &lv_font_montserrat_14, COL_DIM);
    s_spark_rssi = lv_canvas_create(right);
    if (s_spark_rssi_buf) {
        lv_canvas_set_buffer(s_spark_rssi, s_spark_rssi_buf, spark_w, spark_h,
                             LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_pos(s_spark_rssi, PAD, spark_y + 18);
        lv_canvas_fill_bg(s_spark_rssi, COL_CARD2, LV_OPA_COVER);
    }

    make_label(right, "SNR history", PAD, spark_y + 18 + spark_h + 8,
               &lv_font_montserrat_14, COL_DIM);
    s_spark_snr = lv_canvas_create(right);
    if (s_spark_snr_buf) {
        lv_canvas_set_buffer(s_spark_snr, s_spark_snr_buf, spark_w, spark_h,
                             LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_pos(s_spark_snr, PAD, spark_y + 18 + spark_h + 26);
        lv_canvas_fill_bg(s_spark_snr, COL_CARD2, LV_OPA_COVER);
    }

    // Load the screen
    lv_scr_load(s_scr);
}

// ── radio_ui_update ───────────────────────────────────────────────────────────
void radio_ui_update(const RadioState &r, const RadioHistory &h) {
    if (!s_scr) return;

    // Frequency
    char freq_buf[32];
    radio_freq_string(r, freq_buf, sizeof(freq_buf));
    lv_label_set_text(s_lbl_freq, freq_buf);

    // Band / Mode / FW
    lv_label_set_text(s_lbl_band, r.bandName);
    lv_label_set_text(s_lbl_mode, r.modeName);
    char fw_buf[16];
    snprintf(fw_buf, sizeof(fw_buf), "FW %d", r.appVersion);
    lv_label_set_text(s_lbl_fw, fw_buf);

    // RSSI — colour code: green >= 40, amber 20-39, red < 20
    char rssi_buf[16];
    snprintf(rssi_buf, sizeof(rssi_buf), "%d dBuV", r.rssi);
    lv_label_set_text(s_lbl_rssi, rssi_buf);
    lv_color_t rssi_col = (r.rssi >= 40) ? COL_TEAL :
                          (r.rssi >= 20) ? COL_AMBER : COL_RED;
    lv_obj_set_style_text_color(s_lbl_rssi, rssi_col, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_rssi, rssi_col, LV_PART_INDICATOR);
    lv_bar_set_value(s_bar_rssi, r.rssi, LV_ANIM_ON);

    // SNR
    char snr_buf[16];
    snprintf(snr_buf, sizeof(snr_buf), "%d dB", r.snr);
    lv_label_set_text(s_lbl_snr, snr_buf);
    lv_bar_set_value(s_bar_snr, r.snr, LV_ANIM_ON);

    // Vol
    char vol_buf[16];
    snprintf(vol_buf, sizeof(vol_buf), "%d / 63", r.volume);
    lv_label_set_text(s_lbl_vol, vol_buf);
    lv_bar_set_value(s_bar_vol, r.volume, LV_ANIM_OFF);

    // Step (approximate from idx — ATS Mini uses fixed steps per mode)
    // For display we just show the raw index; could be decoded fully later
    char step_buf[12];
    snprintf(step_buf, sizeof(step_buf), "idx %d", r.stepIdx);
    lv_label_set_text(s_lbl_step, step_buf);

    // BW
    char bw_buf[12];
    snprintf(bw_buf, sizeof(bw_buf), "idx %d", r.bandwidthIdx);
    lv_label_set_text(s_lbl_bw, bw_buf);

    // AGC
    char agc_buf[12];
    snprintf(agc_buf, sizeof(agc_buf), "%d", r.agcIdx);
    lv_label_set_text(s_lbl_agc, agc_buf);

    // Battery — show voltage if > 0.5V (i.e. battery fitted)
    char batt_buf[12];
    if (r.batteryVoltage > 0.5f) {
        snprintf(batt_buf, sizeof(batt_buf), "%.1fV", r.batteryVoltage);
    } else {
        snprintf(batt_buf, sizeof(batt_buf), "USB");
    }
    lv_label_set_text(s_lbl_batt, batt_buf);

    // Status
    char seq_buf[32];
    snprintf(seq_buf, sizeof(seq_buf), "Connected  seq:%d", r.seqNum);
    lv_label_set_text(s_lbl_status, seq_buf);
    lv_obj_set_style_text_color(s_lbl_status, COL_TEAL, LV_PART_MAIN);

    // Sparklines
    if (h.count >= 2) {
        draw_sparkline(s_spark_rssi, h.rssi, h.count, 127, COL_TEAL);
        draw_sparkline(s_spark_snr,  h.snr,  h.count, 127, COL_BLUE);
    }
}

// ── radio_ui_set_connected ────────────────────────────────────────────────────
void radio_ui_set_connected(bool connected) {
    if (!s_lbl_status) return;
    if (connected) {
        lv_label_set_text(s_lbl_status, "ATS Mini connected");
        lv_obj_set_style_text_color(s_lbl_status, COL_TEAL, LV_PART_MAIN);
    } else {
        lv_label_set_text(s_lbl_status, "Waiting for ATS Mini...");
        lv_obj_set_style_text_color(s_lbl_status, COL_MUTED, LV_PART_MAIN);
        // Clear frequency display
        if (s_lbl_freq) lv_label_set_text(s_lbl_freq, "---.- MHz");
        if (s_lbl_band) lv_label_set_text(s_lbl_band, "---");
        if (s_lbl_mode) lv_label_set_text(s_lbl_mode, "---");
    }
}

// ── radio_ui_set_screenshot ───────────────────────────────────────────────────
void radio_ui_set_screenshot(const uint16_t *pixel_buf) {
    if (!s_mirror_img || !s_canvas_buf) return;
    memcpy(s_canvas_buf, pixel_buf, 320 * 170 * sizeof(uint16_t));
    // Re-set source to force LVGL to redraw with updated buffer contents
    lv_img_set_src(s_mirror_img, &s_mirror_dsc);
    lv_obj_invalidate(s_mirror_img);
}

// ── radio_ui_screenshot_progress ─────────────────────────────────────────────
void radio_ui_screenshot_progress(int pct) {
    if (!s_progress_card || !s_progress_bar || !s_progress_label) return;

    // Show overlay on first call
    if (lv_obj_has_flag(s_progress_card, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(s_progress_card, LV_OBJ_FLAG_HIDDEN);
    }

    lv_bar_set_value(s_progress_bar, LV_CLAMP(0, pct, 100), LV_ANIM_OFF);

    char buf[32];
    snprintf(buf, sizeof(buf), "Syncing screen... %d%%", pct);
    lv_label_set_text(s_progress_label, buf);
}

// ── radio_ui_screenshot_done ──────────────────────────────────────────────────
void radio_ui_screenshot_done(bool success) {
    if (!s_progress_card) return;
    if (success) {
        // Brief "Done" flash before hiding
        if (s_progress_label)
            lv_label_set_text(s_progress_label, "Sync complete");
        if (s_progress_bar)
            lv_bar_set_value(s_progress_bar, 100, LV_ANIM_OFF);
    } else {
        if (s_progress_label)
            lv_label_set_text(s_progress_label, "Sync failed");
    }
    // Hide after a short moment — LVGL timer
    lv_obj_add_flag(s_progress_card, LV_OBJ_FLAG_HIDDEN);
}
