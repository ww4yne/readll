/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：保存 RSS 正文独立排版偏好；字体文件仍与 EPUB 阅读器共用。
 * English: Persist RSS-specific article typography while sharing the EPUB reader face.
 *
 * 冻结：不保存列表几何；列表固定十二行与固定字号。
 * Frozen: Never persist list geometry; the list stays at twelve rows and a fixed type size.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t px;
    uint8_t line_percent;
    uint8_t paragraph_percent;
    uint8_t margin_px;
    uint8_t tracking;
    uint8_t indent_em;
    int8_t weight;
    uint8_t reading_line;
} rss_preferences_t;

/// 读取并校验偏好；不存在或损坏的字段使用产品默认。/ Load checked preferences, defaulting missing or invalid fields.
void rss_preferences_load(rss_preferences_t *out);
/// 原子保存整组偏好；失败时不修改调用方状态。/ Commit the complete preference set atomically.
bool rss_preferences_save(const rss_preferences_t *preferences);
/// 写入标准或紧凑预设。/ Fill the standard or compact preset.
void rss_preferences_preset(rss_preferences_t *out, bool compact);
/// 校验所有字段。/ Validate every field.
bool rss_preferences_valid(const rss_preferences_t *preferences);
