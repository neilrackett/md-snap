/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: preview_overlay.c
 * Author: Neil Rackett
 * Copyright: 2026 - Neil Rackett
 * License: GPL v3
 * Description: Decode MD/Snap PNGs into a low-res ST planar preview overlay.
 */

#include "preview_overlay.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "chandler.h"
#include "constants.h"
#include "ff.h"
#include "memfunc.h"

// MD/Snap writes 640x400 and 640x480 8-bit PNGs, and 1280x960 1-bit ones
// (TT high), so a row is at most 1280 bytes.
#define PNG_MAX_ROW_BYTES 1280
#define PNG_MAX_COLORS 256
#define PREVIEW_COLORS 16

#define PREVIEW_GROUPS_PER_ROW (PREVIEW_OVERLAY_WIDTH / 16)
#define PREVIEW_ROW_BYTES (PREVIEW_GROUPS_PER_ROW * 8)

#define PREVIEW_BLOCK_OFFSET CHANDLER_APP_FREE_OFFSET
#define PREVIEW_FLAG_OFFSET 0
#define PREVIEW_PALETTE_OFFSET 2
#define PREVIEW_DATA_OFFSET (PREVIEW_PALETTE_OFFSET + 32)

#define IDAT_BUF_SIZE 512
#define PREVIEW_BORDER_INDEX 15

// On a TT the menu is in TT low (target/atarist/src/ttmenu.s, which must
// match): the preview is 144x180 in 8 interleaved planes, after a 256-word TT
// palette whose entries 0 and 255 stay the menu's white paper and black ink.
#define TT_PREVIEW_HEIGHT 180
#define TT_PREVIEW_PALETTE_OFFSET 2
#define TT_PREVIEW_DATA_OFFSET (TT_PREVIEW_PALETTE_OFFSET + PNG_MAX_COLORS * 2)
#define TT_PREVIEW_ROW_BYTES (PREVIEW_GROUPS_PER_ROW * 16)
#define TT_PAPER 0
#define TT_INK 255

typedef struct {
  FIL *file;
  uint32_t remaining;
  uint8_t buf[IDAT_BUF_SIZE];
  UINT pos;
  UINT len;
} idat_reader_t;

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

static bool idatReadByte(idat_reader_t *reader, uint8_t *out) {
  if (reader->pos >= reader->len) {
    if (reader->remaining == 0) {
      return false;
    }
    UINT want = reader->remaining > sizeof(reader->buf)
                    ? (UINT)sizeof(reader->buf)
                    : (UINT)reader->remaining;
    UINT br = 0;
    if (f_read(reader->file, reader->buf, want, &br) != FR_OK || br == 0) {
      return false;
    }
    reader->remaining -= br;
    reader->pos = 0;
    reader->len = br;
  }

  *out = reader->buf[reader->pos++];
  return true;
}

static bool idatReadRawByte(idat_reader_t *reader, uint32_t *blockRemaining,
                            uint8_t *out) {
  if (*blockRemaining == 0) {
    uint8_t hdr[5];
    for (size_t i = 0; i < sizeof(hdr); i++) {
      if (!idatReadByte(reader, &hdr[i])) {
        return false;
      }
    }
    if ((hdr[0] & 0x06) != 0) {
      return false;  // only stored DEFLATE blocks are supported
    }
    uint16_t len = (uint16_t)hdr[1] | ((uint16_t)hdr[2] << 8);
    uint16_t nlen = (uint16_t)hdr[3] | ((uint16_t)hdr[4] << 8);
    if ((uint16_t)~len != nlen) {
      return false;
    }
    *blockRemaining = len;
  }

  if (!idatReadByte(reader, out)) {
    return false;
  }
  (*blockRemaining)--;
  return true;
}

static void writePlanarRow(uint16_t row, const uint8_t *pixels) {
  uint32_t offset = PREVIEW_DATA_OFFSET + row * PREVIEW_ROW_BYTES;
  for (uint16_t group = 0; group < PREVIEW_GROUPS_PER_ROW; group++) {
    uint16_t planes[4] = {0, 0, 0, 0};
    for (uint16_t x = 0; x < 16; x++) {
      uint16_t px = group * 16 + x;
      uint8_t idx = pixels[px];
      if (idx >= 16) {
        idx = PREVIEW_BORDER_INDEX;
      }
      if (row == 0 || row == PREVIEW_OVERLAY_HEIGHT - 1 || px == 0 ||
          px == PREVIEW_OVERLAY_WIDTH - 1) {
        idx = PREVIEW_BORDER_INDEX;
      }
      uint16_t bit = (uint16_t)(0x8000u >> x);
      for (uint16_t plane = 0; plane < 4; plane++) {
        if (idx & (1u << plane)) {
          planes[plane] |= bit;
        }
      }
    }
    for (uint16_t plane = 0; plane < 4; plane++) {
      overlayWriteWord(offset, planes[plane]);
      offset += 2;
    }
  }
}

// One TT low preview row: 8 plane words per 16 pixels, framed in ink.
static void writePlanarRow8(uint16_t row, const uint8_t *pixels) {
  uint32_t offset = TT_PREVIEW_DATA_OFFSET + row * TT_PREVIEW_ROW_BYTES;
  for (uint16_t group = 0; group < PREVIEW_GROUPS_PER_ROW; group++) {
    uint16_t planes[8] = {0};
    for (uint16_t x = 0; x < 16; x++) {
      uint16_t px = group * 16 + x;
      uint8_t idx = pixels[px];
      if (row == 0 || row == TT_PREVIEW_HEIGHT - 1 || px == 0 ||
          px == PREVIEW_OVERLAY_WIDTH - 1) {
        idx = TT_INK;
      }
      uint16_t bit = (uint16_t)(0x8000u >> x);
      for (uint16_t plane = 0; plane < 8; plane++) {
        if (idx & (1u << plane)) {
          planes[plane] |= bit;
        }
      }
    }
    for (uint16_t plane = 0; plane < 8; plane++) {
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

  for (uint32_t i = 0; i < PNG_MAX_COLORS; i++) {
    indexMap[i] = (uint8_t)i;
  }
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
      int dr = (int)((was[k] >> 8) & 15) - (int)((tt[c] >> 8) & 15);
      int dg = (int)((was[k] >> 4) & 15) - (int)((tt[c] >> 4) & 15);
      int db = (int)(was[k] & 15) - (int)(tt[c] & 15);
      uint32_t dist = (uint32_t)(dr * dr + dg * dg + db * db);
      if (dist < bestDist) {
        bestDist = dist;
        indexMap[i] = (uint8_t)c;
      }
    }
  }

  for (uint32_t i = 0; i < PNG_MAX_COLORS; i++) {
    overlayWriteWord(TT_PREVIEW_PALETTE_OFFSET + i * 2, tt[i]);
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
  uint8_t z0 = 0;
  uint8_t z1 = 0;
  if (!idatReadByte(reader, &z0) || !idatReadByte(reader, &z1)) {
    return false;
  }
  return (z0 & 0x0F) == 8;
}

// The next image row into srcRow (filter type 0, as MD/Snap writes them).
static bool idatNextRow(idat_reader_t *reader, uint32_t *blockRemaining) {
  uint8_t filter = 0;
  if (!idatReadRawByte(reader, blockRemaining, &filter) || filter != 0) {
    return false;
  }
  uint32_t rowBytes = (pngWidth * pngDepth + 7) / 8;
  for (uint32_t x = 0; x < rowBytes; x++) {
    if (!idatReadRawByte(reader, blockRemaining, &srcRow[x])) {
      return false;
    }
  }
  return true;
}

// Decode the IDAT stream into the thumbnail, a source pixel for each of its
// pixels. With countOnly, just tally the colours it samples into histogram[].
static bool decodeIdat(FIL *file, uint32_t idatLen, bool countOnly) {
  idat_reader_t reader;
  if (!idatOpen(&reader, file, idatLen)) {
    return false;
  }

  uint16_t height = ttLow ? TT_PREVIEW_HEIGHT : PREVIEW_OVERLAY_HEIGHT;
  uint32_t blockRemaining = 0;
  uint16_t targetRow = 0;
  uint32_t nextSourceRow = 0;
  for (uint32_t y = 0; y < pngHeight; y++) {
    if (!idatNextRow(&reader, &blockRemaining)) {
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
        if (ttLow) {
          writePlanarRow8(targetRow, scaledRow);
        } else {
          writePlanarRow(targetRow, scaledRow);
        }
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
// colours; a first pass counts them, the 254 most used become the palette
// (entries 0 and 255 stay white and black for the menu), the rest take the
// nearest of those, and a second pass draws the thumbnail.
#define TT_COLOURS 4096
#define TT_CHOSEN 0x8000  // in tally[]: the colour's palette entry is in bits 0-7

static uint16_t tally[TT_COLOURS];  // per 12-bit colour: a count, then an entry
static uint32_t sumR[PREVIEW_OVERLAY_WIDTH];
static uint32_t sumG[PREVIEW_OVERLAY_WIDTH];
static uint32_t sumB[PREVIEW_OVERLAY_WIDTH];
static uint32_t sumN[PREVIEW_OVERLAY_WIDTH];

static uint32_t ttDistance(uint16_t a, uint16_t b) {
  int dr = (int)((a >> 8) & 15) - (int)((b >> 8) & 15);
  int dg = (int)((a >> 4) & 15) - (int)((b >> 4) & 15);
  int db = (int)(a & 15) - (int)(b & 15);
  return (uint32_t)(dr * dr + dg * dg + db * db);
}

// One thumbnail row from the sums: counted (draw false) or drawn.
static void blendRow(uint16_t row, bool draw) {
  for (uint16_t x = 0; x < PREVIEW_OVERLAY_WIDTH; x++) {
    uint32_t n = sumN[x];
    uint8_t rgb[3] = {(uint8_t)((sumR[x] + n / 2) / n),
                      (uint8_t)((sumG[x] + n / 2) / n),
                      (uint8_t)((sumB[x] + n / 2) / n)};
    uint16_t c = rgbToTt(rgb);
    if (draw) {
      scaledRow[x] = (uint8_t)(tally[c] & 0xFF);
    } else {
      tally[c]++;
    }
    sumR[x] = sumG[x] = sumB[x] = sumN[x] = 0;
  }
  if (draw) {
    writePlanarRow8(row, scaledRow);
  }
}

static bool blendPass(FIL *file, uint32_t idatLen, bool draw) {
  idat_reader_t reader;
  if (!idatOpen(&reader, file, idatLen)) {
    return false;
  }
  memset(sumR, 0, sizeof(sumR));
  memset(sumG, 0, sizeof(sumG));
  memset(sumB, 0, sizeof(sumB));
  memset(sumN, 0, sizeof(sumN));
  uint32_t blockRemaining = 0;
  uint16_t rows = 0;
  for (uint32_t y = 0; y < pngHeight; y++) {
    if (!idatNextRow(&reader, &blockRemaining)) {
      return false;
    }
    for (uint32_t x = 0; x < pngWidth; x++) {
      uint32_t col = (x * PREVIEW_OVERLAY_WIDTH) / pngWidth;
      const uint8_t *rgb = &pngPalette[rowPixel(x) * 3];
      sumR[col] += rgb[0];
      sumG[col] += rgb[1];
      sumB[col] += rgb[2];
      sumN[col]++;
    }
    // The last source row of a thumbnail row finishes it.
    uint32_t row = (y * TT_PREVIEW_HEIGHT) / pngHeight;
    if (y + 1 == pngHeight || ((y + 1) * TT_PREVIEW_HEIGHT) / pngHeight != row) {
      blendRow((uint16_t)row, draw);
      rows++;
    }
  }
  return rows == TT_PREVIEW_HEIGHT;
}

// After the counting pass: give the most used colours palette entries 1-254,
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
    overlayWriteWord(TT_PREVIEW_PALETTE_OFFSET + i * 2, pal[i]);
  }
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
          (pngWidth * pngDepth + 7) / 8 > PNG_MAX_ROW_BYTES || ihdr[9] != 3 ||
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
          // Blended: count, choose the palette, then draw.
          FSIZE_t idatStart = f_tell(file);
          memset(tally, 0, sizeof(tally));
          if (!blendPass(file, len, false) ||
              f_lseek(file, idatStart) != FR_OK) {
            return false;
          }
          chooseTtPalette();
          return blendPass(file, len, true);
        }
        writeTtPalette();  // 256 colours: the image's own, pixel for pixel
        return decodeIdat(file, len, false);
      }
      if (plteCount <= PREVIEW_COLORS) {
        for (uint32_t i = 0; i < PNG_MAX_COLORS; i++) {
          indexMap[i] = (uint8_t)i;
        }
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

bool preview_overlay_show(const char *folder, const char *filename,
                          bool forTtLow) {
  preview_overlay_disable();
  ttLow = forTtLow;

  char path[128];
  int n = snprintf(path, sizeof(path), "%s/%s", folder, filename);
  if (n <= 0 || n >= (int)sizeof(path)) {
    return false;
  }

  FIL file;
  if (f_open(&file, path, FA_READ) != FR_OK) {
    return false;
  }
  bool ok = decodePngToOverlay(&file);
  f_close(&file);
  if (!ok) {
    preview_overlay_disable();
    return false;
  }

  overlayWriteWord(PREVIEW_FLAG_OFFSET, 1);
  return true;
}
