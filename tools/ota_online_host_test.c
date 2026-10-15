/*
 * SPDX-License-Identifier: Apache-2.0
 * 中文：联网升级禁用契约与刷屏互斥接口。/ English: Disabled online-update contract and display exclusion API.
 */
#include "common.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned take_count;

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) {
    return (void *)1;
}

int xSemaphoreTakeRecursive(SemaphoreHandle_t mutex, uint32_t wait) {
    assert(mutex == (void *)1 && wait == portMAX_DELAY);
    ++take_count;
    return pdTRUE;
}

int xSemaphoreGiveRecursive(SemaphoreHandle_t mutex) {
    assert(mutex == (void *)1 && take_count);
    --take_count;
    return pdTRUE;
}

#include "../main/ota_online.c"

int main(void) {
    readll_update_status_t status = {0};
    readll_online_get_status(&status);
    assert(status.state == READLL_UPDATE_FAILED);
    assert(!strcmp(status.message, "在线升级尚未配置"));
    assert(!status.busy && !readll_online_busy());

    assert(readll_online_check() == ESP_ERR_NOT_SUPPORTED);
    readll_online_get_status(&status);
    assert(status.state == READLL_UPDATE_FAILED && !status.busy);
    assert(!strcmp(status.message, "在线升级尚未配置"));

    assert(readll_online_download() == ESP_ERR_NOT_SUPPORTED);
    assert(readll_online_commit() == ESP_ERR_INVALID_STATE);
    assert(readll_online_display_begin());
    assert(take_count == 1);
    readll_online_display_end(true);
    assert(take_count == 0);

    puts("PASS: online OTA is explicitly not configured, starts no worker, and cannot switch boot slots");
}
