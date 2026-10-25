#include "code_db.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cJSON.h"
#include "esp_log.h"

static char const TAG[] = "code_db";

#define CODE_DB_VERSION       1
#define CODE_DB_MAX_FILE_SIZE (256 * 1024)
#define CODE_DB_MAX_PATH      128

void code_db_init(code_db_t* db) {
    memset(db, 0, sizeof(*db));
    db->settings.frequency_hz = CODE_DB_DEFAULT_FREQUENCY_HZ;
    db->settings.duty_percent = CODE_DB_DEFAULT_DUTY_PERCENT;
}

void code_db_free(code_db_t* db) {
    free(db->entries);
    code_db_init(db);
}

int code_db_find(code_db_t const* db, char const* name) {
    for (size_t i = 0; i < db->count; i++) {
        if (strcasecmp(db->entries[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

esp_err_t code_db_insert(code_db_t* db, size_t index, code_db_entry_t const* entry) {
    if (index > db->count) {
        return ESP_ERR_INVALID_ARG;
    }
    if (db->count == db->capacity) {
        size_t           capacity = db->capacity ? db->capacity * 2 : 16;
        code_db_entry_t* entries  = realloc(db->entries, capacity * sizeof(code_db_entry_t));
        if (entries == NULL) {
            return ESP_ERR_NO_MEM;
        }
        db->entries  = entries;
        db->capacity = capacity;
    }
    memmove(&db->entries[index + 1], &db->entries[index], (db->count - index) * sizeof(code_db_entry_t));
    db->entries[index] = *entry;
    db->count++;
    return ESP_OK;
}

void code_db_remove(code_db_t* db, size_t index) {
    if (index >= db->count) {
        return;
    }
    memmove(&db->entries[index], &db->entries[index + 1], (db->count - index - 1) * sizeof(code_db_entry_t));
    db->count--;
}

static bool file_exists(char const* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static esp_err_t read_file(char const* path, char** out_data, size_t* out_length) {
    FILE* fd = fopen(path, "rb");
    if (fd == NULL) {
        ESP_LOGE(TAG, "Failed to open %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }

    esp_err_t res  = ESP_FAIL;
    char*     data = NULL;
    long      size = -1;
    if (fseek(fd, 0, SEEK_END) == 0) {
        size = ftell(fd);
    }
    if (size < 0 || fseek(fd, 0, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Failed to determine size of %s", path);
    } else if (size > CODE_DB_MAX_FILE_SIZE) {
        ESP_LOGE(TAG, "%s is too large (%ld bytes)", path, size);
        res = ESP_ERR_INVALID_RESPONSE;
    } else if ((data = malloc(size + 1)) == NULL) {
        res = ESP_ERR_NO_MEM;
    } else if (fread(data, 1, size, fd) != (size_t)size) {
        ESP_LOGE(TAG, "Failed to read %s", path);
        free(data);
        data = NULL;
    } else {
        data[size]  = '\0';
        *out_data   = data;
        *out_length = size;
        res         = ESP_OK;
    }
    fclose(fd);
    return res;
}

// Values are stored as hex strings ("0xFB04") but plain JSON numbers are accepted too
static bool parse_u16(cJSON const* item, uint16_t* out) {
    unsigned long value;
    if (cJSON_IsNumber(item)) {
        if (item->valuedouble < 0 || item->valuedouble > 0xFFFF || item->valuedouble != (double)item->valueint) {
            return false;
        }
        value = (unsigned long)item->valueint;
    } else if (cJSON_IsString(item)) {
        char const* text = item->valuestring;
        char*       end  = NULL;
        if (text[0] == '\0' || text[0] == '-') {
            return false;
        }
        errno = 0;
        value = strtoul(text, &end, 0);
        if (*end != '\0' || errno != 0 || value > 0xFFFF) {
            return false;
        }
    } else {
        return false;
    }
    *out = (uint16_t)value;
    return true;
}

static void parse_settings(code_db_t* db, cJSON const* settings) {
    if (!cJSON_IsObject(settings)) {
        return;
    }
    cJSON const* frequency = cJSON_GetObjectItemCaseSensitive(settings, "frequency_hz");
    if (cJSON_IsNumber(frequency) && frequency->valuedouble >= CODE_DB_MIN_FREQUENCY_HZ &&
        frequency->valuedouble <= CODE_DB_MAX_FREQUENCY_HZ) {
        db->settings.frequency_hz = (uint32_t)frequency->valuedouble;
    }
    cJSON const* duty = cJSON_GetObjectItemCaseSensitive(settings, "duty_cycle_percent");
    if (cJSON_IsNumber(duty) && duty->valuedouble >= CODE_DB_MIN_DUTY_PERCENT &&
        duty->valuedouble <= CODE_DB_MAX_DUTY_PERCENT) {
        db->settings.duty_percent = (uint8_t)duty->valuedouble;
    }
}

esp_err_t code_db_load(code_db_t* db, char const* path, size_t* out_skipped) {
    code_db_free(db);
    if (out_skipped != NULL) {
        *out_skipped = 0;
    }

    // code_db_save() replaces the database with a temporary file, if the device lost power halfway through
    // only the temporary file remains
    char tmp_path[CODE_DB_MAX_PATH];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    char const* load_path = path;
    if (!file_exists(path)) {
        if (!file_exists(tmp_path)) {
            ESP_LOGI(TAG, "%s does not exist, starting with an empty database", path);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Recovering database from %s", tmp_path);
        load_path = tmp_path;
    }

    char*     data   = NULL;
    size_t    length = 0;
    esp_err_t res    = read_file(load_path, &data, &length);
    if (res != ESP_OK) {
        return res;
    }

    cJSON* root = cJSON_ParseWithLength(data, length);
    free(data);
    if (!cJSON_IsObject(root)) {
        ESP_LOGE(TAG, "%s does not contain a valid database", load_path);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    parse_settings(db, cJSON_GetObjectItemCaseSensitive(root, "settings"));

    cJSON const* codes = cJSON_GetObjectItemCaseSensitive(root, "codes");
    if (codes != NULL && !cJSON_IsArray(codes)) {
        ESP_LOGE(TAG, "%s: \"codes\" is not an array", load_path);
        cJSON_Delete(root);
        code_db_free(db);
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t       skipped = 0;
    cJSON const* item    = NULL;
    cJSON_ArrayForEach(item, codes) {
        code_db_entry_t entry = {0};
        cJSON const*    name  = cJSON_GetObjectItemCaseSensitive(item, "name");
        if (!cJSON_IsString(name) || name->valuestring[0] == '\0' ||
            strlen(name->valuestring) > CODE_DB_NAME_MAX_LENGTH ||
            !parse_u16(cJSON_GetObjectItemCaseSensitive(item, "address"), &entry.address) ||
            !parse_u16(cJSON_GetObjectItemCaseSensitive(item, "command"), &entry.command)) {
            ESP_LOGW(TAG, "Skipping invalid entry");
            skipped++;
            continue;
        }
        snprintf(entry.name, sizeof(entry.name), "%s", name->valuestring);
        if (code_db_find(db, entry.name) >= 0) {
            ESP_LOGW(TAG, "Skipping duplicate entry \"%s\"", entry.name);
            skipped++;
            continue;
        }
        res = code_db_insert(db, db->count, &entry);
        if (res != ESP_OK) {
            cJSON_Delete(root);
            code_db_free(db);
            return res;
        }
    }

    cJSON_Delete(root);
    if (out_skipped != NULL) {
        *out_skipped = skipped;
    }
    ESP_LOGI(TAG, "Loaded %zu codes from %s", db->count, load_path);
    return ESP_OK;
}

static esp_err_t write_file(char const* path, char const* text) {
    char tmp_path[CODE_DB_MAX_PATH];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE* fd = fopen(tmp_path, "wb");
    if (fd == NULL) {
        ESP_LOGE(TAG, "Failed to create %s: %s", tmp_path, strerror(errno));
        return ESP_FAIL;
    }
    size_t length = strlen(text);
    bool   ok     = fwrite(text, 1, length, fd) == length;
    ok            = (fflush(fd) == 0) && ok;
    ok            = (fsync(fileno(fd)) == 0) && ok;
    ok            = (fclose(fd) == 0) && ok;
    if (!ok) {
        ESP_LOGE(TAG, "Failed to write %s", tmp_path);
        remove(tmp_path);
        return ESP_FAIL;
    }

    // FAT can not rename over an existing file, so the old database is removed first
    if (remove(path) != 0 && errno != ENOENT) {
        ESP_LOGE(TAG, "Failed to remove %s: %s", path, strerror(errno));
        remove(tmp_path);
        return ESP_FAIL;
    }
    if (rename(tmp_path, path) != 0) {
        ESP_LOGE(TAG, "Failed to rename %s to %s: %s", tmp_path, path, strerror(errno));
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t code_db_save(code_db_t const* db, char const* path) {
    cJSON* root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    bool   ok       = cJSON_AddNumberToObject(root, "version", CODE_DB_VERSION) != NULL;
    cJSON* settings = cJSON_AddObjectToObject(root, "settings");
    ok              = ok && settings != NULL;
    ok              = ok && cJSON_AddNumberToObject(settings, "frequency_hz", db->settings.frequency_hz) != NULL;
    ok              = ok && cJSON_AddNumberToObject(settings, "duty_cycle_percent", db->settings.duty_percent) != NULL;
    cJSON* codes    = cJSON_AddArrayToObject(root, "codes");
    ok              = ok && codes != NULL;

    for (size_t i = 0; ok && i < db->count; i++) {
        code_db_entry_t const* entry = &db->entries[i];
        char                   hex[8];
        cJSON*                 item = cJSON_CreateObject();
        if (item == NULL || !cJSON_AddItemToArray(codes, item)) {
            cJSON_Delete(item);
            ok = false;
            break;
        }
        ok = cJSON_AddStringToObject(item, "name", entry->name) != NULL;
        snprintf(hex, sizeof(hex), "0x%04X", entry->address);
        ok = ok && cJSON_AddStringToObject(item, "address", hex) != NULL;
        snprintf(hex, sizeof(hex), "0x%04X", entry->command);
        ok = ok && cJSON_AddStringToObject(item, "command", hex) != NULL;
    }

    char* text = ok ? cJSON_Print(root) : NULL;
    cJSON_Delete(root);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t res = write_file(path, text);
    cJSON_free(text);
    if (res == ESP_OK) {
        ESP_LOGI(TAG, "Saved %zu codes to %s", db->count, path);
    }
    return res;
}
