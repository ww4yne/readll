/* SPDX-License-Identifier: Apache-2.0
 * 中文：轻量底栏边界，绘制和刷新共用，防止局部扫描碰到固定图标。
 * English: Lightweight navigation bounds shared by painting and presentation to protect fixed icons.
 */
#pragma once
#include <stdbool.h>

#define UI_NAV_TAB_COUNT 5
#define UI_NAV_RSS_APP_INDEX 5

// 注册表编号兼容已保存的恢复记录；底栏顺序独立于注册表。/ Preserve persisted registry indices independently of visual order.
static inline int ui_nav_app_index(int slot) {
    const int indices[UI_NAV_TAB_COUNT] = {0, 1, UI_NAV_RSS_APP_INDEX, 2, 3};
    return slot >= 0 && slot < UI_NAV_TAB_COUNT ? indices[slot] : -1;
}

static inline bool ui_nav_is_main(int index) {
    return (index >= 0 && index < 4) || index == UI_NAV_RSS_APP_INDEX;
}

#define UI_NAV_TOP 1096
// 只扩大触控热区，不移动图标或改变内容/底栏的刷新边界。
// Expand touch admission only; icons and content/navigation refresh boundaries stay fixed.
#define UI_NAV_TOUCH_TOP (UI_NAV_TOP - 16)
#define UI_NAV_MARKER_TOP (UI_NAV_TOP + 1)
#define UI_NAV_MARKER_BOTTOM (UI_NAV_TOP + 6)
// 内容向内收至32列边界；横条单独短刷或并入快刷，外扩范围均止于图标首笔之前。
// Round content inward to 32 columns; marker-only or fused fast scans end before any icon ink.
#define UI_NAV_REFRESH_END (UI_NAV_TOP & ~31)
#define UI_NAV_MARKER_SCAN_END ((UI_NAV_MARKER_BOTTOM + 31) & ~31)
