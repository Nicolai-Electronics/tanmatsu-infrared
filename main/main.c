#include <stdio.h>
#include "app.h"
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/power.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "pax_gfx.h"
#include "ui.h"
#include "wifi_remote.h"

#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_hosted.h"
#endif

#if CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#endif

// Constants
static char const TAG[] = "main";

// Global variables
static size_t                     display_h_res        = 0;
static size_t                     display_v_res        = 0;
static bsp_display_color_format_t display_color_format = 0;
static bsp_display_endianness_t   display_data_endian  = 0;
static pax_buf_t                  fb                   = {0};
static QueueHandle_t              input_event_queue    = NULL;

#if defined(CONFIG_BSP_TARGET_KAMI)
// Temporary addition for supporting epaper devices (irrelevant for Tanmatsu)
static pax_col_t palette[] = {0xffffffff, 0xff000000, 0xffff0000};  // white, black, red
#endif

#if CONFIG_BT_NIMBLE_ENABLED
// NimBLE host task: runs until nimble_port_stop() is called
static void ble_host_task(void* param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_on_reset(int reason) {
    ESP_LOGW(TAG, "BLE host reset, reason: %d", reason);
}

static void ble_on_sync(void) {
    ESP_LOGI(TAG, "BLE host synced with controller");
    // Add your GAP/GATT service setup and start advertising here
}

// Starts the NimBLE host. On targets where BT is provided by a co-processor (connected over
// esp-hosted) the companion chip's BT controller must be brought up before the NimBLE host's
// transport buffers exist, otherwise an incoming HCI event can crash the allocator.
// If you don't need BT then disable the CONFIG_BT_NIMBLE_ENABLED configuration option in the
// sdkconfig file too to prevent incoming HCI events from crashing the application.

static esp_err_t ble_init(void) {
#if CONFIG_IDF_TARGET_ESP32P4
    if (esp_hosted_bt_controller_init() != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize hosted BT controller");
    }
    if (esp_hosted_bt_controller_enable() != ESP_OK) {
        ESP_LOGW(TAG, "Failed to enable hosted BT controller");
    }
#endif

    esp_err_t res = nimble_port_init();
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NimBLE host stack: %d", res);
        return res;
    }

    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb  = ble_on_sync;

    nimble_port_freertos_init(ble_host_task);
    return ESP_OK;
}
#endif

void app_main(void) {
    // Start the GPIO interrupt service
    gpio_install_isr_service(0);

    // Initialize the Non Volatile Storage partition
    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        res = nvs_flash_erase();
        if (res != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase NVS flash: %d", res);
            return;
        }
        res = nvs_flash_init();
    }
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS flash: %d", res);
        return;
    }

    // Initialize the Board Support Package
    const bsp_configuration_t bsp_configuration = {
        .display =
            {
                .requested_color_format = BSP_DISPLAY_COLOR_FORMAT_24_888RGB,
                .num_fbs                = 1,
            },
    };
    res = bsp_device_initialize(&bsp_configuration);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BSP: %d", res);
        return;
    }

    // Get display parameters and rotation
    res = bsp_display_get_parameters(&display_h_res, &display_v_res, &display_color_format, &display_data_endian);
    if (res != ESP_ERR_NOT_SUPPORTED) {
        if (res != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get display parameters: %d", res);
            return;
        }

        // Convert ESP-IDF color format into PAX buffer type
        pax_buf_type_t format = PAX_BUF_24_888RGB;
        switch (display_color_format) {
            case BSP_DISPLAY_COLOR_FORMAT_1_PAL:
                format = PAX_BUF_1_PAL;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_2_PAL:
                format = PAX_BUF_2_PAL;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_4_PAL:
                format = PAX_BUF_4_PAL;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_8_PAL:
                format = PAX_BUF_8_PAL;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_16_PAL:
                format = PAX_BUF_16_PAL;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_1_GREY:
                format = PAX_BUF_1_GREY;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_2_GREY:
                format = PAX_BUF_2_GREY;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_4_GREY:
                format = PAX_BUF_4_GREY;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_8_GREY:
                format = PAX_BUF_8_GREY;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_8_332RGB:
                format = PAX_BUF_8_332RGB;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_16_565RGB:
                format = PAX_BUF_16_565RGB;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_4_1111ARGB:
                format = PAX_BUF_4_1111ARGB;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_8_2222ARGB:
                format = PAX_BUF_8_2222ARGB;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_16_4444ARGB:
                format = PAX_BUF_16_4444ARGB;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_24_888RGB:
                format = PAX_BUF_24_888RGB;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_32_8888ARGB:
                format = PAX_BUF_32_8888ARGB;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_18_666RGB:
            default:
                ESP_LOGW(TAG, "BSP requests color format not supported by PAX (%u)", format);
                break;
        }

        // Convert BSP display rotation format into PAX orientation type
        bsp_display_rotation_t display_rotation = bsp_display_get_default_rotation();
        pax_orientation_t      orientation      = PAX_O_UPRIGHT;
        switch (display_rotation) {
            case BSP_DISPLAY_ROTATION_90:
                orientation = PAX_O_ROT_CCW;
                break;
            case BSP_DISPLAY_ROTATION_180:
                orientation = PAX_O_ROT_HALF;
                break;
            case BSP_DISPLAY_ROTATION_270:
                orientation = PAX_O_ROT_CW;
                break;
            case BSP_DISPLAY_ROTATION_0:
            default:
                orientation = PAX_O_UPRIGHT;
                break;
        }

        // Initialize graphics stack
        printf("Initializing framebuffer with w=%d h=%d format=%d endian=%d orientation=%d\n", display_h_res,
               display_v_res, format, display_data_endian, orientation);
        pax_buf_init(&fb, NULL, display_h_res, display_v_res, format);
        pax_buf_reversed(&fb, display_data_endian == BSP_DISPLAY_ENDIAN_BIG);
#if defined(CONFIG_BSP_TARGET_KAMI)
        // Temporary addition for supporting Kami
        fb.palette      = palette;
        fb.palette_size = sizeof(palette) / sizeof(pax_col_t);
#endif
        pax_buf_set_orientation(&fb, orientation);
    } else {
        ESP_LOGI(TAG, "This board has no display support");
    }

    // Get input event queue from BSP
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    if (pax_buf_get_width(&fb) == 0) {
        ESP_LOGE(TAG, "This application requires a display");
        return;
    }
    ui_init(&fb, input_event_queue);

    // Start the radio, it provides the infrared transmitter (and Bluetooth)
    ui_busy("Starting radio...");
    bool radio_started = wifi_remote_initialize() == ESP_OK;
    if (!radio_started) {
        bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
        ESP_LOGE(TAG, "Radio not responding");
    }

#if CONFIG_BT_NIMBLE_ENABLED
    // Only bring up BLE once the radio (or the esp-hosted co-processor link, on targets where BT
    // and WiFi share that link) is confirmed to be up. If your app does not require BLE you can
    // remove this section (and disable Bluetooth/NimBLE in menuconfig).
    if (wifi_remote_get_initialized()) {
        ui_busy("Starting BLE stack...");
        if (ble_init() != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start BLE stack");
        }
    }
#endif

    app_run(radio_started);
}
