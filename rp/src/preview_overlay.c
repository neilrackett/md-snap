/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: preview_overlay.c
 * Author: Neil Rackett
 * Copyright: 2026 - Neil Rackett
 * License: GPL v3
 * Description: Decode MD/Snap PNGs into the menu's planar preview overlay,
 * and keep each finished preview on the SD card so it can be shown again
 * with one small read rather than by reading the whole PNG.
 */

#include "preview_overlay.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "chandler.h"
#include "constants.h"
#include "debug.h"
#include "ff.h"
#include "memfunc.h"
#include "png_writer.h"

// MD/Snap writes 640x400 and 640x480 8-bit PNGs, and 1280x960 1-bit ones
// (TT high), so a row is at most 1280 bytes.
#define PNG_MAX_ROW_BYTES 1280
#define PNG_MAX_WIDTH 1280
#define PREVIEW_COLORS 16

#define PREVIEW_GROUPS_PER_ROW (PREVIEW_OVERLAY_WIDTH / 16)
#define PREVIEW_ROW_BYTES (PREVIEW_GROUPS_PER_ROW * 8)

#define PREVIEW_BLOCK_OFFSET CHANDLER_APP_FREE_OFFSET
#define PREVIEW_FLAG_OFFSET 0
#define PREVIEW_PALETTE_OFFSET 2
#define PREVIEW_DATA_OFFSET (PREVIEW_PALETTE_OFFSET + 32)

// The PNG is read this much at a time: whole sectors go straight from the
// card into the buffer, several to a command. The buffer is in the shared
// region, after the most the preview block uses (the blend's packed colours)
// and before the menu's framebuffer: RP RAM is short, and its heap must not
// reach the region (memmap_rp.ld).
#define IDAT_BUF_SIZE 4096
#define PREVIEW_BORDER_INDEX 15

// On a TT the menu is in TT low (target/atarist/src/ttmenu.s, which must
// match): the preview is 144x180 in 8 interleaved planes, after a 256-word TT
// palette whose entries 0 and 255 stay the menu's white paper and black ink.
#define TT_PREVIEW_HEIGHT 180
#define TT_PREVIEW_DATA_OFFSET (PREVIEW_PALETTE_OFFSET + PNG_MAX_COLORS * 2)
#define TT_PREVIEW_ROW_BYTES (PREVIEW_GROUPS_PER_ROW * 16)
#define TT_PAPER 0
#define TT_INK 255

#define TT_PREVIEW_END \
  (TT_PREVIEW_DATA_OFFSET + TT_PREVIEW_HEIGHT * TT_PREVIEW_ROW_BYTES)
#define ST_PREVIEW_END \
  (PREVIEW_DATA_OFFSET + PREVIEW_OVERLAY_HEIGHT * PREVIEW_ROW_BYTES)

// The blend keeps each thumbnail pixel's 12-bit colour, packed two to three
// bytes, where the preview's planes go, until the palette is chosen (see
// blendImage). It must stay clear of the menu's framebuffer.
#define TT_PACKED_ROW_BYTES (PREVIEW_OVERLAY_WIDTH * 3 / 2)
#define IDAT_BUF_OFFSET                                         \
  ((PREVIEW_BLOCK_OFFSET + TT_PREVIEW_DATA_OFFSET +              \
    TT_PREVIEW_HEIGHT * TT_PACKED_ROW_BYTES + 3) & ~3)
_Static_assert(IDAT_BUF_OFFSET + IDAT_BUF_SIZE <= CHANDLER_FRAMEBUFFER_OFFSET,
               "the PNG read buffer would reach the framebuffer");

// Finished previews, in a hidden folder beside the screenshots: a header,
// then the preview block as it is in memory from the palette on (everything
// but the flag). Checked against the PNG's size and time, and removed when a
// new screenshot is given the PNG's name. Bump PREVIEW_CACHE_VERSION when
// what is drawn changes, so old ones are made again.
#define PREVIEW_CACHE_DIR ".preview"
#define PREVIEW_CACHE_MAGIC 0x5053444Du  // "MDSP"
#define PREVIEW_CACHE_VERSION 2  // 1: unchecked, and some made corrupt

typedef struct {
  uint32_t magic;
  uint8_t version;
  uint8_t ttLow;
  uint8_t reserved[2];
  uint32_t pngSize;
  uint32_t pngStamp;
  uint32_t checksum;  // of the preview block that follows (cacheChecksum)
} preview_cache_header_t;

typedef struct {
  FIL *file;
  uint32_t remaining;  // IDAT bytes not yet read from the file
  uint32_t blockLeft;  // bytes left in the current stored DEFLATE block
  UINT pos;
  UINT len;
} idat_reader_t;

#define idatBuf ((uint8_t *)((uintptr_t)&__rom_in_ram_start__ + IDAT_BUF_OFFSET))
static uint8_t srcRow[PNG_MAX_ROW_BYTES];
static uint8_t scaledRow[PREVIEW_OVERLAY_WIDTH];

// Source image, from IHDR / PLTE.
static uint32_t pngWidth;
static uint32_t pngHeight;
static uint8_t pngDepth;  // bits per pixel: 1, 2, 4 or 8
static uint8_t pngPalette[PNG_MAX_COLORS * 3];
static uint32_t plteCount;

// Source palette index -> preview colour (0-15). A palette of more than 16
// colours (TT low) is cut to the 16 the thumbnail uses most.
static uint8_t indexMap[PNG_MAX_COLORS];
static uint16_t histogram[PNG_MAX_COLORS];

static bool ttLow;  // drawing for the menu in TT low

static uintptr_t overlayBase(void) {
  return (uintptr_t)&__rom_in_ram_start__ + PREVIEW_BLOCK_OFFSET;
}

static void overlayWriteWord(uint32_t offset, uint16_t value) {
  WRITE_WORD(overlayBase(), offset, value);
}

void preview_overlay_disable(void) {
  overlayWriteWord(PREVIEW_FLAG_OFFSET, 0);
}

void preview_overlay_init(void) {
  preview_overlay_disable();
}

static bool readExact(FIL *file, void *buf, UINT len) {
  UINT br = 0;
  return f_read(file, buf, len, &br) == FR_OK && br == len;
}

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static bool skipBytes(FIL *file, uint32_t len) {
  return f_lseek(file, f_tell(file) + len) == FR_OK;
}

static uint16_t rgbToSt(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t sr = (uint16_t)((r * 7u + 127u) / 255u);
  uint16_t sg = (uint16_t)((g * 7u + 127u) / 255u);
  uint16_t sb = (uint16_t)((b * 7u + 127u) / 255u);
  return (uint16_t)((sr << 8) | (sg << 4) | sb);
}

static void writePalette(const uint8_t *plte, uint32_t plteLen) {
  uint16_t colors[16];
  uint32_t count = plteLen / 3;
  if (count > 16) {
    count = 16;
  }

  for (uint32_t i = 0; i < count; i++) {
    colors[i] = rgbToSt(plte[i * 3], plte[i * 3 + 1], plte[i * 3 + 2]);
  }
  for (uint32_t i = count; i < 16; i++) {
    colors[i] = 0;
  }

  for (uint32_t i = 0; i < 16; i++) {
    overlayWriteWord(PREVIEW_PALETTE_OFFSET + i * 2, colors[i]);
  }
}

// The next bytes of the IDAT chunk (its zlib stream).
static bool idatRead(idat_reader_t *reader, uint8_t *out, uint32_t n) {
  while (n > 0) {
    if (reader->pos >= reader->len) {
      if (reader->remaining == 0) {
        return false;
      }
      UINT want = reader->remaining > IDAT_BUF_SIZE ? (UINT)IDAT_BUF_SIZE
                                                    : (UINT)reader->remaining;
      UINT br = 0;
      if (f_read(reader->file, idatBuf, want, &br) != FR_OK || br == 0) {
        return false;
      }
      reader->remaining -= br;
      reader->pos = 0;
      reader->len = br;
    }
    uint32_t k = reader->len - reader->pos;
    if (k > n) {
      k = n;
    }
    memcpy(out, &idatBuf[reader->pos], k);
    reader->pos += k;
    out += k;
    n -= k;
  }
  return true;
}

// The next bytes of the image data, across the stored DEFLATE blocks.
static bool idatReadData(idat_reader_t *reader, uint8_t *out, uint32_t n) {
  while (n > 0) {
    if (reader->blockLeft == 0) {
      uint8_t hdr[5];
      if (!idatRead(reader, hdr, sizeof(hdr))) {
        return false;
      }
      if ((hdr[0] & 0x06) != 0) {
        return false;  // only stored DEFLATE blocks are supported
      }
      uint16_t len = (uint16_t)hdr[1] | ((uint16_t)hdr[2] << 8);
      uint16_t nlen = (uint16_t)hdr[3] | ((uint16_t)hdr[4] << 8);
      if ((uint16_t)~len != nlen) {
        return false;
      }
      reader->blockLeft = len;
      continue;
    }
    uint32_t k = reader->blockLeft < n ? reader->blockLeft : n;
    if (!idatRead(reader, out, k)) {
      return false;
    }
    reader->blockLeft -= k;
    out += k;
    n -= k;
  }
  return true;
}

// One preview row in the menu's interleaved planes, framed: 4 plane words per
// 16 pixels in ST low (16 colours, framed in colour 15), 8 in TT low (framed
// in ink).
static void writePreviewRow(uint16_t row, const uint8_t *pixels) {
  const uint16_t nplanes = ttLow ? 8 : 4;
  const uint16_t height = ttLow ? TT_PREVIEW_HEIGHT : PREVIEW_OVERLAY_HEIGHT;
  const uint8_t border = ttLow ? TT_INK : PREVIEW_BORDER_INDEX;
  const bool edgeRow = row == 0 || row == height - 1;
  uint32_t offset = ttLow ? TT_PREVIEW_DATA_OFFSET + row * TT_PREVIEW_ROW_BYTES
                          : PREVIEW_DATA_OFFSET + row * PREVIEW_ROW_BYTES;
  for (uint16_t group = 0; group < PREVIEW_GROUPS_PER_ROW; group++) {
    uint16_t planes[8] = {0};
    for (uint16_t x = 0; x < 16; x++) {
      uint16_t px = group * 16 + x;
      uint8_t idx = pixels[px];
      if (idx >= (1u << nplanes) || edgeRow || px == 0 ||
          px == PREVIEW_OVERLAY_WIDTH - 1) {
        idx = border;
      }
      uint16_t bit = (uint16_t)(0x8000u >> x);
      for (uint16_t plane = 0; plane < nplanes; plane++) {
        if (idx & (1u << plane)) {
          planes[plane] |= bit;
        }
      }
    }
    for (uint16_t plane = 0; plane < nplanes; plane++) {
      overlayWriteWord(offset, planes[plane]);
      offset += 2;
    }
  }
}

static uint16_t rgbToTt(const uint8_t *rgb) {
  return (uint16_t)((((rgb[0] * 15u + 127u) / 255u) << 8) |
                    (((rgb[1] * 15u + 127u) / 255u) << 4) |
                    ((rgb[2] * 15u + 127u) / 255u));
}

static uint32_t ttDistance(uint16_t a, uint16_t b) {
  int dr = (int)((a >> 8) & 15) - (int)((b >> 8) & 15);
  int dg = (int)((a >> 4) & 15) - (int)((b >> 4) & 15);
  int db = (int)(a & 15) - (int)(b & 15);
  return (uint32_t)(dr * dr + dg * dg + db * db);
}

// Each source colour drawn as itself.
static void mapIdentity(void) {
  for (uint32_t i = 0; i < PNG_MAX_COLORS; i++) {
    indexMap[i] = (uint8_t)i;
  }
}

// The TT low palette: the image's own colours, except that entries 0 and 255
// stay white and black for the menu. Pixels of the image's colours 0 and 255,
// where those are something else, take the nearest of the rest.
static void writeTtPalette(void) {
  uint16_t tt[PNG_MAX_COLORS] = {0};
  for (uint32_t i = 0; i < plteCount; i++) {
    tt[i] = rgbToTt(&pngPalette[i * 3]);
  }
  const uint16_t image0 = tt[TT_PAPER];
  const uint16_t image255 = tt[TT_INK];
  tt[TT_PAPER] = 0x0FFF;
  tt[TT_INK] = 0x0000;

  mapIdentity();
  const uint16_t mine[2] = {TT_PAPER, TT_INK};
  const uint16_t was[2] = {image0, image255};
  for (int k = 0; k < 2; k++) {
    uint16_t i = mine[k];
    if (i >= plteCount || was[k] == tt[i]) {
      continue;  // unused, or already white / black
    }
    uint32_t bestDist = UINT32_MAX;
    for (uint32_t c = 0; c < PNG_MAX_COLORS; c++) {
      if (c != TT_PAPER && c != TT_INK && c >= plteCount) {
        continue;  // not one of the image's colours
      }
      if (c == i) {
        continue;
      }
      uint32_t dist = ttDistance(was[k], tt[c]);
      if (dist < bestDist) {
        bestDist = dist;
        indexMap[i] = (uint8_t)c;
      }
    }
  }

  for (uint32_t i = 0; i < PNG_MAX_COLORS; i++) {
    overlayWriteWord(PREVIEW_PALETTE_OFFSET + i * 2, tt[i]);
  }
}

// Palette index of pixel x in srcRow (packed MSB first below 8 bits).
static uint8_t rowPixel(uint32_t x) {
  if (pngDepth == 8) {
    return srcRow[x];
  }
  uint32_t bit = x * pngDepth;
  uint8_t shift = (uint8_t)(8 - pngDepth - (bit & 7));
  return (uint8_t)((srcRow[bit >> 3] >> shift) & ((1u << pngDepth) - 1));
}

// Pick the PREVIEW_COLORS most used colours (from histogram), map every source
// colour to the nearest of them, and write them as the preview palette.
static void reducePalette(void) {
  uint8_t chosen[PREVIEW_COLORS * 3];
  uint32_t n = 0;
  for (; n < PREVIEW_COLORS; n++) {
    int best = -1;
    for (uint32_t i = 0; i < plteCount; i++) {
      if (histogram[i] && (best < 0 || histogram[i] > histogram[best])) {
        best = (int)i;
      }
    }
    if (best < 0) {
      break;
    }
    histogram[best] = 0;
    memcpy(&chosen[n * 3], &pngPalette[best * 3], 3);
  }
  for (uint32_t i = 0; i < plteCount; i++) {
    uint32_t bestDist = UINT32_MAX;
    for (uint32_t c = 0; c < n; c++) {
      int dr = pngPalette[i * 3] - chosen[c * 3];
      int dg = pngPalette[i * 3 + 1] - chosen[c * 3 + 1];
      int db = pngPalette[i * 3 + 2] - chosen[c * 3 + 2];
      uint32_t dist = (uint32_t)(dr * dr + dg * dg + db * db);
      if (dist < bestDist) {
        bestDist = dist;
        indexMap[i] = (uint8_t)c;
      }
    }
  }
  writePalette(chosen, n * 3);
}

// Start reading an IDAT chunk's zlib stream (stored DEFLATE blocks, as
// MD/Snap writes them).
static bool idatOpen(idat_reader_t *reader, FIL *file, uint32_t idatLen) {
  *reader = (idat_reader_t){.file = file, .remaining = idatLen};
  uint8_t zlib[2];
  if (!idatRead(reader, zlib, sizeof(zlib))) {
    return false;
  }
  return (zlib[0] & 0x0F) == 8;
}

// The next image row into srcRow (filter type 0, as MD/Snap writes them).
static bool idatNextRow(idat_reader_t *reader) {
  uint8_t filter = 0;
  if (!idatReadData(reader, &filter, 1) || filter != 0) {
    return false;
  }
  return idatReadData(reader, srcRow, (pngWidth * pngDepth + 7) / 8);
}

// Decode the IDAT stream into the thumbnail, a source pixel for each of its
// pixels. With countOnly, just tally the colours it samples into histogram[].
static bool decodeIdat(FIL *file, uint32_t idatLen, bool countOnly) {
  idat_reader_t reader;
  if (!idatOpen(&reader, file, idatLen)) {
    return false;
  }

  uint16_t height = ttLow ? TT_PREVIEW_HEIGHT : PREVIEW_OVERLAY_HEIGHT;
  uint16_t targetRow = 0;
  uint32_t nextSourceRow = 0;
  for (uint32_t y = 0; y < pngHeight && targetRow < height; y++) {
    if (!idatNextRow(&reader)) {
      return false;
    }

    if (targetRow < height && y == nextSourceRow) {
      for (uint16_t x = 0; x < PREVIEW_OVERLAY_WIDTH; x++) {
        uint32_t sx = ((uint32_t)x * pngWidth) / PREVIEW_OVERLAY_WIDTH;
        uint8_t idx = rowPixel(sx);
        if (countOnly) {
          histogram[idx]++;
        } else {
          scaledRow[x] = indexMap[idx];
        }
      }
      if (!countOnly) {
        writePreviewRow(targetRow, scaledRow);
      }
      targetRow++;
      nextSourceRow = ((uint32_t)targetRow * pngHeight) / height;
    }
  }

  return targetRow == height;
}

// A TT low preview of a screenshot with fewer than 256 colours is blended:
// each of its pixels is the average of the source pixels it covers, so a
// dither shows as the colour it makes on screen rather than as whichever of
// its pixels a sample lands on. The averages are rounded to the TT's 12-bit
// colours, counted and kept as the PNG is read (once); then the 254 most used
// become the palette (entries 0 and 255 stay white and black for the menu),
// the rest take the nearest of those, and the kept colours are drawn.
#define TT_COLOURS 4096
#define TT_CHOSEN 0x8000  // in tally[]: the colour's palette entry is in bits 0-7

static uint16_t tally[TT_COLOURS];  // per 12-bit colour: a count, then an entry
static uint32_t sumR[PREVIEW_OVERLAY_WIDTH];
static uint32_t sumG[PREVIEW_OVERLAY_WIDTH];
static uint32_t sumB[PREVIEW_OVERLAY_WIDTH];
static uint32_t sumN[PREVIEW_OVERLAY_WIDTH];

// After counting: give the most used colours palette entries 1-254,
// map every other colour to its nearest, and write the palette.
static void chooseTtPalette(void) {
  uint16_t pal[PNG_MAX_COLORS] = {0};
  pal[TT_PAPER] = 0x0FFF;
  pal[TT_INK] = 0x0000;
  if (tally[0x0FFF]) {
    tally[0x0FFF] = TT_CHOSEN | TT_PAPER;
  }
  if (tally[0x0000]) {
    tally[0x0000] = TT_CHOSEN | TT_INK;
  }
  uint16_t used = 1;
  for (; used < TT_INK; used++) {
    int best = -1;
    for (int c = 0; c < TT_COLOURS; c++) {
      if (tally[c] && !(tally[c] & TT_CHOSEN) &&
          (best < 0 || tally[c] > tally[best])) {
        best = c;
      }
    }
    if (best < 0) {
      break;
    }
    tally[best] = TT_CHOSEN | used;
    pal[used] = (uint16_t)best;
  }
  for (int c = 0; c < TT_COLOURS; c++) {
    if (!tally[c] || (tally[c] & TT_CHOSEN)) {
      continue;
    }
    uint32_t bestDist = UINT32_MAX;
    uint16_t bestEntry = TT_INK;
    for (uint16_t e = 0; e < PNG_MAX_COLORS; e++) {
      if (e >= used && e != TT_INK) {
        continue;  // not given a colour
      }
      uint32_t dist = ttDistance((uint16_t)c, pal[e]);
      if (dist < bestDist) {
        bestDist = dist;
        bestEntry = e;
      }
    }
    tally[c] = TT_CHOSEN | bestEntry;
  }
  for (uint32_t i = 0; i < PNG_MAX_COLORS; i++) {
    overlayWriteWord(PREVIEW_PALETTE_OFFSET + i * 2, pal[i]);
  }
}

static void clearSums(void) {
  memset(sumR, 0, sizeof(sumR));
  memset(sumG, 0, sizeof(sumG));
  memset(sumB, 0, sizeof(sumB));
  memset(sumN, 0, sizeof(sumN));
}

// Where the blend keeps a thumbnail row's colours until they are drawn.
static uint8_t *packedRow(uint16_t row) {
  return (uint8_t *)(overlayBase() + TT_PREVIEW_DATA_OFFSET) +
         (uint32_t)row * TT_PACKED_ROW_BYTES;
}

// One thumbnail row from the sums: its colours counted and kept.
static void blendRow(uint16_t row) {
  uint8_t *packed = packedRow(row);
  for (uint16_t x = 0; x < PREVIEW_OVERLAY_WIDTH; x += 2) {
    uint16_t c[2];
    for (int i = 0; i < 2; i++) {
      uint32_t n = sumN[x + i];
      uint8_t rgb[3] = {(uint8_t)((sumR[x + i] + n / 2) / n),
                        (uint8_t)((sumG[x + i] + n / 2) / n),
                        (uint8_t)((sumB[x + i] + n / 2) / n)};
      c[i] = rgbToTt(rgb);
      tally[c[i]]++;
    }
    *packed++ = (uint8_t)(c[0] >> 4);
    *packed++ = (uint8_t)((c[0] << 4) | (c[1] >> 8));
    *packed++ = (uint8_t)c[1];
  }
  clearSums();
}

// Draw the kept colours with the palette chooseTtPalette gave them. Row r's
// planes end at (r + 1) * 144, before row r + 1's colours at (r + 1) * 216,
// so each row is drawn over colours already read.
static void drawBlended(void) {
  for (uint16_t row = 0; row < TT_PREVIEW_HEIGHT; row++) {
    const uint8_t *packed = packedRow(row);
    for (uint16_t x = 0; x < PREVIEW_OVERLAY_WIDTH; x += 2) {
      uint16_t c0 = (uint16_t)((packed[0] << 4) | (packed[1] >> 4));
      uint16_t c1 = (uint16_t)(((packed[1] & 15) << 8) | packed[2]);
      packed += 3;
      scaledRow[x] = (uint8_t)(tally[c0] & 0xFF);
      scaledRow[x + 1] = (uint8_t)(tally[c1] & 0xFF);
    }
    writePreviewRow(row, scaledRow);
  }
}

static bool blendImage(FIL *file, uint32_t idatLen) {
  idat_reader_t reader;
  if (!idatOpen(&reader, file, idatLen)) {
    return false;
  }
  memset(tally, 0, sizeof(tally));
  clearSums();
  uint16_t rows = 0;
  for (uint32_t y = 0; y < pngHeight; y++) {
    if (!idatNextRow(&reader)) {
      return false;
    }
    // Source pixel x goes into thumbnail column x * 144 / width, stepped
    // rather than divided (the image is at least 144 wide).
    uint32_t col = 0;
    uint32_t step = 0;
    for (uint32_t x = 0; x < pngWidth; x++) {
      const uint8_t *rgb = &pngPalette[rowPixel(x) * 3];
      sumR[col] += rgb[0];
      sumG[col] += rgb[1];
      sumB[col] += rgb[2];
      sumN[col]++;
      step += PREVIEW_OVERLAY_WIDTH;
      if (step >= pngWidth) {
        step -= pngWidth;
        col++;
      }
    }
    // The last source row of a thumbnail row finishes it.
    if (y + 1 == pngHeight || ((y + 1) * TT_PREVIEW_HEIGHT) / pngHeight !=
                                  (y * TT_PREVIEW_HEIGHT) / pngHeight) {
      blendRow(rows);
      rows++;
    }
  }
  if (rows != TT_PREVIEW_HEIGHT) {
    return false;
  }
  chooseTtPalette();
  drawBlended();
  return true;
}

static bool decodePngToOverlay(FIL *file) {
  static const uint8_t pngSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  uint8_t sig[8];
  if (!readExact(file, sig, sizeof(sig)) ||
      memcmp(sig, pngSig, sizeof(sig)) != 0) {
    return false;
  }

  bool haveIhdr = false;
  bool havePlte = false;
  while (true) {
    uint8_t hdr[8];
    if (!readExact(file, hdr, sizeof(hdr))) {
      return false;
    }
    uint32_t len = be32(hdr);
    const uint8_t *type = hdr + 4;

    if (memcmp(type, "IHDR", 4) == 0) {
      uint8_t ihdr[13];
      if (len != sizeof(ihdr) || !readExact(file, ihdr, sizeof(ihdr))) {
        return false;
      }
      pngWidth = be32(ihdr);
      pngHeight = be32(ihdr + 4);
      pngDepth = ihdr[8];
      if (pngWidth == 0 || pngHeight == 0 ||
          (pngDepth != 1 && pngDepth != 2 && pngDepth != 4 && pngDepth != 8) ||
          (pngWidth * pngDepth + 7) / 8 > PNG_MAX_ROW_BYTES ||
          pngWidth > PNG_MAX_WIDTH || ihdr[9] != 3 ||
          ihdr[10] != 0 || ihdr[11] != 0 || ihdr[12] != 0) {
        return false;
      }
      if (!skipBytes(file, 4)) {
        return false;
      }
      haveIhdr = true;
    } else if (memcmp(type, "PLTE", 4) == 0) {
      uint32_t used = len > sizeof(pngPalette) ? sizeof(pngPalette) : len;
      if (!readExact(file, pngPalette, used)) {
        return false;
      }
      if (len > used && !skipBytes(file, len - used)) {
        return false;
      }
      if (!skipBytes(file, 4)) {
        return false;
      }
      plteCount = used / 3;
      havePlte = true;
    } else if (memcmp(type, "IDAT", 4) == 0) {
      if (!haveIhdr || !havePlte) {
        return false;
      }
      if (ttLow) {
        if (plteCount < PNG_MAX_COLORS && pngWidth >= PREVIEW_OVERLAY_WIDTH &&
            pngHeight >= TT_PREVIEW_HEIGHT) {
          return blendImage(file, len);
        }
        writeTtPalette();  // 256 colours: the image's own, pixel for pixel
        return decodeIdat(file, len, false);
      }
      if (plteCount <= PREVIEW_COLORS) {
        mapIdentity();
        writePalette(pngPalette, plteCount * 3);
        return decodeIdat(file, len, false);
      }
      // More than 16 colours: one pass to see which the thumbnail uses, then
      // back to the start of the IDAT data to draw it.
      FSIZE_t idatStart = f_tell(file);
      memset(histogram, 0, sizeof(histogram));
      if (!decodeIdat(file, len, true) || f_lseek(file, idatStart) != FR_OK) {
        return false;
      }
      reducePalette();
      return decodeIdat(file, len, false);
    } else if (memcmp(type, "IEND", 4) == 0) {
      return false;
    } else {
      if (!skipBytes(file, len + 4)) {
        return false;
      }
    }
  }
}

static bool decodePng(const char *path) {
  FIL file;
  if (f_open(&file, path, FA_READ) != FR_OK) {
    return false;
  }
  bool ok = decodePngToOverlay(&file);
  f_close(&file);
  return ok;
}

// The preview block from the palette on, as the cache keeps it.
static uint8_t *cachedPart(void) {
  return (uint8_t *)(overlayBase() + PREVIEW_PALETTE_OFFSET);
}

static uint32_t cachedBytes(bool forTtLow) {
  return (forTtLow ? TT_PREVIEW_END : ST_PREVIEW_END) - PREVIEW_PALETTE_OFFSET;
}

// "<folder>/.preview/<name without .png>.tt" (or .st) for a PNG's path.
static bool cachePath(char *out, size_t size, const char *pngPath,
                      bool forTtLow) {
  const char *slash = strrchr(pngPath, '/');
  if (slash == NULL) {
    return false;
  }
  const char *name = slash + 1;
  size_t nameLen = strlen(name);
  if (nameLen > 4 && strcasecmp(name + nameLen - 4, ".png") == 0) {
    nameLen -= 4;
  }
  int n = snprintf(out, size, "%.*s/" PREVIEW_CACHE_DIR "/%.*s.%s",
                   (int)(slash - pngPath), pngPath, (int)nameLen, name,
                   forTtLow ? "tt" : "st");
  return n > 0 && n < (int)size;
}

static uint32_t cacheChecksum(uint32_t n) {
  const uint16_t *w = (const uint16_t *)cachedPart();
  uint32_t sum = 0;
  for (uint32_t i = 0; i < n / 2; i++) {
    sum = ((sum << 1) | (sum >> 31)) + w[i];
  }
  return sum;
}

static bool cacheLoad(const char *pngPath, bool forTtLow, uint32_t pngSize,
                      uint32_t pngStamp) {
  char path[128];
  if (!cachePath(path, sizeof(path), pngPath, forTtLow)) {
    return false;
  }
  FIL file;
  if (f_open(&file, path, FA_READ) != FR_OK) {
    return false;
  }
  preview_cache_header_t hdr;
  uint32_t n = cachedBytes(forTtLow);
  bool ok = readExact(&file, &hdr, sizeof(hdr)) &&
            hdr.magic == PREVIEW_CACHE_MAGIC &&
            hdr.version == PREVIEW_CACHE_VERSION &&
            hdr.ttLow == (forTtLow ? 1 : 0) && hdr.pngSize == pngSize &&
            hdr.pngStamp == pngStamp && f_size(&file) == sizeof(hdr) + n &&
            readExact(&file, cachedPart(), n) &&
            cacheChecksum(n) == hdr.checksum;
  f_close(&file);
  if (ok && forTtLow) {
    // The menu's white paper and black ink, whatever the file says: one
    // that said otherwise would black out the menu.
    overlayWriteWord(PREVIEW_PALETTE_OFFSET + TT_PAPER * 2, 0x0FFF);
    overlayWriteWord(PREVIEW_PALETTE_OFFSET + TT_INK * 2, 0x0000);
  }
  return ok;
}

static void cacheSave(const char *pngPath, bool forTtLow, uint32_t pngSize,
                      uint32_t pngStamp) {
  char path[128];
  if (!cachePath(path, sizeof(path), pngPath, forTtLow)) {
    return;
  }
  // The folder first, hidden, if it is not there yet.
  char *slash = strrchr(path, '/');
  *slash = '\0';
  if (f_mkdir(path) == FR_OK) {
    f_chmod(path, AM_HID, AM_HID);
  }
  *slash = '/';

  FIL file;
  if (f_open(&file, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
    return;
  }
  preview_cache_header_t hdr = {.magic = PREVIEW_CACHE_MAGIC,
                                .version = PREVIEW_CACHE_VERSION,
                                .ttLow = forTtLow ? 1 : 0,
                                .pngSize = pngSize,
                                .pngStamp = pngStamp};
  uint32_t n = cachedBytes(forTtLow);
  hdr.checksum = cacheChecksum(n);
  UINT bw1 = 0;
  UINT bw2 = 0;
  bool ok = f_write(&file, &hdr, sizeof(hdr), &bw1) == FR_OK &&
            bw1 == sizeof(hdr) &&
            f_write(&file, cachedPart(), n, &bw2) == FR_OK && bw2 == n;
  ok = (f_close(&file) == FR_OK) && ok;
  if (!ok) {
    f_unlink(path);
  }
}

// Decode a PNG into the preview block and keep it on the card.
static bool makePreview(const char *path, bool forTtLow, uint32_t pngSize,
                        uint32_t pngStamp) {
  ttLow = forTtLow;
  if (!decodePng(path)) {
    preview_overlay_disable();
    return false;
  }
  cacheSave(path, forTtLow, pngSize, pngStamp);
  return true;
}

bool preview_overlay_show(const char *folder, const char *filename,
                          bool forTtLow, uint32_t pngSize, uint32_t pngStamp) {
  preview_overlay_disable();

  char path[128];
  int n = snprintf(path, sizeof(path), "%s/%s", folder, filename);
  if (n <= 0 || n >= (int)sizeof(path)) {
    return false;
  }

#if defined(_DEBUG) && (_DEBUG != 0)
  absolute_time_t start = get_absolute_time();
#endif
  bool cached = cacheLoad(path, forTtLow, pngSize, pngStamp);
  if (!cached && !makePreview(path, forTtLow, pngSize, pngStamp)) {
    return false;
  }
  DPRINTF("Preview %s: %s, %lld us\n", filename, cached ? "kept" : "made",
          absolute_time_diff_us(start, get_absolute_time()));

  overlayWriteWord(PREVIEW_FLAG_OFFSET, 1);
  return true;
}

bool preview_overlay_prepare(const char *path, bool forTtLow) {
  FILINFO fno;
  if (f_stat(path, &fno) != FR_OK) {
    return false;
  }
  preview_overlay_disable();
  if (!makePreview(path, forTtLow, (uint32_t)fno.fsize,
                   preview_overlay_stamp(fno.fdate, fno.ftime))) {
    return false;
  }
  DPRINTF("Preview %s: made for %s\n", path, forTtLow ? "TT low" : "ST low");
  return true;
}

void preview_overlay_forget(const char *path) {
  char cache[128];
  for (int tt = 0; tt < 2; tt++) {
    if (cachePath(cache, sizeof(cache), path, tt != 0)) {
      f_unlink(cache);
    }
  }
}
