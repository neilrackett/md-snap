#include "reset.h"

void reset_reboot_to_booster(void) {
  DPRINTF("Rebooting to the Booster app via watchdog reset\n");

  // Tag a watchdog scratch register (untouched by the bootrom, which only uses
  // scratch[4..7], and cleared on a cold power-on) so main() routes to Booster
  // on the clean restart. A full chip reset hands Booster pristine hardware,
  // unlike reset_jump_to_booster()'s in-place jump which would carry this app's
  // live pio0/DMA across and corrupt the Booster.
  watchdog_hw->scratch[0] = RESET_BOOSTER_MAGIC;

  save_and_disable_interrupts();
  watchdog_reboot(0, 0, RESET_WATCHDOG_TIMEOUT);
  while (1) {
    tight_loop_contents();
  }
}

void reset_device() {
  DPRINTF("Resetting the device\n");

  save_and_disable_interrupts();
  // watchdog_enable(RESET_WATCHDOG_TIMEOUT, 0);
  watchdog_reboot(0, 0, RESET_WATCHDOG_TIMEOUT);
  // 20 ms timeout, for example, then the chip will reset
  while (1) {
    // Wait for the reset
    DPRINTF("Waiting for the device to reset\n");
  }
  DPRINTF("You should never reach this point\n");
}

void reset_deviceAndEraseFlash() {
  // Erase the settings
  DPRINTF("Erasing the flash memory\n");
  settings_erase(gconfig_getContext());
  DPRINTF("Erasing the app lookup table\n");
  sleep_ms(SEC_TO_MS);

  // Reset the device
  DPRINTF("Resetting the device\n");
  reset_device();
}