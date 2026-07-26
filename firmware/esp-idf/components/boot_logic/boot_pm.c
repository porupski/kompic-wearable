/**
 * @file boot_pm.c
 * @brief See boot_pm.h.
 */

#include "boot_pm.h"

#include "esp_log.h"
#include "esp_pm.h"

static const char *TAG = "BOOT_PM";

esp_err_t boot_pm_init(void)
{
#ifdef CONFIG_PM_ENABLE
    // 240/40 MHz DFS window with automatic light-sleep. The 40 MHz floor
    // still leaves the peripherals workable (APB is derived elsewhere on S3
    // -- it stays at 80 MHz during CPU DFS thanks to the S3's clock tree).
    //
    // If we later find any peripheral misbehaves at min_freq_mhz=40, we can
    // bump the floor to 80. Anything higher defeats the point of DFS.
    esp_pm_config_t cfg = {
        .max_freq_mhz       = 240,
        .min_freq_mhz       = 40,
        .light_sleep_enable = true,
    };
    esp_err_t r = esp_pm_configure(&cfg);
    if (r == ESP_OK) {
        ESP_LOGI(TAG, "PM configured: DFS %d..%d MHz + light-sleep",
                 cfg.min_freq_mhz, cfg.max_freq_mhz);
    } else {
        ESP_LOGW(TAG, "esp_pm_configure returned %s -- continuing", esp_err_to_name(r));
    }
    return r;
#else
    ESP_LOGI(TAG, "CONFIG_PM_ENABLE not set -- power management disabled");
    return ESP_OK;
#endif
}

void boot_pm_dump_locks(void)
{
#if defined(CONFIG_PM_ENABLE) && defined(CONFIG_PM_PROFILING)
    ESP_LOGI(TAG, "── PM lock inventory ──");
    esp_pm_dump_locks(stdout);
#else
    ESP_LOGI(TAG, "PM lock dump unavailable (needs CONFIG_PM_PROFILING=y)");
#endif
}
