/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：用现有 cJSON 严格解析有界 Miniflux 响应，并立即释放解析树。
 *
 * English: Strictly parses bounded Miniflux responses with the existing cJSON and releases each
 * parse tree immediately.
 *
 * 冻结：必需字段不能重复、缺失或被截短；未知字段不影响结果。
 * Frozen: required fields cannot be duplicated, missing, or truncated; unknown fields are ignored.
 */
#include "rss_json.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"

/* ---- 通用校验 / Common validation ---- */

static const cJSON *unique_item(const cJSON *object, const char *name) {
    const cJSON *found = NULL;
    if (!cJSON_IsObject(object)) return NULL;
    for (const cJSON *item = object->child; item; item = item->next) {
        if (item->string && !strcmp(item->string, name)) {
            if (found) return NULL;
            found = item;
        }
    }
    return found;
}

static bool copy_string(const cJSON *object, const char *name, char *out, size_t capacity,
                        bool allow_empty) {
    const cJSON *item = unique_item(object, name);
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    size_t length = strnlen(item->valuestring, capacity);
    if ((!allow_empty && length == 0) || length >= capacity) return false;
    memcpy(out, item->valuestring, length + 1);
    return true;
}

static bool has_nul_escape(const char *json, size_t length) {
    bool in_string = false;
    for (size_t i = 0; i < length; ++i) {
        if (!in_string) {
            if (json[i] == '"') in_string = true;
            continue;
        }
        if (json[i] == '"') {
            in_string = false;
            continue;
        }
        if (json[i] != '\\') continue;
        if (i + 1 >= length) return false;
        if (json[i + 1] == 'u' && i + 5 < length && json[i + 2] == '0' &&
            json[i + 3] == '0' && json[i + 4] == '0' && json[i + 5] == '0') {
            return true;
        }
        ++i;
    }
    return false;
}

static bool exact_i64(const cJSON *object, const char *name, int64_t *out) {
    const cJSON *item = unique_item(object, name);
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < 0 || item->valuedouble > 9007199254740991.0 ||
        floor(item->valuedouble) != item->valuedouble) {
        return false;
    }
    *out = (int64_t)item->valuedouble;
    return true;
}

static bool exact_size(const cJSON *object, const char *name, size_t *out) {
    int64_t value = 0;
    if (!exact_i64(object, name, &value) || (uint64_t)value > SIZE_MAX) return false;
    *out = (size_t)value;
    return true;
}

static rss_json_result_t parse_root(const char *json, size_t length, cJSON **root) {
    *root = NULL;
    if (!json || !length || length > RSS_JSON_RESPONSE_MAX || memchr(json, '\0', length)) {
        return length > RSS_JSON_RESPONSE_MAX ? RSS_JSON_TOO_LARGE : RSS_JSON_INVALID_ARGUMENT;
    }
    if (has_nul_escape(json, length)) return RSS_JSON_INVALID;
    const char *end = NULL;
    cJSON *parsed = cJSON_ParseWithLengthOpts(json, length, &end, false);
    if (!parsed) {
        size_t tail = length;
        while (tail && isspace((unsigned char)json[tail - 1])) --tail;
        if (!tail || (json[tail - 1] != '}' && json[tail - 1] != ']')) return RSS_JSON_TRUNCATED;
        return RSS_JSON_INVALID;
    }
    while (end && end < json + length && isspace((unsigned char)*end)) ++end;
    if (!end || end != json + length) {
        cJSON_Delete(parsed);
        return RSS_JSON_INVALID;
    }
    *root = parsed;
    return RSS_JSON_OK;
}

/* ---- RFC 3339 / RFC 3339 ---- */

static bool leap_year(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static int64_t days_before_year(int year) {
    int64_t y = year - 1;
    return 365 * y + y / 4 - y / 100 + y / 400;
}

static bool digits(const char *text, size_t count, int *value) {
    int result = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!isdigit((unsigned char)text[i])) return false;
        result = result * 10 + text[i] - '0';
    }
    *value = result;
    return true;
}

static bool timestamp_value(const cJSON *object, const char *name, int64_t *out) {
    static const uint16_t month_days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const cJSON *item = unique_item(object, name);
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    const char *text = item->valuestring;
    size_t length = strnlen(text, 48);
    int year, month, day, hour, minute, second;
    if (length < 20 || !digits(text, 4, &year) || text[4] != '-' ||
        !digits(text + 5, 2, &month) || text[7] != '-' || !digits(text + 8, 2, &day) ||
        (text[10] != 'T' && text[10] != 't') || !digits(text + 11, 2, &hour) ||
        text[13] != ':' || !digits(text + 14, 2, &minute) || text[16] != ':' ||
        !digits(text + 17, 2, &second) || year < 1970 || year > 9999 || month < 1 ||
        month > 12 || hour > 23 || minute > 59 || second > 60) {
        return false;
    }
    int max_day = month_days[month] + (month == 2 && leap_year(year));
    if (day < 1 || day > max_day) return false;
    size_t at = 19;
    if (at < length && text[at] == '.') {
        ++at;
        size_t fraction = at;
        while (at < length && isdigit((unsigned char)text[at])) ++at;
        if (at == fraction) return false;
    }
    int offset = 0;
    if (at < length && (text[at] == 'Z' || text[at] == 'z')) {
        ++at;
    } else if (at + 6 == length && (text[at] == '+' || text[at] == '-')) {
        int offset_hour, offset_minute;
        if (!digits(text + at + 1, 2, &offset_hour) || text[at + 3] != ':' ||
            !digits(text + at + 4, 2, &offset_minute) || offset_hour > 23 ||
            offset_minute > 59) {
            return false;
        }
        offset = (offset_hour * 60 + offset_minute) * 60;
        if (text[at] == '-') offset = -offset;
        at += 6;
    } else {
        return false;
    }
    if (at != length) return false;
    int64_t days = days_before_year(year) - days_before_year(1970);
    for (int current = 1; current < month; ++current)
        days += month_days[current] + (current == 2 && leap_year(year));
    days += day - 1;
    int normalized_second = second == 60 ? 59 : second;
    *out = days * 86400 + hour * 3600 + minute * 60 + normalized_second - offset;
    return true;
}

/* ---- 公开解析器 / Public parsers ---- */

rss_json_result_t rss_json_parse_user(const char *json, size_t length, rss_user_t *out) {
    if (!out) return RSS_JSON_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    cJSON *root = NULL;
    rss_json_result_t result = parse_root(json, length, &root);
    if (result != RSS_JSON_OK) return result;
    bool valid = cJSON_IsObject(root) && exact_i64(root, "id", &out->id) && out->id > 0 &&
                 copy_string(root, "username", out->username, sizeof(out->username), false);
    cJSON_Delete(root);
    if (!valid) {
        memset(out, 0, sizeof(*out));
        return RSS_JSON_INVALID;
    }
    return RSS_JSON_OK;
}

rss_json_result_t rss_json_parse_categories(const char *json, size_t length,
                                            rss_category_list_t *out) {
    if (!out) return RSS_JSON_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    cJSON *root = NULL;
    rss_json_result_t result = parse_root(json, length, &root);
    if (result != RSS_JSON_OK) return result;
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return RSS_JSON_INVALID;
    }
    int count = cJSON_GetArraySize(root);
    if (count < 0 || count > RSS_CATEGORY_MAX) {
        cJSON_Delete(root);
        return RSS_JSON_TOO_LARGE;
    }
    bool valid = true;
    for (int i = 0; i < count && valid; ++i) {
        const cJSON *item = cJSON_GetArrayItem(root, i);
        rss_category_t *category = &out->items[i];
        valid = cJSON_IsObject(item) && exact_i64(item, "id", &category->id) && category->id > 0 &&
                copy_string(item, "title", category->title, sizeof(category->title), false);
    }
    if (valid) out->count = (size_t)count;
    cJSON_Delete(root);
    if (!valid) {
        memset(out, 0, sizeof(*out));
        return RSS_JSON_INVALID;
    }
    return RSS_JSON_OK;
}

static bool parse_entry(const cJSON *item, rss_entry_page_t *page, rss_entry_t *entry,
                        bool include_body, rss_json_result_t *error) {
    memset(entry, 0, sizeof(*entry));
    const cJSON *status = unique_item(item, "status");
    const cJSON *starred = unique_item(item, "starred");
    const cJSON *reading_time = unique_item(item, "reading_time");
    const cJSON *feed = unique_item(item, "feed");
    const cJSON *category = cJSON_IsObject(feed) ? unique_item(feed, "category") : NULL;
    const cJSON *content = unique_item(item, "content");
    if (!cJSON_IsObject(item) || !exact_i64(item, "id", &entry->id) || entry->id <= 0 ||
        !exact_i64(item, "feed_id", &entry->feed_id) || entry->feed_id <= 0 ||
        !copy_string(item, "title", entry->title, sizeof(entry->title), false) ||
        !copy_string(item, "url", entry->url, sizeof(entry->url), false) ||
        !copy_string(item, "author", entry->author, sizeof(entry->author), true) ||
        !timestamp_value(item, "published_at", &entry->published_at) ||
        !timestamp_value(item, "changed_at", &entry->changed_at) ||
        !cJSON_IsString(status) || !status->valuestring ||
        (strcmp(status->valuestring, "read") && strcmp(status->valuestring, "unread")) ||
        !cJSON_IsBool(starred) || !cJSON_IsNumber(reading_time) ||
        reading_time->valuedouble < 0 || reading_time->valuedouble > UINT32_MAX ||
        floor(reading_time->valuedouble) != reading_time->valuedouble || !cJSON_IsObject(feed) ||
        !cJSON_IsObject(category) || !cJSON_IsString(content) || !content->valuestring) {
        return false;
    }
    if (!copy_string(feed, "title", entry->feed_title, sizeof(entry->feed_title), true))
        return false;
    entry->status = !strcmp(status->valuestring, "read") ? RSS_ENTRY_STATUS_READ
                                                          : RSS_ENTRY_STATUS_UNREAD;
    entry->bookmarked = cJSON_IsTrue(starred);
    entry->reading_time = (uint32_t)reading_time->valuedouble;
    if (!exact_i64(category, "id", &entry->category_id) || entry->category_id <= 0) return false;
    if (!include_body) return true;
    size_t body_length = strnlen(content->valuestring, RSS_ENTRY_BODY_MAX + 1);
    if (body_length > RSS_ENTRY_BODY_MAX ||
        body_length + 1 > sizeof(page->body_pool) - page->body_used) {
        *error = RSS_JSON_TOO_LARGE;
        return false;
    }
    entry->body_offset = (uint32_t)page->body_used;
    entry->body_length = (uint32_t)body_length;
    memcpy(page->body_pool + page->body_used, content->valuestring, body_length + 1);
    page->body_used += body_length + 1;
    return true;
}

static rss_json_result_t parse_entries(const char *json, size_t length, rss_entry_page_t *out,
                                      bool include_body) {
    if (!out) return RSS_JSON_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    cJSON *root = NULL;
    rss_json_result_t result = parse_root(json, length, &root);
    if (result != RSS_JSON_OK) return result;
    const cJSON *entries = unique_item(root, "entries");
    size_t total = 0;
    if (!cJSON_IsObject(root) || !cJSON_IsArray(entries) || !exact_size(root, "total", &total)) {
        cJSON_Delete(root);
        return RSS_JSON_INVALID;
    }
    int count = cJSON_GetArraySize(entries);
    if (count < 0 || count > RSS_PAGE_ENTRY_MAX) {
        cJSON_Delete(root);
        return RSS_JSON_TOO_LARGE;
    }
    result = RSS_JSON_INVALID;
    bool valid = true;
    for (int i = 0; i < count && valid; ++i) {
        valid = parse_entry(cJSON_GetArrayItem(entries, i), out, &out->items[i],
                            include_body, &result);
    }
    if (valid) {
        out->count = (size_t)count;
        out->total = total;
        result = RSS_JSON_OK;
    }
    cJSON_Delete(root);
    if (result != RSS_JSON_OK) memset(out, 0, sizeof(*out));
    return result;
}

rss_json_result_t rss_json_parse_entries(const char *json, size_t length, rss_entry_page_t *out) {
    return parse_entries(json, length, out, true);
}

rss_json_result_t rss_json_parse_metadata(const char *json, size_t length, rss_entry_page_t *out) {
    return parse_entries(json, length, out, false);
}

rss_json_result_t rss_json_parse_entry(const char *json, size_t length, rss_entry_page_t *out) {
    if (!out) return RSS_JSON_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    cJSON *root = NULL;
    rss_json_result_t result = parse_root(json, length, &root);
    if (result != RSS_JSON_OK) return result;
    result = RSS_JSON_INVALID;
    if (parse_entry(root, out, &out->items[0], true, &result)) {
        out->count = out->total = 1;
        result = RSS_JSON_OK;
    }
    cJSON_Delete(root);
    if (result != RSS_JSON_OK) memset(out, 0, sizeof(*out));
    return result;
}
