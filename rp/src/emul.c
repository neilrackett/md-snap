/**
 * File: emul.c
 * Author: Neil Rackett (from a GOODDATA LABS template)
 * Copyright: 2026 - Neil Rackett
 * License: GPL v3
 * Description: MD/Snap core. A boot menu lists the screenshots already on the
 * SD card; exiting to the desktop installs the resident m68k grabber and the
 * RP then services SELECT-triggered screen captures, writing 640x400 PNGs.
 */

#include "emul.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "aconfig.h"
#include "blink.h"
#include "chandler.h"
#include "commemul.h"
#include "constants.h"
#include "debug.h"
#include "display.h"
#include "ff.h"
#include "memfunc.h"
#include "pico/stdlib.h"
#include "reset.h"
#include "romemul.h"
#include "screenshot.h"
#include "sdcard.h"
#include "select.h"
#include "target_firmware.h"
#include "term.h"

#define SLEEP_LOOP_MS 50
#define DESKTOP_SERVICE_SLEEP_MS 2
#define SCREENSHOT_FOLDER "/screenshots"
#define HOOK_MODE_VBL 0
#define HOOK_MODE_ETV 1
#define HOOK_MODE_MENU 255

// Box-drawing glyph in u8g2_font_amstrad_cpc_extended_8f (horizontal rule).
#define GLYPH_H ((char)129)

// Screenshot list state for the boot menu.
#define MENU_MAX_SHOTS 256
#define MENU_NAME_LEN 28  // fits "Snap_YYYYMMDD_HHMMSS.png" + NUL
#define MENU_PAGE_ROWS 16
#define MENU_LIST_TOP_ROW 4
#define SNAP_AUTOEXIT_ARM_DELAY_MS 6000
#define SNAP_AUTOEXIT_SECONDS 10

static char shotNames[MENU_MAX_SHOTS][MENU_NAME_LEN];
static int shotCount = 0;
static int currentPage = 0;
static bool autoExitActive = false;
static bool autoExitStarted = false;
static bool autoExitCancelled = false;
static absolute_time_t autoExitArmDeadline;
static absolute_time_t autoExitDeadline;
static int autoExitRenderedSeconds = -1;

// Menu navigation requests, set from the keystroke callback (term_loop ctx),
// acted on in the main loop.
typedef enum { MENU_NONE, MENU_EXIT_DESKTOP, MENU_BOOSTER } MenuAction;
static volatile MenuAction menuAction = MENU_NONE;
static volatile int pageDelta = 0;
static volatile bool menuHelpVisible = false;
static bool helpToggleTimeValid = false;
static absolute_time_t helpLastToggle;

// Capture-hook choice. The [H] key toggles it and persists the app MODE setting;
// exit-to-desktop publishes it to the m68k grabber. false = VBL ($70, fast
// legacy transport); true = etv_timer ($400, async stream transport).
static volatile bool g_hookEtv = false;
static volatile bool hookToggleReq = false;

static FATFS fsys;
static bool sdReady = false;

static void loadHookMode(void) {
  SettingsConfigEntry *entry =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_MODE);
  int mode = entry ? atoi(entry->value) : HOOK_MODE_MENU;
  g_hookEtv = (mode == HOOK_MODE_ETV);
  DPRINTF("MD/Snap: hook mode loaded: MODE=%d hook=%s\n", mode,
          g_hookEtv ? "ETV" : "VBL");
}

static void saveHookMode(void) {
  int mode = g_hookEtv ? HOOK_MODE_ETV : HOOK_MODE_VBL;
  int err = settings_put_integer(aconfig_getContext(), ACONFIG_PARAM_MODE, mode);
  if (err == 0) {
    err = settings_save(aconfig_getContext(), true);
  }
  if (err == 0) {
    DPRINTF("MD/Snap: hook mode saved: MODE=%d hook=%s\n", mode,
            g_hookEtv ? "ETV" : "VBL");
  } else {
    DPRINTF("MD/Snap: failed to save hook mode: MODE=%d err=%d\n", mode, err);
  }
}

// --- screenshot list ----------------------------------------------------
// Match only the files we write: "snap_*.png" (case-insensitive). The "snap_"
// prefix also excludes macOS junk like ._AppleDouble files (start with "._")
// and .DS_Store, which would otherwise clutter the list.
static bool isScreenshotName(const char *name) {
  size_t l = strlen(name);
  if (l < 9) return false;  // "snap_" + at least "X.png"
  if (!((name[0] == 's' || name[0] == 'S') && (name[1] == 'n' || name[1] == 'N') &&
        (name[2] == 'a' || name[2] == 'A') && (name[3] == 'p' || name[3] == 'P') &&
        name[4] == '_')) {
    return false;
  }
  const char *e = name + l - 4;
  return e[0] == '.' && (e[1] == 'P' || e[1] == 'p') &&
         (e[2] == 'N' || e[2] == 'n') && (e[3] == 'G' || e[3] == 'g');
}

static void scanShots(void) {
  shotCount = 0;
  if (!sdReady) return;
  DIR dir;
  if (f_opendir(&dir, SCREENSHOT_FOLDER) != FR_OK) return;
  FILINFO fno;
  while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0' &&
         shotCount < MENU_MAX_SHOTS) {
    if (fno.fattrib & AM_DIR) continue;
    if (!isScreenshotName(fno.fname)) continue;
    strncpy(shotNames[shotCount], fno.fname, MENU_NAME_LEN - 1);
    shotNames[shotCount][MENU_NAME_LEN - 1] = '\0';
    shotCount++;
  }
  f_closedir(&dir);
}

static int totalPages(void) {
  int p = (shotCount + MENU_PAGE_ROWS - 1) / MENU_PAGE_ROWS;
  return (p < 1) ? 1 : p;
}

// --- menu rendering ------------------------------------------------------
static int autoExitSecondsRemaining(void) {
  int64_t remUs = absolute_time_diff_us(get_absolute_time(), autoExitDeadline);
  int secs = (int)((remUs + 999999) / 1000000);  // ceil to whole seconds
  return (secs < 1) ? 1 : secs;
}

static void startAutoExitCountdown(void) {
  if (autoExitStarted || autoExitCancelled) return;
  autoExitStarted = true;
  autoExitActive = true;
  autoExitRenderedSeconds = -1;
  autoExitDeadline = make_timeout_time_ms(SNAP_AUTOEXIT_SECONDS * 1000);
}

static bool cancelAutoExitCountdown(void) {
  autoExitCancelled = true;
  if (!autoExitActive) return false;
  autoExitActive = false;
  autoExitRenderedSeconds = -1;
  DPRINTF("Auto-exit countdown cancelled\n");
  return true;
}

static void menuAt(int row, int col, const char *s) {
  char seq[5] = {0x1B, 'Y', (char)(TERM_POS_Y + row), (char)(TERM_POS_X + col),
                 0};
  term_printString(seq);
  term_printString(s);
}

static const char *const menuHelpLines[] = {
    "HELP",
    "",
    "Capture screenshots to your SidecarT's",
    "SD card by pressing the SELECT button.",
    "",
    "",
    "Choose between 2 screen capture modes:",
    "",
    "VBL  Instantly capture GEM and TOS",
    "     apps, some games.",
    "",
    "ETV  Slower, but works with most apps",
    "     and games (experimental).",
    "",
    "",
    "Find out more:",
    "",
    "Web  github.com/neilrackett/md-snap",
    "X    x.com/neilrackett",
};

static void renderMenu(void) {
  term_clearScreen();

  // Header: rule with the title inset at column 2 (md-sidepad style, no spaces).
  char line[TERM_SCREEN_SIZE_X + 1];
  memset(line, GLYPH_H, TERM_SCREEN_SIZE_X);
  const char *title = "MD/Snap";
  memcpy(line + 2, title, strlen(title));
  line[TERM_SCREEN_SIZE_X] = '\0';
  menuAt(0, 0, line);

  if (menuHelpVisible) {
    for (size_t i = 0; i < sizeof(menuHelpLines) / sizeof(menuHelpLines[0]);
         i++) {
      menuAt(2 + (int)i, 0, menuHelpLines[i]);
    }
  } else {
    if (autoExitActive) {
      int secs = autoExitSecondsRemaining();
      autoExitRenderedSeconds = secs;
      char countdown[TERM_SCREEN_SIZE_X + 1];
      snprintf(countdown, sizeof(countdown), "Exit in %ds (any key to cancel)",
               secs);
      menuAt(2, 0, countdown);
    } else if (shotCount == 0) {
      menuAt(2, 0, "You haven't taken any screenshots yet!");
    } else {
      menuAt(2, 0, "Your screenshots");
    }

    if (shotCount > 0) {
      int start = currentPage * MENU_PAGE_ROWS;
      for (int i = 0; i < MENU_PAGE_ROWS; i++) {
        int n = start + i;
        if (n >= shotCount) break;
        char row[TERM_SCREEN_SIZE_X + 1];
        snprintf(row, sizeof(row), " %s", shotNames[n]);
        menuAt(MENU_LIST_TOP_ROW + i, 0, row);
      }
    }
  }

  // Footer rule (row 22): page indicator inset at the header-title column,
  // build version inset near the right.
  memset(line, GLYPH_H, TERM_SCREEN_SIZE_X);
  char pg[TERM_SCREEN_SIZE_X + 1];
  int pgLen = snprintf(pg, sizeof(pg), "Page %d/%d", currentPage + 1,
                       totalPages());
  if (pgLen > 0) {
    memcpy(line + 2, pg, (size_t)pgLen);
  }
  const char *ver = RELEASE_VERSION;  // already includes a leading 'v'
  size_t verLen = strlen(ver);
  if (verLen + 3 < (size_t)TERM_SCREEN_SIZE_X) {
    size_t col = TERM_SCREEN_SIZE_X - 2 - verLen;
    memcpy(line + col, ver, verLen);
  }
  line[TERM_SCREEN_SIZE_X] = '\0';
  menuAt(22, 0, line);

  // Controls with reverse-video key caps (Atari VT52 ESC p / ESC q), including
  // the [H] hook toggle showing the current vector (feat/etv experiment).
  char ctrl[64];
  snprintf(ctrl, sizeof(ctrl),
           "\x1B" "p" "Help" "\x1B" "q" " "
           "\x1B" "p" "Esc" "\x1B" "q" " "
           "\x1B" "p" "B" "\x1B" "q" "ooster "
           "\x1B" "p" "H" "\x1B" "q" "ook:%s",
           g_hookEtv ? "ETV" : "VBL");
  menuAt(23, 0, ctrl);

  // Park the always-drawn block cursor in the empty bottom-right cell so it
  // doesn't sit on top of the footer text (same as md-sidepad).
  menuAt(TERM_SCREEN_SIZE_Y - 1, TERM_SCREEN_SIZE_X - 1, "");

  display_refresh();
}

// --- input ---------------------------------------------------------------
// ST keyboard scan codes for keys that do not produce useful ASCII.
#define SCAN_HELP 0x62
#define SCAN_LEFT 0x4B
#define SCAN_RIGHT 0x4D

static bool menuKeyCb(char ascii, uint8_t scanCode, uint8_t shift) {
  (void)shift;
  DPRINTF("MD/Snap: key ascii=%d scan=%d\n", (int)ascii, (int)scanCode);
  if (cancelAutoExitCountdown()) {
    renderMenu();
  }

  if (scanCode == SCAN_HELP) {
    absolute_time_t now = get_absolute_time();
    if (!helpToggleTimeValid ||
        absolute_time_diff_us(helpLastToggle, now) > 350000) {
      helpLastToggle = now;
      helpToggleTimeValid = true;
      menuHelpVisible = !menuHelpVisible;
      DPRINTF("MD/Snap: help %s\n", menuHelpVisible ? "on" : "off");
      renderMenu();
    }
    return true;
  }

  if (menuHelpVisible) {
    menuHelpVisible = false;
    renderMenu();
  }

  if (ascii == 0x1B) {  // ESC
    menuAction = MENU_EXIT_DESKTOP;
    return true;
  }
  if (ascii == 'b' || ascii == 'B') {
    menuAction = MENU_BOOSTER;
    return true;
  }
  if (ascii == 'h' || ascii == 'H') {
    hookToggleReq = true;
    return true;
  }
  if (scanCode == SCAN_LEFT) {
    pageDelta = -1;
    return true;
  }
  if (scanCode == SCAN_RIGHT) {
    pageDelta = +1;
    return true;
  }
  // Consume every other key too: the menu is the only UI, so ignore stray
  // typing rather than letting it reach the (unused) line editor.
  return true;
}

// --- desktop / screenshot service ---------------------------------------
static void flashLed(void) {
#ifdef BLINK_H
  for (int i = 0; i < 6; i++) {
    blink_on();
    sleep_ms(60);
    blink_off();
    sleep_ms(60);
  }
#endif
}

// Hand control back to the Booster app. Two things have to happen, in order:
//   1. Reset the m68k. Without this the ST keeps running our terminal print
//      loop and just draws garbage over whatever Booster paints — the screen
//      corruption seen on a "naked" jump. CMD_RESET makes the cartridge clear
//      memvalid and jump the reset vector, so the ST cold-boots and re-scans the
//      cartridge (picking up Booster's ROM, since the RP is Booster by then).
//      It must be sent while the ROM read engine is still up so the ST can read
//      the command; then we wait for it to act.
//   2. Stop our cartridge-bus emulation (ROM4 read engine + ROM3 ring) so
//      Booster's re-init claims clean PIO/DMA, and jump (in-place) to Booster.
// Does not return.
static void gotoBooster(void) {
  DPRINTF("gotoBooster: resetting ST, then jumping to Booster\n");
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_RESET);
  sleep_ms(250);  // let the ST poll the sentinel and enter its reset
  commemul_deinit();
  romemul_deinit();
  reset_jump_to_booster();
}

// Install the resident grabber on the ST, boot to the desktop, then service
// SELECT-triggered captures forever. Never returns.
static void exitToDesktop(void) {
  uint32_t romBase = (uint32_t)&__rom_in_ram_start__;
  DPRINTF("MD/Snap: exitToDesktop - installing grabber (hook=%s) and booting\n",
          g_hookEtv ? "ETV" : "VBL");
  term_setRawKeyCallback(NULL);

  // Publish the capture-hook choice to shared-var slot 4 BEFORE CMD_START, so
  // the m68k installer reads it (0 = VBL $70, 1 = etv_timer $400).
  SET_SHARED_VAR(4, g_hookEtv ? 1 : 0, romBase, CHANDLER_SHARED_VARIABLES_OFFSET);
  SET_SHARED_VAR(5, 0, romBase, CHANDLER_SHARED_VARIABLES_OFFSET);

  // Hold CMD_START so the m68k's rom_function installs the resident grabber,
  // then hold CMD_BOOT_GEM so boot_gem reliably latches and continues to GEM.
  for (int i = 0; i < 10; i++) {
    SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_START);
    chandler_loop();
    sleep_ms(SLEEP_LOOP_MS);
  }
  for (int i = 0; i < 10; i++) {
    SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_CONTINUE);
    chandler_loop();
    sleep_ms(SLEEP_LOOP_MS);
  }

  uint8_t captureSeq = 0;
  bool wasPressed = false;
  bool longFired = false;
  absolute_time_t pressStart = get_absolute_time();

  while (true) {
    // Drain incoming screen chunks and ack the m68k handshake.
    chandler_loop();

    if (screenshot_pending()) {
      bool ok = screenshot_writePending();
      if (ok) {
        flashLed();
      }
    }

    // SELECT: short press -> capture (fires on release), long press -> Booster.
    bool pressed = select_detectPush();
    if (pressed && !wasPressed) {
      pressStart = get_absolute_time();
      longFired = false;
    } else if (pressed && wasPressed) {
      if (!longFired &&
          absolute_time_diff_us(pressStart, get_absolute_time()) >=
              (int64_t)SELECT_LONG_RESET * 1000) {
        longFired = true;
        DPRINTF("SELECT long-press -> Booster\n");
        gotoBooster();  // does not return
      }
    } else if (!pressed && wasPressed) {
      if (!longFired) {
        captureSeq++;
        SET_SHARED_VAR(5, 0, romBase, CHANDLER_SHARED_VARIABLES_OFFSET);
        SET_SHARED_VAR(3, captureSeq, romBase, CHANDLER_SHARED_VARIABLES_OFFSET);
        DPRINTF("SELECT -> capture request %u\n", captureSeq);
      }
    }
    wasPressed = pressed;

    sleep_ms(DESKTOP_SERVICE_SLEEP_MS);
  }
}

void emul_start() {
  // Copy the m68k cartridge firmware into the ROM-in-RAM mirror, then bring up
  // the ROM4 read engine and the ROM3 command channel.
  COPY_FIRMWARE_TO_RAM((uint16_t *)target_firmware, target_firmware_length);

  // Initialise the command sentinel to NOP before the cartridge bus comes up.
  // firmware.py zero-trims the image and COPY_FIRMWARE_TO_RAM does not clear the
  // rest of the mirror, so $FA2000 would otherwise hold stale RAM that the m68k
  // could read as a spurious RESET/BOOT_GEM/START before the first display NOP.
  *((volatile uint32_t *)((uintptr_t)&__rom_in_ram_start__ +
                          CHANDLER_CMD_SENTINEL_OFFSET)) = 0;

  if (init_romemul(false) < 0) {
    panic("init_romemul failed: PIO/DMA claim or program load returned <0");
  }
  if (commemul_init() < 0) {
    panic("commemul_init failed: PIO/DMA claim or program load returned <0");
  }
  chandler_init();
  chandler_addCB(term_command_cb);
  chandler_addCB(screenshot_command_cb);

  // Display + SD card.
  display_setupU8g2();

  int sdcardErr = sdcard_initFilesystem(&fsys, SCREENSHOT_FOLDER);
  if (sdcardErr != SDCARD_INIT_OK) {
    DPRINTF("SD card unavailable (error %i). Continuing without SD.\n",
            sdcardErr);
  } else {
    sdReady = true;
    DPRINTF("SD card found & initialized\n");
  }

  display_setupU8g2();  // re-init after SD touched the display path
  screenshot_init(SCREENSHOT_FOLDER);
  loadHookMode();

  // SELECT button (also used for the boot escape hatch in main.c).
  select_configure();

  // Terminal + menu.
  term_init();
  scanShots();
  currentPage = 0;
  menuHelpVisible = false;
  helpToggleTimeValid = false;
  autoExitActive = false;
  autoExitStarted = false;
  autoExitCancelled = false;
  autoExitRenderedSeconds = -1;
  renderMenu();
  autoExitArmDeadline = make_timeout_time_ms(SNAP_AUTOEXIT_ARM_DELAY_MS);
  term_setRawKeyCallback(menuKeyCb);
  DPRINTF("MD/Snap: menu rendered (%d screenshots)\n", shotCount);

#ifdef BLINK_H
  blink_on();
#endif
  DPRINTF("MD/Snap: entering menu loop\n");

  // Menu loop: render screenshots, navigate pages, pick an exit. SELECT is live
  // here too, so the menu can screenshot itself (captured from the RP's own
  // framebuffer). Seed the press state from the current level so a SELECT still
  // held from the boot escape-hatch doesn't fire a capture the instant we enter.
  bool keepActive = true;
  bool selPressed = select_detectPush();
  bool selLong = false;
  absolute_time_t selStart = get_absolute_time();

  while (keepActive) {
    chandler_loop();
    term_loop();

    if (!autoExitStarted && !autoExitCancelled &&
        absolute_time_diff_us(get_absolute_time(), autoExitArmDeadline) <= 0) {
      startAutoExitCountdown();
      renderMenu();
    }

    if (autoExitActive) {
      if (absolute_time_diff_us(get_absolute_time(), autoExitDeadline) <= 0) {
        DPRINTF("Auto-exit countdown elapsed; exiting to desktop\n");
        autoExitActive = false;
        menuAction = MENU_EXIT_DESKTOP;
      } else {
        int secs = autoExitSecondsRemaining();
        if (secs != autoExitRenderedSeconds) {
          renderMenu();
        }
      }
    }

    if (pageDelta != 0) {
      int p = currentPage + pageDelta;
      pageDelta = 0;
      int pages = totalPages();
      if (p < 0) p = 0;
      if (p >= pages) p = pages - 1;
      if (p != currentPage) {
        currentPage = p;
        renderMenu();
      }
    }

    if (hookToggleReq) {
      hookToggleReq = false;
      g_hookEtv = !g_hookEtv;
      saveHookMode();
      renderMenu();  // repaint the footer with the new Hook:VBL/ETV
    }

    // SELECT: short press captures the menu itself; long press -> Booster
    // (same gesture split as the desktop service loop).
    bool pressed = select_detectPush();
    if (pressed && !selPressed) {
      if (cancelAutoExitCountdown()) {
        renderMenu();
      }
      selStart = get_absolute_time();
      selLong = false;
    } else if (pressed && selPressed) {
      if (!selLong && absolute_time_diff_us(selStart, get_absolute_time()) >=
                          (int64_t)SELECT_LONG_RESET * 1000) {
        selLong = true;
        DPRINTF("Menu SELECT long-press -> Booster\n");
        gotoBooster();  // does not return
      }
    } else if (!pressed && selPressed) {
      if (!selLong) {
        DPRINTF("Menu SELECT -> capture menu\n");
        if (screenshot_captureLocal()) {
          flashLed();
          scanShots();   // pick up the just-written file...
          renderMenu();  // ...and show it in the list
        }
      }
    }
    selPressed = pressed;

    if (menuAction != MENU_NONE) {
      keepActive = false;
    }

    sleep_ms(SLEEP_LOOP_MS);
  }

#ifdef BLINK_H
  blink_off();
#endif

  if (menuAction == MENU_BOOSTER) {
    DPRINTF("Menu -> Booster\n");
    gotoBooster();  // does not return
  }

  // Default / MENU_EXIT_DESKTOP: install the grabber and service captures.
  exitToDesktop();  // does not return
}
