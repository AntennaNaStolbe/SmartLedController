#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "storage.h"

#define STORAGE_NS            "appcfg"
#define STORAGE_KEY           "cfg"
#define STORAGE_MAGIC         0x53434C54UL /* 'SLCT' */
#define STORAGE_VERSION       1
#define STORAGE_ROT_KEY       "disp_rot"
#define STORAGE_ROT_MAGIC     0x534C5254UL /* 'SLRT' */
#define STORAGE_ROT_VERSION   1

static const char *TAG = "storage";

typedef struct {
    uint32_t        magic;
    uint32_t        version;
    app_settings_t  settings;
    uint32_t        check;
} storage_blob_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t  rotation;
    uint8_t  reserved[3];
    uint32_t check;
} storage_rotation_blob_t;

static uint32_t storage_checksum(const storage_blob_t *b)
{
    /* FNV-1a over the settings blob (padding neutralised by memset before save) */
    uint32_t h = 2166136261u;
    const uint8_t *p = (const uint8_t *)&b->settings;
    size_t n = sizeof(b->settings);
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t rotation_checksum(const storage_rotation_blob_t *b)
{
    uint32_t h = 2166136261u;
    const uint8_t *p = (const uint8_t *)b;
    size_t n = offsetof(storage_rotation_blob_t, check);
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static void storage_defaults(app_settings_t *out)
{
    if (out == NULL) {
        return;
    }
    out->on_off = false;
    out->brightness = 200;
    out->color_temp_mireds = 2440;          /* mireds x10, ~4100 K */
    out->on_transition_time = 200;          /* ms */
    out->off_transition_time = 200;         /* ms */
    out->startup_behavior = 255;            /* restore previous state */
}

/* Version 0 -> 1 migration would slot in here (PLATFORM.md §3). Currently a
 * mismatch of magic/version simply falls back to defaults. */
static bool storage_load_raw(storage_blob_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(STORAGE_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Fresh chip, or nothing has been saved since a factory reset. */
        ESP_LOGI(TAG, "No stored settings yet, starting from defaults");
        return false;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS namespace '%s' (%s)", STORAGE_NS, esp_err_to_name(err));
        return false;
    }
    size_t len = sizeof(*out);
    err = nvs_get_blob(h, STORAGE_KEY, out, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(*out)) {
        ESP_LOGW(TAG, "No valid settings blob yet (err=%s, len=%d)", esp_err_to_name(err), (int)len);
        return false;
    }
    if (out->magic != STORAGE_MAGIC) {
        ESP_LOGW(TAG, "Settings blob has bad magic 0x%08lx", (unsigned long)out->magic);
        return false;
    }
    if (out->version != STORAGE_VERSION) {
        ESP_LOGW(TAG, "Settings blob version mismatch (%lu != %u)", (unsigned long)out->version, STORAGE_VERSION);
        return false;
    }
    if (out->check != storage_checksum(out)) {
        ESP_LOGW(TAG, "Settings blob checksum mismatch");
        return false;
    }
    return true;
}

void storage_init(void)
{
    /* Nothing to prepare: NVS is opened per call. */
}

bool storage_load(app_settings_t *out)
{
    if (out == NULL) {
        return false;
    }
    storage_blob_t b;
    if (storage_load_raw(&b)) {
        *out = b.settings;
        ESP_LOGI(TAG, "Settings loaded: on=%d, brightness=%u, mireds_x10=%u, startup=%u",
                 b.settings.on_off, b.settings.brightness, b.settings.color_temp_mireds, b.settings.startup_behavior);
        return true;
    }
    storage_defaults(out);
    return false;
}

void storage_save(const app_settings_t *in)
{
    if (in == NULL) {
        return;
    }
    storage_blob_t b;
    memset(&b, 0, sizeof(b));
    b.magic = STORAGE_MAGIC;
    b.version = STORAGE_VERSION;
    memset(&b.settings, 0, sizeof(b.settings));
    b.settings = *in;
    b.check = storage_checksum(&b);

    nvs_handle_t h;
    if (nvs_open(STORAGE_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS namespace for write");
        return;
    }
    esp_err_t err = nvs_set_blob(h, STORAGE_KEY, &b, sizeof(b));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save settings: %s", esp_err_to_name(err));
    }
    nvs_close(h);
}

uint8_t storage_display_rotation_load(void)
{
    nvs_handle_t h;
    if (nvs_open(STORAGE_NS, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }

    storage_rotation_blob_t b;
    size_t len = sizeof(b);
    esp_err_t err = nvs_get_blob(h, STORAGE_ROT_KEY, &b, &len);
    nvs_close(h);

    if (err != ESP_OK || len != sizeof(b) || b.magic != STORAGE_ROT_MAGIC ||
        b.version != STORAGE_ROT_VERSION || b.check != rotation_checksum(&b)) {
        return 0;
    }
    return (uint8_t)(b.rotation & 3U);
}

void storage_display_rotation_save(uint8_t rotation)
{
    storage_rotation_blob_t b;
    memset(&b, 0, sizeof(b));
    b.magic = STORAGE_ROT_MAGIC;
    b.version = STORAGE_ROT_VERSION;
    b.rotation = (uint8_t)(rotation & 3U);
    b.check = rotation_checksum(&b);

    nvs_handle_t h;
    if (nvs_open(STORAGE_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS namespace for display rotation");
        return;
    }
    esp_err_t err = nvs_set_blob(h, STORAGE_ROT_KEY, &b, sizeof(b));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save display rotation: %s", esp_err_to_name(err));
    }
    nvs_close(h);
}

void storage_factory_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(STORAGE_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}