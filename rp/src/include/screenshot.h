/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: screenshot.h
 * Author: Neil Rackett
 * Copyright: 2026 - Neil Rackett
 * License: GPL v3
 * Description: Receives the Atari ST screen pushed by the resident m68k VBL
 * grabber (userfw.s), deplanes it, applies the palette, pixel-doubles per
 * resolution and writes a 640x400 indexed PNG to the SD card. On a TT, its
 * own low/medium/high modes are written as 640x480 / 640x480 / 1280x960 PNGs
 * row by row as the screen arrives (too big to buffer on the RP).
 */

#ifndef SCREENSHOT_H
#define SCREENSHOT_H

#include <stdbool.h>

#include "tprotocol.h"

// Command IDs (must match APP_SCREENSHOT_* in target/atarist/src/main.s).
#define APP_SCREENSHOT_BEGIN 0x10
#define APP_SCREENSHOT_DATA 0x11
#define APP_SCREENSHOT_STREAM_BEGIN 0x12
#define APP_SCREENSHOT_STREAM_DATA 0x13

/**
 * @brief Initialise the screenshot receiver.
 *
 * @param folder Destination folder on the SD card (e.g. "/screenshots").
 */
void screenshot_init(const char *folder);

/**
 * @brief chandler callback: collects BEGIN metadata + 16 screen chunks.
 *
 * Register with chandler_addCB(). Ignores non-screenshot commands.
 */
void __not_in_flash_func(screenshot_command_cb)(TransmissionProtocol *protocol,
                                                uint16_t *payloadPtr);

/**
 * @brief True once a full screen has been received and is ready to write.
 */
bool screenshot_pending(void);

// What became of a screen (screenshot_writePending).
typedef enum {
  SCREENSHOT_SAVED,
  SCREENSHOT_FAILED,       // not written (no SD card, or it is full)
  SCREENSHOT_UNSUPPORTED,  // a mode MD/Snap can't capture: nothing was sent
} screenshot_result_t;

/**
 * @brief Decode the pending screen and write it as a PNG. Clears the pending
 *        flag. Call from the foreground loop (does blocking SD I/O).
 */
screenshot_result_t screenshot_writePending(void);

/**
 * @brief Capture the RP's own 320x200 mono framebuffer (the MD/Snap menu)
 *        straight to a 640x400 PNG — no cartridge round-trip. Lets SELECT
 *        screenshot the app's own screens while the menu is showing.
 *
 * @return true if the PNG was written successfully.
 */
bool screenshot_captureLocal(void);

/**
 * @brief The path of the PNG written last ("" before the first).
 */
const char *screenshot_lastPath(void);

/**
 * @brief The NNNN of a "snap_NNNN_..." name (any case), the order screenshots
 *        are taken in; 0 for any other name.
 */
unsigned screenshot_number(const char *name);

/**
 * @brief The menu preview a machine like the one the last screen came from
 *        shows: 1 TT low (a TT), 0 ST low, -1 none (ST high or TT high, so a
 *        mono monitor).
 */
int screenshot_lastPreviewKind(void);

/**
 * @brief The SD card has been mounted afresh (perhaps another card): find its
 *        next free screenshot number again before the next file.
 */
void screenshot_cardChanged(void);

#endif  // SCREENSHOT_H
