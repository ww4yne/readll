/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：维护有界 RSS metadata/body 缓存，并以校验过的临时文件原子替换持久化。
 *
 * English: Maintains bounded RSS metadata/body caches and persists them through a checked
 * temporary-file replacement.
 *
 * 冻结：缓存必须由调用方放在堆或 PSRAM；单篇与总正文上限不得静默放宽。
 * Frozen: callers must place the cache on heap or PSRAM; per-body and total-body limits must not
 * be silently widened.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rss_model.h"

#define RSS_CACHE_METADATA_MAX 128
#define RSS_CACHE_BODY_MAX 32
#define RSS_CACHE_BODY_TOTAL_MAX (256U * 1024U)
#define RSS_CACHE_BODY_SINGLE_MAX RSS_ENTRY_BODY_MAX
#define RSS_CACHE_PATH_MAX 256

typedef struct {
    rss_entry_t entry;              ///< 条目 metadata / Entry metadata
    uint64_t touched_at;            ///< 本地 LRU 序号 / Local LRU sequence
} rss_cache_metadata_t;

typedef struct {
    int64_t entry_id;               ///< 条目 ID / Entry ID
    uint32_t offset;                ///< 正文池偏移 / Body-pool offset
    uint32_t length;                ///< 正文字节数 / Body byte length
    uint64_t touched_at;            ///< 本地 LRU 序号 / Local LRU sequence
} rss_cache_body_t;

typedef struct {
    int64_t cursor_changed_at;                    ///< 成功同步游标 / Successful sync cursor
    uint64_t sequence;                            ///< LRU 单调序号 / Monotonic LRU sequence
    size_t metadata_count;                        ///< metadata 数量 / Metadata count
    size_t body_count;                            ///< 正文数量 / Body count
    size_t body_used;                             ///< 正文池已用字节 / Used body bytes
    rss_cache_metadata_t metadata[RSS_CACHE_METADATA_MAX]; ///< metadata 缓存 / Metadata cache
    rss_cache_body_t bodies[RSS_CACHE_BODY_MAX];  ///< 正文索引 / Body index
    uint8_t body_data[RSS_CACHE_BODY_TOTAL_MAX];  ///< 紧凑正文池 / Compact body pool
} rss_cache_t;

typedef enum {
    RSS_CACHE_OK = 0,             ///< 成功 / Success
    RSS_CACHE_INVALID_ARGUMENT,   ///< 参数无效 / Invalid argument
    RSS_CACHE_LIMIT,              ///< 单项超限 / Item exceeds a hard bound
    RSS_CACHE_IO,                 ///< 文件操作失败 / File operation failed
    RSS_CACHE_CORRUPT,            ///< 缓存格式或校验错误 / Invalid format or checksum
} rss_cache_result_t;

/// 清空缓存。/ Clear a cache.
void rss_cache_init(rss_cache_t *cache);

/// 查找 metadata。/ Find metadata.
rss_cache_metadata_t *rss_cache_find(rss_cache_t *cache, int64_t entry_id);

/// 插入或更新 metadata，满时淘汰最旧项。/ Insert or update metadata, evicting the oldest if full.
rss_cache_result_t rss_cache_put_metadata(rss_cache_t *cache, const rss_entry_t *entry);

/// 插入或更新正文，必要时按 LRU 淘汰。/ Insert or update a body, evicting by LRU as needed.
rss_cache_result_t rss_cache_put_body(rss_cache_t *cache, int64_t entry_id,
                                      const char *body, size_t length);

/// 查找正文。/ Find a body.
const char *rss_cache_get_body(rss_cache_t *cache, int64_t entry_id, size_t *length);

/// 从文件加载并校验；不存在也显式返回 IO。/ Load and validate; a missing file explicitly returns IO.
rss_cache_result_t rss_cache_load(const char *path, rss_cache_t *cache);

/// 写临时文件、同步并替换目标；失败时保留旧文件。/ Write, sync, and replace via a temporary file.
rss_cache_result_t rss_cache_save_atomic(const char *path, const rss_cache_t *cache);
