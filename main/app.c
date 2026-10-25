#include "app.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bsp/device.h"
#include "bsp/storage.h"
#include "catt_ir.h"
#include "code_db.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "radio_ir_protocol_client.h"
#include "ui.h"

static char const TAG[] = "app";

#define APP_TITLE      "Infrared remote"
#define SD_MOUNT_POINT "/sd"
#define DB_PATH        SD_MOUNT_POINT "/nec_codes.json"
#define DB_BACKUP_PATH SD_MOUNT_POINT "/nec_codes.json.bak"

typedef enum {
    EDITOR_NEW,
    EDITOR_EDIT,
    EDITOR_CUSTOM,
} editor_mode_t;

enum {
    FIELD_NAME,
    FIELD_ADDRESS,
    FIELD_COMMAND,
    FIELD_COUNT,
};

enum {
    SETTING_ADDON,
    SETTING_TRANSMITTER,
    SETTING_FREQUENCY,
    SETTING_DUTY,
    SETTING_COUNT,
};

typedef enum {
    TRANSMITTER_NONE,
    TRANSMITTER_ONBOARD,  // Infrared transmitter of the radio co-processor
    TRANSMITTER_ADDON,    // Infrared add-on on the CATT port
} transmitter_t;

static code_db_t   db;
static size_t      selected = 0;
static size_t      scroll   = 0;
static char        status_text[160];
static ui_status_t status_kind = UI_STATUS_NONE;

// Infrared
static bool          onboard_available = false;
static char          onboard_unavailable_reason[96];
static bool          addon_enabled = false;  // Not stored, to be replaced by detection of the add-on
static transmitter_t transmitter   = TRANSMITTER_NONE;

// The values on the custom code screen are kept between visits
static char custom_address[5] = "";
static char custom_command[5] = "";

// Repeated transmissions of the same code are counted in the status line
static unsigned send_count   = 0;
static uint16_t last_address = 0;
static uint16_t last_command = 0;

static void set_status(ui_status_t kind, char const* format, ...) __attribute__((format(printf, 2, 3)));

static void set_status(ui_status_t kind, char const* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(status_text, sizeof(status_text), format, args);
    va_end(args);
    status_kind = kind;
    send_count  = 0;
}

static void clear_status(void) {
    status_text[0] = '\0';
    status_kind    = UI_STATUS_NONE;
    send_count     = 0;
}

// Leaving the application

static void __attribute__((noreturn)) exit_to_launcher(void) {
    ui_busy("Returning to launcher...");
    bsp_device_restart_to_launcher();
    while (true) {
        vTaskDelay(portMAX_DELAY);
    }
}

static void confirm_exit(void) {
    if (ui_confirm("Exit", "Return to the launcher?", "Exit")) {
        exit_to_launcher();
    }
}

static void __attribute__((noreturn)) fatal_error(char const* title, char const* message) {
    static ui_hint_t const hints[] = {{"F1", "Return to launcher"}};
    ui_alert(title, message, hints, sizeof(hints) / sizeof(hints[0]), true);
    while (true) {
        ui_input_t input = ui_wait_input();
        if (input.key == UI_KEY_F1 || input.key == UI_KEY_ENTER || input.key == UI_KEY_ESC) {
            exit_to_launcher();
        }
    }
}

// Infrared

// 1 or 2 hex digits: standard NEC, the inverted value is added as the upper byte (04 becomes FB04)
// 3 or 4 hex digits: the value is sent exactly as entered (extended NEC)
static bool parse_nec_value(char const* text, uint16_t* out) {
    size_t length = strlen(text);
    if (length == 0 || length > 4 || text[strspn(text, "0123456789abcdefABCDEF")] != '\0') {
        return false;
    }
    uint32_t value = strtoul(text, NULL, 16);
    if (length <= 2) {
        value = (value & 0xFF) | ((~value & 0xFF) << 8);
    }
    *out = (uint16_t)value;
    return true;
}

static char const* transmitter_name(transmitter_t value) {
    switch (value) {
        case TRANSMITTER_ONBOARD:
            return "On-board";
        case TRANSMITTER_ADDON:
            return "CATT add-on";
        default:
            return "None";
    }
}

// Apply the carrier settings to every available transmitter
static esp_err_t apply_carrier(code_db_settings_t const* settings) {
    esp_err_t result = ESP_OK;
    if (onboard_available) {
        ir_protocol_client_config_t config = {
            .frequency_hz = settings->frequency_hz,
            .duty_cycle   = (uint8_t)((settings->duty_percent * 256 + 50) / 100),  // Fraction of 256
        };
        esp_err_t res = ir_protocol_client_set_config(&config);
        if (res != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure the on-board transmitter: %s", esp_err_to_name(res));
            result = res;
        }
    }
    if (addon_enabled) {
        esp_err_t res = catt_ir_set_carrier(settings->frequency_hz, settings->duty_percent);
        if (res != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure the add-on transmitter: %s", esp_err_to_name(res));
            result = res;
        }
    }
    return result;
}

static void send_code(char const* name, uint16_t address, uint16_t command) {
    esp_err_t res = ESP_ERR_NOT_SUPPORTED;
    if (transmitter == TRANSMITTER_ONBOARD) {
        ir_protocol_client_nec_scan_code_t scan_code = {.address = address, .command = command};
        res                                          = ir_protocol_client_send_nec(&scan_code);
    } else if (transmitter == TRANSMITTER_ADDON) {
        res = catt_ir_send_nec(address, command);
    } else {
        set_status(UI_STATUS_ERROR, "No infrared transmitter available, enable the add-on in the settings (F6)");
        return;
    }
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send NEC code %04X %04X: %s", address, command, esp_err_to_name(res));
        set_status(UI_STATUS_ERROR, "Failed to send code (%s)", esp_err_to_name(res));
        return;
    }

    unsigned count      = (send_count > 0 && address == last_address && command == last_command) ? send_count + 1 : 1;
    char     repeat[16] = "";
    if (count > 1) {
        snprintf(repeat, sizeof(repeat), "  (%ux)", count);
    }
    if (name != NULL && name[0] != '\0') {
        set_status(UI_STATUS_SUCCESS, "Sent \"%s\": address 0x%04X, command 0x%04X%s", name, address, command, repeat);
    } else {
        set_status(UI_STATUS_SUCCESS, "Sent address 0x%04X, command 0x%04X%s", address, command, repeat);
    }
    send_count   = count;
    last_address = address;
    last_command = command;
}

// The on-board transmitter is optional, without it only the add-on can be used for sending
static void check_onboard_support(bool radio_started) {
    esp_err_t res = ESP_ERR_INVALID_STATE;
    if (radio_started) {
        ui_busy("Checking infrared support...");
        res = ir_protocol_client_init();
        if (res == ESP_OK) {
            res = ir_protocol_client_get_supported();
        }
    }
    onboard_available = res == ESP_OK;
    if (onboard_available) {
        transmitter = TRANSMITTER_ONBOARD;
        return;
    }

    ESP_LOGW(TAG, "On-board infrared not available: %s", esp_err_to_name(res));
    if (!radio_started) {
        snprintf(onboard_unavailable_reason, sizeof(onboard_unavailable_reason), "the radio did not start");
    } else if (res == ESP_ERR_NOT_SUPPORTED) {
        snprintf(onboard_unavailable_reason, sizeof(onboard_unavailable_reason), "not present on this device");
    } else {
        snprintf(onboard_unavailable_reason, sizeof(onboard_unavailable_reason),
                 "no answer from the radio (%s), its firmware may be outdated", esp_err_to_name(res));
    }
}

// Storage

static void mount_sd_card(void) {
    static ui_hint_t const hints[] = {{"Enter", "Retry"}, {"F1", "Return to launcher"}};
    while (true) {
        ui_busy("Mounting SD card...");
        esp_err_t res = bsp_storage_mount(BSP_STORAGE_TYPE_SDCARD, SD_MOUNT_POINT);
        if (res == ESP_OK) {
            return;
        }
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(res));

        char message[256];
        snprintf(message, sizeof(message),
                 "The SD card could not be mounted (%s).\n\nYour codes are stored in " DB_PATH
                 ". Insert a FAT formatted SD card and try again.",
                 esp_err_to_name(res));
        ui_alert("SD card error", message, hints, sizeof(hints) / sizeof(hints[0]), true);

        bool retry = false;
        while (!retry) {
            ui_input_t input = ui_wait_input();
            if (input.key == UI_KEY_ENTER) {
                retry = true;
            } else if (input.key == UI_KEY_F1 || input.key == UI_KEY_ESC) {
                exit_to_launcher();
            }
        }
    }
}

static void load_database(void) {
    size_t    skipped = 0;
    esp_err_t res     = code_db_load(&db, DB_PATH, &skipped);
    if (res == ESP_OK) {
        if (skipped > 0) {
            set_status(UI_STATUS_ERROR, "Ignored %u invalid or duplicate entries in " DB_PATH, (unsigned)skipped);
        }
        return;
    }

    if (res != ESP_ERR_INVALID_RESPONSE) {
        char message[200];
        snprintf(message, sizeof(message), "Failed to read " DB_PATH " (%s).", esp_err_to_name(res));
        fatal_error("SD card error", message);
    }

    // Move the damaged file aside instead of overwriting it with the next save, so the user can recover it
    remove(DB_BACKUP_PATH);
    if (rename(DB_PATH, DB_BACKUP_PATH) != 0 && rename(DB_PATH ".tmp", DB_BACKUP_PATH) != 0) {
        fatal_error("Damaged code database", DB_PATH " could not be read. Repair or remove the file using a computer.");
    }

    static ui_hint_t const hints[] = {{"Enter", "Continue"}, {"F1", "Return to launcher"}};
    ui_alert("Damaged code database",
             DB_PATH " could not be read and has been renamed to " DB_BACKUP_PATH
                     ".\n\nContinuing with an empty list of codes.",
             hints, sizeof(hints) / sizeof(hints[0]), true);
    while (true) {
        ui_input_t input = ui_wait_input();
        if (input.key == UI_KEY_ENTER) {
            return;
        }
        if (input.key == UI_KEY_F1 || input.key == UI_KEY_ESC) {
            exit_to_launcher();
        }
    }
}

// Writes the database to the SD card, tells the user when that fails
static bool store_database(void) {
    esp_err_t res = code_db_save(&db, DB_PATH);
    if (res == ESP_OK) {
        return true;
    }
    char message[200];
    snprintf(message, sizeof(message),
             "Your changes could not be written to " DB_PATH
             " (%s). Check that the SD card is inserted and not write protected.",
             esp_err_to_name(res));
    ui_message("Saving failed", message, true);
    return false;
}

// Shared helpers for the form screens

static float field_label_width(ui_field_t const* fields, size_t field_count) {
    float width = 0;
    for (size_t i = 0; i < field_count; i++) {
        float label_width = ui_text_width(fields[i].label);
        if (label_width > width) {
            width = label_width;
        }
    }
    return width + ui_padding() * 2;
}

static float draw_fields(ui_field_t const* fields, size_t field_count, size_t focus) {
    float label_width = field_label_width(fields, field_count);
    float y           = ui_content_top() + ui_padding();
    for (size_t i = 0; i < field_count; i++) {
        ui_draw_field(y, label_width, &fields[i], i == focus);
        y += ui_row_height() + ui_padding();
    }
    return y;
}

static void trim(char* text) {
    size_t start  = strspn(text, " ");
    size_t length = strlen(text + start);
    memmove(text, text + start, length + 1);
    while (length > 0 && text[length - 1] == ' ') {
        text[--length] = '\0';
    }
}

// Code editor, used for adding, editing and sending custom codes

// Draws the editor without sending it to the display, so that dialogs can be drawn on top
static void draw_editor(editor_mode_t mode, ui_field_t const* fields, size_t focus) {
    ui_hint_t hints[5];
    size_t    hint_count = 0;
    hints[hint_count++]  = (ui_hint_t){"Enter", "Send"};
    hints[hint_count++]  = (ui_hint_t){"F2", "Save"};
    if (addon_enabled) {
        hints[hint_count++] = (ui_hint_t){"F3", "Learn"};
    }
    hints[hint_count++] = (ui_hint_t){"Esc", "Back"};
    hints[hint_count++] = (ui_hint_t){"F1", "Exit"};

    char const* title = "Send custom code";
    if (mode == EDITOR_NEW) {
        title = "New code";
    } else if (mode == EDITOR_EDIT) {
        title = "Edit code";
    }
    char info[48];
    snprintf(info, sizeof(info), "Transmitter: %s", transmitter_name(transmitter));
    ui_draw_screen(title, info, hints, hint_count);

    float padding = ui_padding();
    float x       = padding * 2;
    float width   = ui_width() - padding * 4;
    float y       = draw_fields(fields, FIELD_COUNT, focus) + padding;

    uint16_t address = 0;
    uint16_t command = 0;
    char     preview[96];
    if (parse_nec_value(fields[FIELD_ADDRESS].value, &address) &&
        parse_nec_value(fields[FIELD_COMMAND].value, &command)) {
        snprintf(preview, sizeof(preview), "Transmits address 0x%04X and command 0x%04X", address, command);
    } else {
        snprintf(preview, sizeof(preview), "Enter an address and a command of 1 to 4 hexadecimal digits");
    }
    ui_draw_text(x, y, UI_COLOR_TEXT, preview);
    y += ui_line_height() * 1.5f;

    ui_draw_wrapped(x, y, width, UI_COLOR_TEXT_DIM,
                    "1 or 2 digits: standard NEC, the inverted byte is added (04 becomes FB04).\n"
                    "3 or 4 digits: the value is sent exactly as entered (extended NEC).");

    ui_draw_status(status_text, status_kind);
}

// Listen to the receiver of the add-on and put the received code in the address and command fields
static bool learn_code(editor_mode_t mode, ui_field_t* fields, size_t focus) {
    if (!addon_enabled) {
        set_status(UI_STATUS_INFO, "Enable the CATT infrared add-on in the settings to learn codes");
        return false;
    }
    esp_err_t res = catt_ir_receive_start();
    if (res != ESP_OK) {
        set_status(UI_STATUS_ERROR, "Failed to start the infrared receiver (%s)", esp_err_to_name(res));
        return false;
    }

    static ui_hint_t const hints[] = {{"Esc", "Cancel"}};
    char const*            message = "Point the remote control at the receiver of the add-on and press a button.";
    bool                   redraw  = true;
    uint16_t               address = 0;
    uint16_t               command = 0;
    while (true) {
        if (redraw) {
            draw_editor(mode, fields, focus);
            ui_show_dialog("Learn code", message, hints, sizeof(hints) / sizeof(hints[0]), false);
            redraw = false;
        }

        res = catt_ir_receive(&address, &command, pdMS_TO_TICKS(50));
        if (res == ESP_OK) {
            break;
        }
        if (res == ESP_ERR_INVALID_RESPONSE) {
            message =
                "The received signal is not an NEC code. Point the remote control at the receiver of the add-on and "
                "press a button.";
            redraw = true;
        } else if (res != ESP_ERR_TIMEOUT) {
            catt_ir_receive_stop();
            set_status(UI_STATUS_ERROR, "Failed to receive (%s)", esp_err_to_name(res));
            return false;
        }

        ui_input_t input;
        if (ui_poll_input(&input, 0) && input.key == UI_KEY_ESC) {
            catt_ir_receive_stop();
            clear_status();
            return false;
        }
    }
    catt_ir_receive_stop();

    // Received codes are shown in full, so they are sent exactly as received
    snprintf(fields[FIELD_ADDRESS].value, fields[FIELD_ADDRESS].max_length + 1, "%04X", address);
    snprintf(fields[FIELD_COMMAND].value, fields[FIELD_COMMAND].max_length + 1, "%04X", command);
    set_status(UI_STATUS_SUCCESS, "Learned address 0x%04X, command 0x%04X", address, command);
    return true;
}

static bool save_code(editor_mode_t mode, size_t index, ui_field_t* fields, size_t* focus) {
    code_db_entry_t entry = {0};
    char*           name  = fields[FIELD_NAME].value;
    trim(name);
    if (name[0] == '\0') {
        set_status(UI_STATUS_ERROR, "Enter a name to save this code");
        *focus = FIELD_NAME;
        return false;
    }
    if (!parse_nec_value(fields[FIELD_ADDRESS].value, &entry.address)) {
        set_status(UI_STATUS_ERROR, "Enter an address of 1 to 4 hexadecimal digits");
        *focus = FIELD_ADDRESS;
        return false;
    }
    if (!parse_nec_value(fields[FIELD_COMMAND].value, &entry.command)) {
        set_status(UI_STATUS_ERROR, "Enter a command of 1 to 4 hexadecimal digits");
        *focus = FIELD_COMMAND;
        return false;
    }
    int existing = code_db_find(&db, name);
    if (existing >= 0 && !(mode == EDITOR_EDIT && (size_t)existing == index)) {
        set_status(UI_STATUS_ERROR, "A code named \"%s\" already exists", db.entries[existing].name);
        *focus = FIELD_NAME;
        return false;
    }
    snprintf(entry.name, sizeof(entry.name), "%s", name);

    // The list in memory is restored when writing to the SD card fails, so it always matches the file
    if (mode == EDITOR_EDIT) {
        code_db_entry_t previous = db.entries[index];
        db.entries[index]        = entry;
        if (!store_database()) {
            db.entries[index] = previous;
            return false;
        }
        selected = index;
    } else {
        esp_err_t res = code_db_insert(&db, db.count, &entry);
        if (res != ESP_OK) {
            set_status(UI_STATUS_ERROR, "Failed to add code (%s)", esp_err_to_name(res));
            return false;
        }
        if (!store_database()) {
            code_db_remove(&db, db.count - 1);
            return false;
        }
        selected = db.count - 1;
    }
    set_status(UI_STATUS_SUCCESS, "Saved \"%s\"", entry.name);
    return true;
}

static void run_editor(editor_mode_t mode, size_t index) {
    char name[CODE_DB_NAME_MAX_LENGTH + 1] = "";
    char address[sizeof(custom_address)]   = "";
    char command[sizeof(custom_command)]   = "";
    if (mode == EDITOR_EDIT) {
        snprintf(name, sizeof(name), "%s", db.entries[index].name);
        snprintf(address, sizeof(address), "%04X", db.entries[index].address);
        snprintf(command, sizeof(command), "%04X", db.entries[index].command);
    } else if (mode == EDITOR_CUSTOM) {
        memcpy(address, custom_address, sizeof(address));
        memcpy(command, custom_command, sizeof(command));
    }

    ui_field_t fields[FIELD_COUNT] = {
        [FIELD_NAME]    = {"Name", UI_FIELD_TEXT, name, CODE_DB_NAME_MAX_LENGTH},
        [FIELD_ADDRESS] = {"Address", UI_FIELD_HEX, address, sizeof(address) - 1},
        [FIELD_COMMAND] = {"Command", UI_FIELD_HEX, command, sizeof(command) - 1},
    };
    size_t focus    = (mode == EDITOR_CUSTOM) ? FIELD_ADDRESS : FIELD_NAME;
    bool   modified = false;
    clear_status();

    while (true) {
        draw_editor(mode, fields, focus);
        ui_flush();
        ui_input_t input = ui_wait_input();
        switch (input.key) {
            case UI_KEY_UP:
                focus = (focus + FIELD_COUNT - 1) % FIELD_COUNT;
                break;
            case UI_KEY_DOWN:
            case UI_KEY_TAB:
                focus = (focus + 1) % FIELD_COUNT;
                break;
            case UI_KEY_ENTER: {
                uint16_t address_value = 0;
                uint16_t command_value = 0;
                if (!parse_nec_value(address, &address_value)) {
                    set_status(UI_STATUS_ERROR, "Enter an address of 1 to 4 hexadecimal digits");
                    focus = FIELD_ADDRESS;
                } else if (!parse_nec_value(command, &command_value)) {
                    set_status(UI_STATUS_ERROR, "Enter a command of 1 to 4 hexadecimal digits");
                    focus = FIELD_COMMAND;
                } else {
                    send_code(name, address_value, command_value);
                }
                break;
            }
            case UI_KEY_F2:
                if (save_code(mode, index, fields, &focus)) {
                    return;
                }
                break;
            case UI_KEY_F3:
                if (learn_code(mode, fields, focus)) {
                    modified = true;
                }
                break;
            case UI_KEY_ESC:
                if (mode == EDITOR_CUSTOM || !modified ||
                    ui_confirm("Discard changes", "Leave without saving your changes?", "Discard")) {
                    return;
                }
                break;
            case UI_KEY_F1:
                confirm_exit();
                break;
            default:
                if (ui_field_handle_input(&fields[focus], &input)) {
                    modified = true;
                }
                break;
        }
        if (mode == EDITOR_CUSTOM) {
            memcpy(custom_address, address, sizeof(custom_address));
            memcpy(custom_command, command, sizeof(custom_command));
        }
    }
}

// Infrared settings

// Edited copy of the settings, applied when the user confirms
typedef struct {
    bool          addon_enabled;
    transmitter_t transmitter;
    char          addon_text[16];
    char          transmitter_text[32];
    char          frequency[12];
    char          duty[4];
    ui_field_t    fields[SETTING_COUNT];
} settings_form_t;

static void settings_form_update(settings_form_t* form) {
    snprintf(form->addon_text, sizeof(form->addon_text), "%s", form->addon_enabled ? "Enabled" : "Disabled");
    snprintf(form->transmitter_text, sizeof(form->transmitter_text), "%s",
             form->transmitter == TRANSMITTER_NONE ? "None available" : transmitter_name(form->transmitter));
}

static void settings_form_toggle_addon(settings_form_t* form) {
    form->addon_enabled = !form->addon_enabled;
    if (!form->addon_enabled && form->transmitter == TRANSMITTER_ADDON) {
        form->transmitter = onboard_available ? TRANSMITTER_ONBOARD : TRANSMITTER_NONE;
    } else if (form->addon_enabled && form->transmitter == TRANSMITTER_NONE) {
        form->transmitter = TRANSMITTER_ADDON;
    }
}

static void settings_form_toggle_transmitter(settings_form_t* form) {
    if (onboard_available && form->addon_enabled) {
        form->transmitter = form->transmitter == TRANSMITTER_ONBOARD ? TRANSMITTER_ADDON : TRANSMITTER_ONBOARD;
    } else if (onboard_available) {
        set_status(UI_STATUS_INFO, "Enable the CATT infrared add-on to use its transmitter");
    } else {
        set_status(UI_STATUS_INFO, "The on-board transmitter is not available");
    }
}

static void draw_settings(settings_form_t const* form, size_t focus) {
    static ui_hint_t const hints[] = {
        {"Enter", "Apply"}, {"<  >", "Change"}, {"F3", "Defaults"}, {"Esc", "Back"}, {"F1", "Exit"},
    };
    ui_draw_screen("Settings", NULL, hints, sizeof(hints) / sizeof(hints[0]));

    float padding = ui_padding();
    float y       = draw_fields(form->fields, SETTING_COUNT, focus) + padding;

    char onboard[160];
    if (onboard_available) {
        snprintf(onboard, sizeof(onboard), "On-board transmitter: available.");
    } else {
        snprintf(onboard, sizeof(onboard), "On-board transmitter: not available, %s.", onboard_unavailable_reason);
    }
    char help[512];
    snprintf(help, sizeof(help),
             "%s\nThe CATT infrared add-on adds a transmitter and a receiver for learning codes, this setting is not "
             "saved.\nNEC remotes use a carrier of 38000 Hz with a duty cycle of 25 to 50%%. Frequency: %d to %d Hz, "
             "duty cycle: %d to %d%%.",
             onboard, CODE_DB_MIN_FREQUENCY_HZ, CODE_DB_MAX_FREQUENCY_HZ, CODE_DB_MIN_DUTY_PERCENT,
             CODE_DB_MAX_DUTY_PERCENT);
    ui_draw_wrapped(padding * 2, y, ui_width() - padding * 4, UI_COLOR_TEXT_DIM, help);

    ui_draw_status(status_text, status_kind);
    ui_flush();
}

static bool settings_form_apply(settings_form_t const* form, size_t* focus) {
    unsigned long frequency = strtoul(form->frequency, NULL, 10);
    unsigned long duty      = strtoul(form->duty, NULL, 10);
    if (frequency < CODE_DB_MIN_FREQUENCY_HZ || frequency > CODE_DB_MAX_FREQUENCY_HZ) {
        set_status(UI_STATUS_ERROR, "The carrier frequency must be between %d and %d Hz", CODE_DB_MIN_FREQUENCY_HZ,
                   CODE_DB_MAX_FREQUENCY_HZ);
        *focus = SETTING_FREQUENCY;
        return false;
    }
    if (duty < CODE_DB_MIN_DUTY_PERCENT || duty > CODE_DB_MAX_DUTY_PERCENT) {
        set_status(UI_STATUS_ERROR, "The duty cycle must be between %d and %d%%", CODE_DB_MIN_DUTY_PERCENT,
                   CODE_DB_MAX_DUTY_PERCENT);
        *focus = SETTING_DUTY;
        return false;
    }

    if (form->addon_enabled && !addon_enabled) {
        esp_err_t res = catt_ir_init();
        if (res == ESP_ERR_NOT_SUPPORTED) {
            set_status(UI_STATUS_ERROR, "This device has no CATT port");
            *focus = SETTING_ADDON;
            return false;
        } else if (res != ESP_OK) {
            set_status(UI_STATUS_ERROR, "Failed to enable the add-on (%s)", esp_err_to_name(res));
            *focus = SETTING_ADDON;
            return false;
        }
        addon_enabled = true;
    } else if (!form->addon_enabled && addon_enabled) {
        catt_ir_deinit();
        addon_enabled = false;
    }
    transmitter = form->transmitter;

    // The carrier is also applied when it did not change, a newly enabled add-on starts with the default carrier
    code_db_settings_t settings = {.frequency_hz = frequency, .duty_percent = duty};
    esp_err_t          res      = apply_carrier(&settings);
    if (res != ESP_OK) {
        set_status(UI_STATUS_ERROR, "The carrier settings were not accepted (%s)", esp_err_to_name(res));
        *focus = SETTING_FREQUENCY;
        return false;
    }

    bool carrier_changed =
        settings.frequency_hz != db.settings.frequency_hz || settings.duty_percent != db.settings.duty_percent;
    db.settings = settings;
    if (carrier_changed && !store_database()) {
        set_status(UI_STATUS_ERROR, "Settings applied, but the carrier settings were not saved");
    } else {
        set_status(UI_STATUS_SUCCESS, "Settings applied");
    }
    return true;
}

static void run_settings(void) {
    settings_form_t form = {
        .addon_enabled = addon_enabled,
        .transmitter   = transmitter,
    };
    snprintf(form.frequency, sizeof(form.frequency), "%" PRIu32, db.settings.frequency_hz);
    snprintf(form.duty, sizeof(form.duty), "%u", db.settings.duty_percent);
    form.fields[SETTING_ADDON]       = (ui_field_t){"CATT infrared add-on", UI_FIELD_CHOICE, form.addon_text, 0};
    form.fields[SETTING_TRANSMITTER] = (ui_field_t){"Transmitter", UI_FIELD_CHOICE, form.transmitter_text, 0};
    form.fields[SETTING_FREQUENCY]   = (ui_field_t){"Carrier frequency (Hz)", UI_FIELD_DECIMAL, form.frequency, 6};
    form.fields[SETTING_DUTY]        = (ui_field_t){"Duty cycle (%)", UI_FIELD_DECIMAL, form.duty, 2};
    size_t focus                     = SETTING_ADDON;
    clear_status();

    while (true) {
        settings_form_update(&form);
        draw_settings(&form, focus);
        ui_input_t input  = ui_wait_input();
        bool       change = input.key == UI_KEY_LEFT || input.key == UI_KEY_RIGHT ||
                            (input.key == UI_KEY_CHARACTER && input.character == ' ');
        switch (input.key) {
            case UI_KEY_UP:
                focus = (focus + SETTING_COUNT - 1) % SETTING_COUNT;
                break;
            case UI_KEY_DOWN:
            case UI_KEY_TAB:
                focus = (focus + 1) % SETTING_COUNT;
                break;
            case UI_KEY_ENTER:
            case UI_KEY_F2:
                if (settings_form_apply(&form, &focus)) {
                    return;
                }
                break;
            case UI_KEY_F3:
                snprintf(form.frequency, sizeof(form.frequency), "%d", CODE_DB_DEFAULT_FREQUENCY_HZ);
                snprintf(form.duty, sizeof(form.duty), "%d", CODE_DB_DEFAULT_DUTY_PERCENT);
                set_status(UI_STATUS_INFO, "Default carrier settings restored, press Enter to apply them");
                break;
            case UI_KEY_ESC:
                clear_status();
                return;
            case UI_KEY_F1:
                confirm_exit();
                break;
            default:
                if (change && focus == SETTING_ADDON) {
                    settings_form_toggle_addon(&form);
                } else if (change && focus == SETTING_TRANSMITTER) {
                    settings_form_toggle_transmitter(&form);
                } else {
                    ui_field_handle_input(&form.fields[focus], &input);
                }
                break;
        }
    }
}

// List of saved codes

static size_t list_visible_rows(void) {
    size_t rows = (size_t)((ui_content_bottom() - ui_content_top()) / ui_row_height());
    return rows > 0 ? rows : 1;
}

static void draw_main_screen(void) {
    static ui_hint_t const hints[] = {
        {"Enter", "Send"}, {"F1", "Exit"},   {"F2", "New"},      {"F3", "Edit"},
        {"F4", "Delete"},  {"F5", "Custom"}, {"F6", "Settings"},
    };
    char info[64];
    if (transmitter == TRANSMITTER_NONE) {
        snprintf(info, sizeof(info), "No transmitter available");
    } else {
        snprintf(info, sizeof(info), "%s, %" PRIu32 ".%" PRIu32 " kHz, %u%% duty cycle", transmitter_name(transmitter),
                 db.settings.frequency_hz / 1000, (db.settings.frequency_hz % 1000) / 100, db.settings.duty_percent);
    }
    ui_draw_screen(APP_TITLE, info, hints, sizeof(hints) / sizeof(hints[0]));

    float  padding = ui_padding();
    float  top     = ui_content_top();
    size_t visible = list_visible_rows();
    if (db.count == 0) {
        ui_draw_wrapped(padding * 2, top + padding, ui_width() - padding * 4, UI_COLOR_TEXT_DIM,
                        "No saved codes yet.\n\nPress F2 to add a code, or F5 to send a custom address and command.");
    } else {
        // Keep the selected row in view
        if (selected >= db.count) {
            selected = db.count - 1;
        }
        if (selected < scroll) {
            scroll = selected;
        }
        if (selected >= scroll + visible) {
            scroll = selected - visible + 1;
        }
        if (scroll + visible > db.count) {
            scroll = db.count > visible ? db.count - visible : 0;
        }

        for (size_t i = scroll; i < db.count && i < scroll + visible; i++) {
            char detail[24];
            snprintf(detail, sizeof(detail), "A:%04X C:%04X", db.entries[i].address, db.entries[i].command);
            ui_draw_list_row(top + (i - scroll) * ui_row_height(), i == selected, db.entries[i].name, detail);
        }
        ui_draw_scrollbar(scroll, visible, db.count);
    }

    ui_draw_status(status_text, status_kind);
    ui_flush();
}

static void delete_selected(void) {
    code_db_entry_t entry = db.entries[selected];
    char            message[96];
    snprintf(message, sizeof(message), "Delete \"%s\"? This can not be undone.", entry.name);
    if (!ui_confirm("Delete code", message, "Delete")) {
        return;
    }
    code_db_remove(&db, selected);
    if (!store_database()) {
        code_db_insert(&db, selected, &entry);  // Keep the list in sync with the file
        return;
    }
    if (selected >= db.count && selected > 0) {
        selected--;
    }
    set_status(UI_STATUS_SUCCESS, "Deleted \"%s\"", entry.name);
}

static void __attribute__((noreturn)) run_main_screen(void) {
    while (true) {
        draw_main_screen();
        ui_input_t input   = ui_wait_input();
        size_t     visible = list_visible_rows();
        bool       empty   = db.count == 0;
        switch (input.key) {
            case UI_KEY_UP:
                if (!empty) {
                    selected = selected > 0 ? selected - 1 : db.count - 1;
                }
                break;
            case UI_KEY_DOWN:
                if (!empty) {
                    selected = selected + 1 < db.count ? selected + 1 : 0;
                }
                break;
            case UI_KEY_PGUP:
                selected = selected > visible ? selected - visible : 0;
                break;
            case UI_KEY_PGDN:
                if (!empty) {
                    selected = selected + visible < db.count ? selected + visible : db.count - 1;
                }
                break;
            case UI_KEY_HOME:
                selected = 0;
                break;
            case UI_KEY_END:
                if (!empty) {
                    selected = db.count - 1;
                }
                break;
            case UI_KEY_ENTER:
                if (!empty) {
                    send_code(db.entries[selected].name, db.entries[selected].address, db.entries[selected].command);
                }
                break;
            case UI_KEY_F1:
                confirm_exit();
                break;
            case UI_KEY_F2:
                run_editor(EDITOR_NEW, 0);
                break;
            case UI_KEY_F3:
                if (empty) {
                    set_status(UI_STATUS_INFO, "There are no saved codes to edit");
                } else {
                    run_editor(EDITOR_EDIT, selected);
                }
                break;
            case UI_KEY_F4:
                if (empty) {
                    set_status(UI_STATUS_INFO, "There are no saved codes to delete");
                } else {
                    delete_selected();
                }
                break;
            case UI_KEY_F5:
                run_editor(EDITOR_CUSTOM, 0);
                break;
            case UI_KEY_F6:
                run_settings();
                break;
            default:
                break;
        }
    }
}

void app_run(bool radio_started) {
    code_db_init(&db);
    check_onboard_support(radio_started);
    mount_sd_card();
    load_database();

    // The radio starts with default settings, apply the stored ones
    esp_err_t res = apply_carrier(&db.settings);
    if (res != ESP_OK) {
        set_status(UI_STATUS_ERROR, "Failed to apply the carrier settings (%s)", esp_err_to_name(res));
    } else if (!onboard_available && status_kind == UI_STATUS_NONE) {
        set_status(UI_STATUS_INFO, "On-board infrared not available: %s", onboard_unavailable_reason);
    }

    run_main_screen();
}
