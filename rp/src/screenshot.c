/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: screenshot.c
 * Author: Neil Rackett
 * Copyright: 2026 - Neil Rackett
 * License: GPL v3
 * Description: ST screen receiver + decoder. See screenshot.h.
 */

#include "screenshot.h"

#include <stdio.h>
#include <string.h>

#include "chandler.h"
#include "constants.h"
#include "debug.h"
#include "ff.h"
#include "png_writer.h"

// An ST screen is always 32000 bytes, pushed as 16 chunks of 2000 bytes plus a
// metadata (BEGIN) command. These must match target/atarist/src/main.s.
#define SHOT_CHUNK_SIZE 2000
#define SHOT_NUM_CHUNKS 16
#define SHOT_SCREEN_SIZE 32000

// ETV mode uses smaller async stream frames so the timer hook never waits for
// RP acknowledgement inside the ISR.
#define SHOT_STREAM_FRAME_SIZE 500
#define SHOT_STREAM_FRAME_COUNT 64
#define SHOT_STREAM_ACK_SLOT 5

#define OUT_WIDTH 640
#define OUT_HEIGHT 400

// Payload word layout for the write transport (send_sync_write_command_to_
// sidecart): [token(2w)][d3(2w)][d4(2w)][d5(2w)][buffer...]. Params and buffer
// therefore start at fixed word offsets.
#define PW_D3 2   // first param word (low half)
#define PW_D4 4    // second param word
#define PW_D5 6    // third param word
#define PW_BUF 8   // buffer data begins here

// Received state.
static uint8_t source[SHOT_SCREEN_SIZE];
static uint16_t paletteRaw[16];
static uint32_t shotRez;          // 0 = low, 1 = med, 2 = high
static uint32_t shotMch;          // _MCH cookie value (high word = machine)
static uint16_t chunksMask;       // bit i set once chunk i has arrived
static uint64_t streamFrameMask;  // bit i set once async stream frame i arrived
static uint16_t streamSeq;
static bool beginReceived;
static volatile bool readyFlag;

static char destFolder[64] = "/screenshots";
static bool nextIndexValid;
static unsigned nextIndex = 1;

static inline uint32_t payload32(const uint16_t *p, int wordOffset) {
  return ((uint32_t)p[wordOffset + 1] << 16) | p[wordOffset];
}

static void write_stream_ack(uint16_t seq, uint16_t nextFrame) {
  uint32_t value = ((uint32_t)seq << 16) | nextFrame;
  uintptr_t slot = (uintptr_t)&__rom_in_ram_start__ +
                   CHANDLER_SHARED_VARIABLES_OFFSET +
                   (SHOT_STREAM_ACK_SLOT * 4);
  *((volatile uint16_t *)(slot + 2)) = (uint16_t)(value & 0xFFFF);
  *((volatile uint16_t *)slot) = (uint16_t)(value >> 16);
  DPRINTF("Screenshot STREAM ACK: seq=%u next=%u\n", seq, nextFrame);
}

void screenshot_init(const char *folder) {
  if (folder && folder[0]) {
    strncpy(destFolder, folder, sizeof(destFolder) - 1);
    destFolder[sizeof(destFolder) - 1] = '\0';
  }
  chunksMask = 0;
  streamFrameMask = 0;
  streamSeq = 0;
  beginReceived = false;
  readyFlag = false;
  write_stream_ack(0, 0);
}

void __not_in_flash_func(screenshot_command_cb)(TransmissionProtocol *protocol,
                                                uint16_t *payloadPtr) {
  (void)payloadPtr;
  const uint16_t *p = protocol->payload;

  switch (protocol->command_id) {
    case APP_SCREENSHOT_BEGIN: {
      shotRez = p[PW_D3];
      shotMch = payload32(p, PW_D4);
      for (int i = 0; i < 16; i++) {
        paletteRaw[i] = p[PW_BUF + i];
      }
      chunksMask = 0;
      streamFrameMask = 0;
      beginReceived = true;
      readyFlag = false;
      DPRINTF("Screenshot BEGIN: rez=%lu mch=0x%08lX\n",
              (unsigned long)shotRez, (unsigned long)shotMch);
      break;
    }
    case APP_SCREENSHOT_DATA: {
      if (!beginReceived) {
        break;  // chunk without metadata; ignore
      }
      uint32_t idx = p[PW_D3];
      if (idx >= SHOT_NUM_CHUNKS) {
        break;
      }
      // Each payload word is a big-endian ST screen word; unpack to bytes.
      uint8_t *dst = source + idx * SHOT_CHUNK_SIZE;
      const uint16_t *w = p + PW_BUF;
      for (int i = 0; i < SHOT_CHUNK_SIZE / 2; i++) {
        dst[i * 2] = (uint8_t)(w[i] >> 8);
        dst[i * 2 + 1] = (uint8_t)(w[i] & 0xFF);
      }
      chunksMask |= (uint16_t)(1u << idx);
      if (chunksMask == 0xFFFF) {
        readyFlag = true;  // all 16 chunks in
      }
      break;
    }
    case APP_SCREENSHOT_STREAM_BEGIN: {
      streamSeq = (uint16_t)p[PW_D3];
      shotRez = p[PW_D4];
      shotMch = payload32(p, PW_D5);
      for (int i = 0; i < 16; i++) {
        paletteRaw[i] = p[PW_BUF + i];
      }
      chunksMask = 0;
      streamFrameMask = 0;
      beginReceived = true;
      readyFlag = false;
      write_stream_ack(streamSeq, 0);
      DPRINTF("Screenshot STREAM BEGIN: seq=%u rez=%lu mch=0x%08lX\n",
              streamSeq, (unsigned long)shotRez, (unsigned long)shotMch);
      break;
    }
    case APP_SCREENSHOT_STREAM_DATA: {
      if (!beginReceived) {
        break;
      }
      uint16_t seq = (uint16_t)p[PW_D3];
      uint32_t idx = p[PW_D4];
      if (seq != streamSeq || idx >= SHOT_STREAM_FRAME_COUNT) {
        break;
      }
      uint8_t *dst = source + idx * SHOT_STREAM_FRAME_SIZE;
      const uint16_t *w = p + PW_BUF;
      for (int i = 0; i < SHOT_STREAM_FRAME_SIZE / 2; i++) {
        dst[i * 2] = (uint8_t)(w[i] >> 8);
        dst[i * 2 + 1] = (uint8_t)(w[i] & 0xFF);
      }
      streamFrameMask |= (uint64_t)1 << idx;
      write_stream_ack(seq, (uint16_t)(idx + 1));
      if (streamFrameMask == UINT64_MAX) {
        readyFlag = true;
      }
      break;
    }
    default:
      break;  // not ours
  }
}

bool screenshot_pending(void) { return readyFlag; }

// --- decode helpers ------------------------------------------------------
static void decode_palette(uint8_t out[][3], int ncolors) {
  bool ste = ((shotMch >> 16) >= 1);  // STE / Mega STE: 4 bits per gun
  for (int i = 0; i < ncolors; i++) {
    uint16_t v = paletteRaw[i];
    uint8_t rn = (v >> 8) & 0xF;
    uint8_t gn = (v >> 4) & 0xF;
    uint8_t bn = v & 0xF;
    if (ste) {
      // STE: 4-bit gun, but the extra LSB lives in bit 3 of each nibble.
      uint8_t r = (uint8_t)(((rn & 7) << 1) | ((rn & 8) >> 3));
      uint8_t g = (uint8_t)(((gn & 7) << 1) | ((gn & 8) >> 3));
      uint8_t b = (uint8_t)(((bn & 7) << 1) | ((bn & 8) >> 3));
      out[i][0] = (uint8_t)(r * 17);
      out[i][1] = (uint8_t)(g * 17);
      out[i][2] = (uint8_t)(b * 17);
    } else {
      // Plain ST: 3 bits per gun (low 3 bits of each nibble).
      uint8_t r = rn & 7;
      uint8_t g = gn & 7;
      uint8_t b = bn & 7;
      out[i][0] = (uint8_t)(r * 255 / 7);
      out[i][1] = (uint8_t)(g * 255 / 7);
      out[i][2] = (uint8_t)(b * 255 / 7);
    }
  }
}

// Build one 640-wide output row of palette indices for the given output line.
static void build_row(int y, uint8_t *idx) {
  if (shotRez == 0) {
    // Low: 320x200, 4 planes, 2x2 doubling.
    const uint8_t *p = source + (y / 2) * 160;
    uint8_t line[320];
    for (int g = 0; g < 20; g++) {
      const uint8_t *gp = p + g * 8;
      uint16_t w0 = (uint16_t)(gp[0] << 8) | gp[1];
      uint16_t w1 = (uint16_t)(gp[2] << 8) | gp[3];
      uint16_t w2 = (uint16_t)(gp[4] << 8) | gp[5];
      uint16_t w3 = (uint16_t)(gp[6] << 8) | gp[7];
      for (int bit = 15; bit >= 0; bit--) {
        uint8_t v = (uint8_t)(((w0 >> bit) & 1) | (((w1 >> bit) & 1) << 1) |
                              (((w2 >> bit) & 1) << 2) | (((w3 >> bit) & 1) << 3));
        line[g * 16 + (15 - bit)] = v;
      }
    }
    for (int c = 0; c < 320; c++) {
      idx[c * 2] = line[c];
      idx[c * 2 + 1] = line[c];
    }
  } else if (shotRez == 1) {
    // Med: 640x200, 2 planes, vertical 1x2 doubling.
    const uint8_t *p = source + (y / 2) * 160;
    for (int g = 0; g < 40; g++) {
      const uint8_t *gp = p + g * 4;
      uint16_t w0 = (uint16_t)(gp[0] << 8) | gp[1];
      uint16_t w1 = (uint16_t)(gp[2] << 8) | gp[3];
      for (int bit = 15; bit >= 0; bit--) {
        uint8_t v = (uint8_t)(((w0 >> bit) & 1) | (((w1 >> bit) & 1) << 1));
        idx[g * 16 + (15 - bit)] = v;
      }
    }
  } else {
    // High: 640x400, 1 plane, 1:1.
    const uint8_t *p = source + y * 80;
    for (int g = 0; g < 40; g++) {
      const uint8_t *gp = p + g * 2;
      uint16_t w0 = (uint16_t)(gp[0] << 8) | gp[1];
      for (int bit = 15; bit >= 0; bit--) {
        idx[g * 16 + (15 - bit)] = (uint8_t)((w0 >> bit) & 1);
      }
    }
  }
}

// Resolution name used as the filename suffix.
static const char *rez_name(void) {
  switch (shotRez) {
    case 0:
      return "low";
    case 1:
      return "medium";
    default:
      return "high";
  }
}

// Find the next free incremental index by scanning snap_NNNN_*.png once; the
// counter then persists for the session and continues past any existing files.
static void find_next_index(void) {
  if (nextIndexValid) {
    return;
  }
  nextIndexValid = true;
  nextIndex = 1;
  DIR dir;
  if (f_opendir(&dir, destFolder) != FR_OK) {
    return;
  }
  FILINFO fno;
  unsigned maxIdx = 0;
  while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0') {
    if (fno.fattrib & AM_DIR) {
      continue;
    }
    unsigned n = 0;
    if (sscanf(fno.fname, "snap_%u_", &n) == 1 ||
        sscanf(fno.fname, "SNAP_%u_", &n) == 1) {
      if (n > maxIdx) {
        maxIdx = n;
      }
    }
  }
  f_closedir(&dir);
  nextIndex = maxIdx + 1;
}

// Build "snap_NNNN_<suffix>.png" (e.g. snap_0001_low.png, snap_0002_menu.png).
static void build_filename(char *path, size_t size, const char *suffix) {
  find_next_index();
  snprintf(path, size, "%s/snap_%04u_%s.png", destFolder, nextIndex, suffix);
}

bool screenshot_writePending(void) {
  readyFlag = false;
  beginReceived = false;

  int ncolors = (shotRez == 0) ? 16 : (shotRez == 1) ? 4 : 2;
  uint8_t palette[16][3];
  decode_palette(palette, ncolors);

  char path[96];
  build_filename(path, sizeof(path), rez_name());

  FIL file;
  if (f_open(&file, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
    DPRINTF("Screenshot: failed to open %s\n", path);
    return false;
  }

  png_writer_t pw;
  bool ok = png_writer_begin(&pw, &file, OUT_WIDTH, OUT_HEIGHT, palette, ncolors);
  uint8_t row[OUT_WIDTH];
  for (int y = 0; y < OUT_HEIGHT && ok; y++) {
    build_row(y, row);
    ok = png_writer_row(&pw, row, OUT_WIDTH);
  }
  if (ok) {
    ok = png_writer_end(&pw);
  }
  f_close(&file);

  if (ok) {
    DPRINTF("Screenshot written: %s\n", path);
    nextIndex++;
  } else {
    DPRINTF("Screenshot: write error on %s\n", path);
  }
  return ok;
}

bool screenshot_captureLocal(void) {
  // The MD/Snap menu is the RP's own 320x200 mono framebuffer (what the m68k
  // print loop copies to the ST screen), so we can grab it directly without any
  // cartridge round-trip. It is a linear ST mono bitmap (40 bytes/row) and reads
  // back in the correct byte order here, so no per-word swap is needed. Doubled
  // 2x2 to 640x400. Palette matches the display (black text on white): the menu
  // is drawn as set pixels (bit 1) = text, so index 0 = white, index 1 = black.
  const uint8_t *fb = (const uint8_t *)((uintptr_t)&__rom_in_ram_start__ +
                                        CHANDLER_FRAMEBUFFER_OFFSET);
  static const uint8_t palette[2][3] = {{255, 255, 255}, {0, 0, 0}};

  char path[96];
  build_filename(path, sizeof(path), "menu");

  FIL file;
  if (f_open(&file, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
    DPRINTF("Screenshot: failed to open %s\n", path);
    return false;
  }

  png_writer_t pw;
  bool ok = png_writer_begin(&pw, &file, OUT_WIDTH, OUT_HEIGHT, palette, 2);
  uint8_t row[OUT_WIDTH];
  for (int y = 0; y < OUT_HEIGHT && ok; y++) {
    const uint8_t *p = fb + (y / 2) * 40;  // 320x200 -> 2x vertical double
    for (int bx = 0; bx < 40; bx++) {
      uint8_t b = p[bx];
      for (int bit = 7; bit >= 0; bit--) {
        uint8_t px = (uint8_t)((b >> bit) & 1);
        int srcx = bx * 8 + (7 - bit);  // 0..319
        row[srcx * 2] = px;             // 2x horizontal double
        row[srcx * 2 + 1] = px;
      }
    }
    ok = png_writer_row(&pw, row, OUT_WIDTH);
  }
  if (ok) {
    ok = png_writer_end(&pw);
  }
  f_close(&file);

  if (ok) {
    DPRINTF("Screenshot (menu) written: %s\n", path);
    nextIndex++;
  } else {
    DPRINTF("Screenshot (menu): write error on %s\n", path);
  }
  return ok;
}
