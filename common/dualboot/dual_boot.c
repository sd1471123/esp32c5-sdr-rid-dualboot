/* Boot-menu glue shared by launcher, SDR and RID.
 *
 * The trick that makes the menu appear on *every* power-on:
 *   1. launcher is the factory app and selects a target with
 *      esp_ota_set_boot_partition(), then reboots;
 *   2. the selected app confirms its OTA state and immediately calls
 *      esp_ota_set_boot_partition(factory), which erases otadata;
 *   3. the next cold boot therefore starts factory (launcher) again.
 */
#include "dual_boot.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include <string.h>

static const char *TAG = "dualboot";

esp_err_t dualboot_confirm_running(void) {
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!run) return ESP_ERR_NOT_FOUND;
    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(run, &state);
    /* factory has no otadata entry, and that is normal. */
    if (err != ESP_OK) return ESP_OK;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) return ESP_OK;
    err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA slot '%s' confirmed", run->label);
    } else {
        ESP_LOGW(TAG, "mark app valid: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t dualboot_release_to_launcher(void) {
    const esp_partition_t *factory =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (!factory) {
        ESP_LOGE(TAG, "no factory partition in the table");
        return ESP_ERR_NOT_FOUND;
    }
    /* Special case in ESP-IDF: setting factory as boot target erases otadata. */
    esp_err_t err = esp_ota_set_boot_partition(factory);
    if (err != ESP_OK) ESP_LOGE(TAG, "release to launcher failed: %s", esp_err_to_name(err));
    return err;
}

esp_err_t dualboot_boot_label(const char *label) {
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!part) {
        ESP_LOGE(TAG, "app partition '%s' not found", label ? label : "(null)");
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) ESP_LOGE(TAG, "boot '%s' failed: %s", label, esp_err_to_name(err));
    return err;
}

void dualboot_reboot_to_launcher(void) {
    dualboot_release_to_launcher();
    esp_restart();
}

const char *dualboot_running_label(void) {
    const esp_partition_t *run = esp_ota_get_running_partition();
    return (run && run->label[0]) ? run->label : "?";
}
