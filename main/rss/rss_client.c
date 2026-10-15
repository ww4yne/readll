/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：实现使用 ESP 证书包的同步 Miniflux HTTPS 请求与有界响应读取。
 *
 * English: Implements synchronous Miniflux HTTPS requests and bounded response reads using the
 * ESP certificate bundle.
 *
 * 冻结：不跟随重定向、不记录请求头，所有响应在解析前必须完整接收。
 * Frozen: redirects are not followed, request headers are not logged, and responses must be
 * completely received before parsing.
 */
#include "rss_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"

#define RSS_CLIENT_ME_RESPONSE_MAX 4096
#define RSS_CLIENT_CATEGORY_RESPONSE_MAX (16U * 1024U)
#define RSS_CLIENT_WRITE_RESPONSE_MAX 1024

static void clear_error(rss_client_error_t *error) {
    if (error) memset(error, 0, sizeof(*error));
}

static bool fail(rss_client_error_t *error, rss_client_code_t code, esp_err_t transport,
                 int status) {
    if (error) {
        error->code = code;
        error->transport_error = transport;
        error->http_status = status;
    }
    return false;
}

bool rss_client_init(rss_client_t *client, const rss_config_t *config) {
    if (!client || !config || strncmp(config->base_url, "https://", 8) ||
        !config->auth_token[0] || config->page_size == 0 ||
        config->page_size > RSS_PAGE_ENTRY_MAX || config->timeout_ms < 1000) {
        return false;
    }
    memset(client, 0, sizeof(*client));
    client->config = *config;
    return true;
}

static int read_response(esp_http_client_handle_t http, char *response, size_t capacity,
                         rss_client_error_t *error) {
    size_t used = 0;
    unsigned again = 0;
    for (;;) {
        char discard[128];
        char *target = response ? response + used : discard;
        size_t room = response ? capacity - 1 - used : sizeof(discard);
        if (response && room == 0) {
            char extra;
            int extra_read = esp_http_client_read(http, &extra, 1);
            if (extra_read > 0) {
                fail(error, RSS_CLIENT_RESPONSE_TOO_LARGE, ESP_OK,
                     esp_http_client_get_status_code(http));
                return -1;
            }
            if (extra_read < 0 && extra_read != -ESP_ERR_HTTP_EAGAIN) {
                fail(error, RSS_CLIENT_TRANSPORT, (esp_err_t)-extra_read,
                     esp_http_client_get_status_code(http));
                return -1;
            }
            if (extra_read == -ESP_ERR_HTTP_EAGAIN && again++ < 8) continue;
            break;
        }
        int count = esp_http_client_read(http, target, (int)room);
        if (count > 0) {
            again = 0;
            if (response) {
                used += (size_t)count;
            } else {
                used += (size_t)count;
                if (used > RSS_CLIENT_WRITE_RESPONSE_MAX) {
                    fail(error, RSS_CLIENT_RESPONSE_TOO_LARGE, ESP_OK,
                         esp_http_client_get_status_code(http));
                    return -1;
                }
            }
            continue;
        }
        if (count == -ESP_ERR_HTTP_EAGAIN && again++ < 8) continue;
        if (count < 0) {
            fail(error, RSS_CLIENT_TRANSPORT, (esp_err_t)-count,
                 esp_http_client_get_status_code(http));
            return -1;
        }
        break;
    }
    if (!esp_http_client_is_complete_data_received(http)) {
        fail(error, RSS_CLIENT_INCOMPLETE, ESP_OK, esp_http_client_get_status_code(http));
        return -1;
    }
    if (response) response[used] = '\0';
    return (int)used;
}

static int request(const rss_client_t *client, esp_http_client_method_t method, const char *path,
                   const char *body, char *response, size_t response_capacity,
                   int expected_status, rss_client_error_t *error) {
    clear_error(error);
    if (!client || !path || path[0] != '/' || (response && response_capacity < 2)) {
        fail(error, RSS_CLIENT_INVALID_ARGUMENT, ESP_ERR_INVALID_ARG, 0);
        return -1;
    }
    char url[RSS_BASE_URL_MAX + 256];
    int url_length = snprintf(url, sizeof(url), "%s%s", client->config.base_url, path);
    if (url_length <= 0 || (size_t)url_length >= sizeof(url)) {
        fail(error, RSS_CLIENT_INVALID_ARGUMENT, ESP_ERR_INVALID_SIZE, 0);
        return -1;
    }
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = (int)client->config.timeout_ms,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t http = esp_http_client_init(&config);
    if (!http) {
        fail(error, RSS_CLIENT_TRANSPORT, ESP_ERR_NO_MEM, 0);
        return -1;
    }
    esp_err_t transport = esp_http_client_set_method(http, method);
    if (transport == ESP_OK)
        transport = esp_http_client_set_header(http, "X-Auth-Token", client->config.auth_token);
    if (transport == ESP_OK)
        transport = esp_http_client_set_header(http, "Accept", "application/json");
    if (transport == ESP_OK)
        transport = esp_http_client_set_header(http, "Accept-Encoding", "identity");
    if (transport == ESP_OK && body)
        transport = esp_http_client_set_header(http, "Content-Type", "application/json");
    size_t body_length = body ? strlen(body) : 0;
    if (transport == ESP_OK) transport = esp_http_client_open(http, (int)body_length);
    if (transport == ESP_OK && body_length) {
        int written = esp_http_client_write(http, body, (int)body_length);
        if (written != (int)body_length) transport = written < 0 ? (esp_err_t)-written : ESP_FAIL;
    }
    int result = -1;
    if (transport != ESP_OK) {
        fail(error, RSS_CLIENT_TRANSPORT, transport, 0);
        goto cleanup;
    }
    int64_t content_length = esp_http_client_fetch_headers(http);
    int status = esp_http_client_get_status_code(http);
    if (status != expected_status) {
        fail(error, RSS_CLIENT_HTTP_STATUS, ESP_OK, status);
        goto cleanup;
    }
    if (response && content_length >= 0 && (uint64_t)content_length >= response_capacity) {
        fail(error, RSS_CLIENT_RESPONSE_TOO_LARGE, ESP_OK, status);
        goto cleanup;
    }
    result = read_response(http, response, response_capacity, error);

cleanup:
    esp_http_client_cleanup(http);
    return result;
}

static bool parse_result(int length, rss_json_result_t parsed, rss_client_error_t *error) {
    if (length < 0) return false;
    if (parsed == RSS_JSON_OK) return true;
    if (error) {
        error->code =
            parsed == RSS_JSON_TOO_LARGE ? RSS_CLIENT_RESPONSE_TOO_LARGE : RSS_CLIENT_JSON;
        error->json_error = parsed;
    }
    return false;
}

bool rss_client_get_me(const rss_client_t *client, rss_user_t *out, rss_client_error_t *error) {
    if (!out) return fail(error, RSS_CLIENT_INVALID_ARGUMENT, ESP_ERR_INVALID_ARG, 0);
    char *response = malloc(RSS_CLIENT_ME_RESPONSE_MAX + 1);
    if (!response) return fail(error, RSS_CLIENT_TRANSPORT, ESP_ERR_NO_MEM, 0);
    int length = request(client, HTTP_METHOD_GET, "/v1/me", NULL, response,
                         RSS_CLIENT_ME_RESPONSE_MAX + 1, 200, error);
    rss_json_result_t parsed = length >= 0
                                   ? rss_json_parse_user(response, (size_t)length, out)
                                   : RSS_JSON_INVALID_ARGUMENT;
    free(response);
    return parse_result(length, parsed, error);
}

bool rss_client_get_categories(const rss_client_t *client, rss_category_list_t *out,
                               rss_client_error_t *error) {
    if (!out) return fail(error, RSS_CLIENT_INVALID_ARGUMENT, ESP_ERR_INVALID_ARG, 0);
    char *response = malloc(RSS_CLIENT_CATEGORY_RESPONSE_MAX + 1);
    if (!response) return fail(error, RSS_CLIENT_TRANSPORT, ESP_ERR_NO_MEM, 0);
    int length = request(client, HTTP_METHOD_GET, "/v1/categories", NULL, response,
                         RSS_CLIENT_CATEGORY_RESPONSE_MAX + 1, 200, error);
    rss_json_result_t parsed = length >= 0
                                   ? rss_json_parse_categories(response, (size_t)length, out)
                                   : RSS_JSON_INVALID_ARGUMENT;
    free(response);
    return parse_result(length, parsed, error);
}

bool rss_client_get_entries(const rss_client_t *client, const rss_entries_query_t *query,
                            char *scratch, size_t scratch_size, rss_entry_page_t *out,
                            rss_client_error_t *error) {
    if (!client || !query || !scratch || scratch_size < RSS_JSON_RESPONSE_MAX + 1 || !out ||
        query->limit == 0 || query->limit > RSS_PAGE_ENTRY_MAX || query->category_id < 0 ||
        query->changed_after < 0 || query->after_entry_id < 0) {
        return fail(error, RSS_CLIENT_INVALID_ARGUMENT, ESP_ERR_INVALID_ARG, 0);
    }
    char path[256];
    int length = snprintf(path, sizeof(path),
                          "/v1/entries?order=id&direction=asc&limit=%u&changed_after=%lld",
                          query->limit, (long long)query->changed_after);
    if (length <= 0 || (size_t)length >= sizeof(path)) {
        return fail(error, RSS_CLIENT_INVALID_ARGUMENT, ESP_ERR_INVALID_SIZE, 0);
    }
    if (query->after_entry_id) {
        int appended = snprintf(path + length, sizeof(path) - (size_t)length,
                                "&after_entry_id=%lld", (long long)query->after_entry_id);
        if (appended <= 0 || (size_t)appended >= sizeof(path) - (size_t)length)
            return fail(error, RSS_CLIENT_INVALID_ARGUMENT, ESP_ERR_INVALID_SIZE, 0);
        length += appended;
    }
    if (query->category_id) {
        int appended = snprintf(path + length, sizeof(path) - (size_t)length, "&category_id=%lld",
                                (long long)query->category_id);
        if (appended <= 0 || (size_t)appended >= sizeof(path) - (size_t)length)
            return fail(error, RSS_CLIENT_INVALID_ARGUMENT, ESP_ERR_INVALID_SIZE, 0);
    }
    int received = request(client, HTTP_METHOD_GET, path, NULL, scratch, scratch_size, 200, error);
    rss_json_result_t parsed = received >= 0
                                   ? rss_json_parse_entries(scratch, (size_t)received, out)
                                   : RSS_JSON_INVALID_ARGUMENT;
    return parse_result(received, parsed, error);
}

bool rss_client_set_read(const rss_client_t *client, const int64_t *entry_ids, size_t count,
                         bool read, rss_client_error_t *error) {
    if (!client || !entry_ids || count == 0 || count > RSS_CLIENT_BATCH_MAX) {
        return fail(error, RSS_CLIENT_INVALID_ARGUMENT, ESP_ERR_INVALID_ARG, 0);
    }
    char body[RSS_CLIENT_BATCH_MAX * 24 + 48];
    size_t used = (size_t)snprintf(body, sizeof(body), "{\"entry_ids\":[");
    for (size_t i = 0; i < count; ++i) {
        if (entry_ids[i] <= 0 || used >= sizeof(body)) {
            return fail(error, RSS_CLIENT_INVALID_ARGUMENT, ESP_ERR_INVALID_ARG, 0);
        }
        int written = snprintf(body + used, sizeof(body) - used, "%s%lld", i ? "," : "",
                               (long long)entry_ids[i]);
        if (written <= 0 || (size_t)written >= sizeof(body) - used)
            return fail(error, RSS_CLIENT_INVALID_ARGUMENT, ESP_ERR_INVALID_SIZE, 0);
        used += (size_t)written;
    }
    int written = snprintf(body + used, sizeof(body) - used, "],\"status\":\"%s\"}",
                           read ? "read" : "unread");
    if (written <= 0 || (size_t)written >= sizeof(body) - used)
        return fail(error, RSS_CLIENT_INVALID_ARGUMENT, ESP_ERR_INVALID_SIZE, 0);
    return request(client, HTTP_METHOD_PUT, "/v1/entries", body, NULL, 0, 204, error) >= 0;
}

bool rss_client_toggle_bookmark(const rss_client_t *client, int64_t entry_id,
                                rss_client_error_t *error) {
    if (!client || entry_id <= 0)
        return fail(error, RSS_CLIENT_INVALID_ARGUMENT, ESP_ERR_INVALID_ARG, 0);
    char path[80];
    int length = snprintf(path, sizeof(path), "/v1/entries/%lld/bookmark", (long long)entry_id);
    if (length <= 0 || (size_t)length >= sizeof(path))
        return fail(error, RSS_CLIENT_INVALID_ARGUMENT, ESP_ERR_INVALID_SIZE, 0);
    return request(client, HTTP_METHOD_PUT, path, NULL, NULL, 0, 204, error) >= 0;
}
