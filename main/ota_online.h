/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：联网更新的清单契约与禁用状态接口。/ English: online-update manifest contracts and disabled-state APIs.
 * 冻结：新发布源配置前不得联网、猜测域名或回退旧源。/ Frozen: never connect, invent a domain or use a legacy fallback before a new origin is configured.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define READLL_OTA_LAYOUT "readll-dual-4m-v1"
#define READLL_OTA_BOARD "RDP-G01-W"
#define READLL_OTA_ONLINE_CONFIGURED 0

typedef enum { READLL_UPDATE_IDLE, READLL_UPDATE_CHECKING, READLL_UPDATE_AVAILABLE,
               READLL_UPDATE_LATEST, READLL_UPDATE_DOWNLOADING, READLL_UPDATE_READY,
               READLL_UPDATE_FAILED, READLL_UPDATE_CANCELLED } readll_update_state_t;
typedef struct {
    char version[33], project[33], board[24], layout[32], minimum_base_version[33];
    char url[256], sha256[65], notes[384];
    uint32_t size;
} readll_release_t;
typedef struct {
    readll_update_state_t state;
    readll_release_t release;
    uint32_t revision, received;
    char message[96];
    bool busy;
} readll_update_status_t;

/// 有界解析、型号与版本验证；不接触网络。/ Bounded parsing, board and version validation without networking.
esp_err_t readll_release_parse(const char *json, size_t length, readll_release_t *release);
int readll_version_compare(const char *a, const char *b);
bool readll_release_url_valid(const char *url);
/// 返回“尚未配置”，不创建任务或发起请求。/ Report “not configured” without creating a task or request.
esp_err_t readll_online_check(void);
esp_err_t readll_online_download(void);
bool readll_online_busy(void);
void readll_online_get_status(readll_update_status_t *status);
void readll_online_cancel_join(void);
/// 刷屏路径共用的递归锁。/ Recursive lock shared by display paths.
bool readll_online_display_begin(void);
void readll_online_display_end(bool held);
/// 联网升级禁用时始终拒绝切换启动槽。/ Always reject boot-slot changes while online updates are disabled.
esp_err_t readll_online_commit(void);
