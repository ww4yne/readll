# 版本变更 / Changelog

按版本简述对用户可见的变化；使用方法见 [README](../README.zh-CN.md)。
User-visible changes by release; see [README](../README.md) for usage.

## 开发中 / Unreleased

- 原创 readll 官网以 “read all” 为主题并列展示书籍与文章，重做布局、配色、文案与可访问预览，保留 WebUSB、TF 下载与说明书入口。
  Introduce an original “read all” website for books and articles with new layout, palette, copy and accessible previews, retaining WebUSB, TF downloads and documentation.
- 新增 Miniflux RSS 阅读：12 条未读队列、纯文本正文分页与已读多设备同步；从菜单进入，不改变四个主页面。只持久化列表与待同步状态，正文按需获取、仅留本次阅读内存，不按时间预缓存；获取失败保留未读。
  Add menu-based Miniflux reading with a 12-row unread queue, paged text and synchronized read state. Persist only lists and pending state; fetch bodies on demand for the current session, with no time-based body caching. Failed opens remain unread.
- 传书网页新增 HTTPS Miniflux URL、每设备 API Key、连接测试与凭据清除；Token 不回显、不写日志。
  Configure and test an HTTPS Miniflux endpoint and per-device API key from the transfer page without exposing the token.
- 增加本地 mock Miniflux 与缓存/同步/错误回归，保留 `readll` project 名以兼容现有 OTA、TF 与打包工具。
  Add local Miniflux/cache/sync regressions and retain the `readll` project identity for OTA, TF and packaging compatibility.
- 项目、固件 UI、模拟器、网页、CMake project 与包名迁移为小写 `readll`；旧项目包不兼容。
  Migrate the project, firmware UI, simulator, website, CMake project and package names to lowercase `readll`; legacy packages are intentionally incompatible.
- 仅设备内联网 OTA 在新 origin 配置前显式禁用，不请求旧源或静默回退；WebUSB 静态刷机站、完整 USB 包与 TF Bin 继续生成。
  Disable only device-side online OTA until a new origin is configured, with no legacy requests or silent fallback; retain the WebUSB static site, complete USB bundle and TF image.

旧项目版本历史不作为 readll 当前品牌历史复制；硬件与第三方上游出处见 README 和第三方许可。
Legacy project release history is not copied into the current readll brand history; hardware and third-party provenance remains in README and notices.