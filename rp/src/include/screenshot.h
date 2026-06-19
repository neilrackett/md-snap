/**
 * File: screenshot.h
 * Author: Neil Rackett
 * Copyright: 2026 - Neil Rackett
 * License: GPL v3
 * Description: Receives the Atari ST screen pushed by the resident m68k VBL
 * grabber (userfw.s), deplanes it, applies the palette, pixel-doubles per
 * resolution and writes a 640x400 indexed PNG to the SD card.
 */

#ifndef SCREENSHOT_H
#define SCREENSHOT_H

#include <stdbool.h>

#include "tprotocol.h"

// Command IDs (must match APP_SCREENSHOT_* in target/atarist/src/main.s).
#define APP_SCREENSHOT_BEGIN 0x10
#define APP_SCREENSHOT_DATA 0x11

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

/**
 * @brief Decode the pending screen and write it as a PNG. Clears the pending
 *        flag. Call from the foreground loop (does blocking SD I/O).
 *
 * @return true if the PNG was written successfully.
 */
bool screenshot_writePending(void);

/**
 * @brief Capture the RP's own 320x200 mono framebuffer (the MD/Snap menu)
 *        straight to a 640x400 PNG — no cartridge round-trip. Lets SELECT
 *        screenshot the app's own screens while the menu is showing.
 *
 * @return true if the PNG was written successfully.
 */
bool screenshot_captureLocal(void);

#endif  // SCREENSHOT_H
