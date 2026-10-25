#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "pax_gfx.h"
#include "sdkconfig.h"

// Colors

#if defined(CONFIG_BSP_TARGET_KAMI)
// Palette indices for the e-paper display of Kami (see palette in main.c: white, black, red)
#define UI_COLOR_BACKGROUND  0
#define UI_COLOR_PANEL       0
#define UI_COLOR_TEXT        1
#define UI_COLOR_TEXT_DIM    1
#define UI_COLOR_ACCENT      2
#define UI_COLOR_SELECTION   2
#define UI_COLOR_KEYCAP_TEXT 0
#define UI_COLOR_SUCCESS     1
#define UI_COLOR_ERROR       2
#else
#define UI_COLOR_BACKGROUND  0xFF14161C
#define UI_COLOR_PANEL       0xFF232733
#define UI_COLOR_TEXT        0xFFECEEF4
#define UI_COLOR_TEXT_DIM    0xFF8A90A2
#define UI_COLOR_ACCENT      0xFFFF6B3D
#define UI_COLOR_SELECTION   0xFF34405C
#define UI_COLOR_KEYCAP_TEXT 0xFF14161C
#define UI_COLOR_SUCCESS     0xFF4CC38A
#define UI_COLOR_ERROR       0xFFFF5370
#endif

// Input

typedef enum {
    UI_KEY_NONE = 0,
    UI_KEY_CHARACTER,  // Printable ASCII character, see ui_input_t.character
    UI_KEY_UP,
    UI_KEY_DOWN,
    UI_KEY_LEFT,
    UI_KEY_RIGHT,
    UI_KEY_HOME,
    UI_KEY_END,
    UI_KEY_PGUP,
    UI_KEY_PGDN,
    UI_KEY_ENTER,
    UI_KEY_ESC,
    UI_KEY_TAB,
    UI_KEY_BACKSPACE,
    UI_KEY_F1,
    UI_KEY_F2,
    UI_KEY_F3,
    UI_KEY_F4,
    UI_KEY_F5,
    UI_KEY_F6,
} ui_key_t;

typedef struct {
    ui_key_t key;
    char     character;
} ui_input_t;

// Widgets

typedef struct {
    char const* key;    // Key name shown on the key cap, e.g. "F1"
    char const* label;  // Action, e.g. "Exit"
} ui_hint_t;

typedef enum {
    UI_STATUS_NONE = 0,
    UI_STATUS_INFO,
    UI_STATUS_SUCCESS,
    UI_STATUS_ERROR,
} ui_status_t;

typedef enum {
    UI_FIELD_TEXT,     // Any printable ASCII character
    UI_FIELD_HEX,      // Hexadecimal digits, shown with a 0x prefix
    UI_FIELD_DECIMAL,  // Decimal digits
    UI_FIELD_CHOICE,   // One of a set of options, changed by the owner of the field (e.g. with the left and right keys)
} ui_field_type_t;

typedef struct {
    char const*     label;
    ui_field_type_t type;
    char*           value;       // Buffer of at least max_length + 1 bytes
    size_t          max_length;  // Maximum number of characters
} ui_field_t;

/// @brief Initialize the user interface, the layout is derived from the size of the framebuffer
void ui_init(pax_buf_t* fb, QueueHandle_t input_queue);

/// @brief Send the framebuffer to the display
void ui_flush(void);

/// @brief Block until the user presses a key
ui_input_t ui_wait_input(void);

/// @brief Wait for the user to press a key
/// @return True if a key was pressed within the timeout
bool ui_poll_input(ui_input_t* out_input, TickType_t timeout);

// Layout

float ui_width(void);
float ui_padding(void);
float ui_row_height(void);
float ui_line_height(void);
float ui_content_top(void);
float ui_content_bottom(void);
float ui_text_width(char const* text);

/// @brief Clear the screen and draw the header (title and optional info text) and the footer (key hints)
void ui_draw_screen(char const* title, char const* info, ui_hint_t const* hints, size_t hint_count);

/// @brief Draw a status message on the line above the footer
void ui_draw_status(char const* text, ui_status_t status);

/// @brief Draw text in the user interface font
void ui_draw_text(float x, float y, pax_col_t color, char const* text);

/// @brief Draw text wrapped at word boundaries
/// @return Height of the drawn text
float ui_draw_wrapped(float x, float y, float max_width, pax_col_t color, char const* text);

/// @brief Draw a row of a list, the detail text is drawn right aligned in a monospace font
void ui_draw_list_row(float y, bool selected, char const* text, char const* detail);

/// @brief Draw a scrollbar to the right of the content area
void ui_draw_scrollbar(size_t first_visible, size_t visible_count, size_t total_count);

/// @brief Draw a labeled text input field
void ui_draw_field(float y, float label_width, ui_field_t const* field, bool focused);

/// @brief Edit the value of a field
/// @return True if the value of the field changed
bool ui_field_handle_input(ui_field_t* field, ui_input_t const* input);

// Dialogs

/// @brief Draw a dialog box on top of the current screen and ask the user to confirm
/// @return True if the user confirmed (Enter or Y), false if the user cancelled (Esc or N)
bool ui_confirm(char const* title, char const* message, char const* confirm_label);

/// @brief Draw a dialog box on top of the current screen and wait for the user to dismiss it
void ui_message(char const* title, char const* message, bool is_error);

/// @brief Draw a dialog box on top of the current screen, handling input is left to the caller
void ui_show_dialog(char const* title, char const* message, ui_hint_t const* hints, size_t hint_count, bool is_error);

/// @brief Clear the screen and show a dialog box, handling input is left to the caller
void ui_alert(char const* title, char const* message, ui_hint_t const* hints, size_t hint_count, bool is_error);

/// @brief Clear the screen and show a message while the application is busy
void ui_busy(char const* message);
