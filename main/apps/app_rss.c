/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：RSS 产品页负责未读队列、共享阅读排版和分片 Miniflux 同步。
 *
 * English: The RSS product page owns the unread queue, shared reader layout, and stepped
 * Miniflux synchronization.
 *
 * 冻结：render 只绘制；不回显令牌、不下载图片、不发送盲目状态切换。
 * Frozen: render only paints; never echo tokens, download images, or send blind state toggles.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "app_font_context.h"
#include "app_registry.h"
#include "book_layout.h"
#include "book_store.h"
#include "esp_heap_caps.h"
#include "html_text.h"
#include "read_pico_transfer.h"
#include "rss_cache.h"
#include "rss_client.h"
#include "rss_preferences.h"
#include "rss_sync.h"
#include "settings.h"
#include "ttf_font.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_nav.h"

#define RSS_CACHE_FILE "/flash/rss.cache"
#define RSS_PENDING_FILE "/flash/rss.pending"
#define RSS_LIST_ROWS 12
#define RSS_LIST_TOP 156
#define RSS_LIST_BOTTOM 1092
#define RSS_LIST_ROW_HEIGHT ((RSS_LIST_BOTTOM - RSS_LIST_TOP) / RSS_LIST_ROWS)
#define RSS_FOOTER_TOP RSS_LIST_BOTTOM
#define RSS_TITLE_PX 24
#define RSS_SOURCE_ICON_SIZE 44
#define RSS_PROGRESS_HEIGHT 4
#define RSS_GRAY_DARK 0x40
#define RSS_TICK_MS 250
#define RSS_NETWORK_POLL_MS 500
#define RSS_REFRESH_MS (5 * 60 * 1000)

typedef enum {
    RSS_VIEW_LIST = 0,
    RSS_VIEW_ARTICLE,
    RSS_VIEW_SETTINGS,
} rss_view_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    rss_sync_operation_t operations[RSS_SYNC_OPERATION_MAX];
} rss_pending_file_t;

static rss_cache_t *s_cache;
static rss_entry_page_t *s_page_buffer;
static char *s_scratch;
static rss_client_t s_client;
static rss_sync_t s_sync;
static rss_view_t s_view;
static size_t s_visible[RSS_CACHE_METADATA_MAX];
static size_t s_visible_count;
static int s_list_page;
static html_text_t s_article_text;
static size_t s_article_page_count;
static size_t s_article_page;
static rss_preferences_t s_preferences;
static bool s_configured;
static bool s_config_invalid;
static bool s_storage_ready;
static bool s_network_owned;
static bool s_time_ready;
static bool s_sync_requested;
static int64_t s_next_tick_ms;
static int64_t s_next_network_poll_ms;
static int64_t s_next_refresh_ms;
static uint32_t s_network_signature;
static read_pico_transfer_status_t s_network_status;
static char s_notice[128];

static void close_article(void);

static void secure_clear(void *data, size_t size) {
    volatile unsigned char *p = data;
    while (size--) *p++ = 0;
}

static const rss_cache_metadata_t *metadata_by_id(int64_t id) {
    if (!s_cache || id <= 0) return NULL;
    for (size_t i = 0; i < s_cache->metadata_count; ++i)
        if (s_cache->metadata[i].entry.id == id) return &s_cache->metadata[i];
    return NULL;
}

static const rss_sync_operation_t *pending_by_id(int64_t id) {
    for (size_t i = 0; i < s_sync.operation_count; ++i)
        if (s_sync.operations[i].entry_id == id) return &s_sync.operations[i];
    return NULL;
}

static bool effective_read(const rss_cache_metadata_t *metadata, bool *pending) {
    const rss_sync_operation_t *operation = pending_by_id(metadata->entry.id);
    if (pending) *pending = operation && operation->desired.read_known;
    return operation && operation->desired.read_known
               ? operation->desired.read
               : metadata->entry.status == RSS_ENTRY_STATUS_READ;
}

static bool visible_entry(const rss_cache_metadata_t *metadata) {
    return !effective_read(metadata, NULL);
}

static void rebuild_visible(void) {
    s_visible_count = 0;
    if (!s_cache) return;
    for (size_t i = 0; i < s_cache->metadata_count; ++i) {
        const rss_cache_metadata_t *candidate = &s_cache->metadata[i];
        if (!visible_entry(candidate)) continue;
        size_t at = s_visible_count;
        while (at > 0) {
            const rss_entry_t *before = &s_cache->metadata[s_visible[at - 1]].entry;
            if (before->published_at > candidate->entry.published_at ||
                (before->published_at == candidate->entry.published_at &&
                 before->id > candidate->entry.id)) {
                break;
            }
            s_visible[at] = s_visible[at - 1];
            --at;
        }
        s_visible[at] = i;
        ++s_visible_count;
    }
    int pages = (int)((s_visible_count + RSS_LIST_ROWS - 1) / RSS_LIST_ROWS);
    if (pages == 0) pages = 1;
    if (s_list_page >= pages) s_list_page = pages - 1;
}

static bool pending_read_file(const char *path, rss_pending_file_t *file) {
    memset(file, 0, sizeof(*file));
    FILE *input = fopen(path, "rb");
    bool ok = input && fread(file, 1, sizeof(*file), input) == sizeof(*file) &&
              fgetc(input) == EOF;
    if (input && fclose(input) != 0) ok = false;
    if (!ok || file->magic != UINT32_C(0x52535031) || file->version != 1 ||
        file->count > RSS_SYNC_OPERATION_MAX) {
        return false;
    }
    for (size_t i = 0; i < file->count; ++i) {
        const rss_sync_operation_t *operation = &file->operations[i];
        if (operation->entry_id <= 0 ||
            (!operation->desired.read_known && !operation->desired.bookmark_known) ||
            (operation->bookmark_confirmation_pending &&
             !operation->desired.bookmark_known)) {
            return false;
        }
        for (size_t j = 0; j < i; ++j)
            if (file->operations[j].entry_id == operation->entry_id) return false;
    }
    return true;
}

static bool remove_if_present(const char *path) {
    return remove(path) == 0 || errno == ENOENT;
}

static bool pending_save(void) {
    if (!s_storage_ready) return false;
    rss_pending_file_t *file = heap_caps_calloc(1, sizeof(*file),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!file) return false;
    file->magic = UINT32_C(0x52535031);
    file->version = 1;
    file->count = (uint16_t)s_sync.operation_count;
    memcpy(file->operations, s_sync.operations,
           s_sync.operation_count * sizeof(s_sync.operations[0]));
    char temporary[] = RSS_PENDING_FILE ".tmp";
    char backup[] = RSS_PENDING_FILE ".bak";
    (void)remove(temporary);
    FILE *output = fopen(temporary, "wb");
    bool ok = output && fwrite(file, 1, sizeof(*file), output) == sizeof(*file) &&
              fflush(output) == 0 && fsync(fileno(output)) == 0;
    if (output && fclose(output) != 0) ok = false;
    if (!ok) {
        free(file);
        remove(temporary);
        return false;
    }
    if (!pending_read_file(temporary, file)) {
        free(file);
        remove(temporary);
        return false;
    }
    bool primary_valid = pending_read_file(RSS_PENDING_FILE, file);
    bool backup_valid = pending_read_file(backup, file);
    if (primary_valid) {
        if (!remove_if_present(backup) || rename(RSS_PENDING_FILE, backup) != 0) {
            free(file);
            remove(temporary);
            return false;
        }
    } else {
        if (!remove_if_present(RSS_PENDING_FILE)) {
            free(file);
            remove(temporary);
            return false;
        }
        if (!backup_valid) (void)remove_if_present(backup);
    }
    if (rename(temporary, RSS_PENDING_FILE) != 0) {
        remove(temporary);
        if (primary_valid) (void)rename(backup, RSS_PENDING_FILE);
        free(file);
        return false;
    }
    (void)remove_if_present(backup);
    free(file);
    return true;
}

static bool pending_restore(const rss_pending_file_t *file) {
    size_t original_count = s_sync.operation_count;
    uint32_t original_revision = s_sync.operation_revision;
    for (size_t i = 0; i < file->count; ++i) {
        if (!rss_sync_restore_operation(&s_sync, &file->operations[i])) {
            s_sync.operation_count = original_count;
            s_sync.operation_revision = original_revision;
            return false;
        }
    }
    return true;
}

static void pending_load(void) {
    rss_pending_file_t *file = heap_caps_malloc(sizeof(*file),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!file) return;
    if (pending_read_file(RSS_PENDING_FILE, file)) {
        (void)pending_restore(file);
    } else {
        char backup[] = RSS_PENDING_FILE ".bak";
        if (pending_read_file(backup, file) && pending_restore(file)) {
            (void)remove_if_present(RSS_PENDING_FILE);
            (void)rename(backup, RSS_PENDING_FILE);
        }
    }
    free(file);
}

static bool allocate_core(void) {
    s_cache = heap_caps_calloc(1, sizeof(*s_cache), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_page_buffer = heap_caps_calloc(1, sizeof(*s_page_buffer),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_scratch = heap_caps_malloc(RSS_JSON_RESPONSE_MAX + 1,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_cache && s_page_buffer && s_scratch;
}

static void free_core(void) {
    close_article();
    free(s_scratch);
    free(s_page_buffer);
    free(s_cache);
    s_scratch = NULL;
    s_page_buffer = NULL;
    s_cache = NULL;
    secure_clear(&s_client, sizeof(s_client));
    memset(&s_sync, 0, sizeof(s_sync));
}

static bool prepare_storage(void) {
    if (book_store_flash_ready()) return true;
    book_store_root_t roots[BOOK_STORE_ROOT_MAX];
    int count = 0;
    return book_store_roots(roots, &count) == ESP_OK && book_store_flash_ready();
}

static void load_cache(void) {
    rss_cache_init(s_cache);
    struct stat st;
    bool existed = stat(RSS_CACHE_FILE, &st) == 0;
    rss_cache_result_t result = rss_cache_load(RSS_CACHE_FILE, s_cache);
    if (result != RSS_CACHE_OK) {
        rss_cache_init(s_cache);
        if (existed)
            snprintf(s_notice, sizeof(s_notice), "%s",
                     result == RSS_CACHE_CORRUPT ? "缓存损坏，已忽略" : "缓存读取失败");
    }
}

static bool load_configuration(void) {
    char base[READ_PICO_MINIFLUX_BASE_URL_MAX + 1] = {0};
    char token[READ_PICO_MINIFLUX_TOKEN_MAX + 1] = {0};
    esp_err_t result = read_pico_transfer_miniflux_get(base, sizeof(base), token, sizeof(token));
    if (result != ESP_OK) {
        secure_clear(token, sizeof(token));
        return false;
    }
    rss_config_t config;
    bool valid = rss_config_init(&config, base, token, 20, 10000) &&
                 rss_client_init(&s_client, &config);
    s_config_invalid = !valid;
    secure_clear(token, sizeof(token));
    secure_clear(&config, sizeof(config));
    return valid;
}

static void request_network(void) {
    read_pico_transfer_status_t status = {0};
    read_pico_transfer_get_status(&status);
    s_network_status = status;
    if (status.mode == READ_PICO_TRANSFER_MODE_STA && status.network_ready) {
        s_sync_requested = true;
        return;
    }
    if (status.state != READ_PICO_TRANSFER_STOPPED) return;
    char ssid[33] = {0};
    bool saved = false;
    if (read_pico_transfer_get_saved_wifi(ssid, &saved) != ESP_OK || !saved) return;
    const read_pico_transfer_cfg_t config = {
        .mode = READ_PICO_TRANSFER_MODE_STA,
        .network_only = true,
    };
    if (read_pico_transfer_start(&config) == ESP_OK) s_network_owned = true;
}

static void format_date(int64_t timestamp, char out[24]) {
    time_t value = (time_t)timestamp;
    struct tm local = {0};
    if (timestamp > 0 && localtime_r(&value, &local))
        strftime(out, 24, "%Y-%m-%d %H:%M", &local);
    else
        snprintf(out, 24, "时间未知");
}

static bool append_text(char *buffer, size_t capacity, size_t *used, const char *text) {
    size_t length = strlen(text);
    if (length >= capacity - *used) return false;
    memcpy(buffer + *used, text, length);
    *used += length;
    buffer[*used] = 0;
    return true;
}

static bool append_html_escaped(char *buffer, size_t capacity, size_t *used, const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        const char *escaped = NULL;
        if (*p == '&') escaped = "&amp;";
        else if (*p == '<') escaped = "&lt;";
        else if (*p == '>') escaped = "&gt;";
        else if (*p == '"') escaped = "&quot;";
        else if (*p == '\'') escaped = "&#39;";
        if (escaped) {
            if (!append_text(buffer, capacity, used, escaped)) return false;
        } else {
            if (*used + 1 >= capacity) return false;
            buffer[(*used)++] = (char)*p;
            buffer[*used] = 0;
        }
    }
    return true;
}

static EpdRect article_rect(void) {
    int margin = s_preferences.margin_px;
    return (EpdRect){margin, 52, UI_LOCK_WIDTH - margin * 2, RSS_FOOTER_TOP - 52};
}

static void configure_article_layout(void) {
    book_layout_set_image_dims(NULL, NULL);
    book_layout_set_runs(s_article_text.runs, s_article_text.run_count);
    book_layout_set_spacing(s_preferences.line_percent, s_preferences.paragraph_percent);
    book_layout_set_typography(((int)s_preferences.tracking - 2) * 2);
    book_layout_set_first_line_indent(s_preferences.indent_em);
    book_layout_set_first_line_indent_adjust(0);
    book_layout_set_reading_line(s_preferences.reading_line);
    book_layout_set_reading_line_offset(0);
    book_layout_set_images_visible(false);
    book_layout_set_vertical(false);
    book_layout_set_chapter_lead(0, 0);
    book_layout_set_chapter_lead_width(0);
}

static void close_article(void) {
    book_layout_set_runs(NULL, 0);
    book_layout_free();
    html_text_free(&s_article_text);
    s_article_page = 0;
    s_article_page_count = 1;
    app_font_activate_system();
}

static bool open_article(int64_t id) {
    const rss_cache_metadata_t *metadata = metadata_by_id(id);
    if (!metadata) return false;
    size_t body_length = 0;
    const char *body = rss_cache_get_body(s_cache, id, &body_length);
    const char *source = metadata->entry.feed_title[0] ? metadata->entry.feed_title :
                         metadata->entry.author[0] ? metadata->entry.author : "RSS";
    char date[24];
    format_date(metadata->entry.published_at, date);
    size_t capacity = body_length + strlen(metadata->entry.title) * 6 +
                      strlen(source) * 6 + 256;
    char *document = heap_caps_calloc(1, capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!document) {
        snprintf(s_notice, sizeof(s_notice), "正文内存不足");
        return false;
    }
    size_t used = 0;
    bool ok = append_text(document, capacity, &used, "<h1>") &&
              append_html_escaped(document, capacity, &used,
                                  metadata->entry.title[0] ? metadata->entry.title : "无标题") &&
              append_text(document, capacity, &used, "</h1><p>") &&
              append_html_escaped(document, capacity, &used, source) &&
              append_text(document, capacity, &used, " · ") &&
              append_html_escaped(document, capacity, &used, date) &&
              append_text(document, capacity, &used, "</p>") &&
              append_text(document, capacity, &used,
                          body && body_length ? body : "<p>正文未缓存</p>");
    if (!ok) {
        free(document);
        snprintf(s_notice, sizeof(s_notice), "正文过长");
        return false;
    }
    close_article();
    app_font_activate_reading();
    ttf_set_stroke_adjust_fine(s_preferences.weight * APP_BOOK_WEIGHT_SUBSTEPS);
    if (html_to_blocks(document, used, &s_article_text) != ESP_OK) {
        free(document);
        app_font_activate_system();
        snprintf(s_notice, sizeof(s_notice), "正文清洗失败");
        return false;
    }
    free(document);
    configure_article_layout();
    if (!book_layout_build_blocks(s_article_text.utf8, s_article_text.len,
                                  s_article_text.blocks, s_article_text.count,
                                  article_rect(), s_preferences.px)) {
        close_article();
        snprintf(s_notice, sizeof(s_notice), "正文排版失败");
        return false;
    }
    s_article_page = 0;
    s_article_page_count = book_layout_page_count();
    if (!s_article_page_count) s_article_page_count = 1;
    s_view = RSS_VIEW_ARTICLE;
    return true;
}

static EpdRect settings_action_rect(void) {
    return (EpdRect){536, 73, 48, 48};
}

static EpdRect read_all_action_rect(void) {
    return (EpdRect){600, 73, 48, 48};
}

static void draw_double_check(uint8_t *fb, EpdRect rect) {
    for (int check = 0; check < 2; ++check) {
        int x = rect.x + 8 + check * 15;
        int y = rect.y + 24;
        for (int stroke = 0; stroke < 2; ++stroke) {
            epd_draw_line(x, y + stroke, x + 6, y + 7 + stroke, UI_GRAY_BLACK, fb);
            epd_draw_line(x + 6, y + 7 + stroke, x + 17, y - 7 + stroke,
                          UI_GRAY_BLACK, fb);
        }
    }
}

static void draw_gear(uint8_t *fb, EpdRect rect) {
    static const int8_t spokes[][4] = {
        {0, -12, 0, -19}, {0, 12, 0, 19}, {-12, 0, -19, 0}, {12, 0, 19, 0},
        {-9, -9, -14, -14}, {9, -9, 14, -14}, {-9, 9, -14, 14}, {9, 9, 14, 14},
    };
    int cx = rect.x + rect.width / 2;
    int cy = rect.y + rect.height / 2;
    epd_draw_circle(cx, cy, 11, UI_GRAY_BLACK, fb);
    epd_draw_circle(cx, cy, 10, UI_GRAY_BLACK, fb);
    epd_draw_circle(cx, cy, 4, UI_GRAY_BLACK, fb);
    for (size_t i = 0; i < sizeof(spokes) / sizeof(spokes[0]); ++i) {
        epd_draw_line(cx + spokes[i][0], cy + spokes[i][1],
                      cx + spokes[i][2], cy + spokes[i][3], UI_GRAY_BLACK, fb);
        epd_draw_line(cx + spokes[i][0] + 1, cy + spokes[i][1],
                      cx + spokes[i][2] + 1, cy + spokes[i][3], UI_GRAY_BLACK, fb);
    }
}

static void draw_footer(uint8_t *fb, const char *left, const char *middle,
                        const char *right) {
    ui_hairline(fb, RSS_FOOTER_TOP, 0, UI_LOCK_WIDTH, RSS_GRAY_DARK);
    ui_text_vc(fb, 66, 1152, 19, left, EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 342, 1152, 19, middle, EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 618, 1152, 19, right, EPD_DRAW_ALIGN_CENTER, false);
}

static void draw_article_progress(uint8_t *fb) {
    const size_t pages = s_article_page_count ? s_article_page_count : 1;
    const int filled = (int)(((s_article_page + 1) * UI_LOCK_WIDTH + pages - 1) / pages);
    const EpdRect track = {0, RSS_FOOTER_TOP, UI_LOCK_WIDTH, RSS_PROGRESS_HEIGHT};
    epd_fill_rect(track, 0xd0, fb);
    for (int step = 0; step < 16; ++step) {
        const int x0 = filled * step / 16;
        const int x1 = filled * (step + 1) / 16;
        if (x1 <= x0) continue;
        const uint8_t gray = (uint8_t)((15 - step) * 0xa0 / 15) & 0xf0;
        epd_fill_rect((EpdRect){x0, RSS_FOOTER_TOP, x1 - x0, RSS_PROGRESS_HEIGHT},
                      gray, fb);
    }
}

static void source_initial(const rss_entry_t *entry, char out[5]) {
    const char *source = entry->feed_title[0] ? entry->feed_title :
                         entry->author[0] ? entry->author : "R";
    unsigned char lead = (unsigned char)source[0];
    size_t bytes = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
    if (bytes > 4 || strnlen(source, bytes) < bytes) bytes = 1;
    memcpy(out, source, bytes);
    out[bytes] = 0;
    if (bytes == 1 && out[0] >= 'a' && out[0] <= 'z') out[0] -= 'a' - 'A';
}

static const char *list_footer_middle(char buffer[40]) {
    if (s_notice[0]) return s_notice;
    if (s_config_invalid) return "Miniflux 配置无效";
    if (!s_configured) return "未配置 Miniflux";
    if (!s_storage_ready) return "内部存储不可用";
    if (s_sync.state == RSS_SYNC_FAILED) {
        int status = s_sync.error.client_error.http_status;
        if (status == 401) return "认证失败";
        if (status >= 400) {
            snprintf(buffer, 40, "服务错误 %d", status);
            return buffer;
        }
        return "同步失败";
    }
    return "产品首页";
}

static void draw_list(uint8_t *fb) {
    ui_nav_status(fb);
    char header[48];
    snprintf(header, sizeof(header), "未读文章 (%u)", (unsigned)s_visible_count);
    ui_text_vc(fb, 36, 100, 34, header, EPD_DRAW_ALIGN_LEFT, false);
    EpdRect settings = settings_action_rect();
    EpdRect read_all = read_all_action_rect();
    ui_draw_control_frame(fb, settings, 8, RSS_GRAY_DARK);
    ui_draw_control_frame(fb, read_all, 8, RSS_GRAY_DARK);
    draw_gear(fb, settings);
    draw_double_check(fb, read_all);
    ui_hairline(fb, RSS_LIST_TOP - 1, 0, UI_LOCK_WIDTH, RSS_GRAY_DARK);
    size_t first = (size_t)s_list_page * RSS_LIST_ROWS;
    for (int row = 0; row < RSS_LIST_ROWS && first + (size_t)row < s_visible_count; ++row) {
        const rss_cache_metadata_t *metadata =
            &s_cache->metadata[s_visible[first + (size_t)row]];
        int y = RSS_LIST_TOP + row * RSS_LIST_ROW_HEIGHT;
        EpdRect icon = {36, y + (RSS_LIST_ROW_HEIGHT - RSS_SOURCE_ICON_SIZE) / 2,
                        RSS_SOURCE_ICON_SIZE, RSS_SOURCE_ICON_SIZE};
        ui_draw_control_frame(fb, icon, 7, RSS_GRAY_DARK);
        char initial[5];
        source_initial(&metadata->entry, initial);
        ui_text_vc(fb, icon.x + icon.width / 2, icon.y + icon.height / 2,
                   21, initial, EPD_DRAW_ALIGN_CENTER, true);
        char title[RSS_ENTRY_TITLE_MAX];
        snprintf(title, sizeof(title), "%s", metadata->entry.title[0]
                                                 ? metadata->entry.title : "无标题");
        (void)ui_text_title_fit(title, RSS_TITLE_PX, 548, title);
        ui_text_vc(fb, 94, y + RSS_LIST_ROW_HEIGHT / 2,
                   RSS_TITLE_PX, title, EPD_DRAW_ALIGN_LEFT, false);
        ui_hairline(fb, y + RSS_LIST_ROW_HEIGHT - 1, 36, 612, UI_GRAY_LIGHT);
    }
    if (!s_visible_count) {
        const char *empty = !s_cache || !s_cache->metadata_count ? "本地缓存为空" :
                            "没有未读文章";
        ui_text_fixed(fb, 342, 560, 30, empty, EPD_DRAW_ALIGN_CENTER, false);
    }
    char middle[40];
    draw_footer(fb, "上一页", list_footer_middle(middle), "下一页");
}

static void draw_article(uint8_t *fb) {
    ui_nav_status(fb);
    book_layout_draw_page(fb, s_article_page, article_rect(), s_preferences.px);
    char middle[40];
    snprintf(middle, sizeof(middle), "%u / %u · 返回列表",
             (unsigned)(s_article_page + 1), (unsigned)s_article_page_count);
    draw_footer(fb, "上一页", middle, "下一页");
    draw_article_progress(fb);
}

static const char *setting_value(int row, char buffer[48]) {
    static const char *const paragraph[] = {"0%", "25%", "50%", "75%"};
    static const char *const tracking[] = {"-4 px", "-2 px", "0 px", "+2 px", "+4 px"};
    static const char *const weight[] = {"细", "较细", "标准", "较粗", "粗"};
    static const char *const reading_line[] = {"关闭", "虚线", "点线"};
    switch (row) {
        case 0:
            return s_preferences.px == 20 && s_preferences.line_percent == 112 ? "紧凑" :
                   s_preferences.px == 22 && s_preferences.line_percent == 120 &&
                   s_preferences.margin_px == 10 ? "标准" : "自定义";
        case 1: return "与 EPUB 共用";
        case 2: snprintf(buffer, 48, "%u px", s_preferences.px); return buffer;
        case 3: snprintf(buffer, 48, "%u%%", s_preferences.line_percent); return buffer;
        case 4: return paragraph[s_preferences.paragraph_percent / 25];
        case 5: snprintf(buffer, 48, "%u px", s_preferences.margin_px); return buffer;
        case 6: return tracking[s_preferences.tracking];
        case 7: snprintf(buffer, 48, "%u 字", s_preferences.indent_em); return buffer;
        case 8: return weight[s_preferences.weight + 2];
        case 9: return reading_line[s_preferences.reading_line];
        default: return "";
    }
}

static void draw_settings(uint8_t *fb) {
    static const char *const labels[] = {
        "排版方案", "阅读字体", "字号", "行距", "段距",
        "页边距", "字距", "首行缩进", "字重", "阅读线",
    };
    ui_nav_status(fb);
    ui_nav_back(fb, 36, 79);
    ui_text_vc(fb, 92, 103, 34, "RSS 设置", EPD_DRAW_ALIGN_LEFT, false);
    ui_hairline(fb, RSS_LIST_TOP - 1, 0, UI_LOCK_WIDTH, RSS_GRAY_DARK);
    const int row_height = (RSS_LIST_BOTTOM - RSS_LIST_TOP) / 10;
    for (int row = 0; row < 10; ++row) {
        int y = RSS_LIST_TOP + row * row_height;
        char value[48];
        ui_text_vc(fb, 36, y + row_height / 2, 23, labels[row],
                   EPD_DRAW_ALIGN_LEFT, false);
        ui_text_vc(fb, 648, y + row_height / 2, 20,
                   setting_value(row, value), EPD_DRAW_ALIGN_RIGHT, false);
        ui_hairline(fb, y + row_height - 1, 36, 612, UI_GRAY_LIGHT);
    }
    draw_footer(fb, "", "返回列表", "");
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    ui_clear_page(fb);
    if (!s_cache) {
        ui_text_fixed(fb, 342, 560, 30, "RSS 内存不足", EPD_DRAW_ALIGN_CENTER, false);
        return;
    }
    if (s_view == RSS_VIEW_LIST) draw_list(fb);
    else if (s_view == RSS_VIEW_ARTICLE) draw_article(fb);
    else draw_settings(fb);
}

static void on_enter(app_ctx_t *ctx) {
    (void)ctx;
    app_font_activate_system();
    s_view = RSS_VIEW_LIST;
    s_list_page = 0;
    s_article_page = 0;
    s_article_page_count = 1;
    rss_preferences_load(&s_preferences);
    s_configured = false;
    s_config_invalid = false;
    s_storage_ready = false;
    s_network_owned = false;
    s_time_ready = false;
    s_sync_requested = false;
    s_next_tick_ms = 0;
    s_next_network_poll_ms = 0;
    s_next_refresh_ms = 0;
    s_network_signature = 0;
    memset(&s_network_status, 0, sizeof(s_network_status));
    s_notice[0] = 0;
    if (!allocate_core()) {
        snprintf(s_notice, sizeof(s_notice), "RSS 内存不足");
        return;
    }
    s_storage_ready = prepare_storage();
    if (!s_storage_ready) {
        snprintf(s_notice, sizeof(s_notice), "内部存储不可用");
        return;
    }
    load_cache();
    s_configured = load_configuration();
    if (!rss_sync_init(&s_sync, &s_client, s_cache, RSS_CACHE_FILE, s_scratch,
                       RSS_JSON_RESPONSE_MAX + 1, s_page_buffer)) {
        snprintf(s_notice, sizeof(s_notice), "RSS 初始化失败");
        return;
    }
    pending_load();
    rebuild_visible();
    if (s_configured) request_network();
}

static void persist_state(void) {
    if (!s_cache || !s_storage_ready) return;
    (void)pending_save();
    if (s_sync.state != RSS_SYNC_COMMITTING)
        (void)rss_cache_save_atomic(RSS_CACHE_FILE, s_cache);
}

static void on_before_lock(app_ctx_t *ctx) {
    (void)ctx;
    persist_state();
}

static void rss_on_exit(app_ctx_t *ctx) {
    (void)ctx;
    persist_state();
    if (s_network_owned) read_pico_transfer_stop();
    free_core();
    s_network_owned = false;
}

static bool enqueue_read(int64_t entry_id) {
    const rss_desired_state_t desired = {
        .read_known = true,
        .read = true,
    };
    if (rss_sync_enqueue(&s_sync, entry_id, &desired)) return true;
    snprintf(s_notice, sizeof(s_notice), "待确认队列已满");
    return false;
}

static bool read_batch_fits(const int64_t *ids, size_t count) {
    size_t additions = 0;
    for (size_t i = 0; i < count; ++i)
        if (!pending_by_id(ids[i])) ++additions;
    if (additions <= RSS_SYNC_OPERATION_MAX - s_sync.operation_count) return true;
    snprintf(s_notice, sizeof(s_notice), "待确认队列已满");
    return false;
}

static bool mark_page_through(size_t item) {
    size_t first = (size_t)s_list_page * RSS_LIST_ROWS;
    if (item < first || item >= s_visible_count) return false;
    int64_t ids[RSS_LIST_ROWS];
    size_t count = item - first + 1;
    for (size_t i = 0; i < count; ++i)
        ids[i] = s_cache->metadata[s_visible[first + i]].entry.id;
    if (!read_batch_fits(ids, count)) return false;
    for (size_t i = 0; i < count; ++i)
        if (!enqueue_read(ids[i])) return false;
    s_notice[0] = 0;
    (void)pending_save();
    s_sync_requested = true;
    rebuild_visible();
    return true;
}

static bool mark_current_page_read(void) {
    size_t first = (size_t)s_list_page * RSS_LIST_ROWS;
    if (first >= s_visible_count) return false;
    size_t count = s_visible_count - first;
    if (count > RSS_LIST_ROWS) count = RSS_LIST_ROWS;
    int64_t ids[RSS_LIST_ROWS];
    for (size_t i = 0; i < count; ++i)
        ids[i] = s_cache->metadata[s_visible[first + i]].entry.id;
    if (!read_batch_fits(ids, count)) return false;
    for (size_t i = 0; i < count; ++i)
        if (!enqueue_read(ids[i])) return false;
    s_notice[0] = 0;
    (void)pending_save();
    s_sync_requested = true;
    rebuild_visible();
    return true;
}

static app_redraw_t article_move(int delta) {
    if (delta > 0 && s_article_page + 1 < s_article_page_count) {
        ++s_article_page;
        return APP_REDRAW_PAGE;
    }
    if (delta < 0 && s_article_page > 0) {
        --s_article_page;
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t list_gesture(const ui_gesture_event_t *event) {
    int pages = (int)((s_visible_count + RSS_LIST_ROWS - 1) / RSS_LIST_ROWS);
    if (event->type == UI_GESTURE_SWIPE_L && s_list_page + 1 < pages) {
        ++s_list_page;
        return APP_REDRAW_PAGE;
    }
    if (event->type == UI_GESTURE_SWIPE_R && s_list_page > 0) {
        --s_list_page;
        return APP_REDRAW_PAGE;
    }
    if (event->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    if (ui_rect_hit(settings_action_rect(), event->x0, event->y0)) {
        s_view = RSS_VIEW_SETTINGS;
        return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit(read_all_action_rect(), event->x0, event->y0)) {
        return mark_current_page_read() ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    }
    if (event->y0 < RSS_LIST_TOP || event->y0 >= RSS_LIST_BOTTOM)
        return APP_REDRAW_NONE;
    int row = ((int)event->y0 - RSS_LIST_TOP) / RSS_LIST_ROW_HEIGHT;
    size_t item = (size_t)s_list_page * RSS_LIST_ROWS + (size_t)row;
    if (item < s_visible_count) {
        int64_t id = s_cache->metadata[s_visible[item]].entry.id;
        if (mark_page_through(item) && open_article(id)) return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t article_gesture(const ui_gesture_event_t *event) {
    if (event->type == UI_GESTURE_SWIPE_L) return article_move(1);
    if (event->type == UI_GESTURE_SWIPE_R) return article_move(-1);
    if (event->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    if (event->x0 < UI_LOCK_WIDTH / 3) return article_move(-1);
    if (event->x0 > UI_LOCK_WIDTH * 2 / 3) return article_move(1);
    return APP_REDRAW_NONE;
}

static app_redraw_t settings_gesture(const ui_gesture_event_t *event) {
    if (event->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    if (event->y0 < RSS_LIST_TOP && event->x0 < 120) {
        s_view = RSS_VIEW_LIST;
        return APP_REDRAW_PAGE;
    }
    if (event->y0 < RSS_LIST_TOP || event->y0 >= RSS_LIST_BOTTOM)
        return APP_REDRAW_NONE;
    int row_height = (RSS_LIST_BOTTOM - RSS_LIST_TOP) / 10;
    int row = ((int)event->y0 - RSS_LIST_TOP) / row_height;
    rss_preferences_t next = s_preferences;
    static const uint8_t px_values[] = {18, 20, 22, 24, 26, 28, 30, 32};
    static const uint8_t line_values[] = {110, 120, 130, 140, 150};
    static const uint8_t margin_values[] = {0, 10, 20, 30, 40, 48};
    switch (row) {
        case 0:
            rss_preferences_preset(&next,
                                   !(s_preferences.px == 20 &&
                                     s_preferences.line_percent == 112));
            break;
        case 1:
            return APP_REDRAW_NONE;
        case 2: {
            size_t index = 0;
            size_t count = sizeof(px_values) / sizeof(px_values[0]);
            while (index + 1 < count &&
                   px_values[index] != s_preferences.px) ++index;
            next.px = px_values[(index + 1) % count];
            break;
        }
        case 3: {
            size_t index = 0;
            size_t count = sizeof(line_values) / sizeof(line_values[0]);
            while (index + 1 < count &&
                   line_values[index] != s_preferences.line_percent) ++index;
            next.line_percent = line_values[(index + 1) % count];
            break;
        }
        case 4: next.paragraph_percent = (next.paragraph_percent + 25) % 100; break;
        case 5: {
            size_t index = 0;
            size_t count = sizeof(margin_values) / sizeof(margin_values[0]);
            while (index + 1 < count &&
                   margin_values[index] != s_preferences.margin_px) ++index;
            next.margin_px = margin_values[(index + 1) % count];
            break;
        }
        case 6: next.tracking = (next.tracking + 1) % 5; break;
        case 7: next.indent_em = (next.indent_em + 1) % 4; break;
        case 8: next.weight = next.weight == 2 ? -2 : next.weight + 1; break;
        case 9: next.reading_line = (next.reading_line + 1) % 3; break;
        default: return APP_REDRAW_NONE;
    }
    if (!rss_preferences_save(&next)) {
        snprintf(s_notice, sizeof(s_notice), "设置保存失败");
        return APP_REDRAW_PAGE;
    }
    s_preferences = next;
    s_notice[0] = 0;
    return APP_REDRAW_PAGE;
}

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *event) {
    (void)ctx;
    if (s_view == RSS_VIEW_LIST) return list_gesture(event);
    if (s_view == RSS_VIEW_ARTICLE) return article_gesture(event);
    return settings_gesture(event);
}

static app_redraw_t on_key(app_ctx_t *ctx, int key) {
    if (s_view == RSS_VIEW_ARTICLE) {
        if (key == UI_KEY_1) return article_move(-1);
        if (key == UI_KEY_3) return article_move(1);
        if (key == UI_KEY_2) {
            close_article();
            s_view = RSS_VIEW_LIST;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_view == RSS_VIEW_SETTINGS) {
        if (key == UI_KEY_2) {
            s_view = RSS_VIEW_LIST;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    int pages = (int)((s_visible_count + RSS_LIST_ROWS - 1) / RSS_LIST_ROWS);
    if (key == UI_KEY_1 && s_list_page > 0) {
        --s_list_page;
        return APP_REDRAW_PAGE;
    }
    if (key == UI_KEY_3 && s_list_page + 1 < pages) {
        ++s_list_page;
        return APP_REDRAW_PAGE;
    }
    if (key == UI_KEY_2) ctx->request_app = app_home_page();
    return APP_REDRAW_NONE;
}

static app_redraw_t network_tick(app_ctx_t *ctx) {
    if (ctx->now_ms < s_next_network_poll_ms) return APP_REDRAW_NONE;
    s_next_network_poll_ms = ctx->now_ms + RSS_NETWORK_POLL_MS;
    read_pico_transfer_status_t status = {0};
    read_pico_transfer_service_poll();
    read_pico_transfer_get_status(&status);
    s_network_status = status;
    bool time_before = s_time_ready;
    if (status.mode == READ_PICO_TRANSFER_MODE_STA && status.network_ready) {
        if (!s_time_ready) {
            uint32_t utc = 0;
            s_time_ready = time(NULL) >= 1704067200 ||
                           read_pico_transfer_sync_time_online(&utc) == ESP_OK;
        }
        if (s_time_ready) s_sync_requested = true;
    }
    uint32_t signature = (uint32_t)status.state |
                         ((uint32_t)status.mode << 4) |
                         ((uint32_t)status.network_ready << 8) |
                         ((uint32_t)status.wifi_configured << 9) |
                         ((uint32_t)status.last_error << 12);
    bool changed = signature != s_network_signature || time_before != s_time_ready;
    s_network_signature = signature;
    return changed ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
}

static app_redraw_t sync_tick(app_ctx_t *ctx) {
    if (!s_configured || !s_storage_ready || ctx->now_ms < s_next_tick_ms)
        return APP_REDRAW_NONE;
    s_next_tick_ms = ctx->now_ms + RSS_TICK_MS;
    read_pico_transfer_status_t network = {0};
    read_pico_transfer_get_status(&network);
    s_network_status = network;
    if (!(network.mode == READ_PICO_TRANSFER_MODE_STA && network.network_ready))
        return network_tick(ctx);
    if (!s_time_ready) return network_tick(ctx);
    if ((s_sync.state == RSS_SYNC_IDLE || s_sync.state == RSS_SYNC_COMPLETE ||
         s_sync.state == RSS_SYNC_FAILED) &&
        (s_sync_requested || s_sync.operation_count ||
         (s_next_refresh_ms && ctx->now_ms >= s_next_refresh_ms))) {
        if (!rss_sync_begin(&s_sync)) return APP_REDRAW_NONE;
        s_sync_requested = false;
        s_notice[0] = 0;
    }
    if (s_sync.state == RSS_SYNC_IDLE || s_sync.state == RSS_SYNC_COMPLETE ||
        s_sync.state == RSS_SYNC_FAILED) {
        return APP_REDRAW_NONE;
    }
    uint32_t pending_revision = s_sync.operation_revision;
    rss_sync_step_result_t result = rss_sync_step(&s_sync);
    if (pending_revision != s_sync.operation_revision) (void)pending_save();
    rebuild_visible();
    if (result == RSS_SYNC_STEP_DONE) {
        s_next_refresh_ms = ctx->now_ms + RSS_REFRESH_MS;
        (void)pending_save();
        if (s_sync.operation_count) s_sync_requested = true;
    } else if (result == RSS_SYNC_STEP_ERROR) {
        s_next_refresh_ms = ctx->now_ms + 60000;
    }
    return APP_REDRAW_PAGE;
}

static app_redraw_t on_tick(app_ctx_t *ctx) {
    return sync_tick(ctx);
}

static bool menu_handle_enabled(app_ctx_t *ctx) {
    (void)ctx;
    return false;
}

const app_desc_t app_rss = {
    .title = "RSS",
    .detail = "Miniflux 未读队列",
    .enter_full = false,
    .owns_keys = true,
    .menu_handle_enabled = menu_handle_enabled,
    .on_enter = on_enter,
    .on_exit = rss_on_exit,
    .on_before_lock = on_before_lock,
    .render = render,
    .on_gesture = on_gesture,
    .on_key = on_key,
    .on_tick = on_tick,
};
