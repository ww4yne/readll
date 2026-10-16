# 版本变更 / Changelog

按版本简述对用户可见的变化；使用方法见 [README](../README.zh-CN.md)。
User-visible changes by release; see [README](../README.md) for usage.

## 0.3.3-rc95

- 原创 readll 单页官网使用 “READ ALL” slogan，并列展示书籍与文章，保留界面预览、WebUSB、TF 下载与页内许可；注明界面沿用 kiikoread，详细操作链接至其使用说明，不复制或托管手册。
  Introduce an original single-page site with the “READ ALL” slogan, books and articles, previews, WebUSB, TF downloads and inline notices. Credit the kiikoread UI and link to its manual instead of copying or hosting it.
- 跟随 kiikoread rc95 的全部固件功能与交互：异步封面及头像、逐行预绘、按来源返回、时间恢复与同步、竖排修复、离散设置滚动、自由按键映射、原始 GC16 全刷及刷后释放像素驱动；合入 MindReset 蜂鸣器中断优化。RSS 操作与存储边界保持不变。
  Follow kiikoread rc95 firmware behavior: asynchronous covers/avatars, row-bounded prefetch, return origins, time restore/sync, vertical typography, discrete settings scrolling, free key mappings, original GC16 and post-refresh pixel release; import MindReset buzzer interrupt optimization. Preserve RSS behavior and storage boundaries.
- 构建自动检查上游变化，不自动合并；网页发布阻止未审查更新。
  Check upstream changes during builds without automatic merging; block publication on unreviewed changes.
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