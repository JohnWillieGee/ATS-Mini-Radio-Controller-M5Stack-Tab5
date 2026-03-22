// =============================================================================
// usb_host_cdc.h
// Thin wrapper around ESP-IDF USB host CDC-ACM driver for Arduino use
// Target: ESP32-P4 (Tab5) — requires ESP-IDF >= 5.x (M5Stack board pkg v3.x)
// =============================================================================

#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

// ---- Callback types ---------------------------------------------------------
typedef void (*usb_connect_cb_t)(void);
typedef void (*usb_disconnect_cb_t)(void);
typedef void (*usb_data_cb_t)(const uint8_t *data, size_t len);

// ---- Public API -------------------------------------------------------------

/**
 * Initialise the USB host stack and register CDC-ACM class driver.
 * Call once in setup(). Callbacks are called from the USB task context.
 *
 * @param on_connect     Called when a CDC device is enumerated
 * @param on_disconnect  Called when device is removed
 * @param on_data        Called with received data bytes
 */
void usb_host_cdc_init(usb_connect_cb_t    on_connect,
                       usb_disconnect_cb_t on_disconnect,
                       usb_data_cb_t       on_data);

/**
 * Must be called repeatedly from loop(). Drives the USB host event loop.
 * Returns quickly if no events are pending.
 */
void usb_host_cdc_task(void);

/**
 * Set CDC line coding (call after device connects, before sending data).
 *
 * @param baud      e.g. 115200
 * @param databits  e.g. 8
 * @param parity    0=none, 1=odd, 2=even
 * @param stopbits  1 or 2
 */
void usb_cdc_set_line_coding(uint32_t baud, uint8_t databits,
                              uint8_t parity, uint8_t stopbits);

/**
 * Write bytes to the CDC device.
 * @return number of bytes actually sent, or -1 on error
 */
int usb_cdc_write(const uint8_t *data, size_t len);

/**
 * Returns true if a CDC device is currently connected and open.
 */
bool usb_cdc_is_connected(void);
