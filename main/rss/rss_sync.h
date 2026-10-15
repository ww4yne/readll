/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：以同步 step 驱动 Miniflux 操作队列和增量同步状态机，不创建任务或 UI。
 *
 * English: Drives the Miniflux operation queue and incremental sync state machine through
 * synchronous steps, without creating tasks or UI.
 *
 * 冻结：read/unread 使用幂等批量；Miniflux 2.3.1 收藏必须先比较再 toggle；游标仅在完整成功并落盘后推进。
 * Frozen: read/unread uses idempotent batches; Miniflux 2.3.1 bookmarks compare before toggle;
 * the cursor advances only after a complete successful sync is persisted.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rss_cache.h"
#include "rss_client.h"

#define RSS_SYNC_OPERATION_MAX 64
#define RSS_SYNC_CURSOR_OVERLAP_SECONDS 300

typedef struct {
    int64_t entry_id;                  ///< 条目 ID / Entry ID
    rss_desired_state_t desired;       ///< 最终目标状态 / Final desired state
    bool bookmark_confirmation_pending; ///< 超时 toggle 等待服务端确认 / Timed-out toggle awaits server confirmation
} rss_sync_operation_t;

typedef enum {
    RSS_SYNC_IDLE = 0,        ///< 空闲 / Idle
    RSS_SYNC_FLUSHING,        ///< 上传本地操作 / Flushing local operations
    RSS_SYNC_FETCHING_ME,     ///< 校验账户 / Validating account
    RSS_SYNC_FETCHING_CATEGORIES, ///< 拉分类 / Fetching categories
    RSS_SYNC_FETCHING_ENTRIES,    ///< 拉条目 / Fetching entries
    RSS_SYNC_COMMITTING,      ///< 原子落盘 / Atomically committing
    RSS_SYNC_COMPLETE,        ///< 本轮完成 / Cycle complete
    RSS_SYNC_FAILED,          ///< 本轮失败 / Cycle failed
} rss_sync_state_t;

typedef enum {
    RSS_SYNC_STEP_PROGRESS = 0, ///< 本 step 成功，仍有工作 / Step succeeded, more work remains
    RSS_SYNC_STEP_DONE,         ///< 本轮完成 / Cycle completed
    RSS_SYNC_STEP_ERROR,        ///< 显式失败 / Explicit failure
} rss_sync_step_result_t;

typedef struct {
    rss_sync_state_t state;               ///< 当前状态 / Current state
    rss_client_error_t client_error;       ///< 最近客户端错误 / Last client error
    rss_cache_result_t cache_error;        ///< 最近缓存错误 / Last cache error
    const char *reason;                    ///< 静态错误原因，不含令牌 / Static reason without token
} rss_sync_error_t;

typedef struct {
    rss_client_t *client;                         ///< 非拥有客户端 / Non-owning client
    rss_cache_t *cache;                           ///< 非拥有缓存 / Non-owning cache
    char cache_path[RSS_CACHE_PATH_MAX];           ///< 持久化路径 / Persistence path
    rss_sync_state_t state;                       ///< 当前状态 / Current state
    rss_sync_operation_t operations[RSS_SYNC_OPERATION_MAX]; ///< 合并操作队列 / Coalesced queue
    size_t operation_count;                       ///< 队列长度 / Queue length
    uint32_t operation_revision;                  ///< 队列内容修订号 / Queue-content revision
    rss_user_t user;                              ///< 最近用户 / Last user
    rss_category_list_t categories;               ///< 最近分类 / Last categories
    uint32_t fetched_count;                       ///< 本轮已拉条目数 / Entries fetched this cycle
    int64_t last_entry_id;                        ///< 当前 ID 分页游标 / Current ID page cursor
    int64_t query_cursor;                         ///< 含 overlap 的查询游标 / Query cursor with overlap
    int64_t candidate_cursor;                     ///< 本轮候选游标 / Candidate cursor for this cycle
    char *scratch;                                ///< HTTP JSON 暂存区 / HTTP JSON scratch buffer
    size_t scratch_size;                          ///< 暂存区大小 / Scratch size
    rss_entry_page_t *page;                       ///< 非拥有分页缓冲 / Non-owning page buffer
    rss_sync_error_t error;                       ///< 最近错误 / Last error
} rss_sync_t;

/// 初始化同步器；scratch 至少 RSS_JSON_RESPONSE_MAX+1 字节，page 应位于堆或 PSRAM。
/// Initialize; scratch needs RSS_JSON_RESPONSE_MAX+1 bytes and page belongs on heap or PSRAM.
bool rss_sync_init(rss_sync_t *sync, rss_client_t *client, rss_cache_t *cache,
                   const char *cache_path, char *scratch, size_t scratch_size,
                   rss_entry_page_t *page);

/// 合并条目的最终目标状态。/ Coalesce an entry's final desired state.
bool rss_sync_enqueue(rss_sync_t *sync, int64_t entry_id, const rss_desired_state_t *desired);

/// 恢复完整持久化操作，包括收藏确认状态。/ Restore a complete persisted operation, including bookmark confirmation.
bool rss_sync_restore_operation(rss_sync_t *sync, const rss_sync_operation_t *operation);

/// 开始一轮：先上传队列，再拉账户、分类和 overlap 条目。/ Begin a flush-and-fetch cycle.
bool rss_sync_begin(rss_sync_t *sync);

/// 同步执行一个网络或落盘步骤。/ Synchronously execute one network or persistence step.
rss_sync_step_result_t rss_sync_step(rss_sync_t *sync);
