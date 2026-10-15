/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：联网升级在新发布源配置前保持显式禁用，同时保留刷屏互斥接口。
 *
 * English: Online updates remain explicitly disabled until a new release origin is configured,
 * while the display exclusion interface stays available.
 *
 * 冻结：不得请求旧源、猜测新域名或静默回退；本地 TF 升级由 ota_update.c 独立处理。
 * Frozen: never request legacy origins, invent a domain, or silently fall back; ota_update.c owns TF updates.
 */
#include "ota_online.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_flash_display;
static readll_update_status_t s_status = {
    .state = READLL_UPDATE_FAILED,
    .message = "在线升级尚未配置",
};

static void set_not_configured(void) {
    s_status.state = READLL_UPDATE_FAILED;
    s_status.busy = false;
    s_status.received = 0;
    snprintf(s_status.message, sizeof(s_status.message), "在线升级尚未配置");
    ++s_status.revision;
}

bool readll_online_display_begin(void) {
    if (!s_flash_display) s_flash_display = xSemaphoreCreateRecursiveMutex();
    return s_flash_display && xSemaphoreTakeRecursive(s_flash_display, portMAX_DELAY) == pdTRUE;
}

void readll_online_display_end(bool held) {
    if (held) xSemaphoreGiveRecursive(s_flash_display);
}

esp_err_t readll_online_check(void) {
    set_not_configured();
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t readll_online_download(void) {
    set_not_configured();
    return ESP_ERR_NOT_SUPPORTED;
}

bool readll_online_busy(void) {
    return false;
}

void readll_online_get_status(readll_update_status_t *status) {
    if (status) memcpy(status, &s_status, sizeof(*status));
}

void readll_online_cancel_join(void) {
    set_not_configured();
}

esp_err_t readll_online_commit(void) {
    set_not_configured();
    return ESP_ERR_INVALID_STATE;
}
