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

#define PNG_SRC_WIDTH 640
#define PNG_SRC_HEIGHT 400

#define PREVIEW_GROUPS_PER_ROW (PREVIEW_OVERLAY_WIDTH / 16)
#define PREVIEW_ROW_BYTES (PREVIEW_GROUPS_PER_ROW * 8)

#define PREVIEW_BLOCK_OFFSET CHANDLER_APP_FREE_OFFSET
#define PREVIEW_FLAG_OFFSET 0
#define PREVIEW_PALETTE_OFFSET 2
#define PREVIEW_DATA_OFFSET (PREVIEW_PALETTE_OFFSET + 32)

#define IDAT_BUF_SIZE 512
#define PREVIEW_BORDER_INDEX 15

typedef struct {
  FIL *file;
  uint32_t remaining;
  uint8_t buf[IDAT_BUF_SIZE];
  UINT pos;
  UINT len;
} idat_reader_t;

static uint8_t srcRow[PNG_SRC_WIDTH];
static uint8_t scaledRow[PREVIEW_OVERLAY_WIDTH];

static uint32_t overlayBase(void) {
  return (uint32_t)&__rom_in_ram_start__ + PREVIEW_BLOCK_OFFSET;
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

static bool decodeIdat(FIL *file, uint32_t idatLen) {
  idat_reader_t reader = {
      .file = file,
      .remaining = idatLen,
      .pos = 0,
      .len = 0,
  };

  uint8_t z0 = 0;
  uint8_t z1 = 0;
  if (!idatReadByte(&reader, &z0) || !idatReadByte(&reader, &z1)) {
    return false;
  }
  if ((z0 & 0x0F) != 8) {
    return false;
  }
  (void)z1;

  uint32_t blockRemaining = 0;
  uint16_t targetRow = 0;
  uint16_t nextSourceRow = 0;
  for (uint16_t y = 0; y < PNG_SRC_HEIGHT; y++) {
    uint8_t filter = 0;
    if (!idatReadRawByte(&reader, &blockRemaining, &filter) || filter != 0) {
      return false;
    }
    for (uint16_t x = 0; x < PNG_SRC_WIDTH; x++) {
      if (!idatReadRawByte(&reader, &blockRemaining, &srcRow[x])) {
        return false;
      }
    }

    if (targetRow < PREVIEW_OVERLAY_HEIGHT && y == nextSourceRow) {
      for (uint16_t x = 0; x < PREVIEW_OVERLAY_WIDTH; x++) {
        uint32_t sx = ((uint32_t)x * PNG_SRC_WIDTH) / PREVIEW_OVERLAY_WIDTH;
        scaledRow[x] = srcRow[sx];
      }
      writePlanarRow(targetRow, scaledRow);
      targetRow++;
      nextSourceRow =
          ((uint32_t)targetRow * PNG_SRC_HEIGHT) / PREVIEW_OVERLAY_HEIGHT;
    }
  }

  return targetRow == PREVIEW_OVERLAY_HEIGHT;
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
      if (be32(ihdr) != PNG_SRC_WIDTH || be32(ihdr + 4) != PNG_SRC_HEIGHT ||
          ihdr[8] != 8 || ihdr[9] != 3 || ihdr[10] != 0 || ihdr[11] != 0 ||
          ihdr[12] != 0) {
        return false;
      }
      if (!skipBytes(file, 4)) {
        return false;
      }
      haveIhdr = true;
    } else if (memcmp(type, "PLTE", 4) == 0) {
      uint8_t plte[16 * 3];
      uint32_t used = len > sizeof(plte) ? sizeof(plte) : len;
      if (!readExact(file, plte, used)) {
        return false;
      }
      if (len > used && !skipBytes(file, len - used)) {
        return false;
      }
      if (!skipBytes(file, 4)) {
        return false;
      }
      writePalette(plte, used);
      havePlte = true;
    } else if (memcmp(type, "IDAT", 4) == 0) {
      if (!haveIhdr || !havePlte) {
        return false;
      }
      return decodeIdat(file, len);
    } else if (memcmp(type, "IEND", 4) == 0) {
      return false;
    } else {
      if (!skipBytes(file, len + 4)) {
        return false;
      }
    }
  }
}

bool preview_overlay_show(const char *folder, const char *filename) {
  preview_overlay_disable();

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
