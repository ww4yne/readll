/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：实现 RSS 有界模型的配置校验与正文边界访问。
 *
 * English: Implements configuration validation and bounded body access for the RSS model.
 *
 * 冻结：不接受 HTTP、凭据截断、查询或片段形式的根地址。
 * Frozen: HTTP roots, truncated credentials, and roots containing query or fragment parts are
 * rejected.
 */
#include "rss_model.h"

#include <string.h>

bool rss_config_init(rss_config_t *out, const char *base_url, const char *auth_token,
                     uint16_t page_size, uint32_t timeout_ms) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!base_url || !auth_token || strncmp(base_url, "https://", 8) || !base_url[8] ||
        strchr(base_url + 8, '?') || strchr(base_url + 8, '#') || !auth_token[0] ||
        page_size == 0 || page_size > RSS_PAGE_ENTRY_MAX || timeout_ms < 1000 ||
        timeout_ms > 120000) {
        return false;
    }
    size_t base_length = strnlen(base_url, sizeof(out->base_url));
    size_t token_length = strnlen(auth_token, sizeof(out->auth_token));
    if (base_length >= sizeof(out->base_url) || token_length >= sizeof(out->auth_token))
        return false;
    while (base_length > 8 && base_url[base_length - 1] == '/') --base_length;
    if (base_length <= 8 || base_length >= sizeof(out->base_url)) return false;
    memcpy(out->base_url, base_url, base_length);
    out->base_url[base_length] = '\0';
    memcpy(out->auth_token, auth_token, token_length + 1);
    out->page_size = page_size;
    out->timeout_ms = timeout_ms;
    return true;
}

const char *rss_entry_body(const rss_entry_page_t *page, const rss_entry_t *entry) {
    if (!page || !entry || entry->body_offset > page->body_used ||
        entry->body_length > page->body_used - entry->body_offset ||
        entry->body_offset + entry->body_length >= sizeof(page->body_pool) ||
        page->body_pool[entry->body_offset + entry->body_length] != '\0') {
        return NULL;
    }
    return page->body_pool + entry->body_offset;
}
