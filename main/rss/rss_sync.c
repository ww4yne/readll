/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：实现可重试操作队列和逐 step 的 Miniflux 增量同步状态机。
 *
 * English: Implements a retryable operation queue and step-driven Miniflux incremental sync state
 * machine.
 *
 * 冻结：失败保留未确认操作；分页或落盘任一步失败都不得提交候选游标。
 * Frozen: failed operations remain queued; a paging or persistence failure never commits the
 * candidate cursor.
 */
#include "rss_sync.h"

#include <limits.h>
#include <string.h>

static void clear_error(rss_sync_t *sync) {
    memset(&sync->error, 0, sizeof(sync->error));
    sync->error.state = sync->state;
}

static rss_sync_step_result_t sync_fail(rss_sync_t *sync, const char *reason) {
    sync->state = RSS_SYNC_FAILED;
    sync->error.state = RSS_SYNC_FAILED;
    sync->error.reason = reason;
    return RSS_SYNC_STEP_ERROR;
}

bool rss_sync_init(rss_sync_t *sync, rss_client_t *client, rss_cache_t *cache,
                   const char *cache_path, char *scratch, size_t scratch_size,
                   rss_entry_page_t *page) {
    if (!sync || !client || !cache || !cache_path || !*cache_path ||
        strnlen(cache_path, RSS_CACHE_PATH_MAX) >= RSS_CACHE_PATH_MAX || !scratch ||
        scratch_size < RSS_JSON_RESPONSE_MAX + 1 || !page) {
        return false;
    }
    memset(sync, 0, sizeof(*sync));
    sync->client = client;
    sync->cache = cache;
    memcpy(sync->cache_path, cache_path, strlen(cache_path) + 1);
    sync->scratch = scratch;
    sync->scratch_size = scratch_size;
    sync->page = page;
    sync->state = RSS_SYNC_IDLE;
    return true;
}

bool rss_sync_enqueue(rss_sync_t *sync, int64_t entry_id, const rss_desired_state_t *desired) {
    if (!sync || entry_id <= 0 || !desired || (!desired->read_known && !desired->bookmark_known))
        return false;
    size_t index = sync->operation_count;
    for (size_t i = 0; i < sync->operation_count; ++i) {
        if (sync->operations[i].entry_id == entry_id) {
            index = i;
            break;
        }
    }
    if (index == sync->operation_count) {
        if (sync->operation_count >= RSS_SYNC_OPERATION_MAX) return false;
        ++sync->operation_count;
        memset(&sync->operations[index], 0, sizeof(sync->operations[index]));
        sync->operations[index].entry_id = entry_id;
        ++sync->operation_revision;
    }
    if (desired->read_known) {
        if (!sync->operations[index].desired.read_known ||
            sync->operations[index].desired.read != desired->read)
            ++sync->operation_revision;
        sync->operations[index].desired.read_known = true;
        sync->operations[index].desired.read = desired->read;
    }
    if (desired->bookmark_known) {
        if (!sync->operations[index].desired.bookmark_known ||
            sync->operations[index].desired.bookmarked != desired->bookmarked)
            ++sync->operation_revision;
        sync->operations[index].desired.bookmark_known = true;
        sync->operations[index].desired.bookmarked = desired->bookmarked;
    }
    return true;
}

bool rss_sync_restore_operation(rss_sync_t *sync, const rss_sync_operation_t *operation) {
    if (!sync || !operation || operation->entry_id <= 0 ||
        (!operation->desired.read_known && !operation->desired.bookmark_known) ||
        (operation->bookmark_confirmation_pending && !operation->desired.bookmark_known) ||
        sync->operation_count >= RSS_SYNC_OPERATION_MAX) {
        return false;
    }
    for (size_t i = 0; i < sync->operation_count; ++i)
        if (sync->operations[i].entry_id == operation->entry_id) return false;
    sync->operations[sync->operation_count++] = *operation;
    ++sync->operation_revision;
    return true;
}

bool rss_sync_begin(rss_sync_t *sync) {
    if (!sync || (sync->state != RSS_SYNC_IDLE && sync->state != RSS_SYNC_COMPLETE &&
                  sync->state != RSS_SYNC_FAILED)) {
        return false;
    }
    clear_error(sync);
    sync->fetched_count = 0;
    sync->last_entry_id = 0;
    sync->candidate_cursor = sync->cache->cursor_changed_at;
    sync->query_cursor = sync->cache->cursor_changed_at > RSS_SYNC_CURSOR_OVERLAP_SECONDS
                             ? sync->cache->cursor_changed_at - RSS_SYNC_CURSOR_OVERLAP_SECONDS
                             : 0;
    sync->state = sync->operation_count ? RSS_SYNC_FLUSHING : RSS_SYNC_FETCHING_ME;
    return true;
}

static void compact_operations(rss_sync_t *sync) {
    size_t previous_count = sync->operation_count;
    size_t output = 0;
    for (size_t i = 0; i < sync->operation_count; ++i) {
        if (sync->operations[i].desired.read_known ||
            sync->operations[i].desired.bookmark_known) {
            if (output != i) sync->operations[output] = sync->operations[i];
            ++output;
        }
    }
    sync->operation_count = output;
    if (output != previous_count) ++sync->operation_revision;
}

static rss_sync_step_result_t flush_read_batch(rss_sync_t *sync, size_t first) {
    bool desired = sync->operations[first].desired.read;
    int64_t ids[RSS_CLIENT_BATCH_MAX] = {0};
    size_t indexes[RSS_CLIENT_BATCH_MAX] = {0};
    size_t count = 0;
    for (size_t i = first; i < sync->operation_count && count < RSS_CLIENT_BATCH_MAX; ++i) {
        if (sync->operations[i].desired.read_known &&
            sync->operations[i].desired.read == desired) {
            ids[count] = sync->operations[i].entry_id;
            indexes[count++] = i;
        }
    }
    if (!rss_client_set_read(sync->client, ids, count, desired, &sync->error.client_error))
        return sync_fail(sync, "read batch failed");
    for (size_t i = 0; i < count; ++i) {
        rss_cache_metadata_t *metadata =
            rss_cache_find(sync->cache, sync->operations[indexes[i]].entry_id);
        if (metadata)
            metadata->entry.status = desired ? RSS_ENTRY_STATUS_READ : RSS_ENTRY_STATUS_UNREAD;
        sync->operations[indexes[i]].desired.read_known = false;
        ++sync->operation_revision;
    }
    compact_operations(sync);
    return RSS_SYNC_STEP_PROGRESS;
}

static rss_sync_step_result_t flush_bookmark(rss_sync_t *sync, size_t index) {
    rss_sync_operation_t *operation = &sync->operations[index];
    rss_cache_metadata_t *metadata = rss_cache_find(sync->cache, operation->entry_id);
    if (!metadata) return sync_fail(sync, "bookmark state is unknown");
    if (metadata->entry.bookmarked != operation->desired.bookmarked) {
        if (!rss_client_toggle_bookmark(sync->client, operation->entry_id,
                                        &sync->error.client_error)) {
            if (sync->error.client_error.code == RSS_CLIENT_TRANSPORT ||
                sync->error.client_error.code == RSS_CLIENT_INCOMPLETE) {
                // toggle 可能已到达服务端；先拉取确认，禁止盲重放。
                // The toggle may have reached the server; fetch confirmation before any retry.
                operation->bookmark_confirmation_pending = true;
                ++sync->operation_revision;
            }
            return sync_fail(sync, "bookmark toggle failed");
        }
        metadata->entry.bookmarked = operation->desired.bookmarked;
    }
    operation->desired.bookmark_known = false;
    ++sync->operation_revision;
    compact_operations(sync);
    return RSS_SYNC_STEP_PROGRESS;
}

static rss_sync_step_result_t flush_one(rss_sync_t *sync) {
    for (size_t i = 0; i < sync->operation_count; ++i) {
        if (sync->operations[i].desired.read_known) return flush_read_batch(sync, i);
    }
    for (size_t i = 0; i < sync->operation_count; ++i) {
        if (sync->operations[i].desired.bookmark_known &&
            !sync->operations[i].bookmark_confirmation_pending) {
            return flush_bookmark(sync, i);
        }
    }
    sync->state = RSS_SYNC_FETCHING_ME;
    return RSS_SYNC_STEP_PROGRESS;
}

static void confirm_bookmark_state(rss_sync_t *sync, int64_t entry_id) {
    for (size_t i = 0; i < sync->operation_count; ++i) {
        if (sync->operations[i].entry_id == entry_id)
            if (sync->operations[i].bookmark_confirmation_pending) {
                sync->operations[i].bookmark_confirmation_pending = false;
                ++sync->operation_revision;
            }
    }
}

static rss_sync_step_result_t fetch_entries(rss_sync_t *sync) {
    rss_entries_query_t query = {
        .changed_after = sync->query_cursor,
        .after_entry_id = sync->last_entry_id,
        .limit = sync->client->config.page_size,
    };
    if (!rss_client_get_entries(sync->client, &query, sync->scratch, sync->scratch_size,
                                sync->page, &sync->error.client_error)) {
        return sync_fail(sync, "entry fetch failed");
    }
    for (size_t i = 0; i < sync->page->count; ++i) {
        const rss_entry_t *entry = &sync->page->items[i];
        if (entry->id <= sync->last_entry_id)
            return sync_fail(sync, "entry IDs are not strictly increasing");
        rss_cache_result_t result = rss_cache_put_metadata(sync->cache, entry);
        if (result == RSS_CACHE_OK) {
            const char *body = rss_entry_body(sync->page, entry);
            result = body ? rss_cache_put_body(sync->cache, entry->id, body, entry->body_length)
                          : RSS_CACHE_CORRUPT;
        }
        if (result != RSS_CACHE_OK) {
            sync->error.cache_error = result;
            return sync_fail(sync, "entry cache update failed");
        }
        confirm_bookmark_state(sync, entry->id);
        if (entry->changed_at > sync->candidate_cursor)
            sync->candidate_cursor = entry->changed_at;
        sync->last_entry_id = entry->id;
    }
    if (sync->page->count && sync->fetched_count > UINT32_MAX - sync->page->count)
        return sync_fail(sync, "entry pagination overflow");
    sync->fetched_count += (uint32_t)sync->page->count;
    if (sync->page->count == query.limit) return RSS_SYNC_STEP_PROGRESS;
    sync->state = RSS_SYNC_COMMITTING;
    return RSS_SYNC_STEP_PROGRESS;
}

rss_sync_step_result_t rss_sync_step(rss_sync_t *sync) {
    if (!sync) return RSS_SYNC_STEP_ERROR;
    sync->error.state = sync->state;
    switch (sync->state) {
        case RSS_SYNC_FLUSHING:
            return flush_one(sync);
        case RSS_SYNC_FETCHING_ME:
            if (!rss_client_get_me(sync->client, &sync->user, &sync->error.client_error))
                return sync_fail(sync, "account fetch failed");
            sync->state = RSS_SYNC_FETCHING_CATEGORIES;
            return RSS_SYNC_STEP_PROGRESS;
        case RSS_SYNC_FETCHING_CATEGORIES:
            if (!rss_client_get_categories(sync->client, &sync->categories,
                                           &sync->error.client_error))
                return sync_fail(sync, "category fetch failed");
            sync->state = RSS_SYNC_FETCHING_ENTRIES;
            return RSS_SYNC_STEP_PROGRESS;
        case RSS_SYNC_FETCHING_ENTRIES:
            return fetch_entries(sync);
        case RSS_SYNC_COMMITTING: {
            int64_t previous = sync->cache->cursor_changed_at;
            sync->cache->cursor_changed_at = sync->candidate_cursor;
            rss_cache_result_t result = rss_cache_save_atomic(sync->cache_path, sync->cache);
            if (result != RSS_CACHE_OK) {
                sync->cache->cursor_changed_at = previous;
                sync->error.cache_error = result;
                return sync_fail(sync, "cache commit failed");
            }
            sync->state = RSS_SYNC_COMPLETE;
            return RSS_SYNC_STEP_DONE;
        }
        case RSS_SYNC_COMPLETE:
            return RSS_SYNC_STEP_DONE;
        case RSS_SYNC_IDLE:
            return sync_fail(sync, "sync has not begun");
        case RSS_SYNC_FAILED:
        default:
            return RSS_SYNC_STEP_ERROR;
    }
}
