/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：直接测试生产客户端、同步与仅 metadata 文件，HTTP 传输由固定响应替代。
 * English: Test the production client, sync and metadata-only files with a deterministic HTTP stub.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "rss_sync.h"

struct FakeHttp { size_t offset; };
static struct FakeHttp http;
static const char *response;
static int status = 200;
static bool complete = true;
static bool transport_fail;
static unsigned article_requests;
static char last_url[512];
static char *entry_json;
static char *list_json;

int esp_crt_bundle_attach(void *unused) { (void)unused; return ESP_OK; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    assert(config->disable_auto_redirect && config->crt_bundle_attach);
    snprintf(last_url, sizeof(last_url), "%s", config->url);
    if (strstr(last_url, "/v1/me"))
        response = "{\"id\":9,\"username\":\"test\"}";
    else if (strstr(last_url, "/v1/categories"))
        response = "[]";
    else if (strstr(last_url, "/v1/entries?"))
        response = list_json;
    else {
        ++article_requests;
        response = entry_json;
    }
    http.offset = 0;
    return &http;
}
esp_err_t esp_http_client_set_method(esp_http_client_handle_t handle,
                                      esp_http_client_method_t method) {
    (void)handle; (void)method; return ESP_OK;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t handle,
                                      const char *key, const char *value) {
    (void)handle; (void)key; (void)value; return ESP_OK;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t handle, int length) {
    (void)handle; (void)length; return transport_fail ? ESP_FAIL : ESP_OK;
}
int esp_http_client_write(esp_http_client_handle_t handle, const char *body, int length) {
    (void)handle; (void)body; return length;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t handle) {
    (void)handle; return (int64_t)strlen(response);
}
int esp_http_client_get_status_code(esp_http_client_handle_t handle) {
    (void)handle; return status;
}
int esp_http_client_read(esp_http_client_handle_t handle, char *buffer, int capacity) {
    size_t count = strlen(response) - handle->offset;
    if (count > (size_t)capacity) count = (size_t)capacity;
    memcpy(buffer, response + handle->offset, count);
    handle->offset += count;
    return (int)count;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t handle) {
    (void)handle; return complete;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t handle) {
    (void)handle; return ESP_OK;
}

static char *read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    assert(size >= 0 && fseek(file, 0, SEEK_SET) == 0);
    char *data = calloc((size_t)size + 1, 1);
    assert(data && fread(data, 1, (size_t)size, file) == (size_t)size);
    assert(fclose(file) == 0);
    *length = (size_t)size;
    return data;
}

static uint32_t crc32(uint32_t crc, const void *data, size_t size) {
    const unsigned char *bytes = data;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (uint32_t)-(int32_t)(crc & 1));
    }
    return crc;
}

static void write_legacy(const char *path, const rss_cache_metadata_t *metadata) {
    struct {
        uint32_t magic;
        uint16_t version, header_size, metadata_size, body_size;
        uint32_t metadata_count, body_count, body_used;
        int64_t cursor;
        uint64_t sequence;
        uint32_t crc;
    } header = {0};
    struct { int64_t id; uint32_t offset, length; uint64_t touched; } body = {101, 0, 11, 1};
    const char content[] = "legacy body";
    header.magic = UINT32_C(0x52535331);
    header.version = 2;
    header.header_size = (uint16_t)sizeof(header);
    header.metadata_size = (uint16_t)sizeof(*metadata);
    header.body_size = (uint16_t)sizeof(body);
    header.metadata_count = header.body_count = 1;
    header.body_used = sizeof(content);
    header.cursor = 1234;
    header.sequence = 10;
    uint32_t crc = crc32(UINT32_MAX, metadata, sizeof(*metadata));
    crc = crc32(crc, &body, sizeof(body));
    header.crc = crc32(crc, content, sizeof(content)) ^ UINT32_MAX;
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(&header, 1, sizeof(header), file) == sizeof(header));
    assert(fwrite(metadata, 1, sizeof(*metadata), file) == sizeof(*metadata));
    assert(fwrite(&body, 1, sizeof(body), file) == sizeof(body));
    assert(fwrite(content, 1, sizeof(content), file) == sizeof(content));
    assert(fclose(file) == 0);
}

int main(int argc, char **argv) {
    assert(argc == 4);
    size_t length;
    entry_json = read_file(argv[1], &length);
    list_json = calloc(length + 64, 1);
    assert(list_json);
    snprintf(list_json, length + 64, "{\"total\":1,\"entries\":[%s]}", entry_json);
    char *scratch = malloc(RSS_JSON_RESPONSE_MAX + 1);
    rss_entry_page_t *page = calloc(1, sizeof(*page));
    rss_cache_t *cache = calloc(1, sizeof(*cache));
    rss_cache_t *loaded = calloc(1, sizeof(*loaded));
    assert(scratch && page && cache && loaded);
    rss_config_t config;
    rss_client_t client;
    rss_client_error_t error;
    assert(rss_config_init(&config, "https://rss.example", "test-token", 20, 10000));
    assert(rss_client_init(&client, &config));
    rss_sync_t sync;
    assert(rss_sync_init(&sync, &client, cache, argv[2], scratch,
                          RSS_JSON_RESPONSE_MAX + 1, page));
    assert(rss_sync_begin(&sync));
    while (sync.state != RSS_SYNC_COMPLETE)
        assert(rss_sync_step(&sync) != RSS_SYNC_STEP_ERROR);
    assert(article_requests == 0);
    assert(page->body_used == 0 && cache->metadata_count == 1);
    assert(rss_cache_load(argv[2], loaded) == RSS_CACHE_OK);
    assert(loaded->metadata_count == 1 && loaded->metadata[0].entry.body_length == 0);
    char *saved = read_file(argv[2], &length);
    assert(length < sizeof(*cache) + 128);
    for (size_t i = 0; i + strlen("selected body") <= length; ++i)
        assert(memcmp(saved + i, "selected body", strlen("selected body")));
    free(saved);

    assert(rss_client_get_entry(&client, 101, scratch, RSS_JSON_RESPONSE_MAX + 1, page, &error));
    assert(article_requests == 1 && strstr(last_url, "/v1/entries/101"));
    assert(!strcmp(rss_entry_body(page, &page->items[0]), "<p>selected body</p>"));
    assert(!rss_client_get_entry(&client, 102, scratch, RSS_JSON_RESPONSE_MAX + 1, page, &error));
    assert(error.code == RSS_CLIENT_JSON && page->count == 0);
    status = 404;
    assert(!rss_client_get_entry(&client, 101, scratch, RSS_JSON_RESPONSE_MAX + 1, page, &error));
    assert(error.code == RSS_CLIENT_HTTP_STATUS && page->count == 0);
    status = 200;
    complete = false;
    assert(!rss_client_get_entry(&client, 101, scratch, RSS_JSON_RESPONSE_MAX + 1, page, &error));
    assert(error.code == RSS_CLIENT_INCOMPLETE && page->count == 0);
    complete = true;
    transport_fail = true;
    assert(!rss_client_get_entry(&client, 101, scratch, RSS_JSON_RESPONSE_MAX + 1, page, &error));
    assert(error.code == RSS_CLIENT_TRANSPORT && page->count == 0);
    transport_fail = false;

    write_legacy(argv[3], &cache->metadata[0]);
    assert(rss_cache_load(argv[3], loaded) == RSS_CACHE_OK);
    assert(loaded->metadata_count == 1 && loaded->metadata[0].entry.id == 101);
    assert(loaded->cursor_changed_at == 1234);
    saved = read_file(argv[3], &length);
    assert((unsigned char)saved[4] == 3);
    for (size_t i = 0; i + strlen("legacy body") <= length; ++i)
        assert(memcmp(saved + i, "legacy body", strlen("legacy body")));
    free(saved);

    free(entry_json); free(list_json); free(scratch); free(page); free(cache); free(loaded);
    puts("RSS production on-demand tests PASS (metadata sync, selected fetch, errors, body-free disk, legacy migration)");
    return 0;
}
