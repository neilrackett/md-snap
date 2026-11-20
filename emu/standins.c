/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: standins.c
 * Description: What EmuMD does not stand in for yet, for MD/Snap's EmuMD
 *              build (mdfw.ini) only. Delete each one when EmuMD provides
 *              it: the link then fails with a duplicate symbol.
 */

#include "commemul.h"
#include "romemul.h"

/* gotoBooster() halts the ROM3 and ROM4 engines before jumping to the
 * Booster. EmuMD has no engines to halt, and stops the firmware at the
 * jump itself. */
void commemul_deinit(void) {}
void romemul_deinit(void) {}
