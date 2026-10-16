/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 产品页清单。注册表编号保持稳定，五栏视觉顺序由 ui_nav_layout.h 映射；工厂页独立。
 *
 * Product page table. Registry indices stay stable; ui_nav_layout.h maps the five visual tabs. Factory pages stay separate.
 */

#include "app_registry.h"

extern const app_desc_t app_book;
extern const app_desc_t app_image;
extern const app_desc_t app_dashboard;
extern const app_desc_t app_files;
extern const app_desc_t app_device_settings;
extern const app_desc_t app_rss;

// 正式产品页面清单；工厂测试与历史示例不进入用户导航。
// Production page registry; factory diagnostics and legacy examples are not user-navigable.
static const app_desc_t* const s_apps[] = {
    &app_dashboard,
    &app_book,
    &app_files,
    &app_device_settings,
    &app_image,
    &app_rss,
};

#define APP_COUNT ((int)(sizeof(s_apps) / sizeof(s_apps[0])))

int app_count(void) {
    return APP_COUNT;
}

const app_desc_t* app_at(int index) {
    if (index < 0 || index >= APP_COUNT) return NULL;
    return s_apps[index];
}

int app_index_of(const app_desc_t* app) {
    for (int i = 0; i < APP_COUNT; i++) {
        if (s_apps[i] == app) return i;
    }
    return -1;
}

const app_desc_t* app_home_page(void) {
    return &app_dashboard;
}
