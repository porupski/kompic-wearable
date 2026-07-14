/*
 * 10_test_max30101.ino  --  MAX30101 wrist/finger PPG diagnostic
 *
 * Streams the MAX30101's Green channel FIFO output at ~12.5 Hz effective
 * rate so you can see whether the raw PPG waveform actually contains
 * any cardiac modulation before we start tuning any beat detector.
 * Three output modes selectable at compile time via OUT_MODE at the top:
 *
 *   OUT_MODE_MONITOR   -- 2 Hz stats to Serial Monitor (default)
 *                         Shows raw green DC, AC p2p over window,
 *                         effective sample rate, and a fast peak-count.
 *   OUT_MODE_PLOT_RAW  -- raw green counts, one per line, for Serial Plotter.
 *   OUT_MODE_PLOT_AC   -- 3 traces per line: raw / baseline (slow EMA) /
 *                         ac (raw - baseline, clipped +/-3000). Best for
 *                         actually seeing the PPG waveform shape.
 *
 * ── Controls ─────────────────────────────────────────────────────────────
 *   single click : cycle green LED current 0x0F -> 0x1F -> 0x3F -> 0x7F ->
 *                  0xFF -> back to 0x0F
 *   double click : BQ25619 ship mode (battery permanently attached)
 *
 * ── RGB status (WS2812B) ─────────────────────────────────────────────────
 *   dim green   0x0F (~4 mA)
 *   green       0x1F (~7 mA)   <- old firmware value
 *   yellow      0x3F (~15 mA)
 *   orange      0x7F (~30 mA)  <- new firmware value
 *   red         0xFF (~50 mA)  <- absolute max
 *
 * ── What to look for ─────────────────────────────────────────────────────
 *  1. On fingertip: raw green DC should read tens of thousands of counts,
 *     and the AC trace should show smooth ~1 Hz undulations locked to
 *     your pulse.
 *  2. On wrist: raw DC likely 5-20k, AC amplitude much smaller (10-500).
 *     If you can see periodic undulations at ~1 Hz in the AC trace, PPG
 *     is present -- just needs better detection tuning.
 *  3. If AC is flat noise no matter where you press: the sensor is
 *     either blocked, mounted wrong, or the LED isn't lighting up.
 *
 * Board: ESP32-S3, Arduino IDE, USB Mode = "Hardware CDC and JTAG".
 */

#include <Arduino.h>
#include <Wire.h>
#include <math.h>

// ═════════════════════════════════════════════════════════════════════════════
// Output-style toggle
// ═════════════════════════════════════════════════════════════════════════════
#define OUT_MODE_MONITOR   0
#define OUT_MODE_PLOT_RAW  1
#define OUT_MODE_PLOT_AC   2
#define OUT_MODE           OUT_MODE_PLOT_AC

// AC clip window for OUT_MODE_PLOT_AC (Serial Plotter auto-scales to this)
#define AC_CLIP  3000

// ═════════════════════════════════════════════════════════════════════════════
// Pins (same board layout as sketches 7-9)
// ═════════════════════════════════════════════════════════════════════════════
#define PIN_BUTTON        16
#define PIN_SDA_BUS1       1     // Wire  -> MAX30101
#define PIN_SCL_BUS1       2
#define PIN_SDA_BUS2       4     // Wire1 -> BQ25619
#define PIN_SCL_BUS2       5
#define PIN_WS2812_DIN    42

// ═════════════════════════════════════════════════════════════════════════════
// I2C addresses
// ═════════════════════════════════════════════════════════════════════════════
#define MAX30101_ADDR     0x57
#define BQ25619_ADDR      0x6A

// ═════════════════════════════════════════════════════════════════════════════
// MAX30101 registers
// ═════════════════════════════════════════════════════════════════════════════
#define M_REG_INT_STATUS1     0x00
#define M_REG_FIFO_WR_PTR     0x04
#define M_REG_OVF_CTR         0x05
#define M_REG_FIFO_RD_PTR     0x06
#define M_REG_FIFO_DATA       0x07
#define M_REG_FIFO_CONFIG     0x08
#define M_REG_MODE_CONFIG     0x09
#define M_REG_SPO2_CONFIG     0x0A
#define M_REG_LED1_PA         0x0C   // Red
#define M_REG_LED2_PA         0x0D   // IR
#define M_REG_LED3_PA         0x0E   // Green
#define M_REG_MULTI_LED_1     0x11
#define M_REG_MULTI_LED_2     0x12
#define M_REG_PART_ID         0xFF
#define M_PART_ID_VAL         0x15

#define M_MODE_RESET          0x40
#define M_MODE_MULTI_LED      0x07

// SPO2_CFG bit fields (bits 6:5 ADC, 4:2 SR, 1:0 PW)
#define M_ADC_16384           0x03
#define M_SR_100              0x01
#define M_PW_411              0x03

// FIFO_CFG (bits 7:5 SMP_AVE, 4 rollover, 3:0 almost-full)
#define M_SMP_AVE_8           0x03

// ═════════════════════════════════════════════════════════════════════════════
// BQ25619 (ship mode)
// ═════════════════════════════════════════════════════════════════════════════
#define BQ_REG_MISC_OP        0x07
#define BQ_REG_STATUS         0x08
#define BQ_STATUS_PG          (1 << 2)
#define BQ_BATFET_DIS         (1 << 5)
#define BQ_BATFET_RST_WVBUS   (1 << 4)
#define BQ_BATFET_DLY         (1 << 3)
#define BQ_BATFET_RST_EN      (1 << 2)

// ═════════════════════════════════════════════════════════════════════════════
// Button
// ═════════════════════════════════════════════════════════════════════════════
#define BTN_DEBOUNCE_MS      30
#define BTN_DOUBLE_GAP_MS    350
#define WS_LEVEL             26

// ═════════════════════════════════════════════════════════════════════════════
// LED-current cycle
// ═════════════════════════════════════════════════════════════════════════════
typedef struct {
    uint8_t     pa_val;
    const char *label;
    uint8_t     r, g, b;
} pa_step_t;
static const pa_step_t PA_STEPS[5] = {
    { 0x0F, "0x0F (~4 mA)",       0,        WS_LEVEL, 0        },   // dim green
    { 0x1F, "0x1F (~7 mA)",       0,        WS_LEVEL, 0        },   // green
    { 0x3F, "0x3F (~15 mA)",      WS_LEVEL, WS_LEVEL, 0        },   // yellow
    { 0x7F, "0x7F (~30 mA)",      WS_LEVEL, WS_LEVEL/2, 0      },   // orange-ish
    { 0xFF, "0xFF (~50 mA, max)", WS_LEVEL, 0,        0        },   // red
};
static uint8_t pa_idx = 3;   // start at 0x7F to match current esp-idf firmware

// ═════════════════════════════════════════════════════════════════════════════
// State
// ═════════════════════════════════════════════════════════════════════════════
static bool max_ok = false;
static bool bq_ok  = false;

// Sample stats over the last window (reset per monitor print)
static uint32_t win_sum         = 0;
static int64_t  win_sq_sum      = 0;
static uint32_t win_min         = 0xFFFFFFFF;
static uint32_t win_max         = 0;
static uint32_t win_count       = 0;
static uint32_t win_start_ms    = 0;

// Baseline for AC extraction (slow EMA)
static float    baseline        = 0.0f;
static bool     baseline_ready  = false;
static uint32_t baseline_start  = 0;

// AC peak tracking + simple beat detector
static float    ac_peak         = 0.0f;
static float    ac_prev         = 0.0f;
static uint32_t last_beat_ms    = 0;
static uint32_t beat_count      = 0;
static float    last_int_ms     = 0.0f;

// ═════════════════════════════════════════════════════════════════════════════
// I2C helpers
// ═════════════════════════════════════════════════════════════════════════════
static bool i2c_ping(TwoWire &bus, uint8_t addr) {
    bus.beginTransmission(addr);
    return bus.endTransmission() == 0;
}
static uint8_t i2c_read_reg(TwoWire &bus, uint8_t addr, uint8_t reg) {
    bus.beginTransmission(addr);
    bus.write(reg);
    if (bus.endTransmission(false) != 0) return 0xFF;
    if (bus.requestFrom((int)addr, 1) != 1) return 0xFF;
    return bus.read();
}
static bool i2c_write_reg(TwoWire &bus, uint8_t addr, uint8_t reg, uint8_t val) {
    bus.beginTransmission(addr);
    bus.write(reg);
    bus.write(val);
    return bus.endTransmission() == 0;
}
static bool i2c_read_buf(TwoWire &bus, uint8_t addr, uint8_t reg, uint8_t *dst, size_t n) {
    bus.beginTransmission(addr);
    bus.write(reg);
    if (bus.endTransmission(false) != 0) return false;
    if (bus.requestFrom((int)addr, (int)n) != (int)n) return false;
    for (size_t i = 0; i < n; i++) dst[i] = bus.read();
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// MAX30101 init: multi-LED mode, only slot 1 = Green active
// ═════════════════════════════════════════════════════════════════════════════
static bool max_init(uint8_t green_pa) {
    // 1. Soft reset -- clean slate
    i2c_write_reg(Wire, MAX30101_ADDR, M_REG_MODE_CONFIG, M_MODE_RESET);
    uint32_t t0 = millis();
    while ((i2c_read_reg(Wire, MAX30101_ADDR, M_REG_MODE_CONFIG) & M_MODE_RESET) &&
           (millis() - t0 < 200)) {
        delay(2);
    }
    // 2. FIFO_CFG: SMP_AVE=8, rollover=1, almost-full=0
    i2c_write_reg(Wire, MAX30101_ADDR, M_REG_FIFO_CONFIG,
                  (uint8_t)(0x50 | (M_SMP_AVE_8 << 5)));
    // 3. SPO2_CFG: ADC=16384 nA, SR=100 Hz, PW=411 us
    uint8_t spo2 = (M_ADC_16384 << 5) | (M_SR_100 << 2) | M_PW_411;
    i2c_write_reg(Wire, MAX30101_ADDR, M_REG_SPO2_CONFIG, spo2);
    // 4. Green ONLY: slot1 = Green (LED#3). SLOT2..4 disabled.
    i2c_write_reg(Wire, MAX30101_ADDR, M_REG_MULTI_LED_1, 0x03);   // slot1=3, slot2=0
    i2c_write_reg(Wire, MAX30101_ADDR, M_REG_MULTI_LED_2, 0x00);   // slot3=0, slot4=0
    // 5. LED currents: Red/IR OFF, Green = requested
    i2c_write_reg(Wire, MAX30101_ADDR, M_REG_LED1_PA, 0x00);
    i2c_write_reg(Wire, MAX30101_ADDR, M_REG_LED2_PA, 0x00);
    i2c_write_reg(Wire, MAX30101_ADDR, M_REG_LED3_PA, green_pa);
    // 6. Enter MULTI-LED mode (also clears reset flag if still set)
    i2c_write_reg(Wire, MAX30101_ADDR, M_REG_MODE_CONFIG, M_MODE_MULTI_LED);
    // 7. Clear FIFO pointers
    i2c_write_reg(Wire, MAX30101_ADDR, M_REG_FIFO_WR_PTR, 0);
    i2c_write_reg(Wire, MAX30101_ADDR, M_REG_OVF_CTR,     0);
    i2c_write_reg(Wire, MAX30101_ADDR, M_REG_FIFO_RD_PTR, 0);
    // 8. Readback for verification.
    uint8_t rb_fifo = i2c_read_reg(Wire, MAX30101_ADDR, M_REG_FIFO_CONFIG);
    uint8_t rb_spo2 = i2c_read_reg(Wire, MAX30101_ADDR, M_REG_SPO2_CONFIG);
    uint8_t rb_mode = i2c_read_reg(Wire, MAX30101_ADDR, M_REG_MODE_CONFIG);
    uint8_t rb_led3 = i2c_read_reg(Wire, MAX30101_ADDR, M_REG_LED3_PA);
    Serial.printf("[MAX] init  MODE=0x%02X FIFO=0x%02X SPO2=0x%02X LED3=0x%02X (wrote green=0x%02X, %s)\n",
                  rb_mode, rb_fifo, rb_spo2, rb_led3, green_pa, PA_STEPS[pa_idx].label);
    return rb_mode == M_MODE_MULTI_LED;
}

// Reads N pending samples from FIFO. Each sample = 3 bytes (18-bit right-aligned).
// Returns the number of samples pulled (0..32).
static uint8_t max_read_fifo(uint32_t *out_buf, uint8_t max_samples) {
    uint8_t wr = i2c_read_reg(Wire, MAX30101_ADDR, M_REG_FIFO_WR_PTR);
    uint8_t rd = i2c_read_reg(Wire, MAX30101_ADDR, M_REG_FIFO_RD_PTR);
    uint8_t avail = (uint8_t)((wr - rd) & 0x1F);   // 32-deep circular
    if (avail == 0) return 0;
    if (avail > max_samples) avail = max_samples;
    uint8_t bytes[96];   // 32 samples * 3 bytes
    size_t n = (size_t)avail * 3;
    if (!i2c_read_buf(Wire, MAX30101_ADDR, M_REG_FIFO_DATA, bytes, n)) return 0;
    for (uint8_t i = 0; i < avail; i++) {
        uint32_t v = ((uint32_t)bytes[i*3+0] << 16) |
                     ((uint32_t)bytes[i*3+1] <<  8) |
                     ((uint32_t)bytes[i*3+2]);
        v &= 0x0003FFFF;   // 18-bit
        out_buf[i] = v;
    }
    return avail;
}

// ═════════════════════════════════════════════════════════════════════════════
// RGB / ship-mode (verbatim from sketch 8/9)
// ═════════════════════════════════════════════════════════════════════════════
static void rgb_off(void) { neopixelWrite(PIN_WS2812_DIN, 0, 0, 0); }
static void rgb_show_pa(uint8_t idx) {
    const pa_step_t &p = PA_STEPS[idx];
    neopixelWrite(PIN_WS2812_DIN, p.r, p.g, p.b);
}
static void ship_mode_countdown(void) {
    Serial.println("[BTN] DOUBLE -> ship mode (2 s red countdown, hands off)");
    for (uint32_t i = 0; i < 50; i++) {
        float s = 0.5f * (1.0f - cosf((float)i / 15.9155f * (float)M_PI));
        neopixelWrite(PIN_WS2812_DIN, (uint8_t)(s * WS_LEVEL), 0, 0);
        delay(40);
    }
    neopixelWrite(PIN_WS2812_DIN, WS_LEVEL, 0, 0);
}
static void enter_ship_mode(void) {
    if (!bq_ok) { Serial.println("[BTN] BQ dead -- no ship mode"); return; }
    ship_mode_countdown();
    if (digitalRead(PIN_BUTTON) == LOW) { rgb_off(); return; }
    uint8_t r07 = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_MISC_OP);
    uint8_t r07_new = r07 | BQ_BATFET_DIS | BQ_BATFET_RST_WVBUS;
    r07_new &= ~BQ_BATFET_DLY;
    r07_new &= ~BQ_BATFET_RST_EN;
    i2c_write_reg(Wire1, BQ25619_ADDR, BQ_REG_MISC_OP, r07_new);
    delay(50);
    uint8_t st = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_STATUS);
    if (st & BQ_STATUS_PG) { rgb_off(); Serial.println("USB present -- unplug"); }
    else                   { while (1) delay(100); }
}

// ═════════════════════════════════════════════════════════════════════════════
// Sample processing + stats
// ═════════════════════════════════════════════════════════════════════════════
static void reset_window(void) {
    win_sum = 0;
    win_sq_sum = 0;
    win_min = 0xFFFFFFFF;
    win_max = 0;
    win_count = 0;
    win_start_ms = millis();
}
static void reset_beat_detector(void) {
    baseline       = 0.0f;
    baseline_ready = false;
    baseline_start = millis();
    ac_peak        = 0.0f;
    ac_prev        = 0.0f;
    last_beat_ms   = 0;
    beat_count     = 0;
    last_int_ms    = 0.0f;
}
static void process_sample(uint32_t raw) {
    // Window stats
    win_sum    += raw;
    win_sq_sum += (int64_t)raw * (int64_t)raw;
    if (raw < win_min) win_min = raw;
    if (raw > win_max) win_max = raw;
    win_count++;

    // Baseline EMA (fast for first 3 s to converge, slow after)
    uint32_t now = millis();
    float x = (float)raw;
    if (baseline == 0.0f) baseline = x;
    uint32_t elapsed = now - baseline_start;
    float alpha = (elapsed < 3000) ? 0.10f : 0.02f;
    baseline = baseline * (1.0f - alpha) + x * alpha;
    if (elapsed >= 3000) baseline_ready = true;

    float ac     = x - baseline;
    float ac_abs = fabsf(ac);
    if (ac_abs > ac_peak) ac_peak = ac_abs;
    else                  ac_peak *= 0.995f;

    // Simple peak-count beat detector on AC. Threshold = 30% of running
    // AC peak, minimum floor 50 counts. Refractory 300 ms.
    if (baseline_ready) {
        float thresh = ac_peak * 0.30f;
        if (thresh < 50.0f) thresh = 50.0f;
        if (ac_prev > ac && ac_prev > thresh &&
            (now - last_beat_ms) > 300) {
            if (last_beat_ms > 0) last_int_ms = (float)(now - last_beat_ms);
            last_beat_ms = now;
            beat_count++;
        }
    }
    ac_prev = ac;

#if OUT_MODE == OUT_MODE_PLOT_RAW
    Serial.println(raw);
#elif OUT_MODE == OUT_MODE_PLOT_AC
    int32_t base = (int32_t)baseline;
    int32_t acc  = (int32_t)raw - base;
    if (acc >  AC_CLIP) acc =  AC_CLIP;
    if (acc < -AC_CLIP) acc = -AC_CLIP;
    Serial.printf("%lu %ld %ld\n",
                  (unsigned long)raw, (long)base, (long)acc);
#endif
}

static void print_monitor_line(void) {
    uint32_t now = millis();
    uint32_t win_dt = now - win_start_ms;
    float rate = (win_dt > 0) ? (1000.0f * (float)win_count / (float)win_dt) : 0.0f;
    if (win_count == 0) {
        Serial.printf("[MAX] PA=%s  no fresh samples (%.0f ms window)\n",
                      PA_STEPS[pa_idx].label, (float)win_dt);
        reset_window();
        return;
    }
    float mean = (float)win_sum / (float)win_count;
    float var  = (float)win_sq_sum / (float)win_count - mean * mean;
    if (var < 0.0f) var = 0.0f;
    float sd = sqrtf(var);
    uint32_t p2p = win_max - win_min;
    float bpm = (last_int_ms > 300.0f && last_int_ms < 2000.0f) ? 60000.0f / last_int_ms : 0.0f;
    Serial.printf("[MAX] PA=%s  n=%u @%.1fHz  min=%lu max=%lu p2p=%lu  mean=%.0f sd=%.0f  base=%.0f acpk=%.0f  beats=%u int=%.0f bpm=%.0f\n",
                  PA_STEPS[pa_idx].label,
                  (unsigned)win_count, rate,
                  (unsigned long)win_min, (unsigned long)win_max, (unsigned long)p2p,
                  mean, sd, baseline, ac_peak,
                  (unsigned)beat_count, last_int_ms, bpm);
    reset_window();
}

// ═════════════════════════════════════════════════════════════════════════════
// Button state machine
// ═════════════════════════════════════════════════════════════════════════════
static void on_single_click(void) {
    pa_idx = (pa_idx + 1) % 5;
    Serial.printf("\n[BTN] single -> PA %s\n", PA_STEPS[pa_idx].label);
    max_init(PA_STEPS[pa_idx].pa_val);
    rgb_show_pa(pa_idx);
    reset_window();
    reset_beat_detector();
}
static void on_double_click(void) { enter_ship_mode(); }

typedef enum { BTN_IDLE, BTN_PRESSED, BTN_WAIT_DBL, BTN_PRESSED_2 } btn_state_t;
static void handle_button(void) {
    static btn_state_t state    = BTN_IDLE;
    static uint32_t    last_change = 0;
    static uint32_t    release_ms  = 0;
    static bool        prev_low    = false;
    uint32_t now = millis();
    bool low = (digitalRead(PIN_BUTTON) == LOW);
    if (low != prev_low && (now - last_change) >= BTN_DEBOUNCE_MS) {
        last_change = now;
        prev_low = low;
        if (low) {
            state = (state == BTN_WAIT_DBL) ? BTN_PRESSED_2 : BTN_PRESSED;
        } else {
            if (state == BTN_PRESSED) {
                release_ms = now; state = BTN_WAIT_DBL;
            } else if (state == BTN_PRESSED_2) {
                state = BTN_IDLE; on_double_click();
            }
        }
    }
    if (state == BTN_WAIT_DBL && (now - release_ms) > BTN_DOUBLE_GAP_MS) {
        state = BTN_IDLE; on_single_click();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Setup + loop
// ═════════════════════════════════════════════════════════════════════════════
void setup(void) {
    Serial.begin(115200);
    for (int i = 0; i < 20 && !Serial; i++) delay(50);
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    Wire.begin (PIN_SDA_BUS1, PIN_SCL_BUS1, 400000);
    Wire1.begin(PIN_SDA_BUS2, PIN_SCL_BUS2, 400000);

    Serial.println();
    Serial.println("=== 10_test_max30101 boot ===");
    if (i2c_ping(Wire, MAX30101_ADDR)) {
        uint8_t part = i2c_read_reg(Wire, MAX30101_ADDR, M_REG_PART_ID);
        if (part == M_PART_ID_VAL) {
            max_ok = true;
            Serial.printf("  MAX30101  0x57 : ACK  PART_ID=0x%02X\n", part);
        } else {
            Serial.printf("  MAX30101  0x57 : ACK  PART_ID=0x%02X (unexpected!)\n", part);
        }
    } else {
        Serial.println("  MAX30101  0x57 : NO ACK");
    }
    if (i2c_ping(Wire1, BQ25619_ADDR)) { bq_ok = true; Serial.println("  BQ25619   0x6A : ACK (ship mode)"); }

    if (!max_ok) {
        while (1) {
            neopixelWrite(PIN_WS2812_DIN, WS_LEVEL, 0, 0); delay(200);
            neopixelWrite(PIN_WS2812_DIN, 0, 0, 0);        delay(200);
        }
    }

    max_init(PA_STEPS[pa_idx].pa_val);
    rgb_show_pa(pa_idx);
    reset_window();
    reset_beat_detector();

    Serial.println();
    Serial.println("controls: single-click = cycle green PA, double-click = ship mode");
    Serial.printf ("output  : %s\n",
                   OUT_MODE == OUT_MODE_MONITOR   ? "Serial MONITOR (2 Hz stats)" :
                   OUT_MODE == OUT_MODE_PLOT_RAW  ? "Serial PLOTTER (raw only)" :
                                                    "Serial PLOTTER (raw + baseline + ac)");
    Serial.println();
}

void loop(void) {
    handle_button();

    uint32_t samples[32];
    uint8_t n = max_read_fifo(samples, 32);
    for (uint8_t i = 0; i < n; i++) process_sample(samples[i]);

#if OUT_MODE == OUT_MODE_MONITOR
    static uint32_t last_print = 0;
    uint32_t now = millis();
    if ((now - last_print) >= 500) {
        last_print = now;
        print_monitor_line();
    }
#endif

    // Pace polling faster than 12.5 Hz effective FIFO update.
    delay(20);
}
