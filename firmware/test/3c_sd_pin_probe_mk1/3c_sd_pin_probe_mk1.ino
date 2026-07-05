/*
 * 3c_sd_pin_probe_mk1  --  SD pin recovery-time probe, Kompic Mk I (iv7.1)
 *
 * Replicates the ESP-IDF "Debug SD connections and pullup strength" test
 * (esp-idf/examples/storage/sd_card/sdmmc, menuconfig CONFIG_EXAMPLE_DEBUG_
 * PIN_CONNECTIONS_AND_PULLUP_STRENGTH) as a plain Arduino sketch.
 *
 * For each of the three routed SDMMC lines (CLK GPIO38, CMD GPIO39, D0 GPIO40)
 * we:
 *   1. Configure the pin as INPUT_OUTPUT_OD (open-drain with input readback).
 *   2. Drive LOW for 10 us.
 *   3. Release (open-drain high). Whatever pull-up is on the net has to bring
 *      the line back up.
 *   4. Count CPU cycles until the pin reads HIGH.  Timeout at 10 000 cycles.
 *
 * We do this pass twice per pin: once with the *internal* pull-up disabled
 * (pure external pull-up measurement) and once with the internal pull-up
 * enabled (control: proves the pin itself is functional). Comparison of the
 * two isolates "external pull-up missing" from "pin electrically dead".
 *
 * Expected on a healthy iv7.1 with 10 k external pull-ups:
 *   external-only : ~200-1500 cycles  (10 k * ~20-25 pF R-C rise)
 *   with internal : similar (10 k || 45 k = 8.2 k, slightly faster)
 * If a pin reads 10000 cycles external-only but is fast with internal
 * enabled, the *external* pull-up chain is broken on that pin -- smoking
 * gun for the SDMMC send_scr 0x107 failure.
 * If a pin reads 10000 cycles in BOTH modes, the pin itself is dead
 * (open-circuit between ESP pad and PCB net -- cold joint at the module).
 *
 * MANDATORY: double-click on the button (GPIO16 == BQ25619 QON) enters
 * BQ ship mode -- the battery is permanently attached, this is the only
 * way to turn the board off between bench sessions.
 */

#include <Arduino.h>
#include <Wire.h>
#include "driver/gpio.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"

// ── Pinout ───────────────────────────────────────────────────────────────────
#define PIN_SD_CLK        38
#define PIN_SD_CMD        39
#define PIN_SD_D0         40
#define PIN_BUTTON        16     // BQ25619 /QON, active LOW

#define PIN_SDA_BUS2       4     // BQ25619 lives on bus 2 in iv7.1
#define PIN_SCL_BUS2       5

// ── BQ25619 ship-mode registers ─────────────────────────────────────────────
#define BQ25619_ADDR         0x6A
#define BQ_REG_MISC_OP       0x07
#define BQ_REG_STATUS        0x08
#define BQ_STATUS_VBUS_MASK  0xE0
#define BQ_BATFET_DIS        (1 << 5)
#define BQ_BATFET_RST_WVBUS  (1 << 4)
#define BQ_BATFET_DLY        (1 << 3)
#define BQ_BATFET_RST_EN     (1 << 2)

// ── Button state machine (copy of 4_demo_mk1 pattern) ───────────────────────
#define BTN_DEBOUNCE_MS      30
#define BTN_DOUBLE_GAP_MS    350

typedef enum {
    BTN_IDLE,
    BTN_PRESSED,
    BTN_WAIT_DBL,
    BTN_PRESSED_2,
} btn_state_t;

// ── I2C helpers ─────────────────────────────────────────────────────────────
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

// ── Ship mode ───────────────────────────────────────────────────────────────
static void enter_ship_mode(void) {
    Serial.println("[BTN ] DOUBLE -> ship mode requested");
    // Belt-and-suspenders: wait for a clean QON-high before writing.
    while (digitalRead(PIN_BUTTON) == LOW) delay(5);
    delay(30);

    uint8_t r07 = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_MISC_OP);
    uint8_t r07_new = r07;
    r07_new |=  BQ_BATFET_DIS;
    r07_new |=  BQ_BATFET_RST_WVBUS;
    r07_new &= ~BQ_BATFET_DLY;
    r07_new &= ~BQ_BATFET_RST_EN;
    Serial.printf("       writing REG07 0x%02X -> 0x%02X\n", r07, r07_new);
    i2c_write_reg(Wire1, BQ25619_ADDR, BQ_REG_MISC_OP, r07_new);

    uint8_t st = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_STATUS);
    if ((st & BQ_STATUS_VBUS_MASK) != 0) {
        Serial.println("       USB present -- BATFET off but chip still awake.");
    } else {
        Serial.println("       BATFET off -- expecting power loss now.");
    }
}

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
        state = BTN_IDLE;   // single click is a no-op in this sketch
    }
}

// ── Pin recovery probe ──────────────────────────────────────────────────────
// Drives pin LOW, releases into open-drain, counts CPU cycles until pin
// reads HIGH. Timeout returned as UINT32_MAX (also caps the counted cycles).
#define PIN_PROBE_TIMEOUT_CYCLES 10000UL

static uint32_t probe_pin_recovery(int pin, bool internal_pullup) {
    gpio_num_t g = (gpio_num_t)pin;

    // Reset -> input+output open-drain -> configure pull mode.
    gpio_reset_pin(g);
    gpio_set_direction(g, GPIO_MODE_INPUT_OUTPUT_OD);
    if (internal_pullup) {
        gpio_set_pull_mode(g, GPIO_PULLUP_ONLY);
    } else {
        gpio_set_pull_mode(g, GPIO_FLOATING);
    }

    // Park pin HIGH (open-drain released), give the line time to settle so
    // whatever pull-up is present has re-charged any parasitic C.
    gpio_set_level(g, 1);
    esp_rom_delay_us(200);

    // Drive LOW for 10 us to fully discharge parasitic C.
    gpio_set_level(g, 0);
    esp_rom_delay_us(10);

    // Release open-drain; measure recovery.
    uint32_t start = esp_cpu_get_cycle_count();
    gpio_set_level(g, 1);
    uint32_t cycles = 0;
    while (gpio_get_level(g) == 0) {
        cycles = esp_cpu_get_cycle_count() - start;
        if (cycles > PIN_PROBE_TIMEOUT_CYCLES) return PIN_PROBE_TIMEOUT_CYCLES;
    }
    cycles = esp_cpu_get_cycle_count() - start;

    // Leave the pin as-configured; we'll reconfigure next round anyway.
    return cycles;
}

// ── Setup / loop ────────────────────────────────────────────────────────────
static const struct { int pin; const char *name; } SD_PINS[] = {
    { PIN_SD_CLK, "CLK  (38)" },
    { PIN_SD_CMD, "CMD  (39)" },
    { PIN_SD_D0,  "D0   (40)" },
};

void setup(void) {
    Serial.begin(115200);
    delay(200);
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    Wire1.begin(PIN_SDA_BUS2, PIN_SCL_BUS2, 100000);
    delay(50);

    Serial.println();
    Serial.println("=== SD pin recovery-time probe (iv7.1) ===");
    Serial.println("Interpretation:");
    Serial.println("   ~50-300  cycles  : strong external pull-up (<=5 k)");
    Serial.println("   ~300-1500 cycles : ~10 k external pull-up (nominal iv7.1)");
    Serial.println("   ~1500-8000       : weak pull-up (internal only, ~45 k)");
    Serial.println("   10000 (timeout)  : no pull-up OR pin disconnected");
    Serial.println();
    Serial.println("Double-click button = BQ ship mode.");
    Serial.println();
}

void loop(void) {
    static uint32_t last_probe = 0;
    handle_button();

    uint32_t now = millis();
    if (now - last_probe < 2000) return;
    last_probe = now;

    Serial.printf("[%6lu ms]  external-only        with-internal-pullup   verdict\n", now);
    for (size_t i = 0; i < sizeof(SD_PINS)/sizeof(SD_PINS[0]); ++i) {
        uint32_t ext = probe_pin_recovery(SD_PINS[i].pin, false);
        uint32_t intn = probe_pin_recovery(SD_PINS[i].pin, true);

        const char *verdict;
        if (ext >= PIN_PROBE_TIMEOUT_CYCLES && intn >= PIN_PROBE_TIMEOUT_CYCLES) {
            verdict = "!! pin DEAD (no internal pull-up response -- open at ESP pad)";
        } else if (ext >= PIN_PROBE_TIMEOUT_CYCLES) {
            verdict = "!! external PULL-UP MISSING (internal works, external open)";
        } else if (ext < 1500) {
            verdict = "OK (external pull-up healthy)";
        } else {
            verdict = "weak external pull-up";
        }
        Serial.printf("   %s  %6lu cycles         %6lu cycles          %s\n",
                      SD_PINS[i].name, (unsigned long)ext,
                      (unsigned long)intn, verdict);
    }
    Serial.println();

    // Leave the pins high-Z between rounds so they don't fight anything.
    for (size_t i = 0; i < sizeof(SD_PINS)/sizeof(SD_PINS[0]); ++i) {
        gpio_reset_pin((gpio_num_t)SD_PINS[i].pin);
    }
}
