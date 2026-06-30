/**
 * sd_pump_test_mk1.ino — focused SD-card retry loop for scope debugging
 *
 * Pumps SD_MMC.begin() in a loop, prints result, tears down, retries.
 * GPIO drive bumped to GPIO_DRIVE_CAP_3 on CLK/CMD/DAT0.
 *
 * 2026-06-29 — added drive-capability instrumentation. The library wraps
 * IDF's sdmmc_host driver, which calls gpio_iomux_*() inside begin() and
 * may reset FUN_DRV back to CAP_2 (~20 mA) before the first SDMMC
 * transaction. We now print gpio_get_drive_capability() readback at three
 * checkpoints per attempt: after setup() set, immediately before begin(),
 * and immediately after begin() — so we can SEE whether CAP_3 is alive on
 * the wire when the actual init runs.
 *
 * Heartbeat: GPIO41 (flashlight LED) ramps up to peak before begin() and
 * back down after, via LEDC PWM. Smooth breathe instead of epileptic blink.
 *
 * Optional clock-sweep mode: cycles through a list of target clocks so you
 * can see which speed gives the most PASS attempts after a pull-up swap.
 * Set SD_CLOCK_SWEEP = 0 to lock to SD_CLOCK_KHZ_LIST[0].
 *
 * Scope hookup (single-channel):
 *   CH1 : pick one of CMD (pin 3 / GPIO39), CLK (pin 5 / GPIO38),
 *         DAT0 (pin 7 / GPIO40)
 *   GND : socket pin 6, short lead
 *   Trigger: CMD falling edge ~1.6 V, Normal mode.
 *
 *   Or trigger off the LED net (GPIO41 / R32) — rising edge of the ramp
 *   marks the start of each attempt window.
 */

#include "FS.h"
#include "SD_MMC.h"
#include "driver/gpio.h"     // gpio_set_drive_capability / gpio_get_drive_capability

// ── Pins ─────────────────────────────────────────────────────────────────────
#define PIN_SD_CLK      38
#define PIN_SD_CMD      39
#define PIN_SD_D0       40
#define PIN_HEARTBEAT   41

// ── Heartbeat (LEDC PWM, smooth breathe) ─────────────────────────────────────
#define LEDC_FREQ_HZ        1000
#define LEDC_RES_BITS       8
#define HEARTBEAT_PEAK      24      // very dim — about 9 % of full duty
#define HEARTBEAT_STEPS     32
#define HEARTBEAT_RAMP_MS   12      // 32 × 12 ms ≈ 380 ms each ramp side

// ── Clock sweep ──────────────────────────────────────────────────────────────
// Cycle through these target clocks one per attempt. With borderline bus
// signal integrity, different clock targets exercise different driver
// timing paths. Once one consistently PASSes, lock SD_CLOCK_SWEEP = 0.
#define SD_CLOCK_SWEEP      1
static const int sd_clocks_khz[] = { 400, 1000, 4000 };

#define RETRY_INTERVAL_MS   100      // short delay so cadence ≈ ramp_up + begin + ramp_down

static uint32_t attempts = 0;
static uint32_t passes   = 0;
static uint32_t fails    = 0;

static const char *card_kind(sdcard_type_t t) {
    switch (t) {
        case CARD_MMC:  return "MMC";
        case CARD_SD:   return "SDSC";
        case CARD_SDHC: return "SDHC";
        case CARD_NONE: return "NONE";
        default:        return "UNKN";
    }
}

static void sd_force_max_drive(void) {
    gpio_set_drive_capability((gpio_num_t)PIN_SD_CLK, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability((gpio_num_t)PIN_SD_CMD, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability((gpio_num_t)PIN_SD_D0,  GPIO_DRIVE_CAP_3);
}

static const char *drive_name(gpio_drive_cap_t c) {
    switch (c) {
        case GPIO_DRIVE_CAP_0: return "0(~5mA)";
        case GPIO_DRIVE_CAP_1: return "1(~10mA)";
        case GPIO_DRIVE_CAP_2: return "2(~20mA)";
        case GPIO_DRIVE_CAP_3: return "3(~40mA)";
        default:               return "?";
    }
}

static void sd_print_drive(const char *tag) {
    gpio_drive_cap_t c_clk = GPIO_DRIVE_CAP_2, c_cmd = GPIO_DRIVE_CAP_2, c_d0 = GPIO_DRIVE_CAP_2;
    gpio_get_drive_capability((gpio_num_t)PIN_SD_CLK, &c_clk);
    gpio_get_drive_capability((gpio_num_t)PIN_SD_CMD, &c_cmd);
    gpio_get_drive_capability((gpio_num_t)PIN_SD_D0,  &c_d0);
    Serial.printf("       drive[%s] CLK=%s CMD=%s D0=%s\n",
                  tag, drive_name(c_clk), drive_name(c_cmd), drive_name(c_d0));
}

static void heartbeat_ramp(uint8_t from, uint8_t to) {
    if (from == to) { ledcWrite(PIN_HEARTBEAT, to); return; }
    int dir   = (to > from) ? +1 : -1;
    int steps = (to > from) ? (to - from) : (from - to);
    int step_ms = (steps > 0) ? (HEARTBEAT_RAMP_MS * HEARTBEAT_STEPS / steps) : 0;
    int v = from;
    while (v != to) {
        ledcWrite(PIN_HEARTBEAT, (uint8_t)v);
        delay(step_ms);
        v += dir;
    }
    ledcWrite(PIN_HEARTBEAT, to);
}

void setup() {
    Serial.begin(115200);
    delay(700);

    ledcAttach(PIN_HEARTBEAT, LEDC_FREQ_HZ, LEDC_RES_BITS);
    ledcWrite(PIN_HEARTBEAT, 0);

    // Initial drive-strength bump in setup(), once. The per-attempt re-apply
    // still runs (in case the SDMMC peripheral's GPIO matrix re-config resets
    // FUN_DRV back to default) but this gives us a baseline "set once at boot"
    // that we can compare against the readback values printed per attempt.
    pinMode(PIN_SD_CLK, OUTPUT);
    pinMode(PIN_SD_CMD, OUTPUT);
    pinMode(PIN_SD_D0,  OUTPUT);
    sd_force_max_drive();

    Serial.println("\n==========================================");
    Serial.println("  Kompic Mk I -- SD pump test (drive + sweep)");
    Serial.printf ("  1-bit SDMMC, retry every ~%u ms\n",
                   (unsigned)(2 * HEARTBEAT_RAMP_MS * HEARTBEAT_STEPS + RETRY_INTERVAL_MS));
    Serial.printf ("  Pins: CLK=GPIO%d CMD=GPIO%d D0=GPIO%d  (drive=CAP_3)\n",
                   PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
    if (SD_CLOCK_SWEEP) {
        Serial.print("  Clock sweep:");
        for (size_t i = 0; i < sizeof(sd_clocks_khz)/sizeof(sd_clocks_khz[0]); i++)
            Serial.printf(" %d", sd_clocks_khz[i]);
        Serial.println(" kHz");
    } else {
        Serial.printf("  Clock locked: %d kHz\n", sd_clocks_khz[0]);
    }
    Serial.printf ("  Heartbeat: GPIO%d LEDC, peak duty=%u/255 (breathe)\n",
                   PIN_HEARTBEAT, (unsigned)HEARTBEAT_PEAK);
    sd_print_drive("setup-end");
    Serial.println("==========================================");
    Serial.println();
}

void loop() {
    attempts++;
    uint32_t t0 = millis();

    int clk_khz = SD_CLOCK_SWEEP
        ? sd_clocks_khz[(attempts - 1) % (sizeof(sd_clocks_khz)/sizeof(sd_clocks_khz[0]))]
        : sd_clocks_khz[0];

    // Smooth ramp up to peak before begin()
    heartbeat_ramp(0, HEARTBEAT_PEAK);

    bool pins_ok = SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
    if (!pins_ok) {
        heartbeat_ramp(HEARTBEAT_PEAK, 0);
        fails++;
        Serial.printf("[%6lu ms] #%lu @ %d kHz  setPins FAILED  (pass=%lu fail=%lu)\n",
                      (unsigned long)t0, (unsigned long)attempts, clk_khz,
                      (unsigned long)passes, (unsigned long)fails);
        delay(RETRY_INTERVAL_MS);
        return;
    }

    sd_force_max_drive();
    sd_print_drive("pre-begin");

    bool ok = SD_MMC.begin("/sdcard", true /*mode1bit*/,
                           false /*format_if_fail*/, clk_khz);

    sd_print_drive("post-begin");
    sd_force_max_drive();
    uint32_t elapsed = millis() - t0;

    if (ok) {
        passes++;
        sdcard_type_t t = SD_MMC.cardType();
        uint64_t size_mb = SD_MMC.cardSize() / (1024ULL * 1024ULL);
        Serial.printf("[%6lu ms] #%lu @ %d kHz  PASS in %u ms  type=%s  size=%llu MB  "
                      "(pass=%lu fail=%lu)\n",
                      (unsigned long)t0, (unsigned long)attempts, clk_khz,
                      (unsigned)elapsed, card_kind(t),
                      (unsigned long long)size_mb,
                      (unsigned long)passes, (unsigned long)fails);
    } else {
        fails++;
        Serial.printf("[%6lu ms] #%lu @ %d kHz  FAIL in %u ms  "
                      "(pass=%lu fail=%lu)\n",
                      (unsigned long)t0, (unsigned long)attempts, clk_khz,
                      (unsigned)elapsed,
                      (unsigned long)passes, (unsigned long)fails);
    }

    SD_MMC.end();

    // Smooth ramp down to 0 after begin()
    heartbeat_ramp(HEARTBEAT_PEAK, 0);

    delay(RETRY_INTERVAL_MS);
}
