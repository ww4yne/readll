/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：严格解析有界 Miniflux JSON 响应，未知字段跳过，错误与截断显式返回。
 *
 * English: Strictly parses bounded Miniflux JSON responses, skips unknown fields, and reports
 * malformed or truncated input explicitly.
 *
 * 冻结：解析器不接受尾随载荷、嵌入零、超限数组或超限字符串。
 * Frozen: the parser rejects trailing payloads, embedded NULs, oversized arrays, and strings.
 */
#pragma once

#include <stddef.h>

#include "rss_model.h"

#define RSS_JSON_RESPONSE_MAX (96U * 1024U)

typedef enum {
    RSS_JSON_OK = 0,             ///< 成功 / Success
    RSS_JSON_INVALID_ARGUMENT,   ///< 参数无效 / Invalid argument
    RSS_JSON_TOO_LARGE,          ///< 响应或字段超限 / Response or field exceeds a bound
    RSS_JSON_TRUNCATED,          ///< JSON 被截断 / JSON is truncated
    RSS_JSON_INVALID,            ///< 类型、字段或格式错误 / Invalid type, field, or format
    RSS_JSON_NO_MEMORY,          ///< cJSON 分配失败 / cJSON allocation failed
} rss_json_result_t;

/// 解析 /v1/me。/ Parse /v1/me.
rss_json_result_t rss_json_parse_user(const char *json, size_t length, rss_user_t *out);

/// 解析 /v1/categories。/ Parse /v1/categories.
rss_json_result_t rss_json_parse_categories(const char *json, size_t length,
                                            rss_category_list_t *out);

/// 解析 /v1/entries；正文复制到页内正文池。/ Parse /v1/entries and copy bodies into the page pool.
rss_json_result_t rss_json_parse_entries(const char *json, size_t length, rss_entry_page_t *out);

/// 只解析列表 metadata，不保留正文。/ Parse list metadata without retaining bodies.
rss_json_result_t rss_json_parse_metadata(const char *json, size_t length, rss_entry_page_t *out);

/// 解析单篇 /v1/entries/{id}，正文仅存于页内。/ Parse one entry with its body in the page pool only.
rss_json_result_t rss_json_parse_entry(const char *json, size_t length, rss_entry_page_t *out);
