# ATS Mini Radio Controller — M5Stack Tab5

A standalone Arduino sketch that turns the **M5Stack Tab5** (ESP32-P4) into a large secondary display and touch controller for the **ATS Mini V4 SDR receiver**.

![ATS Radio Screen running on Tab5]("docs/20260322_144339_1.jpg")

## Features

- **Live data display** — frequency, band, mode, RSSI, SNR, volume, battery, step, bandwidth, AGC
- **Touch controls** — 10 buttons for tuning, volume, band, mode, bandwidth, sleep and encoder click
- **RSSI colour coding** — green (strong), amber (marginal), red (weak)
- **Signal history** — rolling RSSI and SNR sparkline charts
- **Screen mirror** — on-demand capture and display of the ATS Mini's 320×170 screen via BMP screenshot
- **Download progress bar** — visual indicator during the ~20 second screenshot transfer
- **Auto stream recovery** — CSV data stream automatically restarts after screenshot completes

---

## Hardware Required

| Item | Notes |
|---|---|
| [M5Stack Tab5](https://m5stack.com/products/tab5) | ESP32-P4, 1280×720 touchscreen |
| [ATS Mini V4](https://github.com/esp32-si4732/ats-mini) | SI4732 SDR receiver |
| USB-A to USB-C OTG cable | Tab5 USB-A host port → ATS Mini USB-C |

The Tab5 USB-A port acts as a USB host. No additional hardware or wiring is required beyond the cable.

---

## Software Dependencies

### Arduino IDE
Version **2.x** recommended.

### Board Package
Install via Arduino IDE → **File → Preferences → Additional Board Manager URLs**:

```
https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/arduino/package_m5stack_index.json
```

Then: **Tools → Board → Board Manager** → search **M5Stack** → install.

Select board: **M5Stack → M5Tab5**

> ⚠️ Board package v3.2.x has a known WiFi SDIO crash bug. This sketch does not use WiFi so it is unaffected. If you later integrate WiFi features, consider downgrading to v2.1.4.

### Libraries
Install all via **Tools → Manage Libraries**:

| Library | Version | Notes |
|---|---|---|
| **lvgl** by LVGL | 8.4.x | Must be version 8.x — NOT version 9 |
| **M5Unified** by M5Stack | Latest | Required for Tab5 display/power init |
| **M5GFX** by M5Stack | Latest | Display driver, pulled in by M5Unified |

> ⚠️ After installing lvgl, you **must** copy `lv_conf.h` from this repository into the correct location (see below).

---

## Installation

### 1. Clone or download this repository

```bash
git clone https://github.com/YOUR_USERNAME/ATS_Radio_Screen.git
```

### 2. Configure lv_conf.h

LVGL requires a configuration file. Copy the provided `lv_conf.h` into the lvgl library folder:

**Windows:**
```
C:\Users\<YourName>\Documents\Arduino\libraries\lv_conf.h
```
*(Place it alongside the `lvgl` folder, not inside it)*

**macOS / Linux:**
```
~/Arduino/libraries/lv_conf.h
```

The provided `lv_conf.h` is pre-configured for the Tab5 with:
- `LV_COLOR_DEPTH 16` (RGB565)
- `LV_COLOR_16_SWAP 0`
- PSRAM-backed LVGL memory allocator
- Montserrat fonts enabled: 14, 16, 20, 28, 40, 48

### 3. Open the sketch

Open `ATS_Radio_Screen/ATS_Radio_Screen.ino` in Arduino IDE.

### 4. Board settings

In **Tools**, set the following:

| Setting | Value |
|---|---|
| Board | M5Tab5 |
| CPU Frequency | 360MHz |
| Flash Size | 16MB (128Mb) |
| PSRAM | Enabled |
| USB CDC On Boot | Enabled |
| USB Mode | Hardware CDC and JTAG |
| Upload Speed | 921600 |
| Partition Scheme | Default (2×6.5MB app, 3.6MB SPIFFS) |

### 5. Upload

Connect your Tab5 via USB-C. Select the correct COM port and click Upload.

---

## Usage

1. Flash the sketch to the Tab5
2. Connect the ATS Mini to the Tab5 USB-A port using a USB-A to USB-C OTG cable
3. Power on the ATS Mini
4. The Tab5 will automatically detect the ATS Mini, start the data stream, and display live radio data

### Touch Controls

| Button | Action |
|---|---|
| Band- / Band+ | Previous / next band |
| Vol- / Vol+ | Volume down / up |
| Tune- / Tune+ | Tune frequency down / up |
| Click | Encoder button click (open menu / select) |
| Mode | Next modulation mode (FM/AM/LSB/USB) |
| BW | Next bandwidth filter |
| Sleep | Toggle sleep mode |
| Sync Screen | Capture ATS Mini display (~20 seconds) |

### Screen Mirror

Tap **Sync Screen** to capture the ATS Mini's display. A progress bar shows download status. The captured image renders in the right panel. The data stream resumes automatically after capture.

---

## Project Structure

```
ATS_Radio_Screen/
├── ATS_Radio_Screen.ino   Main sketch — LVGL init, USB callbacks, BMP state machine
├── usb_host_cdc.h         USB host CDC-ACM driver header
├── usb_host_cdc.cpp       USB host CDC-ACM driver (ESP32-P4, usb/usb_host.h only)
├── radio_state.h          RadioState struct, history ring buffer
├── radio_state.cpp        CSV parser, frequency formatter
├── radio_ui.h             LVGL screen API
├── radio_ui.cpp           Full LVGL layout — panels, bars, buttons, mirror, progress
└── lv_conf.h              LVGL configuration for Tab5
```

---

## Technical Notes

### USB Host Driver
The driver uses only `usb/usb_host.h` from the ESP-IDF — **`cdc_acm_host.h` is intentionally not used** as it requires the ESP Component Registry and is not bundled with the M5Stack Arduino board package. The driver implements CDC-ACM bulk transfer handling directly.

The ATS Mini enumerates as:
- VID `0x303A` / PID `0x1001` (ESP32-S3 native USB CDC)
- Interface 1, class `0x0A` (CDC Data)
- Bulk IN `0x81`, Bulk OUT `0x01`

### BMP Screenshot Protocol
Sending `C` to the ATS Mini triggers a BMP dump of its 320×170 framebuffer encoded as packed lowercase hex (no spaces, no header, no end marker). The Tab5 decodes this inline as bytes arrive, using the BMP file size header to detect completion. Transfer time is approximately 20 seconds at 115200 baud.

### Dual-Core Architecture
- **Core 0** — USB host task: USB enumeration, bulk transfers, CSV parsing, BMP receive
- **Core 1** — Arduino `loop()`: LVGL tick, UI updates, touch handling

Shared state is protected by a FreeRTOS mutex. `delay()` is never called from USB callbacks to avoid deadlocking the USB task.

---

## ATS Mini Firmware Reference

Full serial protocol documentation:
[https://esp32-si4732.github.io/ats-mini/manual.html](https://esp32-si4732.github.io/ats-mini/manual.html)

This sketch is tested against ATS Mini firmware v2.33.

---

## Known Limitations

- **Step / Bandwidth** fields display as index numbers (`idx 0`, `idx 1` etc.) rather than decoded values (e.g. `100 kHz`, `Auto`). Decoding tables are available in the ATS Mini source.
- **No WiFi** — this sketch is standalone and does not connect to the internet.
- Screen mirror is on-demand only (tap Sync Screen) — not a live video feed.

---

## Roadmap

- [ ] Step and bandwidth index decoding to human-readable values
- [ ] Full-screen screenshot viewer (second page / swipe)
- [ ] Optional integration as a screen within the Tab5 Weather Dashboard

---

## License

MIT License. See `LICENSE` for details.

---

## Credits

- [ATS Mini](https://github.com/esp32-si4732/ats-mini) — open source SDR firmware by the ATS Mini Community
- [LVGL](https://lvgl.io) — Light and Versatile Graphics Library
- [M5Stack](https://m5stack.com) — Tab5 hardware and board support package
