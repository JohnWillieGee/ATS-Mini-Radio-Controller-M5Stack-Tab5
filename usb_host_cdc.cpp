// =============================================================================
// usb_host_cdc.cpp  — v2
// ESP32-P4 USB host CDC-ACM using only usb/usb_host.h
// (cdc_acm_host.h is NOT used — it requires the ESP Component Registry)
//
// This implements just enough CDC-ACM class handling to:
//   - Enumerate a CDC device (read device/config descriptors)
//   - Open Bulk IN endpoint (data from device)
//   - Open Bulk OUT endpoint (data to device)
//   - Set line coding via control transfer (SET_LINE_CODING)
//   - Receive data via repeated IN transfers
//   - Send data via OUT transfer
//
// CDC-ACM class codes:
//   Class 0x02 (Communications), subclass 0x02 (ACM)
//   OR device class 0xEF with interface class 0x0A (CDC Data)
//
// NOTES:
//   - Runs a single FreeRTOS task pinned to core 0
//   - Arduino loop() just calls usb_host_cdc_task() which yields
//   - Callbacks fire from the USB task — safe for Serial.printf,
//     NOT safe for direct LVGL calls (use a flag + handle in loop())
// =============================================================================

#include "usb_host_cdc.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <string.h>
#include "usb/usb_host.h"

// ---- CDC-ACM request constants ----------------------------------------------
#define CDC_SET_LINE_CODING         0x20
#define CDC_REQ_TYPE_CLASS_IFACE    0x21  // bmRequestType: class, interface, host->device

// ---- Internal state ---------------------------------------------------------
static usb_connect_cb_t    s_on_connect    = nullptr;
static usb_disconnect_cb_t s_on_disconnect = nullptr;
static usb_data_cb_t       s_on_data       = nullptr;

static usb_host_client_handle_t s_client_hdl   = nullptr;
static usb_device_handle_t      s_dev_hdl       = nullptr;
static SemaphoreHandle_t        s_dev_mutex     = nullptr;
static TaskHandle_t             s_usb_task_hdl  = nullptr;

// Event queue: callback posts connect/disconnect events here; task drains it
typedef enum { USB_EVT_NEW_DEV, USB_EVT_DEV_GONE } usb_evt_type_t;
typedef struct { usb_evt_type_t type; uint8_t addr; } usb_evt_t;
static QueueHandle_t s_evt_queue = nullptr;

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg) {
    usb_evt_t evt = {};
    switch (msg->event) {
        case USB_HOST_CLIENT_EVENT_NEW_DEV:
            evt.type = USB_EVT_NEW_DEV;
            evt.addr = msg->new_dev.address;
            xQueueSendFromISR(s_evt_queue, &evt, nullptr);
            break;
        case USB_HOST_CLIENT_EVENT_DEV_GONE:
            evt.type = USB_EVT_DEV_GONE;
            evt.addr = 0;
            xQueueSendFromISR(s_evt_queue, &evt, nullptr);
            break;
        default:
            break;
    }
}

// Endpoints
static uint8_t  s_ep_bulk_in  = 0;
static uint8_t  s_ep_bulk_out = 0;
static uint16_t s_ep_in_mps   = 64;
static uint16_t s_ep_out_mps  = 64;

// Transfer objects (pre-allocated)
#define IN_BUF_SIZE  512
#define OUT_BUF_SIZE 64

static usb_transfer_t *s_in_xfer   = nullptr;
static usb_transfer_t *s_out_xfer  = nullptr;
static usb_transfer_t *s_ctrl_xfer = nullptr;

static volatile bool s_dev_connected = false;

// ---- Forward declarations ---------------------------------------------------
static void submit_in_transfer(void);

// ---- Find bulk endpoints AND the interface that owns them (single pass) -----
//
// ESP32-S3 CDC composite layout (VID=303A PID=1001):
//   Interface 0: CDC Control (class 0x02)  — interrupt EP only
//   Interface 1: CDC Data   (class 0x0A)  — bulk IN 0x81, bulk OUT 0x01
//   Interface 2: CDC Control (class 0x02)  — second port (if present)
//   Interface 3: CDC Data   (class 0x0A)  — bulk IN 0x83, bulk OUT 0x02
//
// Strategy: walk descriptors tracking current interface; record the FIRST
// interface that has BOTH a bulk IN and a bulk OUT endpoint. Claim that one.

static uint8_t s_claimed_iface = 0xFF;  // interface we actually claimed

static bool find_and_claim_cdc(usb_device_handle_t dev_hdl) {
    const usb_config_desc_t *cfg_desc;
    if (usb_host_get_active_config_descriptor(dev_hdl, &cfg_desc) != ESP_OK) {
        Serial.println("[CDC] Failed to get config descriptor");
        return false;
    }

    const uint8_t *p     = (const uint8_t *)cfg_desc;
    int            total = cfg_desc->wTotalLength;
    int            offset = 0;

    // Per-interface candidate tracking
    uint8_t  cur_iface     = 0xFF;
    uint8_t  cur_class     = 0x00;
    uint8_t  cand_ep_in    = 0;
    uint8_t  cand_ep_out   = 0;
    uint16_t cand_mps_in   = 64;
    uint16_t cand_mps_out  = 64;

    while (offset < total) {
        uint8_t len  = p[offset];
        uint8_t type = p[offset + 1];
        if (len == 0) break;

        if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            // Entering a new interface — check if previous one was a complete candidate
            if (cur_iface != 0xFF && cand_ep_in != 0 && cand_ep_out != 0) {
                // Found a complete bulk pair on previous interface — claim it
                esp_err_t err = usb_host_interface_claim(s_client_hdl, dev_hdl,
                                                          cur_iface, 0);
                if (err == ESP_OK) {
                    s_ep_bulk_in  = cand_ep_in;
                    s_ep_bulk_out = cand_ep_out;
                    s_ep_in_mps   = cand_mps_in;
                    s_ep_out_mps  = cand_mps_out;
                    s_claimed_iface = cur_iface;
                    Serial.printf("[CDC] Claimed iface %d (class 0x%02X): "
                                  "IN=0x%02X OUT=0x%02X\n",
                                  cur_iface, cur_class, cand_ep_in, cand_ep_out);
                    return true;
                } else {
                    Serial.printf("[CDC] Claim iface %d failed: 0x%x — trying next\n",
                                  cur_iface, err);
                }
            }

            // Start tracking new interface
            cur_iface   = p[offset + 2];
            cur_class   = p[offset + 5];
            cand_ep_in  = 0;
            cand_ep_out = 0;

            // Only look for bulk EPs on CDC Data (0x0A) or vendor (0xFF) interfaces.
            // Skip CDC Control (0x02) — it only has an interrupt EP.
            if (cur_class == 0x02) {
                cur_iface = 0xFF;  // mark as skip
            }
        }

        if (type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && cur_iface != 0xFF) {
            uint8_t  ep_addr = p[offset + 2];
            uint8_t  ep_attr = p[offset + 3];
            uint16_t mps     = (uint16_t)p[offset + 4] | ((uint16_t)p[offset + 5] << 8);

            if ((ep_attr & 0x03) == 0x02) {  // Bulk transfer type
                if (ep_addr & 0x80) {
                    cand_ep_in  = ep_addr;
                    cand_mps_in = mps;
                    Serial.printf("[CDC] iface %d: Bulk IN  0x%02X mps=%u\n",
                                  cur_iface, ep_addr, mps);
                } else {
                    cand_ep_out  = ep_addr;
                    cand_mps_out = mps;
                    Serial.printf("[CDC] iface %d: Bulk OUT 0x%02X mps=%u\n",
                                  cur_iface, ep_addr, mps);
                }
            }
        }

        offset += len;
    }

    // Check final interface in descriptor
    if (cur_iface != 0xFF && cand_ep_in != 0 && cand_ep_out != 0) {
        esp_err_t err = usb_host_interface_claim(s_client_hdl, dev_hdl, cur_iface, 0);
        if (err == ESP_OK) {
            s_ep_bulk_in    = cand_ep_in;
            s_ep_bulk_out   = cand_ep_out;
            s_ep_in_mps     = cand_mps_in;
            s_ep_out_mps    = cand_mps_out;
            s_claimed_iface = cur_iface;
            Serial.printf("[CDC] Claimed iface %d (class 0x%02X): "
                          "IN=0x%02X OUT=0x%02X\n",
                          cur_iface, cur_class, cand_ep_in, cand_ep_out);
            return true;
        }
        Serial.printf("[CDC] Claim iface %d failed: 0x%x\n", cur_iface, err);
    }

    Serial.println("[CDC] No claimable CDC Data interface with bulk pair found");
    return false;
}

// ---- Transfer callbacks -----------------------------------------------------

static void in_transfer_cb(usb_transfer_t *xfer) {
    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED && xfer->actual_num_bytes > 0) {
        if (s_on_data) {
            s_on_data(xfer->data_buffer, xfer->actual_num_bytes);
        }
    } else if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        if (xfer->status != USB_TRANSFER_STATUS_CANCELED) {
            Serial.printf("[CDC] IN xfer status=%d\n", xfer->status);
        }
        return;  // Don't resubmit on error/cancel
    }
    // Continuous resubmit
    if (s_dev_connected) {
        submit_in_transfer();
    }
}

static void out_transfer_cb(usb_transfer_t *xfer) {
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        Serial.printf("[CDC] OUT xfer status=%d\n", xfer->status);
    }
}

static void ctrl_transfer_cb(usb_transfer_t *xfer) {
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        Serial.printf("[CDC] CTRL xfer status=%d\n", xfer->status);
    }
}

// ---- Submit IN transfer -----------------------------------------------------

static void submit_in_transfer(void) {
    if (!s_in_xfer || !s_dev_hdl) return;

    s_in_xfer->device_handle    = s_dev_hdl;
    s_in_xfer->bEndpointAddress = s_ep_bulk_in;
    s_in_xfer->num_bytes        = IN_BUF_SIZE;
    s_in_xfer->callback         = in_transfer_cb;
    s_in_xfer->context          = nullptr;
    s_in_xfer->timeout_ms       = 0;   // No timeout on bulk IN

    esp_err_t err = usb_host_transfer_submit(s_in_xfer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.printf("[CDC] IN submit err: 0x%x\n", err);
    }
}

// ---- Device open / close ----------------------------------------------------

static void open_device(uint8_t dev_addr) {
    Serial.printf("[USB] New device addr=%d — opening\n", dev_addr);

    esp_err_t err = usb_host_device_open(s_client_hdl, dev_addr, &s_dev_hdl);
    if (err != ESP_OK) {
        Serial.printf("[USB] device_open failed: 0x%x\n", err);
        return;
    }

    // Print descriptor info
    const usb_device_desc_t *dev_desc;
    usb_host_get_device_descriptor(s_dev_hdl, &dev_desc);
    Serial.printf("[USB] VID=0x%04X PID=0x%04X Class=0x%02X\n",
                  dev_desc->idVendor, dev_desc->idProduct, dev_desc->bDeviceClass);

    s_ep_bulk_in  = 0;
    s_ep_bulk_out = 0;
    s_claimed_iface = 0xFF;

    if (!find_and_claim_cdc(s_dev_hdl)) {
        Serial.println("[CDC] Failed to find/claim CDC interface");
        usb_host_device_close(s_client_hdl, s_dev_hdl);
        s_dev_hdl = nullptr;
        return;
    }

    // Allocate transfers
    if (!s_in_xfer)   usb_host_transfer_alloc(IN_BUF_SIZE, 0, &s_in_xfer);
    if (!s_out_xfer)  usb_host_transfer_alloc(OUT_BUF_SIZE, 0, &s_out_xfer);
    if (!s_ctrl_xfer) usb_host_transfer_alloc(64, 0, &s_ctrl_xfer);

    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    s_dev_connected = true;
    xSemaphoreGive(s_dev_mutex);

    if (s_on_connect) s_on_connect();

    submit_in_transfer();
}

static void close_device(void) {
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    s_dev_connected = false;
    xSemaphoreGive(s_dev_mutex);

    // Free transfers (cancels any pending)
    if (s_in_xfer)   { usb_host_transfer_free(s_in_xfer);   s_in_xfer   = nullptr; }
    if (s_out_xfer)  { usb_host_transfer_free(s_out_xfer);  s_out_xfer  = nullptr; }
    if (s_ctrl_xfer) { usb_host_transfer_free(s_ctrl_xfer); s_ctrl_xfer = nullptr; }

    if (s_dev_hdl) {
        usb_host_device_close(s_client_hdl, s_dev_hdl);
        s_dev_hdl = nullptr;
    }

    if (s_on_disconnect) s_on_disconnect();
}

// ---- USB host task ----------------------------------------------------------

static void usb_host_task(void *arg) {
    while (true) {
        // Process library-level events (non-blocking, 0 ticks)
        uint32_t flags = 0;
        usb_host_lib_handle_events(0, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }

        // Pump the client event machinery (fires client_event_cb internally)
        usb_host_client_handle_events(s_client_hdl, pdMS_TO_TICKS(10));

        // Drain our event queue
        usb_evt_t evt;
        while (xQueueReceive(s_evt_queue, &evt, 0) == pdTRUE) {
            if (evt.type == USB_EVT_NEW_DEV) {
                open_device(evt.addr);
            } else {
                Serial.println("[USB] Device removed");
                close_device();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ---- Public API -------------------------------------------------------------

void usb_host_cdc_init(usb_connect_cb_t    on_connect,
                       usb_disconnect_cb_t on_disconnect,
                       usb_data_cb_t       on_data) {
    s_on_connect    = on_connect;
    s_on_disconnect = on_disconnect;
    s_on_data       = on_data;
    s_dev_mutex  = xSemaphoreCreateMutex();
    s_evt_queue  = xQueueCreate(8, sizeof(usb_evt_t));

    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_cfg);
    if (err != ESP_OK) {
        Serial.printf("[USB] usb_host_install failed: 0x%x\n", err);
        return;
    }

    const usb_host_client_config_t client_cfg = {
        .is_synchronous    = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg          = nullptr,
        },
    };
    err = usb_host_client_register(&client_cfg, &s_client_hdl);
    if (err != ESP_OK) {
        Serial.printf("[USB] client_register failed: 0x%x\n", err);
        return;
    }

    xTaskCreatePinnedToCore(usb_host_task, "usb_host",
                            4096, nullptr, 10, &s_usb_task_hdl, 0);

    Serial.println("[USB] Host stack ready — waiting for device");
}

void usb_host_cdc_task(void) {
    vTaskDelay(1);  // yield — actual work done in usb_host_task
}

void usb_cdc_set_line_coding(uint32_t baud, uint8_t databits,
                              uint8_t parity, uint8_t stopbits) {
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    bool ok = (s_dev_hdl != nullptr && s_ctrl_xfer != nullptr);
    xSemaphoreGive(s_dev_mutex);
    if (!ok) {
        Serial.println("[CDC] set_line_coding: no device");
        return;
    }

    uint8_t *buf = s_ctrl_xfer->data_buffer;
    // USB setup packet (8 bytes)
    buf[0] = CDC_REQ_TYPE_CLASS_IFACE;
    buf[1] = CDC_SET_LINE_CODING;
    buf[2] = 0x00; buf[3] = 0x00;  // wValue
    buf[4] = 0x00; buf[5] = 0x00;  // wIndex (interface 0)
    buf[6] = 0x07; buf[7] = 0x00;  // wLength = 7

    // Line coding payload (7 bytes)
    buf[8]  = (baud)       & 0xFF;
    buf[9]  = (baud >> 8)  & 0xFF;
    buf[10] = (baud >> 16) & 0xFF;
    buf[11] = (baud >> 24) & 0xFF;
    buf[12] = (stopbits == 2) ? 2 : 0;
    buf[13] = parity;
    buf[14] = databits;

    s_ctrl_xfer->device_handle    = s_dev_hdl;
    s_ctrl_xfer->bEndpointAddress = 0x00;
    s_ctrl_xfer->num_bytes        = 15;
    s_ctrl_xfer->callback         = ctrl_transfer_cb;
    s_ctrl_xfer->context          = nullptr;
    s_ctrl_xfer->timeout_ms       = 1000;

    esp_err_t err = usb_host_transfer_submit_control(s_client_hdl, s_ctrl_xfer);
    if (err != ESP_OK) {
        Serial.printf("[CDC] SET_LINE_CODING err: 0x%x\n", err);
    } else {
        Serial.printf("[CDC] Line coding set: %lu baud %dN%d\n", baud, databits, stopbits);
    }
    delay(100);
}

int usb_cdc_write(const uint8_t *data, size_t len) {
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    bool ok = (s_dev_hdl != nullptr && s_out_xfer != nullptr && s_dev_connected);
    xSemaphoreGive(s_dev_mutex);
    if (!ok || len == 0) return -1;
    if (len > OUT_BUF_SIZE) len = OUT_BUF_SIZE;

    memcpy(s_out_xfer->data_buffer, data, len);
    s_out_xfer->device_handle    = s_dev_hdl;
    s_out_xfer->bEndpointAddress = s_ep_bulk_out;
    s_out_xfer->num_bytes        = len;
    s_out_xfer->callback         = out_transfer_cb;
    s_out_xfer->context          = nullptr;
    s_out_xfer->timeout_ms       = 1000;

    esp_err_t err = usb_host_transfer_submit(s_out_xfer);
    if (err != ESP_OK) {
        Serial.printf("[CDC] OUT submit err: 0x%x\n", err);
        return -1;
    }
    delay(50);
    return (int)len;
}

bool usb_cdc_is_connected(void) {
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    bool c = s_dev_connected;
    xSemaphoreGive(s_dev_mutex);
    return c;
}
