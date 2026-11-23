# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

See also: `programming.md` (full shared-region table and budget rules), `README.md` (high-level region/userfw overview), `AGENTS.md` (overlapping playbook + troubleshooting table).

## What this repo is

Template for a **Sidecartridge Multi-device microfirmware app** targeting Atari ST / STE / MegaST(E). Each "app" is a UF2 image that runs on a Raspberry Pi Pico (RP2040) plugged into the Multi-device cartridge slot, emulating a ROM cartridge for the Atari while also handling networking, SD card I/O, and config. Public build/usage docs are at <https://docs.sidecartridge.com/sidecartridge-multidevice/programming/>.

This repo has been turned into **MD/Snap** — see below.

## MD/Snap (this app) — implementation notes

MD/Snap captures the Atari ST screen to a `640×400` indexed PNG on the SD card when **SELECT** is
pressed. Flow: boot menu lists existing `/screenshots/*.png` → **ESC** installs a resident m68k
grabber and boots to the GEM desktop → **SELECT** captures (snapshot → push over the cartridge window
→ RP deplanes/pixel-doubles/writes PNG → LED flash). **B** / hold-SELECT-at-boot → Booster. **[H]**
toggles the capture hook (VBL vs ETV — see below), persisted in the app `MODE` setting.

Key files (beyond the template): `target/atarist/src/userfw.s` (installer + resident VBL grabber +
async ETV stream grabber), `target/atarist/src/ttmenu.s` (the menu on a TT: TT low with a 256-colour
preview, or TT high drawn 4x), `rp/src/screenshot.c` (chunk/stream assembly + deplane + palette decode),
`rp/src/png_writer.c` (streaming indexed PNG), `rp/src/preview_overlay.c` (menu preview thumbnail),
`rp/src/emul.c` (menu + selection/preview + auto-exit + exit-to-desktop resident service loop), plus
reverse-video added to `term.c`/`display_term.c` and the SELECT escape hatch in `main.c`.

### Hard-won gotchas (these cost real debugging time — don't relearn them)

- **The command sentinel `$FA2000` is NOT auto-initialised.** `firmware.py` **zero-trims** the cartridge
  image (it stops at the last non-zero byte, ~2.5 KB), and `COPY_FIRMWARE_TO_RAM` copies only that and
  does **not** clear the rest of the ROM-in-RAM mirror. So `$FA2000` holds stale RP RAM until the first
  display command. `emul.c` explicitly writes it to `CMD_NOP` right after `COPY_FIRMWARE_TO_RAM`; without
  that the m68k can read garbage as a spurious RESET/BOOT_GEM/START and boot straight to the desktop
  "as if the ROM wasn't there." (This invalidates the template's "padded to 64 KB" claim in the Build
  section — it's trimmed, not padded.)
- **Resident grabber must live in RAM.** The cartridge region `$FA0000-$FAFFFF` bus-errors on *stores*
  (reads/exec are fine). The installer `Malloc`s a block, copies the position-independent resident
  handler in, and hooks the **`$70` VBL autovector** (chained). The 32000-byte screen snapshot buffer
  is the extra space `Malloc`'d past the resident block (NOT assembled into the image — that would blow
  the 8 KB cartridge budget), reached via a stored pointer.
- **Resident code calls the ROM transport via absolute `jsr`,** not `bsr` — a PC-relative `bsr` from
  the RAM copy can't reach `send_sync_write_command_to_sidecart` in ROM. The symbol is `xdef`'d in
  `main.s` and `xref`'d in `userfw.s`.
- **Protocol payload cap is ~2048 B.** VBL mode pushes the 32000-byte screen as 16×2000-byte chunks
  plus a BEGIN metadata command (one chunk per VBL frame). ETV mode streams it as 64×500-byte frames
  plus a STREAM_BEGIN (see the capture-hooks section).
- **The menu on a TT (`ttmenu.s`).** `TTMENU_REZ` switches a colour TT to TT low (7) unless it is in
  ST high or TT high, and reports 7 as the menu rez (shared-var slot 6). `TTMENU_DRAW` reads `$FF8262`:
  TT low draws the 320x200 menu into 8 planes with each line doubled, centred (40-line borders), plus a
  144x180 8-plane preview and a 256-word TT palette (`preview_overlay.c`, `forTtLow`; entries 0/255 are
  kept white/black for the menu text); TT high draws it 4x (1280x800, 80-line borders), no preview.
  A screenshot of fewer than 256 colours is box-filtered into the TT preview (dithers show as the colour
  they make): one pass over the PNG counts the 12-bit averages in `tally[4096]` (8 KB) and keeps each
  thumbnail pixel's colour packed 12-bit *in the preview block itself* (38880 B, where the planes then go:
  row r's planes end before row r+1's packed colours), then the 254 most used become the palette, the rest
  map to the nearest, and the kept colours are drawn. 256-colour ones stay pixel-for-pixel.
- **Previews are cached on the SD card** (`/screenshots/.preview/<name>.tt` or `.st`, hidden): a 20-byte
  header (magic, `PREVIEW_CACHE_VERSION`, variant, the PNG's size and FAT date/time from `scanShots`, a
  checksum of the block) and then the preview block from the palette on, exactly as in memory, read
  straight back in one `f_read`. One is used only if the checksum matches and, for a TT, palette entries
  0/255 are still white/black: version 1 files written by the build whose heap overran into the region
  (below) blacked out the whole TT menu when their screenshot was highlighted.
  Decoding means reading the whole 256-307 KB PNG at 12.5 MHz SPI (~0.3-0.6 s); a cached one is ~26 KB.
  The desktop loop makes the preview straight after each capture (`preview_overlay_prepare`, TT variant
  from a TT, none for ST high/TT high, i.e. a mono monitor) and `preview_overlay_forget`s any kept for a
  PNG of the same name first. **Bump `PREVIEW_CACHE_VERSION` whenever what a preview looks like changes.**
  `Preview <name>: kept|made, N us` in the debug log times each one.
  The menu code relocates to 4 KB *below* the screen, so the 153600-byte TT screen doesn't reach it.
  Offsets past +32767 need `adda.l`, not a `lea` displacement (68000).
  `TTMENU_REZ` also switches the **TT's internal speaker off** for the menu (YM port A bit 6 set, via
  XBIOS Giaccess/Ongibit), always, with no setting, as md-sidepad and the Booster do. Only on a TT: other
  machines use that bit for something else. It records the mode it switched from and whether it switched
  the speaker off in `tt_state` (`main.s`'s RAM copy, passed in a1, because ttmenu.s runs from ROM), and
  `TTMENU_EXIT` puts both back in `boot_gem` and before `.reset`. Without that, a TT with no
  DESKTOP.INF/NEWDESK.INF came up on the desktop in TT low; with one, TOS sets the mode from it anyway.
  It also keeps all 256 TT palette words (in the 512 bytes below the screen, above main.s's relocated
  code) and puts them back after the mode: otherwise the desktop's text came up in a preview's colours
  (grey). **XBIOS trashes d0-d2/a0-a2**, so anything that keeps a1 across a trap must reload it: a
  `speaker_off` that didn't sent the "palette kept" flag to a stray low address.
- **TT support (`_MCH` $0002xxxx):** the grabber sends the whole `$FF8262` shifter word as "rez" (mode
  bits 8-10, ST palette bank bits 0-3) plus all 256 TT palette words (`$FF8400`, plain `0RGB` 4-bit —
  NOT the STE bit order), and reads the `$FF820D` base low byte. ST modes on a TT stay 32000 bytes and
  use the buffered path (colours from the bank; ST high = entries 254/255, swapped by bit 1 of entry 0).
  TT low/med/high are 153600 bytes — too big for the RP's ~44 KB of free RAM — so they go as 80×1920 B
  (VBL) / 160×960 B (ETV) whole-row chunks and the RP **writes PNG rows from inside the command
  callback**, before the reply token/credit goes out; that is the backpressure. `chandler` has a single
  pending slot, so chunks must arrive in order: repeats are skipped, a gap drops the capture. TT high is
  written as a 1-bit PNG. The installer `Mxalloc`s the block and 154 KB buffer from **ST-RAM
  only**: TOS 3 tests TT-RAM *after* the cartridge installs (the memory test shows below MD/Snap's
  banner), writing patterns over a handler living there, and the next VBL ran them (11 bombs, line-F).
  Hatari reproduces it only with `--ttram N --addr24 off --fast-boot off` (fast boot skips the test).
- **The RP heap must stop at 0x20030000.** `__StackLimit` in `rp/src/memmap_rp.ld` is the heap's ceiling
  (`_sbrk`); it used to be the end of `ROM_IN_RAM`, so a heap that grew past RAM wrote into the cartridge
  window: the ST's code, the command sentinel at `$FA2000`, the menu. It showed as **4 bombs** when the
  preview was turned on, after 7.5 KB more `.bss` left the heap less room (settings and FatFs's LFN buffers
  malloc, in 4 KB `sbrk` steps). It is now `ORIGIN(RAM) + LENGTH(RAM)`: a malloc that does not fit fails.
  Keep big buffers out of `.bss` where the shared region has room: the preview's 4 KB PNG read buffer sits
  in it, after the blend's packed colours and before the framebuffer (`IDAT_BUF_OFFSET`).
- **The menu lists newest first:** `sortShots` orders by the number in `snap_NNNN_`, highest first (as a
  number, so snap_10000 comes before snap_9999); a screenshot of the menu is selected once taken.
- **Unsupported screens (a Falcon's own modes):** on a Falcon (`_MCH` $0003xxxx) `falcon_rez` in
  `userfw.s` works out the mode from the Videl as Hatari's `videl.c` does: depth from `$FF8266` bits 10/8/4
  (1/16/8 bits) else the ST shifter, width from `$FF8210`, height from `VDE - VDB` (`$FF82AA/$FF82A8`,
  halved unless interlaced, halved again when doubled, `$FF82C2`), `$FF820E` 0, mono monitor (`$FF8006`
  bits 6-7 = 0) 640x400 only. Only 320x200x4, 640x200x2 and 640x400x1 (within 8 lines) are ST low/med/
  high; anything else sends just the BEGIN with rez `$FFFF` (`REZ_UNSUPPORTED` / `SHOT_REZ_UNSUPPORTED`)
  and no chunks or frames (ETV stops at the begin-ack). The RP saves nothing and `screenshot_writePending`
  returns `SCREENSHOT_UNSUPPORTED`: three long LED flashes. A Falcon-native 320x200 16-colour mode (RGB)
  passes as ST low, but its colours may come from the Falcon palette, which isn't read. Checked in Hatari
  with EmuTOS and an AUTO program calling `VsetMode` for each mode.
- **SD card taken out:** there is no card-detect pin, so `sdcard_isPresent()` (`sdcard.c`) asks the
  card (`sd_test_com`: CMD13, or a light init once it is marked uninitialised; MISO is pulled up, so a
  missing card answers $FF at once). `sdCardReady()` in `emul.c` runs it as SELECT is released: no card →
  two long LED flashes and the ST is **not** asked for the screen; a card back (or another) is mounted
  again with `sdcard_initFilesystem` and `screenshot_cardChanged()` rescans for the next free number.
  A write that fails anyway also gets the two long flashes. EmuMD replaces `sdcard.c`, so `emu/standins.c`
  has a stand-in: `-O nocard=FROM[-TO]` (emulated ms) takes the card out for a while.
- **No RTC / no wall clock:** the RP has no battery clock and we don't fetch NTP. Filenames are
  sequential `snap_NNNN_<low|med|high|tt_low|tt_med|tt_high|menu>.png` (RP scans the folder for the next free index). Do not
  reintroduce timestamped names without a real time source.
- **Palette decode is ST-vs-STE:** the grabber sends the `_MCH` cookie value; the RP picks 3-bit (ST)
  or 4-bit-with-reordered-LSB (STE) decoding.
- **ESC is a normal keystroke now.** `check_keys` in `main.s` was changed to forward *every* key
  (including ESC) as `APP_TERMINAL_KEYSTROKE`; the legacy `APP_TERMINAL_START` command-line terminal is
  gone. The RP menu handles ESC/B/arrows in `menuKeyCb` (a `term_setRawKeyCallback` hook) and consumes
  all keys so stray typing can't reach the old line editor.
- **The RP never resets after exit-to-desktop** — it stays resident forever (serving ROM4 via DMA,
  polling SELECT, writing PNGs). Short SELECT = capture; long press (`SELECT_LONG_RESET`) = Booster.

### Capture hooks — VBL (sync) and ETV (async stream)

The cartridge (ROM) port has **no IRQ line and no bus-master line** — it's a passive slave. So the RP
can never force the 68000 to run our code or read ST RAM itself; the grabber only runs when a *system
interrupt vector we hooked* is still pointed at us **and** serviced. There are **two hooks**, chosen by
`[H]` (RP publishes the choice to `HOOK_FLAG_ADDR`, shared-var slot 4; persisted in app `MODE`). The
installer in `userfw.s` hooks one or the other; the handler bodies are separate.

- **VBL (`$70`) — synchronous, fast (instant).** The resident handler runs the *blocking* transport
  (`send_sync_write_command_to_sidecart`, RANDOM_TOKEN handshake) and dribbles 16×2000-byte chunks one
  per VBL frame. Captures the desktop, GEM apps, res changes, and "system-friendly" programs that keep
  TOS's VBL.
- **ETV (`etv_timer` `$400`) — async stream, slower (~1–2 s).** Reaches programs that replace the VBL
  but keep TOS's 200 Hz timer-C (so `etv_timer`, which TOS calls from the timer-C ISR, still fires).
  **`etv_timer` runs at 50 Hz, not 200:** TOS calls it on every fourth timer C interrupt, every 20 ms
  (`_timr_ms`, `$442`; confirmed on hardware). Tick counts in `userfw.s` are 20 ms each.
  This is the hook that extends reach to many games.

**Why ETV took two failed tries first, and what made it work.** Earlier attempts hooked Timer C
(`$114`) and `etv_timer` (`$400`) but ran the *blocking* multi-chunk handshake transport inside that
timer ISR — both **froze the ST** (`Screenshot BEGIN` repeating forever, frozen foreground: the
blocking wait wedges the timer ISR before the state machine can advance). The fix is to keep the timer
handler **non-blocking**: `resident_etv_start` is a state machine that snapshots once, then per tick
emits **one bounded frame** (500 bytes; 960 for a TT mode) via `etv_async_write` (same ROM3 protocol as the sync transport
but **skips the RANDOM_TOKEN wait**) and returns immediately. Flow control is external: the RP publishes
credit to `STREAM_ACK_ADDR` (slot 5 = `seq<<16 | next_frame`) and the handler sends the next of the
64 frames (160 for a TT mode) in the same tick that sees credit advance, so at most a frame a tick
(50 a second: ~1.3 s for an ST screen, ~3.2 s for a TT one); `ETV_RETRY_TICKS` (5, 100 ms) resends,
`ETV_ABORT_TICKS` (200, 4 s) gives up. **Lesson: never *block* in the timer ISR — an async, credit-flow-controlled stream is what makes a
timer-C-driven hook safe** (this is the lightweight-handler discipline md-devops uses).

- **68030 data cache (TT, Falcon).** When `_CPU` is 30 the installer sets `res_030`; each resident tick
  empties the data cache (`dcache_flush`, CACR CD) so the capture counter and stream credit are read
  from the RP, and `cache_guard_begin/_end` turn the data cache off (CACR ED) around the protocol
  reads and the sync transport's reply-token wait. `movec` is hand-assembled (`dc.w`) as the file is
  built for the 68000. Defensive: Hatari did not show the stale reads either way.
- **Mega STE cache guard.** ROM3 is read-as-side-effect; on a 16 MHz Mega STE with cache enabled those
  reads can be served from cache and never reach the RP. `cache_guard_begin`/`_end` clear only bit 0 of
  `$FFFF8E21` (preserving the other MSTE_CC bits) around the protocol reads, for `_MCH == Mega STE`
  only, and restore it after. Both `call_transport` (VBL) and `etv_async_write` (ETV) wrap their reads.
- **Still uncapturable:** programs that replace/disable *both* the VBL and `etv_timer`/timer-C, run
  interrupts-masked at IPL 7, or bang hardware directly — that's downstream hardware capture (RGBtoHDMI)
  territory.

### Build tooling changes vs the stock template
- The credit `boot_gem` prints (as MD/Net's: `MD/Snap <version> (c)2026 Neil Rackett` / `GPLv3
  neilrackett.com/atarist`) is `target/atarist/src/version.inc`, written by `target/atarist/build.sh` on the
  host from the root `version.txt` (stcmd's container sees only `target/atarist`, whose own `version.txt`
  is stale). It has no terminator and must not be padded: it runs straight into `bg_msg`'s SELECT line.

- `build.sh` calls `tools/bump_version.sh` (auto-increments the patch version in `version.txt` and
  syncs `rp/`+`target/` on every build) instead of plain `cp version.txt`. Set `SKIP_VERSION_BUMP=1`
  to suppress for a one-off.
- Root `Makefile` adds `make build` / `make debug` (resolve the UUID from `APP_UUID_KEY` env →
  `uuid.txt` → a default) and `make uart` / `uart-log` / `uart-list` (auto-detect a serial device,
  `UART_DEV=…` / `UART_BAUD=…` to override). `uuid.txt` holds this app's dev UUID.
- `desc/app.json` is the MD/Snap descriptor (name/description/URLs point at `neilrackett.com`); the
  release GitHub workflow is intentionally disabled (`branches: [__disabled__]`, `tags: [__disabled__]`).
- `DPRINTF` is gated on `_DEBUG` and compiled out of release builds, so leaving diagnostic prints in is
  free. Several are deliberately kept (`Screenshot BEGIN/written`, `SELECT -> capture request`, menu
  trace) — handy for the next hardware iteration.
- **The build stops at the first failing step**, and `make` fails with it: the m68k build (e.g. a
  `bne.s` out of range), `firmware.py`, copying `target_firmware.h`, cmake, the RP make. It used to carry
  on, so a failed m68k build quietly shipped the *previous* cartridge in the RP firmware.
- **`firmware.py` requires the zero-trimmed image to be an EVEN number of bytes.** Because it zero-trims,
  the *last non-zero byte* in the cartridge image must sit at an odd offset (→ even length). A stray
  non-zero data byte near the end at an even offset (e.g. giving a resident state field a non-zero
  default) makes the trim odd-length → `firmware.py` raises `ValueError` and the build stops. Keep
  trailing resident-block data zero (let the installer set it at runtime).

## Build

Top-level build is driven by `build.sh` in the repo root:

```bash
# <board_type> = pico | pico_w | sidecartos_16mb
# <build_type> = debug | release   (note: always compiled as MinSizeRel — see below)
# <app_uuid_key> = UUID4 identifying this app, must match desc/app.json
./build.sh pico_w release 123e4567-e89b-12d3-a456-426614174000
```

Required host environment:
- ARM GNU Toolchain 14.2 — export `PICO_TOOLCHAIN_PATH` to its `arm-none-eabi/bin` dir.
- `atarist-toolkit-docker` (`stcmd`) — needed for the m68k target. `stcmd` requires a PTY (`pty=true`).
- SDK paths (auto-set from the repo if unset): `PICO_SDK_PATH`, `PICO_EXTRAS_PATH`, `FATFS_SDK_PATH`.

Build flow (orchestrated by `build.sh`):
1. Copies `version.txt` into `rp/` and `target/atarist/`.
2. Builds the Atari ST target (`target/atarist/build.sh`) via `stcmd make`. Enforces an **8 KB hard limit** on `BOOT.BIN` (the cartridge code budget — `CHANDLER_CARTRIDGE_CODE_SIZE` in `rp/src/include/chandler.h`, mirrored as `CARTRIDGE_CODE_SIZE` in `target/atarist/src/main.s`); a build that exceeds it aborts with `ERROR: cartridge code is N bytes; limit is 8192`. A separate copy (`FIRMWARE.IMG`) is then padded to 64 KB to fill the entire shared region, and `firmware.py` converts it into `rp/src/include/target_firmware.h` (a C byte array embedded in the RP firmware).
3. Builds the RP firmware (`rp/build.sh`): pins submodule versions (pico-sdk 2.2.0, pico-extras sdk-2.2.0, fatfs-sdk at a specific commit), runs CMake, produces `rp/dist/rp-<board>.uf2`. The FatFs configuration lives at `rp/src/ff/ffconf.h` and shadows the submodule's default via `target_include_directories(... BEFORE PRIVATE)` in `rp/src/CMakeLists.txt`, so the `fatfs-sdk` submodule stays pristine.
4. Computes MD5, renames to `dist/<APP_UUID>-<VERSION>.uf2`, and substitutes UUID/MD5/version into `dist/<APP_UUID>.json` from the `desc/app.json` template.

### Build gotchas
- **CMake always builds with `-DCMAKE_BUILD_TYPE=MinSizeRel`** regardless of the `<build_type>` argument. A full `Release` previously caused breakage (memory/over-optimization). The legacy line is left commented in `rp/build.sh`. `<build_type>` only controls the `DEBUG_MODE` macro and the dist filename.
- `CHARACTER_GAP_MS` must remain defined (700) in `rp/src/include/blink.h` — removing it breaks the RP build.
- Harmless VASM warnings during the m68k build (`target data type overflow`, `trailing garbage after option -D`) can be ignored.
- VASM/`stcmd` errors like `the input device is not a TTY` mean `stcmd` was invoked without a PTY. `target/atarist/build.sh` already exports `STCMD_NO_TTY=1` for every `stcmd` call it makes; you only need to export it yourself if invoking `stcmd` directly from a non-TTY context (CI, sub-shells, build wrappers). Without it the m68k build can fail silently and the previous `BOOT.BIN` survives — leading to a working RP firmware that displays garbage on the ST because `target_firmware.h` is stale.

### CI / release
- `.github/workflows/build.yml` builds `pico_w` Release on PR.
- `.github/workflows/release.yml` triggers on `v*` tags: builds, attaches UF2 + JSON to the GitHub Release, uploads to `s3://atarist.sidecartridge.com/`.
- `make tag` tags HEAD with the contents of `version.txt` and pushes the tag (which triggers release).
- `upload_s3.sh <file>` is a manual one-off uploader; needs `AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY`.

### Tests
There is no test suite. "Verification" is: build succeeds, UF2 boots on hardware, manual interaction over the serial debug console — and, short of hardware, running it in Hatari with EmuMD (below).

### EmuMD (Hatari on this computer)
`emu/emumd` is a submodule; `mdfw.ini` builds `build/md-snap.mdfw` with `template = sidecartridge`; `make emu` runs it (`emu/emu.mk`). How-to and gotchas are in AGENTS.md §3a. The ones that cost time:
- SELECT is pressed on a schedule: `make emu SELECT=20000` / `-O select=MS[+HOLD],...` (emulated ms since Hatari started).
- Hatari's `--tos-res` turns on Hatari's **own** cartridge, so MD/Snap's never boots (no menu, no grabber). Set a TT's resolution with a floppy `NEWDESK.INF` instead; TT high is `--monitor mono`.
- EmuMD's RP is infinitely fast, so the VBL transport never waits and ETV's credit loop runs at ~3 frames/s; neither says anything about real timing.
- `-O flash=FILE` keeps the app's settings between runs (e.g. the ETV hook), alongside `select=`.

## Architecture

The firmware is a **two-target build**: m68k assembly that runs on the Atari ST is compiled into a ROM image, embedded as a C array inside the RP2040 firmware, and served back to the Atari over the cartridge bus that the RP2040 emulates via PIO + DMA.

### Atari ST side (`target/atarist/`)
- `src/main.s` — m68k cartridge boot + dispatch + terminal. Lives at `$FA0000` in the ST address space (ROM4 cartridge region). Defines the cartridge header (`CA_MAGIC`, `CA_INIT`, …), command magic numbers, and the shared-variable layout used to talk to the RP2040.
- `src/userfw.s` — **the primary extension point for app-specific m68k code.** `src/userfw.ld` places `main.s` at offset `0x0000` (2 KB budget, nearly full), `userfw.s` at offset `0x0800` (4 KB budget) and `ttmenu.s` at `0x1800` (2 KB; `TTMENU_REZ`/`TTMENU_DRAW` in `main.s`); `main.s` exposes the latter as `USERFW equ (ROM4_ADDR + $800)`. When the RP-side terminal command `f` ([F]irmware) is selected, the RP writes `CMD_START = 4` to the cartridge sentinel; the m68k's vsync-polled `check_commands` dispatches to `rom_function`, which `jmp`s to `USERFW`. The default `userfw.s` is a Cconws demo — replace its body with your own logic.
- Adding more m68k modules: add a new `.text_<name>` section in `userfw.ld`, mirror the offset with an `equ (ROM4_ADDR + $????)` in `main.s`, and add the `.o` target to `target/atarist/Makefile` (same pattern as `gemdrive.ld` in `md-drives-emulator`).
- Built via `stcmd make release` (m68k assembler in Docker); the cartridge image (header + all `.text_*` sections) must fit in 8 KB. A 64 KB padded copy is then converted to `target_firmware.h` for inclusion in the RP build.

### Shared 64 KB cartridge region
The Atari ST sees a 64 KB window at `$FA0000`–`$FAFFFF` (mirrored RP-side at `0x20030000`). This is the **single source of truth** for any cross-target data layout — both sides derive every offset symbolically from constants in `rp/src/include/chandler.h` (RP-side) and `target/atarist/src/main.s` (m68k side). **Apps must never hard-code an address inside this region** — always reference the named offset/symbol.

| Offset | Symbol | Size | Purpose |
| --- | --- | --- | --- |
| `$FA0000` | cartridge image | 8 KB | m68k header + all `.text_*` sections (hard limit) |
| `$FA2000` | `CMD_MAGIC_SENTINEL` | 4 B | m68k polls here for NOP/RESET/command words |
| `$FA2004` | `RANDOM_TOKEN`, `RANDOM_TOKEN_SEED`, 60 × 4 B indexed shared variables | ~768 B | fixed-offset metadata block (first 512 B until `$FA2300`) |
| `$FA2300` | `APP_FREE` | ~48 KB | contiguous arena for app buffers |
| `$FAE0C0` | `FRAMEBUFFER` | 8000 B | 320×200 monochrome framebuffer; sits at the top of the region so an overrun walks off the end of the 64 KB window instead of corrupting the metadata block |

See `programming.md` for the full table and budget rules.

### RP2040 side (`rp/src/`)
- `main.c` — only sets clock/voltage, calls `gconfig_init` (global config) then `aconfig_init` (per-app config), and hands off to `emul_start()`. If config init fails it jumps to the **Booster** app via `reset_jump_to_booster()` to bootstrap. **Don't add features to `main.c`** — put them in `emul.c` or a new module.
- `emul.c` / `emul.h` — the application's main loop and entry point. This is where to add new features.
- `romemul.c` / `romemul.pio` — PIO programs and the runtime that emulates the cartridge ROM/RAM bus to the Atari (driven by `READ_*` / `WRITE_*` GPIOs defined in `include/constants.h`).
- `gconfig.c` / `aconfig.c` — global vs per-app configuration stored in dedicated flash sectors, on top of `settings/` (a key-value store).
- `network.c`, `httpc/`, `download.c` — Wi-Fi (CYW43, lwIP poll mode), HTTPS-capable HTTP client, firmware download support.
- `sdcard.c`, `hw_config.c` — FatFs over SPI/SDIO via the bundled `fatfs-sdk`.
- `display.c`, `display_term.c`, `term.c`, `u8g2/` — terminal-style display rendered into the Atari framebuffer at `$FAE0C0` and/or a local OLED.
- `blink.c`, `select.c`, `reset.c`, `tprotocol.c` — LED Morse status, SELECT-button handling, soft reset/jump-to-booster, transport protocol primitives.

### Memory layout (`rp/src/memmap_rp.ld`)
The RP2040's 2 MB flash is sliced into named regions, and code is responsible for not stomping on them:

| Region | Origin | Length | Purpose |
| --- | --- | --- | --- |
| `FLASH` | `0x10000000` | 1024 K | App code |
| `ROM_TEMP` | `0x10100000` | 128 K | Scratch area for loaded ROMs |
| `BOOSTER_APP_FLASH` | `0x10120000` | 768 K | Reserved for the Booster app (do not write from this app) |
| `CONFIG_FLASH` | `0x101E0000` | 120 K | 30 sectors of per-app config |
| `GLOBAL_LOOKUP_FLASH` | `0x101FE000` | 4 K | UUID → config-sector lookup |
| `GLOBAL_CONFIG_FLASH` | `0x101FF000` | 4 K | Global config |
| `RAM` | `0x20000000` | 128 K | Normal RAM |
| `ROM_IN_RAM` | `0x20020000` | 128 K | ROM data mirrored to RAM for fast bus access |

The build assumes Core 0 owns flash writes (`PICO_FLASH_ASSUME_CORE0_SAFE=1`). The PIO bus emulation runs hot — Core 0 also overclocks to 225 MHz at `VREG_VOLTAGE_1_10`.

### App identity
`CURRENT_APP_UUID_KEY` (set from the `APP_UUID_KEY` env var at CMake time, with a placeholder default) is the app's UUID4. It must match the `uuid` field in `desc/app.json` and is used as the key into `GLOBAL_LOOKUP_FLASH` to find this app's config sector. Mismatch → app jumps to Booster.

## Editing guardrails

- **Never modify** `pico-sdk/`, `pico-extras/`, or `fatfs-sdk/` — they are git submodules pinned to specific upstream revisions, and the build re-pins them on every run. To change FatFs configuration, edit `rp/src/ff/ffconf.h` (project-owned override); the include path is set up so this file wins over the submodule's default.
- Don't touch `main.c` for feature work — start in `emul.c`.
- Match the existing C style (clang-format config in `.clang-format`, clang-tidy in `.clang-tidy` — both wired up via CMake when the binaries are on `PATH`).

---

## Working style

These behavioral guidelines bias toward caution over speed. For trivial tasks, use judgment.

### 1. Think before coding

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them — don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

### 2. Simplicity first

Minimum code that solves the problem. Nothing speculative.
- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

### 3. Surgical changes

Touch only what you must. Clean up only your own mess.
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it — don't delete it.
- When your changes orphan an import/variable/function, remove it. Don't remove pre-existing dead code unless asked.

The test: every changed line should trace directly to the user's request.

### 4. Goal-driven execution

Define success criteria. Loop until verified.
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan with a verification check per step.

### 5. No AI attribution

Never add AI-tool attribution to commits, PR descriptions, code comments,
docs, or any other artifact. This means **no**:
- "Generated with Claude Code", "Co-authored by Claude", "Made with ChatGPT",
  or any similar phrasing.
- `Co-Authored-By: Claude …`, `Co-Authored-By: ChatGPT …`, or any other
  AI co-author trailer.
- "AI-assisted", "written with the help of an LLM", etc., as comments or
  changelog entries.

Write the message as the human author. Do not mention AI tools used to
produce the work.
