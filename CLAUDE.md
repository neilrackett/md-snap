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
→ RP deplanes/pixel-doubles/writes PNG → LED flash). **B** / hold-SELECT-at-boot → Booster.

Key files (beyond the template): `target/atarist/src/userfw.s` (resident VBL grabber + installer),
`rp/src/screenshot.c` (chunk assembly + deplane + palette decode), `rp/src/png_writer.c` (streaming
indexed PNG), `rp/src/emul.c` (menu + exit-to-desktop resident service loop), plus reverse-video
added to `term.c`/`display_term.c` and the SELECT escape hatch in `main.c`.

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
- **Protocol payload cap is ~2048 B**, so the 32000-byte screen is pushed as 16×2000-byte chunks plus a
  BEGIN metadata command; the grabber dribbles one chunk per VBL frame.
- **No RTC / no wall clock:** the RP has no battery clock and we don't fetch NTP. Filenames are
  sequential `snap_NNNN_<low|medium|high>.png` (RP scans the folder for the next free index). Do not
  reintroduce timestamped names without a real time source.
- **Palette decode is ST-vs-STE:** the grabber sends the `_MCH` cookie value; the RP picks 3-bit (ST)
  or 4-bit-with-reordered-LSB (STE) decoding.
- **ESC is a normal keystroke now.** `check_keys` in `main.s` was changed to forward *every* key
  (including ESC) as `APP_TERMINAL_KEYSTROKE`; the legacy `APP_TERMINAL_START` command-line terminal is
  gone. The RP menu handles ESC/B/arrows in `menuKeyCb` (a `term_setRawKeyCallback` hook) and consumes
  all keys so stray typing can't reach the old line editor.
- **The RP never resets after exit-to-desktop** — it stays resident forever (serving ROM4 via DMA,
  polling SELECT, writing PNGs). Short SELECT = capture; long press (`SELECT_LONG_RESET`) = Booster.

### In-game capture — investigated, hard limit (read before trying again)

The cartridge (ROM) port has **no IRQ line and no bus-master line** — it's a passive slave. So the RP
can never force the 68000 to run our code or read ST RAM itself; the grabber only runs when a *system
interrupt vector we hooked* is still pointed at us **and** being serviced. Consequences:
- Desktop, GEM apps, resolution changes, and "system-friendly" games (keep TOS's VBL) capture fine.
- **Full-takeover games** (replace all vectors / run interrupts-masked at IPL 7 / bang hardware) cannot
  be captured from the cartridge by *any* means — that needs downstream hardware capture (RGBtoHDMI).
- **A multi-vector experiment (also hooking Timer C `$114`) CRASHED the ST with an address error
  (3 bombs).** Running the multi-ms snapshot/push out of the Timer C ISR — which *is* TOS's 200 Hz
  system-clock handler — destabilises TOS; symptom was `Screenshot BEGIN` repeating at ~200 Hz with no
  DATA. It was reverted. **Lesson: never run long work from Timer C / the system-clock ISR.** The
  VBL-only grabber is stable for software capture.

### Build tooling changes vs the stock template

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
There is no test suite. "Verification" is: build succeeds, UF2 boots on hardware, manual interaction over the serial debug console.

## Architecture

The firmware is a **two-target build**: m68k assembly that runs on the Atari ST is compiled into a ROM image, embedded as a C array inside the RP2040 firmware, and served back to the Atari over the cartridge bus that the RP2040 emulates via PIO + DMA.

### Atari ST side (`target/atarist/`)
- `src/main.s` — m68k cartridge boot + dispatch + terminal. Lives at `$FA0000` in the ST address space (ROM4 cartridge region). Defines the cartridge header (`CA_MAGIC`, `CA_INIT`, …), command magic numbers, and the shared-variable layout used to talk to the RP2040.
- `src/userfw.s` — **the primary extension point for app-specific m68k code.** `src/userfw.ld` places `main.s` at offset `0x0000` (2 KB budget) and `userfw.s` at offset `0x0800` (6 KB budget); `main.s` exposes the latter as `USERFW equ (ROM4_ADDR + $800)`. When the RP-side terminal command `f` ([F]irmware) is selected, the RP writes `CMD_START = 4` to the cartridge sentinel; the m68k's vsync-polled `check_commands` dispatches to `rom_function`, which `jmp`s to `USERFW`. The default `userfw.s` is a Cconws demo — replace its body with your own logic.
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
