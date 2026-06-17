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

// --- CRC32 (bitwise, no table) ------------------------------------------
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len) {
  for (uint32_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int k = 0; k < 8; k++) {
      uint32_t mask = -(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
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
  for (uint32_t i = 0; i < len; i++) {
    pw->buf[pw->buf_len++] = data[i];
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
                      const uint8_t palette[][3], uint16_t ncolors) {
  memset(pw, 0, sizeof(*pw));
  pw->f = f;
  pw->adler_a = 1;
  pw->adler_b = 0;

  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  png_raw(pw, sig, sizeof(sig));

  // IHDR: width, height, bit depth 8, colour type 3 (indexed), no interlace.
  uint8_t ihdr[13];
  be32(ihdr, width);
  be32(ihdr + 4, height);
  ihdr[8] = 8;   // bit depth
  ihdr[9] = 3;   // colour type: indexed
  ihdr[10] = 0;  // compression
  ihdr[11] = 0;  // filter
  ihdr[12] = 0;  // interlace
  png_chunk(pw, "IHDR", ihdr, sizeof(ihdr));

  // PLTE: ncolors RGB triplets.
  uint8_t plte[PNG_MAX_COLORS * 3];
  if (ncolors > PNG_MAX_COLORS) {
    ncolors = PNG_MAX_COLORS;
  }
  for (uint16_t i = 0; i < ncolors; i++) {
    plte[i * 3 + 0] = palette[i][0];
    plte[i * 3 + 1] = palette[i][1];
    plte[i * 3 + 2] = palette[i][2];
  }
  png_chunk(pw, "PLTE", plte, (uint32_t)ncolors * 3);

  // IDAT: zlib stream of stored DEFLATE blocks. Compute the total data length
  // up front so the chunk length can be written before streaming.
  uint32_t raw = (uint32_t)height * (1u + width);  // filter byte + row per line
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

// Emit one uncompressed (raw) image byte through the stored-block machinery,
// folding it into Adler32 along the way.
static void png_emit_raw_byte(png_writer_t *pw, uint8_t byte) {
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
  png_emit(pw, &byte, 1);
  pw->block_remaining--;
  pw->raw_remaining--;

  // Adler32 over the raw (uncompressed) data.
  pw->adler_a = (pw->adler_a + byte) % 65521u;
  pw->adler_b = (pw->adler_b + pw->adler_a) % 65521u;
}

bool png_writer_row(png_writer_t *pw, const uint8_t *indices, uint16_t width) {
  if (pw->error) {
    return false;
  }
  png_emit_raw_byte(pw, 0);  // PNG filter type 0 (None)
  for (uint16_t i = 0; i < width; i++) {
    png_emit_raw_byte(pw, indices[i]);
  }
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
