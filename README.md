# MD/Snap

<img width="320" height="200" alt="snap_0030_menu" src="https://github.com/user-attachments/assets/d129d29e-fb68-495d-af72-8a2bd904b6c8" /> <img width="320" height="200" alt="snap_0013_low" src="https://github.com/user-attachments/assets/a1c1d582-832f-4ced-a39c-ccfd49e5d36f" />

<img width="320" height="200" alt="snap_0028_low" src="https://github.com/user-attachments/assets/2394aa5b-9665-47b2-8479-1312dd966b1e" /> <img width="320" height="200" alt="snap_0019_low" src="https://github.com/user-attachments/assets/64d82c79-e57f-497c-b4cd-5d61b4c6d363" />

Microfirmware for the [SidecarTridge Multi-device](https://sidecartridge.com) by [Neil Rackett](https://x.com/neilrackett)

## Introduction

MD/Snap turns your SidecarTridge Multi-device into a screenshot button for your Atari ST: press **SELECT** on the SidecarT and the current screen is saved as a 640x400 PNG in the `/screenshots` folder on the SD card.

When the app starts you'll see a list of the screenshots you've already taken. Press **ESC** to start using your ST as normal and from then on, every press of **SELECT** grabs the screen and flashes SidecarT's LED to let you know it's worked.

It works in all ST resolutions (low, medium and high) and corrects the aspect ratio for you, so the saved 640×400 image looks like what you see on screen rather than squashed pixels.

On a TT the menu is shown in TT low, so a screenshot's preview has all of its colours (up to 256), and on a TT's mono monitor in TT high. The TT's internal speaker is switched off while the menu is shown, and when you go to the desktop the speaker and the screen mode are put back as they were. It also captures TT low, medium and high: TT low (320×480, 256 colours) is saved as 640×480, TT medium as 640×480 and TT high as a 1280×960 mono PNG. A TT screen is nearly five times the size of an ST one, so it takes a little longer to save.

## Installation

1. Download the latest files from the [releases page](https://github.com/neilrackett/md-snap/releases).
2. Copy the `.uf2` and `.json` files to the `/apps` folder of your SidecarT's microSD card.
3. On the Booster screen, press ESC for the app list and select the MD/Snap app.
4. To return to Booster, power on while holding SELECT or press B when the menu appears.

## Capture mode

You can choose between 2 screen capture modes:

- VBL: Instantly capture GEM and simple TOS apps, and some games.
- ETV: This mode is slower, typically 1-2s to save a screenshot, but works with a wider selection of apps and games (experimental).

## Known limitations

- Anything that replaces VBL, ETV or Timer C will block the screen grabber, so it may not work in all games.
- It doesn't work for apps that bypass standard screen layout, for example overscan or sync-scrolling.

## Running it on your computer

MD/Snap also runs in [Hatari](https://www.hatari-emu.org) on your Mac or Linux PC, with [EmuMD](https://github.com/neilrackett/emumd) standing in for the Multi-device and `build/sd` for its microSD card:

```bash
git submodule update --init emu/emumd
emu/emumd/tools/mdfw hatari   # once: the Hatari that runs it, and EmuTOS
make emu                      # build build/md-snap.mdfw and run it on an STE
make emu MACHINE=tt TOS=~/.cache/emumd/emutos-512k-1.4/etos512uk.img   # on a TT
make emu SELECT=20000,30000   # press SELECT 20 and 30 seconds in, for two screenshots
make emu RECORD=mdsnap.avi    # recording picture and sound until you quit
```

Screenshots land in `build/sd/screenshots`. `SELECT=5000+11000` holds SELECT for 11 seconds from 5 seconds in, which is a long press. It needs libslirp for the template's network code (`brew install libslirp pkg-config`, or `sudo apt install libslirp-dev libglib2.0-dev pkg-config`). See [EmuMD's guide](https://github.com/neilrackett/emumd/blob/main/docs/GUIDE.md) for more.

## What's next?

A few ideas we've had (no promises we'll do them all):

- Preview thumbnails in the menu?

Think you can help? Got an idea of your own? We'd love to hear from you, so why not let me know on [X](https://x.com/neilrackett) or submit a PR.

## License

The source code of the project is licensed under the GNU General Public License v3.0. The full license is accessible in the [LICENSE](LICENSE) file.
