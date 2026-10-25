#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define CODE_DB_NAME_MAX_LENGTH 32

#define CODE_DB_DEFAULT_FREQUENCY_HZ 38000
#define CODE_DB_DEFAULT_DUTY_PERCENT 33
#define CODE_DB_MIN_FREQUENCY_HZ     2000
#define CODE_DB_MAX_FREQUENCY_HZ     500000
#define CODE_DB_MIN_DUTY_PERCENT     1
#define CODE_DB_MAX_DUTY_PERCENT     50

typedef struct {
    char     name[CODE_DB_NAME_MAX_LENGTH + 1];
    uint16_t address;  // NEC address as sent on the wire (low byte first)
    uint16_t command;  // NEC command as sent on the wire (low byte first)
} code_db_entry_t;

typedef struct {
    uint32_t frequency_hz;
    uint8_t  duty_percent;
} code_db_settings_t;

typedef struct {
    code_db_entry_t*   entries;
    size_t             count;
    size_t             capacity;
    code_db_settings_t settings;
} code_db_t;

/// @brief Initialize an empty database with default settings
void code_db_init(code_db_t* db);

/// @brief Release all memory held by a database
void code_db_free(code_db_t* db);

/// @brief Load a database from a JSON file, a missing file results in an empty database
/// @param[out] out_skipped Number of invalid or duplicate entries that were ignored (optional)
/// @return ESP_OK on success, ESP_ERR_INVALID_RESPONSE if the file does not contain a valid database,
///         ESP_ERR_NO_MEM if out of memory, ESP_FAIL if the file could not be read
esp_err_t code_db_load(code_db_t* db, char const* path, size_t* out_skipped);

/// @brief Write a database to a JSON file
/// @return ESP_OK on success, ESP_ERR_NO_MEM if out of memory, ESP_FAIL if the file could not be written
esp_err_t code_db_save(code_db_t const* db, char const* path);

/// @brief Find an entry by name (case insensitive)
/// @return Index of the entry or -1 if not found
int code_db_find(code_db_t const* db, char const* name);

/// @brief Insert an entry at the given index (index == count appends)
esp_err_t code_db_insert(code_db_t* db, size_t index, code_db_entry_t const* entry);

/// @brief Remove the entry at the given index
void code_db_remove(code_db_t* db, size_t index);
