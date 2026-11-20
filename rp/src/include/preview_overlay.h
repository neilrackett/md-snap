/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: preview_overlay.h
 * Author: Neil Rackett
 * Copyright: 2026 - Neil Rackett
 * License: GPL v3
 * Description: Low-res ST screenshot preview overlay support.
 */

#ifndef PREVIEW_OVERLAY_H
#define PREVIEW_OVERLAY_H

#include <stdbool.h>

#define PREVIEW_OVERLAY_X 176
#define PREVIEW_OVERLAY_Y 32
#define PREVIEW_OVERLAY_WIDTH 144
#define PREVIEW_OVERLAY_HEIGHT 90

void preview_overlay_init(void);
void preview_overlay_disable(void);
/**
 * @brief Decode a screenshot into the menu's preview block.
 *
 * @param forTtLow the menu is in TT low on a TT (144x180, 256 colours);
 *                 otherwise ST low (144x90, 16 colours)
 */
bool preview_overlay_show(const char *folder, const char *filename,
                          bool forTtLow);

#endif  // PREVIEW_OVERLAY_H
