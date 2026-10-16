/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：实现 RSS metadata LRU 缓存、校验序列化与可恢复文件替换。
 *
 * English: Implements RSS metadata LRU caching, checked serialization, and recoverable file
 * replacement.
 *
 * 冻结：损坏文件必须整体拒绝；写入失败不得以半文件覆盖上一份有效缓存。
 * Frozen: corrupt files are rejected as a whole; failed writes must not replace the last valid
 * cache.
 * 冻结：缓存格式不包含正文，旧正文缓存必须重建为仅 metadata。
 * Frozen: the cache format contains no bodies; legacy body caches are rebuilt as metadata only.
 */
#include "rss_cache.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define RSS_CACHE_MAGIC UINT32_C(0x52535331)
#define RSS_CACHE_VERSION UINT16_C(3)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint16_t metadata_size;
    uint32_t metadata_count;
    int64_t cursor_changed_at;
    uint64_t sequence;
    uint32_t payload_crc32;
} rss_cache_file_header_t;

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
} rss_cache_legacy_header_t;

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

static uint32_t payload_crc(const rss_cache_t *cache) {
    uint32_t crc = UINT32_MAX;
    crc = crc32_update(crc, cache->metadata, cache->metadata_count * sizeof(cache->metadata[0]));
    return crc ^ UINT32_MAX;
}

static bool cache_valid(const rss_cache_t *cache) {
    if (!cache || cache->metadata_count > RSS_CACHE_METADATA_MAX ||
        cache->cursor_changed_at < 0) {
        return false;
    }
    for (size_t i = 0; i < cache->metadata_count; ++i) {
        const rss_entry_t *entry = &cache->metadata[i].entry;
        if (entry->id <= 0 || entry->body_offset || entry->body_length) return false;
    }
    return true;
}

static bool load_legacy(FILE *file, rss_cache_t *cache) {
    rss_cache_legacy_header_t header;
    if (fseek(file, 0, SEEK_SET) != 0 ||
        fread(&header, 1, sizeof(header), file) != sizeof(header) ||
        header.magic != RSS_CACHE_MAGIC || header.version != 2 ||
        header.header_size != sizeof(header) ||
        header.metadata_size != sizeof(cache->metadata[0]) || header.body_size != 24 ||
        header.metadata_count > RSS_CACHE_METADATA_MAX || header.body_count > 32 ||
        header.body_used > 256U * 1024U ||
        fread(cache->metadata, sizeof(cache->metadata[0]), header.metadata_count, file) !=
            header.metadata_count)
        return false;
    uint32_t crc = crc32_update(UINT32_MAX, cache->metadata,
                                header.metadata_count * sizeof(cache->metadata[0]));
    size_t remaining = header.body_count * header.body_size + header.body_used;
    // 旧正文只流式校验并丢弃，绝不加载到新缓存。/ Validate and discard legacy bodies in bounded chunks.
    unsigned char discard[512];
    while (remaining) {
        size_t count = remaining < sizeof(discard) ? remaining : sizeof(discard);
        if (fread(discard, 1, count, file) != count) return false;
        crc = crc32_update(crc, discard, count);
        remaining -= count;
    }
    if ((crc ^ UINT32_MAX) != header.payload_crc32 || fgetc(file) != EOF) return false;
    cache->metadata_count = header.metadata_count;
    cache->cursor_changed_at = header.cursor_changed_at;
    cache->sequence = header.sequence;
    return cache_valid(cache);
}

static rss_cache_result_t load_file(const char *path, rss_cache_t *cache, bool *legacy) {
    rss_cache_init(cache);
    FILE *file = fopen(path, "rb");
    if (!file) return RSS_CACHE_IO;
    rss_cache_file_header_t header;
    bool read_header = fread(&header, 1, sizeof(header), file) == sizeof(header);
    *legacy = read_header && header.version == 2;
    if (*legacy) {
        bool ok = load_legacy(file, cache);
        if (fclose(file) != 0) ok = false;
        if (ok) return RSS_CACHE_OK;
        rss_cache_init(cache);
        return RSS_CACHE_CORRUPT;
    }
    bool ok = read_header &&
              header.magic == RSS_CACHE_MAGIC && header.version == RSS_CACHE_VERSION &&
              header.header_size == sizeof(header) &&
              header.metadata_size == sizeof(cache->metadata[0]) &&
              header.metadata_count <= RSS_CACHE_METADATA_MAX &&
              fread(cache->metadata, sizeof(cache->metadata[0]), header.metadata_count, file) ==
                  header.metadata_count &&
              fgetc(file) == EOF;
    if (fclose(file) != 0) ok = false;
    if (ok) {
        cache->metadata_count = header.metadata_count;
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
    bool legacy = false;
    rss_cache_result_t result = load_file(path, cache, &legacy);
    if (result == RSS_CACHE_OK)
        return legacy ? rss_cache_save_atomic(path, cache) : result;
    char backup[RSS_CACHE_PATH_MAX + 5];
    int length = snprintf(backup, sizeof(backup), "%s.bak", path);
    if (length <= 0 || (size_t)length >= sizeof(backup)) return result;
    rss_cache_result_t recovered = load_file(backup, cache, &legacy);
    if (recovered != RSS_CACHE_OK) return result;
    if (legacy) return rss_cache_save_atomic(path, cache);
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
        .metadata_count = (uint32_t)cache->metadata_count,
        .cursor_changed_at = cache->cursor_changed_at,
        .sequence = cache->sequence,
        .payload_crc32 = payload_crc(cache),
    };
    bool ok = fwrite(&header, 1, sizeof(header), file) == sizeof(header) &&
              fwrite(cache->metadata, sizeof(cache->metadata[0]), cache->metadata_count, file) ==
                  cache->metadata_count &&
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
