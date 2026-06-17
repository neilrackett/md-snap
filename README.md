# MD/Snap

Microfirmware for the [SidecarTridge Multi-device](https://sidecartridge.com) by [Neil Rackett](https://x.com/neilrackett)

## Introduction

MD/Snap turns your SidecarTridge Multi-device into a screenshot button for your Atari ST: press **SELECT** on the SidecarT and the current screen is saved as a 640x400 PNG in the `/screenshots` folder on the SD card.

When the app starts you'll see a list of the screenshots you've already taken. Press **ESC** to start using your ST as normal and from then on, every press of **SELECT** grabs the screen and flashes SidecarT's LED to let you know it's worked.

It works in all ST resolutions (low, medium and high) and corrects the aspect ratio for you, so the saved 640×400 image looks like what you see on screen rather than squashed pixels.

## Installation

1. Download the latest files from the [releases page](https://github.com/neilrackett/md-snap/releases).
2. Copy the `.uf2` and `.json` files to the `/apps` folder of your SidecarT's microSD card.
3. On the Booster screen, press ESC for the app list and select the MD/Snap app.
4. To return to Booster, power on while holding SELECT or press B when the menu appears.

## Known limitations

- Works perfectly in GEM and simple TOS apps, but may not work in some games.
- It doesn't work for apps that bypass standard screen layout, for example apps that use overscan or sync-scrolling.

## What's next?

A few ideas we've had (no promises we'll do them all):

- View your screenshots from the app?
- Preview thumbnails in the menu?

Think you can help? Got an idea of your own? We'd love to hear from you, so why not let me know on [X](https://x.com/neilrackett) or submit a PR.

## License

The source code of the project is licensed under the GNU General Public License v3.0. The full license is accessible in the [LICENSE](LICENSE) file.
