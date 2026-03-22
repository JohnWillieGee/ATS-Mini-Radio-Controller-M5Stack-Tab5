// =============================================================================
// radio_state.h
// Shared state struct for ATS Mini V4 SDR — populated by USB RX task,
// consumed by LVGL UI task.
// All writes from usb_host_task (core 0), all reads from LVGL loop (core 1).
// Access guarded by g_radio_mutex.
// =============================================================================
#pragma once
#include <Arduino.h>
#include <freertos/semphr.h>

// ── Parsed state from ATS Mini CSV stream ────────────────────────────────────
struct RadioState {
    int     appVersion;         // firmware (e.g. 233 = v2.33)
    long    rawFrequency;       // raw units (FM=10kHz, AM/SSB=1kHz)
    int     bfo;                // SSB BFO offset Hz
    int     bandCal;            // SSB calibration offset
    char    bandName[16];       // e.g. "VHF", "40M", "MW1"
    char    modeName[8];        // "FM" / "AM" / "LSB" / "USB"
    int     stepIdx;
    int     bandwidthIdx;
    int     agcIdx;
    int     volume;             // 0–63
    int     rssi;               // 0–127 dBuV
    int     snr;                // 0–127 dB
    int     tuningCap;          // 0–6143
    float   batteryVoltage;     // derived: raw × 1.702 / 1000
    int     seqNum;             // 0–255

    // Derived
    long    displayFreqHz;      // ready-to-display frequency in Hz
    bool    valid;              // true once first complete frame received
};

// ── RSSI history ring buffer (last 60 samples) ───────────────────────────────
#define RSSI_HIST_LEN 60
#define SNR_HIST_LEN  60

struct RadioHistory {
    int  rssi[RSSI_HIST_LEN];
    int  snr[SNR_HIST_LEN];
    int  head;       // next write index
    int  count;      // how many valid samples
};

// ── Globals (defined in ATS_Radio_Screen.ino) ────────────────────────────────
extern RadioState   g_radio;
extern RadioHistory g_history;
extern SemaphoreHandle_t g_radio_mutex;
extern volatile bool g_radio_connected;

// ── CSV parser ───────────────────────────────────────────────────────────────
bool radio_parse_csv(const char *line, RadioState &r);

// ── Frequency display helper ─────────────────────────────────────────────────
// Returns display string like "105.70 MHz" or "7.100 MHz"
void radio_freq_string(const RadioState &r, char *buf, size_t buflen);
