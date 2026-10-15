/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：实现 RSS metadata/body LRU 缓存、校验序列化与可恢复文件替换。
 *
 * English: Implements RSS metadata/body LRU caching, checked serialization, and recoverable file
 * replacement.
 *
 * 冻结：损坏文件必须整体拒绝；写入失败不得以半文件覆盖上一份有效缓存。
 * Frozen: corrupt files are rejected as a whole; failed writes must not replace the last valid
 * cache.
 */
#include "rss_cache.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define RSS_CACHE_MAGIC UINT32_C(0x52535331)
#define RSS_CACHE_VERSION UINT16_C(2)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint16_t metadata_size;
    uint16_t body_size;
    uint32_t metadata_count;
    uint32_t body_count;
    uint32_t body_used;
    int64_t cursor_changed_at;
    uint64_t sequence;
    uint32_t payload_crc32;
} rss_cache_file_header_t;

static uint32_t crc32_update(uint32_t crc, const void *data, size_t length) {
    const uint8_t *bytes = data;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (uint32_t)-(int32_t)(crc & 1));
    }
    return crc;
}

static uint64_t touch(rss_cache_t *cache) {
    if (++cache->sequence == 0) {
        cache->sequence = 1;
        for (size_t i = 0; i < cache->metadata_count; ++i) cache->metadata[i].touched_at = 1;
        for (size_t i = 0; i < cache->body_count; ++i) cache->bodies[i].touched_at = 1;
    }
    return cache->sequence;
}

void rss_cache_init(rss_cache_t *cache) {
    if (cache) memset(cache, 0, sizeof(*cache));
}

rss_cache_metadata_t *rss_cache_find(rss_cache_t *cache, int64_t entry_id) {
    if (!cache || entry_id <= 0) return NULL;
    for (size_t i = 0; i < cache->metadata_count; ++i) {
        if (cache->metadata[i].entry.id == entry_id) {
            cache->metadata[i].touched_at = touch(cache);
            return &cache->metadata[i];
        }
    }
    return NULL;
}

rss_cache_result_t rss_cache_put_metadata(rss_cache_t *cache, const rss_entry_t *entry) {
    if (!cache || !entry || entry->id <= 0) return RSS_CACHE_INVALID_ARGUMENT;
    size_t index = cache->metadata_count;
    for (size_t i = 0; i < cache->metadata_count; ++i) {
        if (cache->metadata[i].entry.id == entry->id) {
            index = i;
            break;
        }
    }
    if (index == cache->metadata_count) {
        if (cache->metadata_count < RSS_CACHE_METADATA_MAX) {
            ++cache->metadata_count;
        } else {
            index = 0;
            for (size_t i = 1; i < cache->metadata_count; ++i) {
                if (cache->metadata[i].touched_at < cache->metadata[index].touched_at) index = i;
            }
        }
    }
    cache->metadata[index].entry = *entry;
    cache->metadata[index].entry.body_offset = 0;
    cache->metadata[index].entry.body_length = 0;
    cache->metadata[index].touched_at = touch(cache);
    return RSS_CACHE_OK;
}

static void remove_body_at(rss_cache_t *cache, size_t index) {
    rss_cache_body_t removed = cache->bodies[index];
    size_t stored = (size_t)removed.length + 1;
    size_t tail = cache->body_used - removed.offset - stored;
    memmove(cache->body_data + removed.offset, cache->body_data + removed.offset + stored, tail);
    cache->body_used -= stored;
    for (size_t i = 0; i < cache->body_count; ++i) {
        if (i != index && cache->bodies[i].offset > removed.offset)
            cache->bodies[i].offset -= (uint32_t)stored;
    }
    if (index + 1 < cache->body_count) {
        memmove(&cache->bodies[index], &cache->bodies[index + 1],
                (cache->body_count - index - 1) * sizeof(cache->bodies[0]));
    }
    --cache->body_count;
}

rss_cache_result_t rss_cache_put_body(rss_cache_t *cache, int64_t entry_id,
                                      const char *body, size_t length) {
    if (!cache || entry_id <= 0 || (!body && length)) return RSS_CACHE_INVALID_ARGUMENT;
    if (length > RSS_CACHE_BODY_SINGLE_MAX || length + 1 > RSS_CACHE_BODY_TOTAL_MAX)
        return RSS_CACHE_LIMIT;
    for (size_t i = 0; i < cache->body_count; ++i) {
        if (cache->bodies[i].entry_id == entry_id) {
            remove_body_at(cache, i);
            break;
        }
    }
    while (cache->body_count >= RSS_CACHE_BODY_MAX ||
           length + 1 > RSS_CACHE_BODY_TOTAL_MAX - cache->body_used) {
        if (!cache->body_count) return RSS_CACHE_LIMIT;
        size_t oldest = 0;
        for (size_t i = 1; i < cache->body_count; ++i) {
            if (cache->bodies[i].touched_at < cache->bodies[oldest].touched_at) oldest = i;
        }
        remove_body_at(cache, oldest);
    }
    rss_cache_body_t *descriptor = &cache->bodies[cache->body_count++];
    descriptor->entry_id = entry_id;
    descriptor->offset = (uint32_t)cache->body_used;
    descriptor->length = (uint32_t)length;
    descriptor->touched_at = touch(cache);
    if (length) memcpy(cache->body_data + cache->body_used, body, length);
    cache->body_data[cache->body_used + length] = '\0';
    cache->body_used += length + 1;
    return RSS_CACHE_OK;
}

const char *rss_cache_get_body(rss_cache_t *cache, int64_t entry_id, size_t *length) {
    if (length) *length = 0;
    if (!cache || entry_id <= 0) return NULL;
    for (size_t i = 0; i < cache->body_count; ++i) {
        rss_cache_body_t *body = &cache->bodies[i];
        if (body->entry_id == entry_id && body->offset <= cache->body_used &&
            body->length < cache->body_used - body->offset &&
            cache->body_data[body->offset + body->length] == '\0') {
            body->touched_at = touch(cache);
            if (length) *length = body->length;
            return (const char *)cache->body_data + body->offset;
        }
    }
    return NULL;
}

static uint32_t payload_crc(const rss_cache_t *cache) {
    uint32_t crc = UINT32_MAX;
    crc = crc32_update(crc, cache->metadata, cache->metadata_count * sizeof(cache->metadata[0]));
    crc = crc32_update(crc, cache->bodies, cache->body_count * sizeof(cache->bodies[0]));
    crc = crc32_update(crc, cache->body_data, cache->body_used);
    return crc ^ UINT32_MAX;
}

static bool cache_valid(const rss_cache_t *cache) {
    if (!cache || cache->metadata_count > RSS_CACHE_METADATA_MAX ||
        cache->body_count > RSS_CACHE_BODY_MAX || cache->body_used > RSS_CACHE_BODY_TOTAL_MAX ||
        cache->cursor_changed_at < 0) {
        return false;
    }
    for (size_t i = 0; i < cache->metadata_count; ++i) {
        if (cache->metadata[i].entry.id <= 0) return false;
    }
    for (size_t i = 0; i < cache->body_count; ++i) {
        const rss_cache_body_t *body = &cache->bodies[i];
        if (body->entry_id <= 0 || body->length > RSS_CACHE_BODY_SINGLE_MAX ||
            body->offset > cache->body_used || body->length >= cache->body_used - body->offset ||
            cache->body_data[body->offset + body->length] != '\0') {
            return false;
        }
        size_t begin = body->offset;
        size_t end = begin + body->length + 1;
        for (size_t j = i + 1; j < cache->body_count; ++j) {
            size_t other_begin = cache->bodies[j].offset;
            size_t other_end = other_begin + cache->bodies[j].length + 1;
            if (begin < other_end && other_begin < end) return false;
        }
    }
    return true;
}

static rss_cache_result_t load_file(const char *path, rss_cache_t *cache) {
    rss_cache_init(cache);
    FILE *file = fopen(path, "rb");
    if (!file) return RSS_CACHE_IO;
    rss_cache_file_header_t header;
    bool ok = fread(&header, 1, sizeof(header), file) == sizeof(header) &&
              header.magic == RSS_CACHE_MAGIC && header.version == RSS_CACHE_VERSION &&
              header.header_size == sizeof(header) &&
              header.metadata_size == sizeof(cache->metadata[0]) &&
              header.body_size == sizeof(cache->bodies[0]) &&
              header.metadata_count <= RSS_CACHE_METADATA_MAX &&
              header.body_count <= RSS_CACHE_BODY_MAX &&
              header.body_used <= RSS_CACHE_BODY_TOTAL_MAX &&
              fread(cache->metadata, sizeof(cache->metadata[0]), header.metadata_count, file) ==
                  header.metadata_count &&
              fread(cache->bodies, sizeof(cache->bodies[0]), header.body_count, file) ==
                  header.body_count &&
              fread(cache->body_data, 1, header.body_used, file) == header.body_used &&
              fgetc(file) == EOF;
    if (fclose(file) != 0) ok = false;
    if (ok) {
        cache->metadata_count = header.metadata_count;
        cache->body_count = header.body_count;
        cache->body_used = header.body_used;
        cache->cursor_changed_at = header.cursor_changed_at;
        cache->sequence = header.sequence;
        ok = cache_valid(cache) && payload_crc(cache) == header.payload_crc32;
    }
    if (!ok) {
        rss_cache_init(cache);
        return RSS_CACHE_CORRUPT;
    }
    return RSS_CACHE_OK;
}

rss_cache_result_t rss_cache_load(const char *path, rss_cache_t *cache) {
    if (!path || !*path || strnlen(path, RSS_CACHE_PATH_MAX) >= RSS_CACHE_PATH_MAX || !cache)
        return RSS_CACHE_INVALID_ARGUMENT;
    rss_cache_result_t result = load_file(path, cache);
    if (result == RSS_CACHE_OK) return result;
    char backup[RSS_CACHE_PATH_MAX + 5];
    int length = snprintf(backup, sizeof(backup), "%s.bak", path);
    if (length <= 0 || (size_t)length >= sizeof(backup)) return result;
    rss_cache_result_t recovered = load_file(backup, cache);
    if (recovered != RSS_CACHE_OK) return result;
    remove(path);
    (void)rename(backup, path);
    return RSS_CACHE_OK;
}

static rss_cache_result_t write_cache_file(const char *path, const rss_cache_t *cache) {
    FILE *file = fopen(path, "wb");
    if (!file) return RSS_CACHE_IO;
    rss_cache_file_header_t header = {
        .magic = RSS_CACHE_MAGIC,
        .version = RSS_CACHE_VERSION,
        .header_size = sizeof(header),
        .metadata_size = sizeof(cache->metadata[0]),
        .body_size = sizeof(cache->bodies[0]),
        .metadata_count = (uint32_t)cache->metadata_count,
        .body_count = (uint32_t)cache->body_count,
        .body_used = (uint32_t)cache->body_used,
        .cursor_changed_at = cache->cursor_changed_at,
        .sequence = cache->sequence,
        .payload_crc32 = payload_crc(cache),
    };
    bool ok = fwrite(&header, 1, sizeof(header), file) == sizeof(header) &&
              fwrite(cache->metadata, sizeof(cache->metadata[0]), cache->metadata_count, file) ==
                  cache->metadata_count &&
              fwrite(cache->bodies, sizeof(cache->bodies[0]), cache->body_count, file) ==
                  cache->body_count &&
              fwrite(cache->body_data, 1, cache->body_used, file) == cache->body_used &&
              fflush(file) == 0 && fsync(fileno(file)) == 0;
    if (fclose(file) != 0) ok = false;
    if (!ok) {
        remove(path);
        return RSS_CACHE_IO;
    }
    return RSS_CACHE_OK;
}

rss_cache_result_t rss_cache_save_atomic(const char *path, const rss_cache_t *cache) {
    if (!path || !*path || strnlen(path, RSS_CACHE_PATH_MAX) >= RSS_CACHE_PATH_MAX || !cache)
        return RSS_CACHE_INVALID_ARGUMENT;
    if (!cache_valid(cache)) return RSS_CACHE_CORRUPT;
    char temporary[RSS_CACHE_PATH_MAX + 5];
    char backup[RSS_CACHE_PATH_MAX + 5];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) <= 0 ||
        snprintf(backup, sizeof(backup), "%s.bak", path) <= 0) {
        return RSS_CACHE_INVALID_ARGUMENT;
    }
    remove(temporary);
    rss_cache_result_t written = write_cache_file(temporary, cache);
    if (written != RSS_CACHE_OK) return written;
    remove(backup);
    // 支持覆盖式 rename 的文件系统直接原子替换；FAT 失败时再走可恢复轮换。
    // Filesystems with replacing rename commit atomically; FAT falls back to recoverable rotation.
    if (rename(temporary, path) == 0) return RSS_CACHE_OK;
    bool rotated = rename(path, backup) == 0;
    if (!rotated) {
        remove(temporary);
        return RSS_CACHE_IO;
    }
    if (rename(temporary, path) != 0) {
        remove(temporary);
        (void)rename(backup, path);
        return RSS_CACHE_IO;
    }
    remove(backup);
    return RSS_CACHE_OK;
}
