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
#include <stdint.h>

#define PREVIEW_OVERLAY_X 176
#define PREVIEW_OVERLAY_Y 32
#define PREVIEW_OVERLAY_WIDTH 144
#define PREVIEW_OVERLAY_HEIGHT 90

void preview_overlay_init(void);
void preview_overlay_disable(void);
/**
 * @brief A PNG's time, as the preview cache checks it.
 */
static inline uint32_t preview_overlay_stamp(uint16_t fdate, uint16_t ftime) {
  return ((uint32_t)fdate << 16) | ftime;
}

/**
 * @brief Show a screenshot's preview in the menu: the one kept on the SD card
 *        if it is still the PNG's, else decoded from the PNG (and kept).
 *
 * @param forTtLow the menu is in TT low on a TT (144x180, 256 colours);
 *                 otherwise ST low (144x90, 16 colours)
 * @param pngSize, pngStamp the PNG's size and preview_overlay_stamp(), from
 *                 its directory entry
 */
bool preview_overlay_show(const char *folder, const char *filename,
                          bool forTtLow, uint32_t pngSize, uint32_t pngStamp);

/**
 * @brief Make and keep a just-written screenshot's preview, without showing
 *        it, so the menu has it at once. Uses the preview block.
 */
bool preview_overlay_prepare(const char *path, bool forTtLow);

/**
 * @brief Remove a PNG's kept previews: a new screenshot is taking its name.
 */
void preview_overlay_forget(const char *path);

#endif  // PREVIEW_OVERLAY_H
