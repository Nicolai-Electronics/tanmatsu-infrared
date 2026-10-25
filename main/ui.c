#include "ui.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "bsp/display.h"
#include "bsp/input.h"
#include "esp_log.h"
#include "pax_fonts.h"
#include "pax_text.h"

static char const TAG[] = "ui";

static pax_buf_t*        fb          = NULL;
static QueueHandle_t     input_queue = NULL;
static pax_font_t const* font        = NULL;
static pax_font_t const* mono_font   = NULL;
static float             font_size;
static float             mono_size;
static float             screen_width;
static float             screen_height;
static float             padding;
static float             row_height;
static float             line_height;
static float             header_height;
static float             footer_height;
static float             keycap_height;
static float             radius;

void ui_init(pax_buf_t* buffer, QueueHandle_t queue) {
    fb            = buffer;
    input_queue   = queue;
    screen_width  = pax_buf_get_width(fb);
    screen_height = pax_buf_get_height(fb);

    // Tanmatsu sized screens get the anti-aliased Saira font, small screens the 7x9 pixel Sky font
    bool large = screen_width >= 480 && screen_height >= 320;
    if (large) {
        font      = pax_font_saira_regular;
        font_size = 18;
        mono_font = pax_font_sky_mono;
        mono_size = 18;  // Twice the native size of the font, keeps pixels sharp
        padding   = 10;
        radius    = 6;
    } else {
        font      = pax_font_sky;
        font_size = 9;
        mono_font = pax_font_sky_mono;
        mono_size = 9;
        padding   = 3;
        radius    = 2;
    }
    row_height    = font_size + (large ? 16 : 4);
    line_height   = font_size + (large ? 6 : 2);
    header_height = font_size + 2 * padding;
    footer_height = font_size + 2 * padding;
    keycap_height = font_size + (large ? 8 : 2);
}

void ui_flush(void) {
    esp_err_t res =
        bsp_display_blit(0, 0, pax_buf_get_width_raw(fb), pax_buf_get_height_raw(fb), pax_buf_get_pixels(fb));
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to blit to display: %d", res);
    }
}

static bool translate_event(bsp_input_event_t const* event, ui_input_t* out) {
    *out = (ui_input_t){0};
    switch (event->type) {
        case INPUT_EVENT_TYPE_NAVIGATION:
            if (!event->args_navigation.state) {
                return false;  // Ignore key releases
            }
            switch (event->args_navigation.key) {
                case BSP_INPUT_NAVIGATION_KEY_UP:
                    out->key = UI_KEY_UP;
                    break;
                case BSP_INPUT_NAVIGATION_KEY_DOWN:
                    out->key = UI_KEY_DOWN;
                    break;
                case BSP_INPUT_NAVIGATION_KEY_LEFT:
                    out->key = UI_KEY_LEFT;
                    break;
                case BSP_INPUT_NAVIGATION_KEY_RIGHT:
                    out->key = UI_KEY_RIGHT;
                    break;
                case BSP_INPUT_NAVIGATION_KEY_HOME:
                    out->key = UI_KEY_HOME;
                    break;
                case BSP_INPUT_NAVIGATION_KEY_END:
                    out->key = UI_KEY_END;
                    break;
                case BSP_INPUT_NAVIGATION_KEY_PGUP:
                    out->key = UI_KEY_PGUP;
                    break;
                case BSP_INPUT_NAVIGATION_KEY_PGDN:
                    out->key = UI_KEY_PGDN;
                    break;
                case BSP_INPUT_NAVIGATION_KEY_RETURN:
                case BSP_INPUT_NAVIGATION_KEY_GAMEPAD_A:
                case BSP_INPUT_NAVIGATION_KEY_JOYSTICK_PRESS:
                    out->key = UI_KEY_ENTER;
                    break;
                case BSP_INPUT_NAVIGATION_KEY_ESC:
                case BSP_INPUT_NAVIGATION_KEY_GAMEPAD_B:
                    out->key = UI_KEY_ESC;
                    break;
                case BSP_INPUT_NAVIGATION_KEY_TAB:
                    out->key = UI_KEY_TAB;
                    break;
                case BSP_INPUT_NAVIGATION_KEY_BACKSPACE:
                    out->key = UI_KEY_BACKSPACE;
                    break;
                case BSP_INPUT_NAVIGATION_KEY_F1:
                    out->key = UI_KEY_F1;
                    break;
                case BSP_INPUT_NAVIGATION_KEY_F2:
                    out->key = UI_KEY_F2;
                    break;
                case BSP_INPUT_NAVIGATION_KEY_F3:
                    out->key = UI_KEY_F3;
                    break;
                case BSP_INPUT_NAVIGATION_KEY_F4:
                    out->key = UI_KEY_F4;
                    break;
                case BSP_INPUT_NAVIGATION_KEY_F5:
                    out->key = UI_KEY_F5;
                    break;
                case BSP_INPUT_NAVIGATION_KEY_F6:
                    out->key = UI_KEY_F6;
                    break;
                default:
                    return false;
            }
            return true;
        case INPUT_EVENT_TYPE_KEYBOARD: {
            // Backspace, tab and enter also generate navigation events, those are handled above
            char character = event->args_keyboard.ascii;
            if (character < 0x20 || character > 0x7E) {
                return false;
            }
            out->key       = UI_KEY_CHARACTER;
            out->character = character;
            return true;
        }
        default:
            return false;
    }
}

ui_input_t ui_wait_input(void) {
    ui_input_t input;
    while (!ui_poll_input(&input, portMAX_DELAY)) {
    }
    return input;
}

bool ui_poll_input(ui_input_t* out_input, TickType_t timeout) {
    bsp_input_event_t event;
    while (xQueueReceive(input_queue, &event, timeout) == pdTRUE) {
        if (translate_event(&event, out_input)) {
            return true;
        }
    }
    return false;
}

// Layout

float ui_width(void) {
    return screen_width;
}

float ui_padding(void) {
    return padding;
}

float ui_row_height(void) {
    return row_height;
}

float ui_line_height(void) {
    return line_height;
}

float ui_content_top(void) {
    return header_height + padding / 2;
}

float ui_content_bottom(void) {
    // The status line sits between the content and the footer
    return screen_height - footer_height - row_height;
}

float ui_text_width(char const* text) {
    return pax_text_size(font, font_size, text).x;
}

// Copy text to out, shortening it with an ellipsis when it is wider than max_width
static void fit_text(char* out, size_t out_size, pax_font_t const* text_font, float text_size, char const* text,
                     float max_width) {
    size_t full_length = strlen(text);
    size_t length      = full_length;
    while (true) {
        snprintf(out, out_size, "%.*s%s", (int)length, text, length < full_length ? "..." : "");
        if (length == 0 || pax_text_size(text_font, text_size, out).x <= max_width) {
            return;
        }
        length--;
    }
}

static void draw_hints(float x, float y, ui_hint_t const* hints, size_t hint_count, float max_x) {
    float text_offset = (keycap_height - font_size) / 2;
    for (size_t i = 0; i < hint_count; i++) {
        float key_width   = pax_text_size(font, font_size, hints[i].key).x + padding;
        float label_width = pax_text_size(font, font_size, hints[i].label).x;
        if (x + key_width + padding / 2 + label_width > max_x) {
            break;  // Out of space
        }
        pax_draw_round_rect(fb, UI_COLOR_ACCENT, x, y, key_width, keycap_height, radius / 2);
        pax_center_text(fb, UI_COLOR_KEYCAP_TEXT, font, font_size, x + key_width / 2, y + text_offset, hints[i].key);
        x += key_width + padding / 2;
        pax_draw_text(fb, UI_COLOR_TEXT, font, font_size, x, y + text_offset, hints[i].label);
        x += label_width + padding * 1.5f;
    }
}

void ui_draw_screen(char const* title, char const* info, ui_hint_t const* hints, size_t hint_count) {
    pax_background(fb, UI_COLOR_BACKGROUND);

    pax_simple_rect(fb, UI_COLOR_PANEL, 0, 0, screen_width, header_height);
    pax_simple_rect(fb, UI_COLOR_ACCENT, 0, header_height - 2, screen_width, 2);
    pax_draw_text(fb, UI_COLOR_TEXT, font, font_size, padding, padding, title);
    if (info != NULL) {
        pax_right_text(fb, UI_COLOR_TEXT_DIM, font, font_size, screen_width - padding, padding, info);
    }

    float footer_y = screen_height - footer_height;
    pax_simple_rect(fb, UI_COLOR_PANEL, 0, footer_y, screen_width, footer_height);
    draw_hints(padding, footer_y + (footer_height - keycap_height) / 2, hints, hint_count, screen_width - padding);
}

void ui_draw_status(char const* text, ui_status_t status) {
    if (status == UI_STATUS_NONE || text == NULL || text[0] == '\0') {
        return;
    }
    pax_col_t color = UI_COLOR_TEXT_DIM;
    if (status == UI_STATUS_SUCCESS) {
        color = UI_COLOR_SUCCESS;
    } else if (status == UI_STATUS_ERROR) {
        color = UI_COLOR_ERROR;
    }
    char fitted[160];
    fit_text(fitted, sizeof(fitted), font, font_size, text, screen_width - 4 * padding);
    pax_draw_text(fb, color, font, font_size, padding * 2, ui_content_bottom() + (row_height - font_size) / 2, fitted);
}

void ui_draw_text(float x, float y, pax_col_t color, char const* text) {
    pax_draw_text(fb, color, font, font_size, x, y, text);
}

// Word wrap text, when draw is false the text is only measured
static float wrap_text(float x, float y, float max_width, pax_col_t color, char const* text, bool draw) {
    char        line[160] = "";
    char        candidate[sizeof(line)];
    float       height     = 0;
    char const* position   = text;
    bool        line_ready = false;

    while (*position != '\0') {
        size_t word_length = strcspn(position, " \n");
        int    length =
            snprintf(candidate, sizeof(candidate), "%s%s%.*s", line, line[0] ? " " : "", (int)word_length, position);
        bool fits = length < (int)sizeof(candidate) && pax_text_size(font, font_size, candidate).x <= max_width;
        if (line[0] != '\0' && !fits) {
            // The word does not fit anymore, emit the current line and start a new one with the word
            if (draw) {
                pax_draw_text(fb, color, font, font_size, x, y + height, line);
            }
            height += line_height;
            snprintf(line, sizeof(line), "%.*s", (int)word_length, position);
        } else {
            memcpy(line, candidate, sizeof(line));
        }
        line_ready  = true;
        position   += word_length;

        if (*position == '\n') {
            if (draw) {
                pax_draw_text(fb, color, font, font_size, x, y + height, line);
            }
            height     += line_height;
            line[0]     = '\0';
            line_ready  = false;
            position++;
        }
        while (*position == ' ') {
            position++;
        }
    }
    if (line_ready && line[0] != '\0') {
        if (draw) {
            pax_draw_text(fb, color, font, font_size, x, y + height, line);
        }
        height += line_height;
    }
    return height;
}

float ui_draw_wrapped(float x, float y, float max_width, pax_col_t color, char const* text) {
    return wrap_text(x, y, max_width, color, text, true);
}

void ui_draw_list_row(float y, bool selected, char const* text, char const* detail) {
    float x     = padding;
    float width = screen_width - 2 * padding;
    if (selected) {
        pax_draw_round_rect(fb, UI_COLOR_SELECTION, x, y + 1, width, row_height - 2, radius);
        pax_simple_rect(fb, UI_COLOR_ACCENT, x, y + row_height / 4, padding / 2 > 2 ? padding / 2 : 2, row_height / 2);
    }

    float right      = x + width - padding;
    float text_right = right;
    if (detail != NULL) {
        pax_right_text(fb, selected ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM, mono_font, mono_size, right,
                       y + (row_height - mono_size) / 2, detail);
        text_right -= pax_text_size(mono_font, mono_size, detail).x + 2 * padding;
    }

    float text_x = x + padding * 1.5f;
    char  fitted[96];
    fit_text(fitted, sizeof(fitted), font, font_size, text, text_right - text_x);
    pax_draw_text(fb, UI_COLOR_TEXT, font, font_size, text_x, y + (row_height - font_size) / 2, fitted);
}

void ui_draw_scrollbar(size_t first_visible, size_t visible_count, size_t total_count) {
    if (total_count <= visible_count || visible_count == 0) {
        return;
    }
    float bar_width    = padding > 4 ? 4 : 1;
    float x            = screen_width - padding / 2 - bar_width / 2;
    float top          = ui_content_top();
    float track_height = visible_count * row_height;
    float thumb_height = track_height * visible_count / total_count;
    if (thumb_height < bar_width * 4) {
        thumb_height = bar_width * 4;
    }
    float thumb_y = top + (track_height - thumb_height) * first_visible / (total_count - visible_count);
    pax_simple_rect(fb, UI_COLOR_PANEL, x, top, bar_width, track_height);
    pax_simple_rect(fb, UI_COLOR_TEXT_DIM, x, thumb_y, bar_width, thumb_height);
}

void ui_draw_field(float y, float label_width, ui_field_t const* field, bool focused) {
    float x = padding * 2;
    pax_draw_text(fb, focused ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM, font, font_size, x, y + (row_height - font_size) / 2,
                  field->label);

    float box_x     = x + label_width;
    float box_width = screen_width - box_x - padding * 2;
    pax_draw_round_rect(fb, UI_COLOR_PANEL, box_x, y, box_width, row_height, radius);
    pax_outline_round_rect(fb, focused ? UI_COLOR_ACCENT : UI_COLOR_TEXT_DIM, box_x, y, box_width, row_height, radius);
    if (focused) {
        pax_outline_round_rect(fb, UI_COLOR_ACCENT, box_x + 1, y + 1, box_width - 2, row_height - 2, radius);
    }

    if (field->type == UI_FIELD_CHOICE) {
        // The value is changed with the left and right keys, which the arrows hint at
        pax_col_t arrow_color  = focused ? UI_COLOR_ACCENT : UI_COLOR_TEXT_DIM;
        float     text_y       = y + (row_height - font_size) / 2;
        float     text_x       = box_x + padding;
        text_x                += pax_draw_text(fb, arrow_color, font, font_size, text_x, text_y, "<").x + padding;
        pax_draw_text(fb, UI_COLOR_TEXT, font, font_size, text_x, text_y, field->value);
        pax_right_text(fb, arrow_color, font, font_size, box_x + box_width - padding, text_y, ">");
        return;
    }

    // Numeric values use the monospace font so that digits line up
    bool              numeric    = field->type != UI_FIELD_TEXT;
    pax_font_t const* value_font = numeric ? mono_font : font;
    float             value_size = numeric ? mono_size : font_size;
    float             text_x     = box_x + padding;
    float             text_y     = y + (row_height - value_size) / 2;
    if (field->type == UI_FIELD_HEX) {
        text_x += pax_draw_text(fb, UI_COLOR_TEXT_DIM, value_font, value_size, text_x, text_y, "0x").x;
    }
    text_x += pax_draw_text(fb, UI_COLOR_TEXT, value_font, value_size, text_x, text_y, field->value).x;
    if (focused) {
        pax_simple_rect(fb, UI_COLOR_ACCENT, text_x + 1, text_y, padding > 4 ? 2 : 1, value_size);
    }
}

bool ui_field_handle_input(ui_field_t* field, ui_input_t const* input) {
    if (field->type == UI_FIELD_CHOICE) {
        return false;  // Choices are handled by the owner of the field
    }
    size_t length = strlen(field->value);
    if (input->key == UI_KEY_BACKSPACE) {
        if (length == 0) {
            return false;
        }
        field->value[length - 1] = '\0';
        return true;
    }
    if (input->key != UI_KEY_CHARACTER || length >= field->max_length) {
        return false;
    }

    char character = input->character;
    if (field->type == UI_FIELD_HEX) {
        if (!isxdigit((unsigned char)character)) {
            return false;
        }
        character = (char)toupper((unsigned char)character);
    } else if (field->type == UI_FIELD_DECIMAL && !isdigit((unsigned char)character)) {
        return false;
    }
    field->value[length]     = character;
    field->value[length + 1] = '\0';
    return true;
}

// Dialogs

static void draw_dialog(char const* title, char const* message, ui_hint_t const* hints, size_t hint_count,
                        pax_col_t accent) {
    float width = screen_width - 4 * padding;
    if (width > font_size * 30) {
        width = font_size * 30;
    }
    float inner_width    = width - 2 * padding;
    float message_height = wrap_text(0, 0, inner_width, 0, message, false);
    float height         = padding + font_size + padding + message_height + padding + keycap_height + padding;
    float x              = (screen_width - width) / 2;
    float y              = (screen_height - height) / 2;

    pax_draw_round_rect(fb, UI_COLOR_PANEL, x, y, width, height, radius);
    pax_outline_round_rect(fb, accent, x, y, width, height, radius);
    pax_draw_text(fb, accent, font, font_size, x + padding, y + padding, title);
    wrap_text(x + padding, y + 2 * padding + font_size, inner_width, UI_COLOR_TEXT, message, true);
    draw_hints(x + padding, y + height - padding - keycap_height, hints, hint_count, x + width - padding);
}

static void dim_screen(void) {
#if !defined(CONFIG_BSP_TARGET_KAMI)
    pax_simple_rect(fb, 0xA0000000, 0, 0, screen_width, screen_height);
#endif
}

bool ui_confirm(char const* title, char const* message, char const* confirm_label) {
    ui_hint_t const hints[] = {{"Enter", confirm_label}, {"Esc", "Cancel"}};
    dim_screen();
    draw_dialog(title, message, hints, sizeof(hints) / sizeof(hints[0]), UI_COLOR_ACCENT);
    ui_flush();
    while (true) {
        ui_input_t input = ui_wait_input();
        char       c     = (char)tolower((unsigned char)input.character);
        if (input.key == UI_KEY_ENTER || (input.key == UI_KEY_CHARACTER && c == 'y')) {
            return true;
        }
        if (input.key == UI_KEY_ESC || (input.key == UI_KEY_CHARACTER && c == 'n')) {
            return false;
        }
    }
}

void ui_message(char const* title, char const* message, bool is_error) {
    ui_hint_t const hints[] = {{"Enter", "OK"}};
    dim_screen();
    draw_dialog(title, message, hints, 1, is_error ? UI_COLOR_ERROR : UI_COLOR_ACCENT);
    ui_flush();
    while (true) {
        ui_input_t input = ui_wait_input();
        if (input.key == UI_KEY_ENTER || input.key == UI_KEY_ESC) {
            return;
        }
    }
}

void ui_show_dialog(char const* title, char const* message, ui_hint_t const* hints, size_t hint_count, bool is_error) {
    dim_screen();
    draw_dialog(title, message, hints, hint_count, is_error ? UI_COLOR_ERROR : UI_COLOR_ACCENT);
    ui_flush();
}

void ui_alert(char const* title, char const* message, ui_hint_t const* hints, size_t hint_count, bool is_error) {
    pax_background(fb, UI_COLOR_BACKGROUND);
    draw_dialog(title, message, hints, hint_count, is_error ? UI_COLOR_ERROR : UI_COLOR_ACCENT);
    ui_flush();
}

void ui_busy(char const* message) {
    pax_background(fb, UI_COLOR_BACKGROUND);
    pax_center_text(fb, UI_COLOR_TEXT, font, font_size, screen_width / 2, (screen_height - font_size) / 2, message);
    ui_flush();
}
