// SPDX-License-Identifier: GPL-2.0-or-later
#include "esp_usb_hid.h"

#if CONFIG_TD_USB_HOST

#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"

#include "esp_input_remote.h"

static const char *TAG = "doom_usb_hid";

typedef struct {
    hid_host_device_handle_t handle;
    hid_host_driver_event_t event;
} hid_event_t;

static QueueHandle_t event_queue;
static esp_err_t host_install_result = ESP_FAIL;
static int started;

static int report_has_key(const hid_keyboard_input_report_boot_t *report,
                          uint8_t code)
{
    for (unsigned i = 0; i < HID_KEYBOARD_KEY_MAX; ++i)
        if (report->key[i] == code) return 1;
    return 0;
}

static uint32_t keyboard_mask(const uint8_t *data, size_t length)
{
    if (length < sizeof(hid_keyboard_input_report_boot_t)) return 0;
    const hid_keyboard_input_report_boot_t *report =
            (const hid_keyboard_input_report_boot_t *)data;
    uint32_t mask = 0;

    /* USB HID Usage Tables keyboard page codes.  Letter aliases make either
     * the classic DOOM layout or cursor-key-only keyboards useful. */
    if (report_has_key(report, 0x52) || report_has_key(report, 0x1a))
        mask |= ESP_DOOM_CTL_FORWARD;             /* Up / W */
    if (report_has_key(report, 0x51) || report_has_key(report, 0x16))
        mask |= ESP_DOOM_CTL_BACK;                /* Down / S */
    if (report_has_key(report, 0x50) || report_has_key(report, 0x04))
        mask |= ESP_DOOM_CTL_LEFT;                /* Left / A */
    if (report_has_key(report, 0x4f) || report_has_key(report, 0x07))
        mask |= ESP_DOOM_CTL_RIGHT;               /* Right / D */
    if (report_has_key(report, 0x2c) || report_has_key(report, 0x08))
        mask |= ESP_DOOM_CTL_USE;                 /* Space / E */
    if (report_has_key(report, 0x28)) mask |= ESP_DOOM_CTL_ENTER;
    if (report_has_key(report, 0x29)) mask |= ESP_DOOM_CTL_ESCAPE;
    if (report_has_key(report, 0x2b)) mask |= ESP_DOOM_CTL_MAP;
    if (report_has_key(report, 0x0d)) mask |= ESP_DOOM_CTL_FIRE; /* J */

    uint8_t modifier = report->modifier.val;
    if (modifier & 0x11) mask |= ESP_DOOM_CTL_FIRE;   /* L/R Ctrl */
    if (modifier & 0x22) mask |= ESP_DOOM_CTL_RUN;    /* L/R Shift */
    if (modifier & 0x44) mask |= ESP_DOOM_CTL_STRAFE; /* L/R Alt */

    for (uint8_t weapon = 0; weapon < 7; ++weapon)
        if (report_has_key(report, (uint8_t)(0x1e + weapon)))
            mask |= ESP_DOOM_CTL_WEAPON1 << weapon;
    return mask;
}

static void interface_callback(hid_host_device_handle_t handle,
                               hid_host_interface_event_t event, void *arg)
{
    (void)arg;
    if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
        uint8_t data[16];
        size_t length = 0;
        if (hid_host_device_get_raw_input_report_data(handle, data,
                                                       sizeof(data), &length)
                == ESP_OK)
            esp_input_source_update(ESP_INPUT_SOURCE_USB,
                                    keyboard_mask(data, length));
    } else if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        esp_input_source_disconnect(ESP_INPUT_SOURCE_USB);
        esp_err_t result = hid_host_device_close(handle);
        if (result != ESP_OK)
            ESP_LOGW(TAG, "device close: %s", esp_err_to_name(result));
        ESP_LOGI(TAG, "keyboard disconnected; all USB keys released");
    } else if (event == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR) {
        esp_input_source_disconnect(ESP_INPUT_SOURCE_USB);
        ESP_LOGW(TAG, "keyboard transfer error; all USB keys released");
    }
}

static void open_device(hid_host_device_handle_t handle)
{
    hid_host_dev_params_t params;
    esp_err_t result = hid_host_device_get_params(handle, &params);
    if (result != ESP_OK || params.proto != HID_PROTOCOL_KEYBOARD) return;

    const hid_host_device_config_t config = {
        .callback = interface_callback,
        .callback_arg = NULL,
    };
    if ((result = hid_host_device_open(handle, &config)) != ESP_OK) goto fail;
    if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
        if ((result = hid_class_request_set_protocol(
                     handle, HID_REPORT_PROTOCOL_BOOT)) != ESP_OK) goto close;
        if ((result = hid_class_request_set_idle(handle, 0, 0)) != ESP_OK)
            goto close;
    }
    if ((result = hid_host_device_start(handle)) != ESP_OK) goto close;
    ESP_LOGI(TAG, "USB boot keyboard ready");
    return;

close:
    hid_host_device_close(handle);
fail:
    ESP_LOGW(TAG, "keyboard open failed: %s", esp_err_to_name(result));
}

static void driver_callback(hid_host_device_handle_t handle,
                            hid_host_driver_event_t event, void *arg)
{
    (void)arg;
    hid_event_t item = {.handle = handle, .event = event};
    if (event_queue) xQueueSend(event_queue, &item, 0);
}

static void event_task(void *arg)
{
    (void)arg;
    hid_event_t item;
    for (;;) {
        if (xQueueReceive(event_queue, &item, portMAX_DELAY) &&
            item.event == HID_HOST_DRIVER_EVENT_CONNECTED)
            open_device(item.handle);
    }
}

static void library_task(void *caller)
{
    const usb_host_config_t config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    host_install_result = usb_host_install(&config);
    xTaskNotifyGive(caller);
    if (host_install_result != ESP_OK) vTaskDelete(NULL);

    for (;;) {
        uint32_t flags = 0;
        esp_err_t result = usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (result != ESP_OK)
            ESP_LOGW(TAG, "host event: %s", esp_err_to_name(result));
    }
}

esp_err_t esp_usb_hid_start(void)
{
    if (started) return ESP_OK;
    started = 1;
    event_queue = xQueueCreate(4, sizeof(hid_event_t));
    if (!event_queue) return ESP_ERR_NO_MEM;

    if (xTaskCreatePinnedToCore(library_task, "usb_events", 3072,
                                xTaskGetCurrentTaskHandle(), 3, NULL, 0)
            != pdPASS)
        return ESP_ERR_NO_MEM;
    if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000))) return ESP_ERR_TIMEOUT;
    if (host_install_result != ESP_OK) return host_install_result;

    const hid_host_driver_config_t driver = {
        .create_background_task = true,
        .task_priority = 4,
        .stack_size = 3072,
        .core_id = 0,
        .callback = driver_callback,
        .callback_arg = NULL,
    };
    esp_err_t result = hid_host_install(&driver);
    if (result != ESP_OK) return result;
    if (xTaskCreatePinnedToCore(event_task, "hid_events", 3072, NULL, 3,
                                NULL, 0) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "native USB host ready; waiting for keyboard");
    return ESP_OK;
}

#else

esp_err_t esp_usb_hid_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
