# RSS core / RSS 核心

本目录只提供无 UI、无任务的同步核心。调用方负责 Wi-Fi 生命周期，并把大对象
`rss_cache_t`、`rss_entry_page_t` 与 `RSS_JSON_RESPONSE_MAX+1` 暂存区放在堆或 PSRAM。

This directory is a synchronous core without UI or tasks. The caller owns Wi-Fi lifecycle and
must allocate `rss_cache_t`, `rss_entry_page_t`, and the `RSS_JSON_RESPONSE_MAX+1` scratch buffer
on heap or PSRAM.

## Bounds / 边界

- 每页最多 20 条，JSON 响应最多 96 KiB。/ At most 20 entries and 96 KiB per JSON response.
- 单篇正文最多 24 KiB，解析页正文总量 64 KiB。/ A body is at most 24 KiB; a parsed page holds
  at most 64 KiB of bodies.
- 持久缓存最多 128 条 metadata、32 篇正文、正文总量 256 KiB。/ Persistent cache holds at most
  128 metadata records, 32 bodies, and 256 KiB of body data.
- metadata 保存有界订阅源标题，供设备列表生成固定首字符图标。/ Metadata keeps a bounded
  feed title for the device's fixed source-initial icon.
- 超限、截断、尾随 JSON、类型错误和不完整 HTTP 响应都显式失败，不截短、不回退。
  / Oversized, truncated, trailing, mistyped, or incomplete input fails explicitly; nothing is
  silently truncated or accepted through fallback.

## Sync contract / 同步契约

`rss_sync_begin()` 后重复调用 `rss_sync_step()`。本地 read/unread 按目标值批量 PUT，因此可重试；
Miniflux 2.3.1 的 bookmark API 只有 toggle，所以先与缓存状态比较，状态未知时失败而不猜测。
增量查询从已提交 `changed_at` 游标回退 300 秒，靠 ID upsert 去重；只有所有分页成功且缓存原子落盘
后才推进游标。

After `rss_sync_begin()`, call `rss_sync_step()` repeatedly. Read/unread uses idempotent
target-value batch PUTs. Miniflux 2.3.1 exposes bookmark as toggle only, so the queue compares
against cached state first and fails instead of guessing when state is unknown. Incremental fetch
overlaps the committed `changed_at` cursor by 300 seconds and deduplicates by ID; the cursor
advances only after every page succeeds and the cache replacement is committed.
