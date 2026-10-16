/* SPDX-License-Identifier: Apache-2.0
 * 中文：提供首页用的 RSS 本地快照；由持有缓存和待同步文件格式的 RSS 页面实现。
 * English: Local RSS snapshots for Home, implemented by the RSS owner of cache and pending-file formats.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool counts_known;       ///< 本地计数已验证 / Local counts validated
    size_t unread;           ///< 叠加待同步操作后的缓存未读 / Cached unread after pending overrides
    size_t pending;          ///< 待服务端确认的操作数 / Operations awaiting server confirmation
    char status[64];         ///< 不含账户秘密的状态 / Status without account secrets
} rss_overview_t;

/// 进入首页时读取本地快照，不联网、不获取正文；失败通过 status 显示，计数未知。
/// Read a local snapshot on Home entry without networking or bodies; status exposes failures and counts remain unknown.
void rss_overview_load(rss_overview_t *out);
