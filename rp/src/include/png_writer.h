/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: png_writer.h
 * Author: Neil Rackett
 * Copyright: 2026 - Neil Rackett
 * License: GPL v3
 * Description: Minimal streaming indexed (colour-type 3) PNG encoder.
 *
 * Writes a palette PNG (1, 2, 4 or 8 bits per pixel) one row at a time
 * straight to a FatFs file, so a full image never has to be buffered in RAM. The IDAT zlib stream uses
 * uncompressed ("stored") DEFLATE blocks: the total compressed length is
 * computable up front from width/height, so the chunk length can be written
 * before the data is streamed. CRC32 (per chunk) and Adler32 (over the raw
 * image data) are accumulated incrementally.
 */

#ifndef PNG_WRITER_H
#define PNG_WRITER_H

#include <stdbool.h>
#include <stdint.h>

#include "ff.h"

#define PNG_MAX_COLORS 256

typedef struct {
  FIL *f;
  uint32_t crc;             // running CRC32 of the IDAT chunk (type + data)
  uint32_t adler_a;         // Adler32 low half
  uint32_t adler_b;         // Adler32 high half
  uint32_t raw_remaining;   // uncompressed bytes still to emit
  uint32_t block_remaining; // bytes left in the current stored DEFLATE block
  uint8_t buf[512];         // output staging buffer to batch f_write calls
  uint16_t buf_len;
  bool error;               // sticky write-error flag
} png_writer_t;

/**
 * @brief Start a PNG: writes signature, IHDR and PLTE, opens the IDAT stream.
 *
 * @param pw       writer state (caller-owned, zero-initialised internally)
 * @param f        open FatFs file (FA_WRITE|FA_CREATE_ALWAYS)
 * @param width    image width in pixels
 * @param height   image height in pixels
 * @param palette  ncolors RGB triplets
 * @param ncolors  number of palette entries (1..PNG_MAX_COLORS)
 * @param bit_depth bits per pixel: 1, 2, 4 or 8
 * @return true on success
 */
bool png_writer_begin(png_writer_t *pw, FIL *f, uint16_t width, uint16_t height,
                      const uint8_t palette[][3], uint16_t ncolors,
                      uint8_t bit_depth);

/**
 * @brief Emit one scanline: (width * bit_depth + 7) / 8 bytes of palette
 *        indices, packed leftmost pixel first (MSB first below 8 bits).
 */
bool png_writer_row(png_writer_t *pw, const uint8_t *indices, uint16_t len);

/**
 * @brief Finish the IDAT stream and write IEND.
 */
bool png_writer_end(png_writer_t *pw);

#endif  // PNG_WRITER_H
