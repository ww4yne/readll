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

底栏依次为 **首页 / 书架 / RSS / 文件 / 设置**，点 **RSS** 进入未读列表。
首页的 RSS 信息卡也可进入；设置中不再放阅读入口。正文与 RSS 排版设置隐藏底栏，保留原三键操作；列表中键返回首页。

The bottom tabs are **Home / Bookshelf / RSS / Files / Settings**. Open the unread
list from **RSS** or the RSS card on Home, not Settings. Article and RSS typography
views hide the tabs and retain their hardware keys; the middle list key returns Home.

首页信息卡显示**缓存未读数**与**待同步操作数**，叠加尚未确认的已读/未读操作，不冒充服务端总未读数（本地最多 128 条 metadata）。
配置、缓存、存储或内存读取失败显示具体状态与未知计数，不显示假的零。同步结果只保留本次开机已知状态；冷启动显示本地列表状态，不声称刚刚同步成功。
进入首页读取一次本地快照，绘制与空闲时钟更新不读缓存、不联网，不获取或保存正文。

Home shows **cached unread** and **pending-operation** counts, applying unconfirmed
read/unread overrides. These are not server-wide totals: the local cache holds at
most 128 metadata entries. Configuration/cache/storage/memory failures show an
explicit status and unknown counts, not a fabricated zero. Sync results reflect
this boot only; cold boot reports a local list rather than claiming recent success.
Home loads its snapshot once on entry. Painting and idle clock updates do not read
the cache, connect, fetch bodies or persist them.

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
- 成功打开某条文章时，当前页中该条及其上方条目立即进入本地已读队列；标题栏双对勾把当前页
  最多 12 条全部标为已读。返回列表后只显示剩余未读，即使离线也保持一致。
  / Successfully opening an entry queues that entry and every visible entry above it as read. The title-bar
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

## 列表持久化与按需正文 / Persistent lists and on-demand articles

浏览列表时通常会略过大部分条目。设备因此只持久化最多 128 条 metadata（标题、来源、
时间、ID 与状态）和独立的待同步操作，不保存任何正文，也不按发布时间、缓存时长或定时器
预缓存正文。metadata 达到容量时按本地访问序号淘汰，不按文章年龄过期。

Most entries are skipped during list review. The device therefore persists
only up to 128 metadata records (title, source, timestamps, ID and state) plus
the separate pending-operation queue. It never persists bodies or pre-caches
them by publication date, age or timer. Metadata eviction uses a local access
sequence at capacity, not article age.

点击文章时调用 `GET /v1/entries/{id}`，仅为本次阅读解析正文，单篇上限仍为 24 KiB；
返回列表或离开 RSS 后释放。打开失败或离线时明确提示，所选条目及上方条目不变为已读，
可重试；整页已读仍可离线排队。已经打开的文章可在断网后继续翻页，但关闭后必须联网重开。

Opening calls `GET /v1/entries/{id}` and parses that body for the current
reading session only, retaining the 24 KiB per-article limit. Returning to the
list or leaving RSS releases it. Offline or failed opens report an error and
leave the selected entry and those above it unread, so opening can be retried.
Mark-page-as-read still queues offline. An already-open article remains
readable after disconnecting, but reopening it requires a connection.

Miniflux 标准列表 API 仍返回 `content`；设备接收有界响应后丢弃该字段，不建立正文池。
这减少本地保留、存储写入与正文容量限制对跳读的影响，但不声称减少服务端传输字节；
列表响应仍有 96 KiB 的硬上限。定时同步只更新 metadata 和状态，不建立正文缓存。

The standard Miniflux list API still returns `content`; the device discards it
after receiving the bounded response, without filling a body pool. This reduces
retention and storage writes and keeps per-body limits out of list triage; it
does not claim reduced server bandwidth. List responses still have a 96 KiB
hard bound. Periodic synchronization updates only metadata and state.

旧版 v2 缓存加载时流式校验并丢弃正文，保留 metadata 与同步游标，立即原子重写为
仅 metadata 的 v3 文件；独立待同步操作不受影响。迁移写入失败会显式报告存储错误。

Loading a legacy v2 cache validates and discards bodies in bounded chunks,
preserves metadata and the sync cursor, and immediately rewrites a metadata-only
v3 file atomically. The separate pending queue is unaffected. A failed migration
write reports a storage error.

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

列表与待同步状态保存在内部文件系统，正文仅在 PSRAM/内存中处理。TF 卡不是 RSS
正确性的前提。内部存储不可用时明确提示，不伪装为已持久化。

The internal filesystem stores lists and pending state; bodies are processed
only in PSRAM/memory. An SD card is not required for RSS correctness.
Unavailable internal storage is reported rather than presented as durable state.

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
