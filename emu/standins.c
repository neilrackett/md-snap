/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: standins.c
 * Description: What EmuMD does not stand in for yet, for MD/Snap's EmuMD
 *              build (mdfw.ini) only. Delete each one when EmuMD provides
 *              it: the link then fails with a duplicate symbol.
 */

#include <limits.h>
#include <stdlib.h>

#include "commemul.h"
#include "mdfw.h"
#include "romemul.h"
#include "sdcard.h"

/* gotoBooster() halts the ROM3 and ROM4 engines before jumping to the
 * Booster. EmuMD has no engines to halt, and stops the firmware at the
 * jump itself. */
void commemul_deinit(void) {}
void romemul_deinit(void) {}

/* sdcard_isPresent() asks the card over SPI. EmuMD's microSD card is always
 * there, unless -O nocard=FROM[-TO] (ms of emulated time since Hatari
 * started) takes it out for a while, to try SELECT without one. */
bool sdcard_isPresent(bool *fresh) {
  static bool wasOut;
  *fresh = false;
  const char *spec = mdfw_option("nocard");
  if (spec != NULL) {
    char *end = NULL;
    unsigned long from = strtoul(spec, &end, 10);
    unsigned long to = (*end == '-') ? strtoul(end + 1, NULL, 10) : ULONG_MAX;
    uint64_t ms = mdfw_time_us() / 1000;
    if (ms >= from && ms < to) {
      wasOut = true;
      return false;
    }
  }
  if (wasOut) {
    wasOut = false;
    *fresh = true;  // put back: mounted again
  }
  return true;
}
