/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：用独立 NVS namespace 原子保存 RSS 正文排版偏好。
 * English: Atomically persist RSS article typography in a dedicated NVS namespace.
 *
 * 冻结：不修改 EPUB 设置，不保存列表几何。
 * Frozen: Never mutate EPUB settings or persist list geometry.
 */
#include "rss_preferences.h"

#include <stdbool.h>
#include <string.h>

#include "nvs.h"

#define RSS_PREFS_NS "rp_rss_view"
#define RSS_PREFS_KEY "typography"
#define RSS_PREFS_VERSION 1

typedef struct {
    uint8_t version;
    rss_preferences_t preferences;
} rss_preferences_record_t;

void rss_preferences_preset(rss_preferences_t *out, bool compact) {
    if (!out) return;
    *out = (rss_preferences_t){
        .px = compact ? 20 : 22,
        .line_percent = compact ? 112 : 120,
        .paragraph_percent = 50,
        .margin_px = 10,
        .tracking = 2,
        .indent_em = 2,
        .weight = 0,
        .reading_line = 0,
    };
}

bool rss_preferences_valid(const rss_preferences_t *preferences) {
    return preferences &&
           preferences->px >= 18 && preferences->px <= 32 &&
           preferences->line_percent >= 110 && preferences->line_percent <= 150 &&
           preferences->paragraph_percent <= 75 &&
           preferences->paragraph_percent % 25 == 0 &&
           preferences->margin_px <= 48 &&
           preferences->tracking <= 4 &&
           preferences->indent_em <= 3 &&
           preferences->weight >= -2 && preferences->weight <= 2 &&
           preferences->reading_line <= 2;
}

void rss_preferences_load(rss_preferences_t *out) {
    if (!out) return;
    rss_preferences_preset(out, false);
    nvs_handle_t handle;
    if (nvs_open(RSS_PREFS_NS, NVS_READONLY, &handle) != ESP_OK) return;
    rss_preferences_record_t record = {0};
    size_t size = sizeof(record);
    esp_err_t result = nvs_get_blob(handle, RSS_PREFS_KEY, &record, &size);
    nvs_close(handle);
    if (result == ESP_OK && size == sizeof(record) &&
        record.version == RSS_PREFS_VERSION && rss_preferences_valid(&record.preferences)) {
        *out = record.preferences;
    }
}

bool rss_preferences_save(const rss_preferences_t *preferences) {
    if (!rss_preferences_valid(preferences)) return false;
    const rss_preferences_record_t record = {
        .version = RSS_PREFS_VERSION,
        .preferences = *preferences,
    };
    nvs_handle_t handle;
    if (nvs_open(RSS_PREFS_NS, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t result = nvs_set_blob(handle, RSS_PREFS_KEY, &record, sizeof(record));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}
