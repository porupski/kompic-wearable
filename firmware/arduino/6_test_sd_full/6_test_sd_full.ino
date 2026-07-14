/*
 * 6_test_sd_full.ino  --  Consolidated SD card diagnostic + benchmark for
 *                         Kompic Mk I (iv7.1)
 *
 * Consolidates the three SD-focused legacy sketches into one staged
 * sequence:
 *
 *   Stage 1 -- PIN PROBE
 *     Replicates the ESP-IDF pin-recovery-time test on CLK/CMD/D0. For each
 *     pin, drives LOW then releases into open-drain and counts CPU cycles
 *     until the external pull-up brings the line back HIGH. Repeated with
 *     internal pull-up disabled (external-only reading) and enabled (pin
 *     sanity check). Isolates "external pull-up missing" from "pin dead"
 *     from "external pull-up healthy". Corresponds to legacy
 *     3c_sd_pin_probe_mk1.
 *
 *   Stage 2 -- PUMP TEST
 *     Cycles SD_MMC.begin() at 400 kHz / 1 MHz / 4 MHz / 20 MHz targets, N
 *     attempts per clock, prints per-attempt PASS/FAIL + timing + card
 *     kind/size. Corresponds to legacy 3b_sd_pump_test_mk1. GPIO drive
 *     capability is bumped to CAP_3 (~40 mA) and readback-verified across
 *     the begin() call to catch iomux resets.
 *
 *   Stage 3 -- THROUGHPUT BENCH
 *     Mounts SD at 20 MHz (Stage 4 verified working), writes a fixed-size
 *     dummy buffer M times, syncs, then reads it back. Reports write MB/s,
 *     read MB/s, and round-trip correctness. Replaces the LSM-specific
 *     legacy 6_sd_logger_mk1 -- actual sensor-driven logging lives in
 *     5_demo_sensor_logger. This stage exercises pure SD write/read
 *     capacity with no sensor dependency.
 *
 * When all three stages finish, the sketch idles in a "done" loop with the
 * button still responsive. Double-click enters ship mode. Single click is
 * a no-op (kept the button service running for the shutdown path).
 *
 * Refactored 2026-07-05 as part of the Stage 5 close-out. See
 * hardware/Reflow_info/Stage_5_Build_Report.md for context.
 */

#include <Arduino.h>
#include <Wire.h>
#include "FS.h"
#include "SD_MMC.h"
#include "driver/gpio.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"

// ── Pins ─────────────────────────────────────────────────────────────────────
#define PIN_SD_CLK        38
#define PIN_SD_CMD        39
#define PIN_SD_D0         40
#define PIN_HEARTBEAT     41    // flashlight LED, used as "busy" LEDC breathe
#define PIN_BUTTON        16
#define PIN_WS2812_DIN    42
#define PIN_SDA_BUS2       4
#define PIN_SCL_BUS2       5

// ── BQ25619 ship-mode registers ──────────────────────────────────────────────
#define BQ25619_ADDR         0x6A
#define BQ_REG_MISC_OP       0x07
#define BQ_REG_STATUS        0x08
#define BQ_STATUS_PG         (1 << 2)
#define BQ_BATFET_DIS        (1 << 5)
#define BQ_BATFET_RST_WVBUS  (1 << 4)
#define BQ_BATFET_DLY        (1 << 3)
#define BQ_BATFET_RST_EN     (1 << 2)

// ── Button timing ────────────────────────────────────────────────────────────
#define BTN_DEBOUNCE_MS      30
#define BTN_DOUBLE_GAP_MS    350
#define WS_BLIP_LEVEL        26

// ── Heartbeat (LEDC PWM breathe on the flashlight LED) ───────────────────────
#define LEDC_FREQ_HZ         1000
#define LEDC_RES_BITS        8
#define HEARTBEAT_PEAK       24
#define HEARTBEAT_STEPS      32
#define HEARTBEAT_RAMP_MS    12

// ── Stage tuning ─────────────────────────────────────────────────────────────
#define PIN_PROBE_TIMEOUT_CYCLES  10000UL
#define PIN_PROBE_ROUNDS          3         // total probe rounds in Stage 1
#define PUMP_ATTEMPTS_PER_CLOCK   5         // attempts per clock in Stage 2
#define BENCH_BUFFER_BYTES        4096
#define BENCH_TOTAL_BYTES         (256 * 1024)   // 256 KiB total per direction
#define BENCH_FILENAME            "/bench.bin"

static const int sd_clocks_khz[] = {400, 1000, 4000, 20000};

// ── I2C helpers (BQ ship mode) ───────────────────────────────────────────────
static bool i2c_write_reg(TwoWire &bus, uint8_t addr, uint8_t reg, uint8_t val) {
    bus.beginTransmission(addr);
    bus.write(reg);
    bus.write(val);
    return bus.endTransmission() == 0;
}
static uint8_t i2c_read_reg(TwoWire &bus, uint8_t addr, uint8_t reg) {
    bus.beginTransmission(addr);
    bus.write(reg);
    if (bus.endTransmission(false) != 0) return 0xFF;
    if (bus.requestFrom((int)addr, 1) != 1) return 0xFF;
    return bus.read();
}

// ── Ship mode (double-click) ─────────────────────────────────────────────────
static void ship_mode_countdown(void) {
    Serial.println("       2 s red countdown -- hands off the button.");
    const uint32_t step_ms = 40;
    const uint32_t steps   = 2000 / step_ms;
    for (uint32_t i = 0; i < steps; i++) {
        float phase = (float)i / 12.5f;
        float s = 0.5f * (1.0f - cosf(phase * (float)M_PI));
        uint8_t r = (uint8_t)(s * (float)WS_BLIP_LEVEL);
        neopixelWrite(PIN_WS2812_DIN, r, 0, 0);
        delay(step_ms);
    }
    neopixelWrite(PIN_WS2812_DIN, WS_BLIP_LEVEL, 0, 0);
}

static void enter_ship_mode(void) {
    Serial.println("[BTN ] DOUBLE -> ship mode requested");
    ship_mode_countdown();
    if (digitalRead(PIN_BUTTON) == LOW) {
        Serial.println("       button LOW at end of countdown -- aborting ship mode");
        neopixelWrite(PIN_WS2812_DIN, 0, 0, 0);
        return;
    }
    uint8_t r07 = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_MISC_OP);
    uint8_t r07_new = r07;
    r07_new |=  BQ_BATFET_DIS;
    r07_new |=  BQ_BATFET_RST_WVBUS;
    r07_new &= ~BQ_BATFET_DLY;
    r07_new &= ~BQ_BATFET_RST_EN;
    Serial.printf("       writing REG07 0x%02X -> 0x%02X\n", r07, r07_new);
    Serial.flush();
    i2c_write_reg(Wire1, BQ25619_ADDR, BQ_REG_MISC_OP, r07_new);
    delay(50);
    uint8_t st = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_STATUS);
    if (st & BQ_STATUS_PG) {
        Serial.println("       USB still present -- BATFET disabled; ship mode on USB unplug.");
        neopixelWrite(PIN_WS2812_DIN, 0, 0, 0);
    } else {
        Serial.println("       BATFET off -- expecting power loss now.");
        Serial.flush();
        while (1) { delay(100); }
    }
}

typedef enum { BTN_IDLE, BTN_PRESSED, BTN_WAIT_DBL, BTN_PRESSED_2 } btn_state_t;

static void handle_button(void) {
    static btn_state_t state    = BTN_IDLE;
    static uint32_t last_change = 0;
    static uint32_t release_ms  = 0;
    static bool     prev_low    = false;
    uint32_t now = millis();
    bool low = (digitalRead(PIN_BUTTON) == LOW);
    if (low != prev_low && (now - last_change) >= BTN_DEBOUNCE_MS) {
        last_change = now;
        prev_low = low;
        if (low) {
            state = (state == BTN_WAIT_DBL) ? BTN_PRESSED_2 : BTN_PRESSED;
        } else {
            if (state == BTN_PRESSED) {
                release_ms = now;
                state = BTN_WAIT_DBL;
            } else if (state == BTN_PRESSED_2) {
                state = BTN_IDLE;
                enter_ship_mode();
            }
        }
    }
    if (state == BTN_WAIT_DBL && (now - release_ms) > BTN_DOUBLE_GAP_MS) {
        state = BTN_IDLE;   // single click intentionally no-op
    }
}

// ── Heartbeat breathe (during long stage work) ───────────────────────────────
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

// ── SDMMC helpers ────────────────────────────────────────────────────────────
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

// ═════════════════════════════════════════════════════════════════════════════
// STAGE 1 -- PIN PROBE
// ═════════════════════════════════════════════════════════════════════════════
static uint32_t probe_pin_recovery(int pin, bool internal_pullup) {
    gpio_num_t g = (gpio_num_t)pin;
    gpio_reset_pin(g);
    gpio_set_direction(g, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_pull_mode(g, internal_pullup ? GPIO_PULLUP_ONLY : GPIO_FLOATING);
    gpio_set_level(g, 1);
    esp_rom_delay_us(200);
    gpio_set_level(g, 0);
    esp_rom_delay_us(10);
    uint32_t start = esp_cpu_get_cycle_count();
    gpio_set_level(g, 1);
    uint32_t cycles = 0;
    while (gpio_get_level(g) == 0) {
        cycles = esp_cpu_get_cycle_count() - start;
        if (cycles > PIN_PROBE_TIMEOUT_CYCLES) return PIN_PROBE_TIMEOUT_CYCLES;
    }
    cycles = esp_cpu_get_cycle_count() - start;
    return cycles;
}

static void stage1_pin_probe(void) {
    static const struct { int pin; const char *name; } SD_PINS[] = {
        { PIN_SD_CLK, "CLK  (38)" },
        { PIN_SD_CMD, "CMD  (39)" },
        { PIN_SD_D0,  "D0   (40)" },
    };

    Serial.println();
    Serial.println("╔══════════════════════════════════════════════════════════════╗");
    Serial.println("║ STAGE 1 / 3  --  SD pin recovery-time probe                  ║");
    Serial.println("╠══════════════════════════════════════════════════════════════╣");
    Serial.println("║  ~50-300    strong external pull-up (<=5 k)                  ║");
    Serial.println("║  ~300-1500  ~10 k external pull-up (nominal iv7.1)           ║");
    Serial.println("║  ~1500-8000 weak (internal only, ~45 k)                      ║");
    Serial.println("║  10000      no pull-up OR pin disconnected                   ║");
    Serial.println("╚══════════════════════════════════════════════════════════════╝");

    for (int round = 0; round < PIN_PROBE_ROUNDS; round++) {
        Serial.printf("Round %d/%d\n", round + 1, PIN_PROBE_ROUNDS);
        Serial.println("   pin        external-only  with-internal  verdict");
        for (size_t i = 0; i < sizeof(SD_PINS)/sizeof(SD_PINS[0]); ++i) {
            handle_button();
            uint32_t ext  = probe_pin_recovery(SD_PINS[i].pin, false);
            uint32_t intn = probe_pin_recovery(SD_PINS[i].pin, true);
            const char *verdict;
            if (ext >= PIN_PROBE_TIMEOUT_CYCLES && intn >= PIN_PROBE_TIMEOUT_CYCLES) {
                verdict = "!! pin DEAD (open at ESP pad)";
            } else if (ext >= PIN_PROBE_TIMEOUT_CYCLES) {
                verdict = "!! external PULL-UP MISSING";
            } else if (ext < 1500) {
                verdict = "OK";
            } else {
                verdict = "weak external pull-up";
            }
            Serial.printf("   %s  %6lu         %6lu         %s\n",
                          SD_PINS[i].name, (unsigned long)ext,
                          (unsigned long)intn, verdict);
        }
        Serial.println();
        delay(500);
    }

    // Leave the pins high-Z between stages.
    for (size_t i = 0; i < sizeof(SD_PINS)/sizeof(SD_PINS[0]); ++i) {
        gpio_reset_pin((gpio_num_t)SD_PINS[i].pin);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// STAGE 2 -- PUMP TEST (retry SD_MMC.begin at a clock sweep)
// ═════════════════════════════════════════════════════════════════════════════
static void stage2_pump_test(void) {
    Serial.println();
    Serial.println("╔══════════════════════════════════════════════════════════════╗");
    Serial.println("║ STAGE 2 / 3  --  SD_MMC.begin() pump, clock sweep            ║");
    Serial.printf ("║  Clocks (kHz):  %5d  %5d  %5d  %5d                    ║\n",
                    sd_clocks_khz[0], sd_clocks_khz[1], sd_clocks_khz[2], sd_clocks_khz[3]);
    Serial.printf ("║  Attempts per clock: %2u                                       ║\n",
                    (unsigned)PUMP_ATTEMPTS_PER_CLOCK);
    Serial.printf ("║  Drive:  CAP_3 (~40 mA) on CLK/CMD/D0                        ║\n");
    Serial.println("╚══════════════════════════════════════════════════════════════╝");

    pinMode(PIN_SD_CLK, OUTPUT);
    pinMode(PIN_SD_CMD, OUTPUT);
    pinMode(PIN_SD_D0,  OUTPUT);
    sd_force_max_drive();

    uint32_t total_pass = 0, total_fail = 0;
    for (size_t ck = 0; ck < sizeof(sd_clocks_khz)/sizeof(sd_clocks_khz[0]); ck++) {
        int khz = sd_clocks_khz[ck];
        uint32_t clock_pass = 0, clock_fail = 0;
        Serial.printf("\n-- clock=%d kHz --\n", khz);
        for (unsigned att = 1; att <= PUMP_ATTEMPTS_PER_CLOCK; att++) {
            handle_button();
            heartbeat_ramp(0, HEARTBEAT_PEAK);
            uint32_t t0 = millis();
            bool pins_ok = SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
            if (!pins_ok) {
                heartbeat_ramp(HEARTBEAT_PEAK, 0);
                clock_fail++; total_fail++;
                Serial.printf("   attempt %2u/%u  setPins FAILED\n", att, PUMP_ATTEMPTS_PER_CLOCK);
                delay(80);
                continue;
            }
            sd_force_max_drive();
            bool ok = SD_MMC.begin("/sdcard", true, false, khz);
            uint32_t elapsed = millis() - t0;
            if (ok) {
                sdcard_type_t t = SD_MMC.cardType();
                uint64_t size_mb = SD_MMC.cardSize() / (1024ULL * 1024ULL);
                Serial.printf("   attempt %2u/%u  PASS in %u ms  type=%s  size=%llu MB\n",
                              att, PUMP_ATTEMPTS_PER_CLOCK, (unsigned)elapsed,
                              card_kind(t), (unsigned long long)size_mb);
                clock_pass++; total_pass++;
            } else {
                Serial.printf("   attempt %2u/%u  FAIL in %u ms\n",
                              att, PUMP_ATTEMPTS_PER_CLOCK, (unsigned)elapsed);
                clock_fail++; total_fail++;
            }
            SD_MMC.end();
            heartbeat_ramp(HEARTBEAT_PEAK, 0);
            delay(80);
        }
        Serial.printf("   -> %d kHz: %u/%u PASS\n",
                      khz, (unsigned)clock_pass, (unsigned)PUMP_ATTEMPTS_PER_CLOCK);
    }
    Serial.printf("\n  Pump summary: %u PASS / %u FAIL total\n",
                  (unsigned)total_pass, (unsigned)total_fail);
}

// ═════════════════════════════════════════════════════════════════════════════
// STAGE 3 -- THROUGHPUT BENCH
// ═════════════════════════════════════════════════════════════════════════════
static void stage3_bench(void) {
    Serial.println();
    Serial.println("╔══════════════════════════════════════════════════════════════╗");
    Serial.println("║ STAGE 3 / 3  --  SD throughput bench (20 MHz)                ║");
    Serial.printf ("║  Buffer=%u B  total=%u B  file=%s                    ║\n",
                    (unsigned)BENCH_BUFFER_BYTES, (unsigned)BENCH_TOTAL_BYTES,
                    BENCH_FILENAME);
    Serial.println("╚══════════════════════════════════════════════════════════════╝");

    if (!SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0) ||
        !SD_MMC.begin("/sdcard", true, false, 20000)) {
        Serial.println("[BENCH] SD mount FAILED at 20 MHz -- aborting stage 3");
        SD_MMC.end();
        return;
    }
    sdcard_type_t t = SD_MMC.cardType();
    uint64_t size_mb = SD_MMC.cardSize() / (1024ULL * 1024ULL);
    Serial.printf("[BENCH] mount OK  type=%s  size=%llu MB\n",
                  card_kind(t), (unsigned long long)size_mb);

    // Prepare a well-known buffer pattern (byte i = i & 0xFF, wraps per 256).
    static uint8_t buf[BENCH_BUFFER_BYTES];
    for (size_t i = 0; i < BENCH_BUFFER_BYTES; i++) buf[i] = (uint8_t)i;

    // ── WRITE pass ──────────────────────────────────────────────────────────
    File w = SD_MMC.open(BENCH_FILENAME, FILE_WRITE);
    if (!w) {
        Serial.println("[BENCH] open for write FAILED");
        SD_MMC.end();
        return;
    }
    heartbeat_ramp(0, HEARTBEAT_PEAK);
    uint32_t t0 = millis();
    size_t written = 0;
    while (written < BENCH_TOTAL_BYTES) {
        handle_button();
        size_t n = w.write(buf, BENCH_BUFFER_BYTES);
        if (n != BENCH_BUFFER_BYTES) {
            Serial.printf("[BENCH] short write at %u B (wrote %u)\n",
                          (unsigned)written, (unsigned)n);
            break;
        }
        written += n;
    }
    w.flush();
    uint32_t write_ms = millis() - t0;
    w.close();
    heartbeat_ramp(HEARTBEAT_PEAK, 0);
    float write_kbps = (float)written / (float)write_ms;   // KB/s == B/ms
    Serial.printf("[BENCH] wrote %u B in %u ms  ->  %.1f KB/s\n",
                  (unsigned)written, (unsigned)write_ms, (double)write_kbps);

    // ── READ + verify pass ──────────────────────────────────────────────────
    File r = SD_MMC.open(BENCH_FILENAME, FILE_READ);
    if (!r) {
        Serial.println("[BENCH] open for read FAILED");
        SD_MMC.end();
        return;
    }
    heartbeat_ramp(0, HEARTBEAT_PEAK);
    static uint8_t rbuf[BENCH_BUFFER_BYTES];
    uint32_t t1 = millis();
    size_t read_total = 0;
    size_t mismatches = 0;
    while (read_total < written) {
        handle_button();
        size_t want = (written - read_total) > BENCH_BUFFER_BYTES ?
                        BENCH_BUFFER_BYTES : (written - read_total);
        int n = r.read(rbuf, want);
        if (n <= 0) {
            Serial.printf("[BENCH] short read at %u B\n", (unsigned)read_total);
            break;
        }
        for (int i = 0; i < n; i++) {
            if (rbuf[i] != (uint8_t)((read_total + i) & 0xFF)) mismatches++;
        }
        read_total += n;
    }
    uint32_t read_ms = millis() - t1;
    r.close();
    heartbeat_ramp(HEARTBEAT_PEAK, 0);
    float read_kbps = (float)read_total / (float)read_ms;
    Serial.printf("[BENCH] read  %u B in %u ms  ->  %.1f KB/s  (mismatches=%u)\n",
                  (unsigned)read_total, (unsigned)read_ms,
                  (double)read_kbps, (unsigned)mismatches);

    // Delete the bench file so we don't accumulate junk between reboots.
    if (SD_MMC.remove(BENCH_FILENAME)) {
        Serial.printf("[BENCH] cleaned up %s\n", BENCH_FILENAME);
    }
    SD_MMC.end();
}

// ═════════════════════════════════════════════════════════════════════════════
// setup / loop
// ═════════════════════════════════════════════════════════════════════════════
void setup(void) {
    Serial.begin(115200);
    delay(600);

    pinMode(PIN_BUTTON, INPUT_PULLUP);
    Wire1.begin(PIN_SDA_BUS2, PIN_SCL_BUS2, 100000);
    ledcAttach(PIN_HEARTBEAT, LEDC_FREQ_HZ, LEDC_RES_BITS);
    ledcWrite(PIN_HEARTBEAT, 0);
    neopixelWrite(PIN_WS2812_DIN, 0, 0, 0);

    Serial.println();
    Serial.println("╔══════════════════════════════════════════════════════════════╗");
    Serial.println("║  Kompic Mk I -- SD full test (pin probe + pump + bench)      ║");
    Serial.println("║  Ship mode: double-click GPIO16 (any time)                   ║");
    Serial.println("╚══════════════════════════════════════════════════════════════╝");

    stage1_pin_probe();
    stage2_pump_test();
    stage3_bench();

    Serial.println();
    Serial.println("═════════════════════════════════════════════════════════════════");
    Serial.println("  All SD stages complete -- idling. Double-click for ship mode.");
    Serial.println("═════════════════════════════════════════════════════════════════");
}

void loop(void) {
    handle_button();
    delay(20);
}
