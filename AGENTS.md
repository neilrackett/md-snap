# AGENTS.md — Microfirmware Template Playbook

Welcome to the md-microfirmware-template workspace. This is the quick primer so any agent can get productive fast.

## 1. Environment setup (do this before touching the repo)
- **Host tooling**
  - ARM GNU Toolchain 14.2 at `/Applications/ArmGNUToolchain/14.2.rel1/arm-none-eabi/bin` (export `PICO_TOOLCHAIN_PATH` to this path).
  - Raspberry Pi Debug Probe / Picoprobe wired to the Multi-device header (TX, RX and both GND pins **must** be connected).
  - `atarist-toolkit-docker` installed and working (`stcmd` requires a PTY, so run with `pty=true`).
  - Git + GNU Make + VS Code with the C/C++ Extension Pack, CMake Tools and Cortex-Debug.
- **SDK environment variables** (add them to your shell profile):
  ```bash
  export PICO_SDK_PATH=$REPO_ROOT/pico-sdk
  export PICO_EXTRAS_PATH=$REPO_ROOT/pico-extras
  export FATFS_SDK_PATH=$REPO_ROOT/fatfs-sdk
  ```
- **Optional debugger helpers**
  ```bash
  export ARM_GDB_PATH=/path/to/arm-none-eabi/bin
  export PICO_OPENOCD_PATH=/path/to/openocd/tcl
  ```
- **Workspace root:** `/Users/openclaw/.openclaw/workspace/md-microfirmware-template`

## 2. Common Commands
```bash
# List workspace via stcmd (requires PTY)
stcmd ls

# Build firmware (example board + UUID)
cd md-microfirmware-template
PICO_TOOLCHAIN_PATH=/Applications/ArmGNUToolchain/14.2.rel1/arm-none-eabi/bin \
  ./build.sh pico_w release 123e4567-e89b-12d3-a456-426614174000
```

## 3. Build Notes & Gotchas
- `CHARACTER_GAP_MS` constant lives in `rp/src/include/blink.h`. Keep it defined (700 ms) or the RP build fails.
- Expect harmless VASM warnings (`target data type overflow`, `trailing garbage after option -D`).
- The build script auto-copies `version.txt`, rebuilds the Atari target, then the RP target.
- Successful builds drop UF2s into `dist/` as `<UUID>-v<version>.uf2` and print the MD5 used in the generated JSON manifest.
- **Every push to `main` is a release** (`.github/workflows/release.yml`): `make` with the `APP_UUID_KEY` secret, a tag named from `version.txt` (left alone if it already exists), the `latest` tag moved to the commit, and the UF2 + JSON replaced on the `latest` GitHub Release. A push that only changes `**.md`, `emu/` or `mdfw.ini` is skipped (run the workflow by hand to release one anyway). `make build`, which CI runs, releases `version.txt` as-is and `make debug` bumps it, so bump it before pushing a new version. `.github/workflows/pr.yml` builds pull requests to `main` and publishes nothing.
- The Atari cartridge image (header + m68k code) must fit in 8 KB; `target/atarist/build.sh` enforces this against `BOOT.BIN` and aborts if exceeded. The 8 KB matches `CHANDLER_CARTRIDGE_CODE_SIZE` in `rp/src/include/chandler.h` and `CARTRIDGE_CODE_SIZE` in `target/atarist/src/main.s`.
- FatFs configuration lives at `rp/src/ff/ffconf.h`. `rp/src/CMakeLists.txt` puts that directory ahead of the `fatfs-sdk` include path with `target_include_directories(... BEFORE PRIVATE)`, so the override wins and the submodule stays clean. Do not edit the submodule's copy.

## 3a. Running it on your computer (EmuMD)
`emu/emumd` (a submodule) builds the firmware for the host and runs it in Hatari (`mdfw.ini`, `template = sidecartridge`: the firmware's own `main()` runs, with EmuMD standing in for romemul, commemul, SELECT, the SD card driver and the Booster). `git submodule update --init emu/emumd`, then `emu/emumd/tools/mdfw hatari` once, then `make emu` (`emu/emu.mk`; `MACHINE=tt TOS=...` for a TT; in automation, `emu/emumd/tools/mdfw run --headless --frames N --screenshot out.png`). The SD card is `build/sd`.
- EmuMD never presses SELECT, so a capture can't be triggered there yet; the menu, the TT/ST boot and the grabber's install can be checked.
- `emu/standins.c` holds what EmuMD doesn't stand in for yet (`commemul_deinit`, `romemul_deinit`); delete each one when EmuMD provides it (the link fails with a duplicate symbol).
- Code that keeps a ROM-in-RAM or flash address must use `uintptr_t`, not `uint32_t`: the same on the RP2040, whole on a 64-bit host.
- A TT needs a 512 KB TOS (EmuTOS's `etos512uk.img`, or TOS 3.06). Don't use Hatari's `--tos-res`: it turns on Hatari's own cartridge, so MD/Snap's never boots. Boot from a floppy with a `NEWDESK.INF` instead (`#E 18 0R 00 06`, R = 1-3 ST low/med/high, 4 TT medium, 6 TT low; TT high is `--monitor mono`).

## 4. Troubleshooting
| Symptom | Fix |
| --- | --- |
| `the input device is not a TTY` when using `stcmd` | `target/atarist/build.sh` already sets `STCMD_NO_TTY=1` for every stcmd call. If you invoke `stcmd` directly from a non-TTY context, export `STCMD_NO_TTY=1` first. |
| `arm-none-eabi-gcc not found` | Ensure `PICO_TOOLCHAIN_PATH` points to the Arm GNU toolchain bin dir |
| Build stops with missing `CHARACTER_GAP_MS` | Re-add `#define CHARACTER_GAP_MS 700` to `rp/src/include/blink.h` |
| `ERROR: cartridge code is N bytes; limit is 8192` | The m68k cartridge grew past 8 KB. Trim `target/atarist/src/main.s` (and its includes) or move data into APP_BUFFERS / SHARED_VARIABLES rather than embedding it in the cartridge image. |
| Final steps fail copying UF2 | Upstream compile failed—scroll back for the first error before the copy step |
| The Atari ST display shows garbage but commands work | Almost always means `target_firmware.h` is stale. Confirm `target/atarist/dist/BOOT.BIN` was regenerated by the current build (compare timestamp to the rest of `dist/`); if `stcmd make` failed silently and was ignored the previous BOOT.BIN survives and the m68k boots with the wrong shared-region addresses. |

## 5. Editing Guardrails
- Agents are **not allowed** to modify code inside these directories under any circumstances:
  - `/fatfs-sdk`
  - `/pico-sdk`
  - `/pico-extras`
  - `/emu/emumd` (a submodule: change EmuMD in its own repository)
- To change FatFs configuration, edit `rp/src/ff/ffconf.h`, not the file inside `/fatfs-sdk`.
- **Never add AI-tool attribution** to commits, PR descriptions, code comments, docs, or any other artifact. No `Co-Authored-By: Claude …`, no "Generated with Claude Code / ChatGPT / etc.", no "AI-assisted" notes. Write everything as the human author.

Keep this file updated as the process evolves so every agent starts with the latest tribal knowledge.
