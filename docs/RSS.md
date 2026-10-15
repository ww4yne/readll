# RSS 阅读 / RSS reading

`readll` 使用 Miniflux 作为订阅、文章和多设备状态的权威来源。首版不直接抓取
RSS/Atom，不模拟 Miniflux 网页会话，也不要求专用中转服务。

`readll` uses Miniflux as the source of truth for subscriptions,
entries and multi-device state. The first release does not fetch RSS/Atom
directly, emulate Miniflux browser sessions or require a dedicated proxy.

## 兼容范围 / Compatibility

- Miniflux 2.3.1 及以上。/ Miniflux 2.3.1 or later.
- HTTPS base URL，例如 `https://rss.wenb.in`。/ An HTTPS base URL, such as
  `https://rss.wenb.in`.
- 每设备独立 API Key，通过 `X-Auth-Token` 认证。/ A per-device API key sent
  with `X-Auth-Token`.
- 按时间排序的完整未读队列、正文文本和已读同步。设备 UI 不提供分类切换或星标入口。
  / A complete time-ordered unread queue, article text and read-state synchronization.
  The device UI does not expose category switching or bookmarks.

Fever、Google Reader、设备端订阅管理、搜索、正文图片和附件不属于首版范围。

Fever, Google Reader, on-device subscription management, search, article
images and enclosures are outside the first-release scope.

## 配置 / Configuration

在设备传书网页的 RSS 区域填写 Miniflux 地址和 API Key。地址必须使用 HTTPS；保存前会调用
`/v1/me` 验证。已有 Token 不会从设备回显，只能覆盖或清除。

Enter the Miniflux URL and API key in the RSS section of the transfer web
page. The URL must use HTTPS and `/v1/me` is checked before saving. An existing
token is never returned by the device; it can only be replaced or cleared.

API token 必须是 1 至 255 字节的非空白 ASCII；网页、HTTP API、NVS 与 RSS 核心使用同一边界。

The API token must contain 1 to 255 bytes of non-whitespace ASCII. The web UI,
HTTP API, NVS storage and RSS core use the same limit.

在 Miniflux 中为每台设备创建独立 API Key。不要使用账户主密码。设备丢失时在 Miniflux
中撤销该 Key，并在设备仍可访问时清除本地配置。

Create a separate Miniflux API key for every device. Do not use the account
password. Revoke that key in Miniflux if a device is lost, and clear the local
configuration when the device is still available.

## 同步语义 / Synchronization semantics

设备写入的是明确目标状态，不发送盲目 toggle：

The device records explicit desired states rather than blind toggles:

- 已读/未读使用 Miniflux 的设置状态接口，可安全重试。
  / Read/unread uses the Miniflux set-status API and is safe to retry.
- 打开某条文章时，当前页中该条及其上方条目立即进入本地已读队列；标题栏双对勾把当前页
  最多 12 条全部标为已读。返回列表后只显示剩余未读，即使离线也保持一致。
  / Opening an entry queues that entry and every visible entry above it as read. The title-bar
  double check marks the current page of up to 12 entries read. Returning to the list shows only
  the remaining unread entries, including while offline.
- Miniflux 2.3.1 的星标接口是 toggle；设备先比较服务器状态，仅在不同时调用，并在下一次
  拉取中确认。超时请求不会直接重放。
  / The Miniflux 2.3.1 bookmark API is a toggle. The device compares the
  server state first, calls it only when different, and confirms through the
  next pull. A timed-out request is not replayed blindly.
- 本地待提交操作先上传；确认后服务器状态为权威。
  / Pending local operations are uploaded first; the server is authoritative
  after confirmation.
- 增量拉取使用重叠的 `changed_after` 窗口和 entry ID 去重，并定期核对完整未读集合。
  / Incremental pulls use an overlapping `changed_after` window with entry-ID
  deduplication and periodically reconcile the complete unread set.

## 离线缓存 / Offline cache

默认限制为 128 条 metadata、32 篇正文、单篇 24 KiB、正文总量 256 KiB。正文按 LRU
淘汰；待同步操作不会因正文淘汰而丢失。NVS 只保存配置、游标和小型状态，文章存入可写
文件系统。

The initial limits are 128 metadata records, 32 article bodies, 24 KiB per
body and 256 KiB in total. Bodies use LRU eviction; pending synchronization
operations survive body eviction. NVS stores only configuration, cursors and
small state, while entries use the writable filesystem.

## 设备阅读界面 / Device reading UI

- RSS 入口直接显示全部未读，每页固定 12 条；列表标题单行截断，左侧固定图标显示来源首字符。
  / RSS opens directly to all unread entries, 12 per page. Titles remain on one line and a fixed
  left icon shows the first source character.
- 正文与 EPUB 共用 `html_text` 和 `book_layout`，但 RSS 独立保存排版数值。默认字号 22 px、
  行高 120%、页边距 10 px；另有 20 px / 112% 紧凑预设。
  / Articles share EPUB's `html_text` and `book_layout`, while RSS persists independent typography.
  Defaults are 22 px, 120% line height and 10 px margins, with a 20 px / 112% compact preset.
- 设置页提供阅读字体、字号、行距、段距、页边距、字距、首行缩进、字重和阅读线。列表几何
  固定，不提供数量或字号调整。
  / Settings expose the shared reading face, size, line and paragraph spacing, margins, tracking,
  first-line indent, weight and reading guides. List geometry and type size remain fixed.
- RSS 正文不显示图片，也不为图片保留空白页或占位。
  / RSS articles do not display images or reserve image pages or placeholders.
- 正文底部使用 4 px 分页进度条：剩余部分为浅灰，已读长度按当前页计算，并从灰度渐变到
  末端纯黑。
  / A 4 px pagination bar sits below the article: the remaining track is light gray, while the
  completed width follows the current page and gradients from gray to pure black at its endpoint.

如果内部文件系统空间不足，设备降级为只缓存 metadata，不写满分区。TF 卡不是首版正确性
的前提。

If internal storage is insufficient, the device falls back to metadata-only
caching instead of filling the partition. An SD card is not required for
first-release correctness.

## 安全边界 / Security boundaries

- 不在固件、日志、诊断或网页读取响应中包含 Token。
  / Never include the token in firmware constants, logs, diagnostics or web
  read responses.
- 正式构建不允许 HTTP 或跳过证书校验。
  / Production builds do not allow HTTP or disabled certificate validation.
- API Key 当前没有细粒度 scope；每设备独立、可撤销是最低要求。
  / API keys currently lack fine-grained scopes; per-device revocability is
  the minimum requirement.

## 性能门槛 / Performance gate

若 20 条列表响应超过 512 KiB、内部 heap 峰值超过 96 KiB、PSRAM 峰值超过 2 MiB，
或 Wi-Fi 已连接时空缓存首屏 P95 超过 3 秒，应停止扩大设备端实现，改用
`/device/v1` 薄代理裁剪字段和提供严格游标。

If a 20-entry response exceeds 512 KiB, internal-heap growth exceeds 96 KiB,
PSRAM growth exceeds 2 MiB, or cold-list P95 exceeds three seconds on an
already-connected network, stop expanding the device client and add a thin
`/device/v1` proxy for field projection and a strict cursor.
