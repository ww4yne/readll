/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：定义 RSS 核心的有界配置、分类、条目与目标状态，不负责网络或持久化。
 *
 * English: Defines bounded RSS configuration, category, entry, and desired-state models;
 * networking and persistence stay outside this file.
 *
 * 冻结：所有外部字符串都必须有固定上限，条目正文只能引用页内有界正文池。
 * Frozen: every external string remains bounded, and entry bodies only reference the bounded
 * page pool.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RSS_BASE_URL_MAX 256
#define RSS_AUTH_TOKEN_MAX 256
#define RSS_USER_NAME_MAX 96
#define RSS_CATEGORY_TITLE_MAX 128
#define RSS_ENTRY_TITLE_MAX 256
#define RSS_FEED_TITLE_MAX 128
#define RSS_ENTRY_URL_MAX 512
#define RSS_ENTRY_AUTHOR_MAX 128
#define RSS_PAGE_ENTRY_MAX 20
#define RSS_CATEGORY_MAX 32
#define RSS_PAGE_BODY_MAX (64U * 1024U)
#define RSS_ENTRY_BODY_MAX (24U * 1024U)

typedef enum {
    RSS_ENTRY_STATUS_UNREAD = 0,  ///< 未读 / Unread
    RSS_ENTRY_STATUS_READ,        ///< 已读 / Read
} rss_entry_status_t;

typedef struct {
    char base_url[RSS_BASE_URL_MAX];     ///< HTTPS 服务根地址 / HTTPS service base URL
    char auth_token[RSS_AUTH_TOKEN_MAX]; ///< Miniflux API 令牌 / Miniflux API token
    uint16_t page_size;                 ///< 每页条目数，1..20 / Entries per page, 1..20
    uint32_t timeout_ms;                ///< 单次 HTTP 超时 / Per-request HTTP timeout
} rss_config_t;

typedef struct {
    int64_t id;                         ///< 用户 ID / User ID
    char username[RSS_USER_NAME_MAX];   ///< 用户名 / Username
} rss_user_t;

typedef struct {
    int64_t id;                              ///< 分类 ID / Category ID
    char title[RSS_CATEGORY_TITLE_MAX];      ///< 分类标题 / Category title
} rss_category_t;

typedef struct {
    size_t count;                            ///< 有效分类数 / Valid category count
    rss_category_t items[RSS_CATEGORY_MAX];  ///< 有界分类数组 / Bounded category array
} rss_category_list_t;

typedef struct {
    int64_t id;                         ///< 条目 ID / Entry ID
    int64_t feed_id;                    ///< 订阅源 ID / Feed ID
    int64_t category_id;                ///< 分类 ID，未知为 0 / Category ID, zero when absent
    int64_t published_at;               ///< UTC Unix 秒 / UTC Unix seconds
    int64_t changed_at;                 ///< UTC Unix 秒 / UTC Unix seconds
    uint32_t reading_time;              ///< Miniflux 阅读分钟数 / Miniflux reading minutes
    rss_entry_status_t status;          ///< 服务端已读状态 / Server read status
    bool bookmarked;                    ///< 服务端收藏状态 / Server bookmark state
    char feed_title[RSS_FEED_TITLE_MAX]; ///< 订阅源标题 / Feed title
    char title[RSS_ENTRY_TITLE_MAX];    ///< 标题 / Title
    char url[RSS_ENTRY_URL_MAX];        ///< 原文 URL / Original URL
    char author[RSS_ENTRY_AUTHOR_MAX];  ///< 作者，可为空 / Author, may be empty
    uint32_t body_offset;               ///< 页正文池偏移 / Offset in page body pool
    uint32_t body_length;               ///< 正文字节数 / Body byte length
} rss_entry_t;

typedef struct {
    size_t count;                              ///< 有效条目数 / Valid entry count
    size_t total;                              ///< 响应声明总数 / Response-declared total
    size_t body_used;                          ///< 正文池已用字节 / Used body-pool bytes
    rss_entry_t items[RSS_PAGE_ENTRY_MAX];     ///< 有界条目数组 / Bounded entry array
    char body_pool[RSS_PAGE_BODY_MAX];         ///< 零结尾 HTML 正文 / NUL-terminated HTML bodies
} rss_entry_page_t;

typedef struct {
    bool read_known;                     ///< 是否要求已读状态 / Whether read state is requested
    bool read;                           ///< 目标已读状态 / Desired read state
    bool bookmark_known;                 ///< 是否要求收藏状态 / Whether bookmark state is requested
    bool bookmarked;                     ///< 目标收藏状态 / Desired bookmark state
} rss_desired_state_t;

/// 校验并规范化配置；失败时清空输出。/ Validate and normalize configuration; clear output on failure.
bool rss_config_init(rss_config_t *out, const char *base_url, const char *auth_token,
                     uint16_t page_size, uint32_t timeout_ms);

/// 返回条目正文指针；边界无效时返回 NULL。/ Return an entry body pointer, or NULL for invalid bounds.
const char *rss_entry_body(const rss_entry_page_t *page, const rss_entry_t *entry);
