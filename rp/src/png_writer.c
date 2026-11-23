/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: png_writer.c
 * Author: Neil Rackett
 * Copyright: 2026 - Neil Rackett
 * License: GPL v3
 * Description: Minimal streaming indexed PNG encoder. See png_writer.h.
 */

#include "png_writer.h"

#include <string.h>

#define PNG_STORED_BLOCK_MAX 0xFFFF  // max literal bytes per stored DEFLATE block

#define ADLER_MOD 65521u
#define ADLER_NMAX 5552  // bytes before the Adler32 sums need reducing

// --- CRC32, a nibble at a time (a 64-byte table) --------------------------
// TT screens are written from inside the command callback, so this is on the
// ST's critical path: about 4x quicker than bit by bit.
static const uint32_t crc_nibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len) {
  for (uint32_t i = 0; i < len; i++) {
    crc ^= data[i];
    crc = (crc >> 4) ^ crc_nibble[crc & 15];
    crc = (crc >> 4) ^ crc_nibble[crc & 15];
  }
  return crc;
}

// --- low-level file helpers ---------------------------------------------
static void png_flush(png_writer_t *pw) {
  if (pw->error || pw->buf_len == 0) {
    pw->buf_len = 0;
    return;
  }
  UINT bw = 0;
  FRESULT fr = f_write(pw->f, pw->buf, pw->buf_len, &bw);
  if (fr != FR_OK || bw != pw->buf_len) {
    pw->error = true;
  }
  pw->buf_len = 0;
}

// Emit IDAT data bytes: staged for f_write and folded into the IDAT CRC.
static void png_emit(png_writer_t *pw, const uint8_t *data, uint32_t len) {
  pw->crc = crc32_update(pw->crc, data, len);
  while (len > 0) {
    uint32_t k = sizeof(pw->buf) - pw->buf_len;
    if (k > len) {
      k = len;
    }
    memcpy(&pw->buf[pw->buf_len], data, k);
    pw->buf_len += (uint16_t)k;
    data += k;
    len -= k;
    if (pw->buf_len == sizeof(pw->buf)) {
      png_flush(pw);
    }
  }
}

// Write raw bytes straight to file (used for chunk framing, not IDAT data).
static void png_raw(png_writer_t *pw, const uint8_t *data, uint32_t len) {
  png_flush(pw);  // keep ordering with any staged IDAT bytes
  if (pw->error) {
    return;
  }
  UINT bw = 0;
  FRESULT fr = f_write(pw->f, data, len, &bw);
  if (fr != FR_OK || bw != len) {
    pw->error = true;
  }
}

static void be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

// Write a complete, self-contained chunk (length, type, data, CRC).
static void png_chunk(png_writer_t *pw, const char *type, const uint8_t *data,
                      uint32_t len) {
  uint8_t hdr[8];
  be32(hdr, len);
  memcpy(hdr + 4, type, 4);
  png_raw(pw, hdr, 8);

  uint32_t crc = 0xFFFFFFFFu;
  crc = crc32_update(crc, (const uint8_t *)type, 4);
  if (len) {
    crc = crc32_update(crc, data, len);
    png_raw(pw, data, len);
  }
  uint8_t crcb[4];
  be32(crcb, crc ^ 0xFFFFFFFFu);
  png_raw(pw, crcb, 4);
}

// --- public API ---------------------------------------------------------
bool png_writer_begin(png_writer_t *pw, FIL *f, uint16_t width, uint16_t height,
                      const uint8_t palette[][3], uint16_t ncolors,
                      uint8_t bit_depth) {
  memset(pw, 0, sizeof(*pw));
  pw->f = f;
  pw->adler_a = 1;
  pw->adler_b = 0;

  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  png_raw(pw, sig, sizeof(sig));

  // IHDR: width, height, bit depth, colour type 3 (indexed), no interlace.
  uint8_t ihdr[13];
  be32(ihdr, width);
  be32(ihdr + 4, height);
  ihdr[8] = bit_depth;
  ihdr[9] = 3;   // colour type: indexed
  ihdr[10] = 0;  // compression
  ihdr[11] = 0;  // filter
  ihdr[12] = 0;  // interlace
  png_chunk(pw, "IHDR", ihdr, sizeof(ihdr));

  // PLTE: ncolors RGB triplets (palette[][3] is already laid out that way).
  if (ncolors > PNG_MAX_COLORS) {
    ncolors = PNG_MAX_COLORS;
  }
  png_chunk(pw, "PLTE", &palette[0][0], (uint32_t)ncolors * 3);

  // IDAT: zlib stream of stored DEFLATE blocks. Compute the total data length
  // up front so the chunk length can be written before streaming.
  uint32_t row_bytes = ((uint32_t)width * bit_depth + 7u) / 8u;
  uint32_t raw = (uint32_t)height * (1u + row_bytes);  // filter byte + row
  uint32_t num_blocks =
      (raw + PNG_STORED_BLOCK_MAX - 1) / PNG_STORED_BLOCK_MAX;
  if (num_blocks == 0) {
    num_blocks = 1;  // a zero-length stored block still needs a header
  }
  uint32_t idat_len = 2 /* zlib header */ + raw + 5u * num_blocks /* block hdrs */
                      + 4 /* adler32 */;

  uint8_t hdr[8];
  be32(hdr, idat_len);
  memcpy(hdr + 4, "IDAT", 4);
  png_raw(pw, hdr, 8);

  pw->crc = 0xFFFFFFFFu;
  pw->crc = crc32_update(pw->crc, (const uint8_t *)"IDAT", 4);

  uint8_t zlib_hdr[2] = {0x78, 0x01};  // CMF/FLG: deflate, 32K window, no dict
  png_emit(pw, zlib_hdr, 2);

  pw->raw_remaining = raw;
  pw->block_remaining = 0;
  return !pw->error;
}

// Emit uncompressed image bytes through the stored-block machinery, folding
// them into Adler32 along the way.
static void png_emit_raw(png_writer_t *pw, const uint8_t *data, uint32_t len) {
  while (len > 0) {
    if (pw->block_remaining == 0) {
      uint32_t blen = pw->raw_remaining;
      if (blen > PNG_STORED_BLOCK_MAX) {
        blen = PNG_STORED_BLOCK_MAX;
      }
      uint8_t bhdr[5];
      bhdr[0] = (pw->raw_remaining <= PNG_STORED_BLOCK_MAX) ? 1 : 0;  // BFINAL
      bhdr[1] = (uint8_t)(blen & 0xFF);
      bhdr[2] = (uint8_t)(blen >> 8);
      bhdr[3] = (uint8_t)(~blen & 0xFF);
      bhdr[4] = (uint8_t)((~blen >> 8) & 0xFF);
      png_emit(pw, bhdr, 5);
      pw->block_remaining = blen;
    }
    uint32_t k = pw->block_remaining < len ? pw->block_remaining : len;
    if (k > ADLER_NMAX) {
      k = ADLER_NMAX;  // the sums can't overflow before they are reduced
    }
    png_emit(pw, data, k);
    pw->block_remaining -= k;
    pw->raw_remaining -= k;

    // Adler32 over the raw (uncompressed) data, reduced once per run.
    uint32_t a = pw->adler_a;
    uint32_t b = pw->adler_b;
    for (uint32_t i = 0; i < k; i++) {
      a += data[i];
      b += a;
    }
    pw->adler_a = a % ADLER_MOD;
    pw->adler_b = b % ADLER_MOD;
    data += k;
    len -= k;
  }
}

bool png_writer_row(png_writer_t *pw, const uint8_t *indices, uint16_t len) {
  if (pw->error) {
    return false;
  }
  static const uint8_t filter = 0;  // PNG filter type 0 (None)
  png_emit_raw(pw, &filter, 1);
  png_emit_raw(pw, indices, len);
  return !pw->error;
}

bool png_writer_end(png_writer_t *pw) {
  // Adler32 trailer (big-endian), part of the IDAT data so it folds into CRC.
  uint32_t adler = (pw->adler_b << 16) | pw->adler_a;
  uint8_t ab[4];
  be32(ab, adler);
  png_emit(pw, ab, 4);

  // IDAT CRC (written directly, must not include itself).
  uint8_t crcb[4];
  be32(crcb, pw->crc ^ 0xFFFFFFFFu);
  png_raw(pw, crcb, 4);

  png_chunk(pw, "IEND", NULL, 0);
  png_flush(pw);
  return !pw->error;
}
