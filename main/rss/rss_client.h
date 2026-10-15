/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：提供同步、有界的 Miniflux HTTPS 客户端，不拥有 Wi-Fi 生命周期。
 *
 * English: Provides a synchronous bounded Miniflux HTTPS client without owning Wi-Fi lifecycle.
 *
 * 冻结：仅接受 HTTPS 根地址，使用系统证书包，禁止重定向，任何错误都不得记录令牌。
 * Frozen: only HTTPS roots are accepted, the system certificate bundle is used, redirects are
 * disabled, and errors must never log the token.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "rss_json.h"

#define RSS_CLIENT_BATCH_MAX 32

typedef enum {
    RSS_CLIENT_OK = 0,             ///< 成功 / Success
    RSS_CLIENT_INVALID_ARGUMENT,   ///< 参数无效 / Invalid argument
    RSS_CLIENT_TRANSPORT,          ///< TLS、连接或读写错误 / TLS, connection, or I/O error
    RSS_CLIENT_HTTP_STATUS,        ///< 非预期 HTTP 状态 / Unexpected HTTP status
    RSS_CLIENT_RESPONSE_TOO_LARGE, ///< 响应超过上限 / Response exceeds its bound
    RSS_CLIENT_INCOMPLETE,         ///< 响应未完整接收 / Response was incomplete
    RSS_CLIENT_JSON,               ///< JSON 无效 / Invalid JSON
} rss_client_code_t;

typedef struct {
    rss_client_code_t code;        ///< 结构化错误码 / Structured error code
    esp_err_t transport_error;     ///< ESP 传输错误 / ESP transport error
    int http_status;               ///< HTTP 状态，未收到为 0 / HTTP status, zero if unavailable
    rss_json_result_t json_error;  ///< JSON 错误 / JSON error
} rss_client_error_t;

typedef struct {
    rss_config_t config;           ///< 配置副本 / Configuration copy
} rss_client_t;

typedef struct {
    int64_t category_id;           ///< 0 表示全部分类 / Zero means all categories
    int64_t changed_after;         ///< UTC Unix 秒，0 表示不筛选 / UTC Unix seconds, zero disables
    int64_t after_entry_id;        ///< ID 游标，0 表示第一页 / ID cursor, zero means first page
    uint16_t limit;                ///< 1..20 / 1..20
} rss_entries_query_t;

/// 初始化客户端。/ Initialize a client.
bool rss_client_init(rss_client_t *client, const rss_config_t *config);

/// 获取当前用户。/ Fetch the current user.
bool rss_client_get_me(const rss_client_t *client, rss_user_t *out, rss_client_error_t *error);

/// 获取分类。/ Fetch categories.
bool rss_client_get_categories(const rss_client_t *client, rss_category_list_t *out,
                               rss_client_error_t *error);

/// 获取一页条目，调用方提供至少 RSS_JSON_RESPONSE_MAX+1 字节暂存区。
/// Fetch one entry page; the caller supplies at least RSS_JSON_RESPONSE_MAX+1 bytes of scratch.
bool rss_client_get_entries(const rss_client_t *client, const rss_entries_query_t *query,
                            char *scratch, size_t scratch_size, rss_entry_page_t *out,
                            rss_client_error_t *error);

/// 幂等批量设置 read/unread。/ Idempotently set read/unread for a batch.
bool rss_client_set_read(const rss_client_t *client, const int64_t *entry_ids, size_t count,
                         bool read, rss_client_error_t *error);

/// 调用 Miniflux 单条收藏 toggle。/ Invoke the Miniflux bookmark toggle for one entry.
bool rss_client_toggle_bookmark(const rss_client_t *client, int64_t entry_id,
                                rss_client_error_t *error);
