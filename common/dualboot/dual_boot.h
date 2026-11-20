#pragma once
#include "esp_err.h"
#include <stdbool.h>

/* Labels in partitions/merged_16mb.csv */
#define DUALBOOT_LABEL_SDR   "sdr"
#define DUALBOOT_LABEL_RID_A "rid_a"
#define DUALBOOT_LABEL_RID_B "rid_b"

/* Call at the very start of every app's app_main().
 * If the running OTA slot is waiting for confirmation (new image after OTA),
 * mark it valid so the bootloader does not roll it back on the next reboot. */
esp_err_t dualboot_confirm_running(void);

/* Make `factory` (the launcher) the boot target again.  ESP-IDF implements this
 * by erasing otadata; the second-stage bootloader then always boots factory. */
esp_err_t dualboot_release_to_launcher(void);

/* Set the next boot target by partition label, then reboot. */
esp_err_t dualboot_boot_label(const char *label);
void dualboot_reboot_to_launcher(void);

/* Current running app partition label, or "?" when unavailable. */
const char *dualboot_running_label(void);
