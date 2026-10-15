/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：在 host 或 IDF 测试目标中把共享 fixture 直接送入生产 RSS JSON 解析器。
 *
 * English: Feeds shared fixtures directly into the production RSS JSON parser in host or IDF tests.
 *
 * 冻结：只调用生产解析 API，不复制解析逻辑。
 * Frozen: call the production parser API without duplicating parser logic.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rss_json.h"

static char *read_fixture(const char *directory, const char *name, size_t *length) {
    char path[512];
    assert(snprintf(path, sizeof(path), "%s/%s", directory, name) > 0);
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    assert(size > 0 && fseek(file, 0, SEEK_SET) == 0);
    char *data = malloc((size_t)size + 1);
    assert(data);
    assert(fread(data, 1, (size_t)size, file) == (size_t)size);
    assert(fclose(file) == 0);
    data[size] = 0;
    *length = (size_t)size;
    return data;
}

static void user_case(const char *directory) {
    size_t length;
    char *json = read_fixture(directory, "me_unknown.json", &length);
    rss_user_t user;
    assert(rss_json_parse_user(json, length, &user) == RSS_JSON_OK);
    assert(user.id == 9 && !strcmp(user.username, "wayne"));
    free(json);
}

static void category_case(const char *directory) {
    size_t length;
    char *json = read_fixture(directory, "categories_unknown.json", &length);
    rss_category_list_t categories;
    assert(rss_json_parse_categories(json, length, &categories) == RSS_JSON_OK);
    assert(categories.count == 2 && categories.items[1].id == 2);
    free(json);
}

static void entry_case(const char *directory, const char *name, rss_json_result_t expected) {
    size_t length;
    char *json = read_fixture(directory, name, &length);
    rss_entry_page_t *page = calloc(1, sizeof(*page));
    assert(page);
    assert(rss_json_parse_entries(json, length, page) == expected);
    if (expected == RSS_JSON_OK) {
        assert(page->count == 1 && page->items[0].id == 101);
        assert(!strcmp(rss_entry_body(page, &page->items[0]), "<p>body</p>"));
    }
    free(page);
    free(json);
}

static void token_boundary_case(void) {
    rss_config_t config;
    char token[257];
    memset(token, 'a', 255);
    token[255] = 0;
    assert(rss_config_init(&config, "https://rss.example", token, 20, 10000));
    assert(strlen(config.auth_token) == 255);
    token[255] = 'a';
    token[256] = 0;
    assert(!rss_config_init(&config, "https://rss.example", token, 20, 10000));
}

int main(int argc, char **argv) {
    assert(argc == 3);
    user_case(argv[1]);
    category_case(argv[1]);
    entry_case(argv[1], "entries_unknown.json", RSS_JSON_OK);
    entry_case(argv[1], "entries_truncated.json", RSS_JSON_TRUNCATED);
    entry_case(argv[2], "generated_entries_oversized.json", RSS_JSON_TOO_LARGE);
    token_boundary_case();
    puts("RSS production C parser fixtures PASS");
    return 0;
}
