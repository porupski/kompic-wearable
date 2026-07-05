/*
 * 6_sd_logger_mk1  --  SD data-logger bring-up, Kompic Mk I (iv7.1)
 *
 * First real use of the SD card after Stage 4 verified the bus works end-to-end.
 * Logs the LSM6DSV16X IMU (bus 1, 0x6B) at 100 Hz to a CSV file on the SD
 * card at 4 MHz SDMMC 1-bit. Nothing else runs -- keep the pipeline simple
 * for the first bring-up. Once this proves out we widen it to more sensors.
 *
 * CSV row format (one file per boot):
 *   ms,ax,ay,az,gx,gy,gz,tc
 *
 * Values are raw 16-bit LSM6DSV16X counts. Convert to physical units host-side
 * using the datasheet sensitivities for the current FS: FS_XL = +/-2 g,
 * FS_G = +/-250 dps, temp = 256 LSB/degC + 25 degC offset (Table 4 / sec. 6.1.2).
 *
 * On boot the sketch:
 *   1. Brings up I2C bus 1 (LSM) and bus 2 (BQ25619 for ship mode).
 *   2. Configures LSM6DSV16X at 240 Hz XL + G using the same register writes
 *      the Stage-3 smoke test used successfully.
 *   3. Mounts SD at 4 MHz SDMMC 1-bit.
 *   4. Prints a directory summary of /sdcard/ and the FIRST 3 lines of the
 *      most recent LSM_NNNN.csv (proves the previous session's write path
 *      actually persisted content; not just an empty file).
 *   5. Persists a boot counter in NVS ("logger" namespace, key "boot_seq")
 *      so each session gets its own filename.
 *   6. Opens /LSM_<seq>.csv, writes the header, enters the sample loop.
 *
 * The sample loop reads 14 bytes (temp + G + XL) from LSM_REG_OUT_TEMP every
 * LSM_SAMPLE_MS milliseconds and writes one CSV line. Flushes every
 * FLUSH_EVERY samples (default = 100 = 1 second) so a power loss loses at
 * most the last second.
 *
 * MANDATORY: double-click on the button (GPIO16 == BQ25619 /QON) enters BQ
 * ship mode. Battery is permanently attached on iv7.1; ship mode is the only
 * way to turn off between sessions. Handler mirrors 4_demo_mk1.ino -- action
 * fires on the RELEASE of the second click, and the log file is flushed +
 * closed before the BATFET write so the FAT metadata makes it to the card.
 */

#include <Arduino.h>
#include <Wire.h>
#include <SD_MMC.h>
#include <FS.h>
#include <Preferences.h>
#include "driver/gpio.h"

// ── Pins ─────────────────────────────────────────────────────────────────────
#define PIN_SD_CLK        38
#define PIN_SD_CMD        39
#define PIN_SD_D0         40
#define PIN_BUTTON        16
#define PIN_SDA_BUS1       1
#define PIN_SCL_BUS1       2
#define PIN_SDA_BUS2       4
#define PIN_SCL_BUS2       5

// ── I2C addresses ────────────────────────────────────────────────────────────
#define LSM6DSV_ADDR      0x6B
#define BQ25619_ADDR      0x6A

// ── LSM6DSV16X registers (Stage-3 smoke known-working config) ───────────────
#define LSM_REG_WHO         0x0F
#define LSM_WHO_VAL         0x70
#define LSM_REG_CTRL1       0x10   // XL ODR + FS
#define LSM_REG_CTRL2       0x11   // G  ODR + FS
#define LSM_REG_CTRL3       0x12   // reset + control
#define LSM_REG_OUT_TEMP    0x20   // temp_l temp_h gx_l gx_h ... az_l az_h (14 B)

// ── BQ25619 ship-mode registers ─────────────────────────────────────────────
#define BQ_REG_MISC_OP       0x07
#define BQ_REG_STATUS        0x08
#define BQ_STATUS_VBUS_MASK  0xE0
#define BQ_BATFET_DIS        (1 << 5)
#define BQ_BATFET_RST_WVBUS  (1 << 4)
#define BQ_BATFET_DLY        (1 << 3)
#define BQ_BATFET_RST_EN     (1 << 2)

// ── Logger config ────────────────────────────────────────────────────────────
#define LSM_SAMPLE_MS       10       // 100 Hz sample rate → ~4 KB/s CSV
#define FLUSH_EVERY         100      // flush every ~1 s of samples
#define SD_CLOCK_KHZ        4000     // 4 MHz -- plenty for ~5 KB/s
#define LOG_FILE_PREFIX     "LSM_"
#define LOG_FILE_EXT        ".csv"

// ── Button state machine ─────────────────────────────────────────────────────
#define BTN_DEBOUNCE_MS      30
#define BTN_DOUBLE_GAP_MS    350

typedef enum {
    BTN_IDLE,
    BTN_PRESSED,
    BTN_WAIT_DBL,
    BTN_PRESSED_2,
} btn_state_t;

// ── Globals ──────────────────────────────────────────────────────────────────
static Preferences   prefs;
static File          log_file;
static uint32_t      boot_seq       = 0;
static uint32_t      sample_count   = 0;
static bool          lsm_ok         = false;
static bool          sd_ok          = false;
static char          log_path[32]   = "";

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
static bool i2c_read_buf(TwoWire &bus, uint8_t addr, uint8_t reg,
                         uint8_t *out, size_t n) {
    bus.beginTransmission(addr);
    bus.write(reg);
    if (bus.endTransmission(false) != 0) return false;
    if (bus.requestFrom((int)addr, (int)n) != (int)n) return false;
    for (size_t i = 0; i < n; i++) out[i] = bus.read();
    return true;
}

// ── LSM6DSV16X init (matches Stage-3 smoke, 240 Hz both XL and G) ───────────
static bool lsm_init(void) {
    if (i2c_read_reg(Wire, LSM6DSV_ADDR, LSM_REG_WHO) != LSM_WHO_VAL) {
        Serial.println("[LSM ] WHO_AM_I mismatch -- chip not responding");
        return false;
    }
    // Soft reset (CTRL3 SW_RESET bit), wait, then config.
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL3, 0x01);
    delay(20);
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL3, 0x44);   // BDU + IF_INC
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL1, 0x07);   // XL ODR ~240 Hz
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL2, 0x07);   // G  ODR ~240 Hz
    Serial.println("[LSM ] init OK  (XL+G @ ~240 Hz, FS defaults)");
    return true;
}

// ── SD boot-time review ─────────────────────────────────────────────────────
// Lists /sdcard/, sums file sizes, prints first 3 lines of the most recent
// LSM_NNNN.csv (lexical max, which is also numerical max because seq is
// zero-padded). Proves the previous session actually wrote data through the
// FAT layer, not just created an empty file.
static void sd_review(void) {
    Serial.println("[SD  ] listing /sdcard/");
    File root = SD_MMC.open("/");
    if (!root || !root.isDirectory()) {
        Serial.println("       failed to open root directory");
        return;
    }
    uint32_t file_count = 0;
    uint64_t total_size = 0;
    char latest_name[32] = "";

    File f = root.openNextFile();
    while (f) {
        if (!f.isDirectory()) {
            const char *n = f.name();
            size_t sz = f.size();
            Serial.printf("       %-24s  %10u bytes\n", n, (unsigned)sz);
            file_count++;
            total_size += sz;
            // "LSM_NNNN.csv" -- lex compare finds the largest sequence number.
            if (strncmp(n, LOG_FILE_PREFIX, strlen(LOG_FILE_PREFIX)) == 0 &&
                strcmp(n, latest_name) > 0) {
                strncpy(latest_name, n, sizeof(latest_name) - 1);
                latest_name[sizeof(latest_name) - 1] = '\0';
            }
        }
        File next = root.openNextFile();
        f.close();
        f = next;
    }
    root.close();
    Serial.printf("       total %u files, %llu bytes\n",
                  (unsigned)file_count, (unsigned long long)total_size);

    if (latest_name[0] == '\0') {
        Serial.println("[SD  ] no prior LSM_*.csv files -- fresh card.");
        return;
    }
    char path[48];
    snprintf(path, sizeof(path), "/%s", latest_name);
    Serial.printf("[SD  ] head -3 %s:\n", path);
    File h = SD_MMC.open(path, FILE_READ);
    if (!h) {
        Serial.println("       (open failed)");
        return;
    }
    for (int i = 0; i < 3 && h.available(); i++) {
        String line = h.readStringUntil('\n');
        Serial.printf("       %s\n", line.c_str());
    }
    h.close();
}

// ── New-file opening ────────────────────────────────────────────────────────
static bool open_new_log_file(void) {
    prefs.begin("logger", false);
    boot_seq = prefs.getUInt("boot_seq", 0) + 1;
    prefs.putUInt("boot_seq", boot_seq);
    prefs.end();

    snprintf(log_path, sizeof(log_path),
             "/%s%04lu%s", LOG_FILE_PREFIX,
             (unsigned long)boot_seq, LOG_FILE_EXT);
    log_file = SD_MMC.open(log_path, FILE_WRITE);
    if (!log_file) {
        Serial.printf("[SD  ] failed to open %s for write\n", log_path);
        return false;
    }
    log_file.println("ms,ax,ay,az,gx,gy,gz,tc");
    log_file.flush();
    Serial.printf("[SD  ] opened %s (boot_seq=%lu)\n",
                  log_path, (unsigned long)boot_seq);
    return true;
}

// ── Ship mode ───────────────────────────────────────────────────────────────
static void close_log_file(void) {
    if (log_file) {
        log_file.flush();
        log_file.close();
        Serial.printf("[SD  ] closed %s (%lu samples)\n",
                      log_path, (unsigned long)sample_count);
    }
}

static void enter_ship_mode(void) {
    Serial.println("[BTN ] DOUBLE -> ship mode requested");
    close_log_file();
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
        Serial.println("       USB present -- BATFET off but chip stays awake.");
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
        state = BTN_IDLE;   // single click -- no action in this sketch
    }
}

// ── Setup / loop ────────────────────────────────────────────────────────────
void setup(void) {
    Serial.begin(115200);
    delay(200);

    pinMode(PIN_BUTTON, INPUT_PULLUP);
    Wire .begin(PIN_SDA_BUS1, PIN_SCL_BUS1, 400000);
    Wire1.begin(PIN_SDA_BUS2, PIN_SCL_BUS2, 100000);
    delay(50);

    Serial.println();
    Serial.println("=== 6_sd_logger_mk1 -- Kompic Mk I (iv7.1) ===");
    Serial.println("Double-click button to enter BQ ship mode.");

    lsm_ok = lsm_init();

    // SD_MMC 1-bit at 4 MHz -- verified in Stage 4 build report.
    SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
    sd_ok = SD_MMC.begin("/sdcard", /*mode1bit*/ true,
                         /*format_if_mount_failed*/ false,
                         SD_CLOCK_KHZ);
    if (!sd_ok) {
        Serial.println("[SD  ] mount FAILED -- check card is FAT16/FAT32 msdos");
        Serial.println("       (arduino-esp32 SD_MMC doesn't do exFAT by default)");
        return;
    }
    Serial.printf("[SD  ] mounted /sdcard  type=%d  size=%llu MB  clk=%d kHz\n",
                  (int)SD_MMC.cardType(),
                  (unsigned long long)(SD_MMC.cardSize() / (1024ULL * 1024ULL)),
                  SD_CLOCK_KHZ);

    sd_review();

    if (!lsm_ok) {
        Serial.println("[LSM ] not initialized -- log will contain zeros; fix IMU wiring first");
        return;
    }
    if (!open_new_log_file()) return;

    Serial.printf("[LOG ] starting sample loop, %d Hz -> %s\n",
                  1000 / LSM_SAMPLE_MS, log_path);
}

void loop(void) {
    handle_button();
    if (!sd_ok || !lsm_ok || !log_file) {
        delay(50);
        return;
    }

    static uint32_t last_sample = 0;
    uint32_t now = millis();
    if (now - last_sample < LSM_SAMPLE_MS) return;
    last_sample = now;

    uint8_t buf[14];
    if (!i2c_read_buf(Wire, LSM6DSV_ADDR, LSM_REG_OUT_TEMP, buf, 14)) {
        return;  // dropped sample; next tick retries
    }
    int16_t tc = (int16_t)((buf[1]  << 8) | buf[0]);
    int16_t gx = (int16_t)((buf[3]  << 8) | buf[2]);
    int16_t gy = (int16_t)((buf[5]  << 8) | buf[4]);
    int16_t gz = (int16_t)((buf[7]  << 8) | buf[6]);
    int16_t ax = (int16_t)((buf[9]  << 8) | buf[8]);
    int16_t ay = (int16_t)((buf[11] << 8) | buf[10]);
    int16_t az = (int16_t)((buf[13] << 8) | buf[12]);

    log_file.printf("%lu,%d,%d,%d,%d,%d,%d,%d\n",
                    (unsigned long)now, ax, ay, az, gx, gy, gz, tc);
    sample_count++;
    if ((sample_count % FLUSH_EVERY) == 0) {
        log_file.flush();
        // Cheap heartbeat every ~1 s so serial confirms samples are flowing.
        Serial.printf("[LOG ] %lu samples  last: ax=%d ay=%d az=%d  gx=%d gy=%d gz=%d  tc=%d\n",
                      (unsigned long)sample_count, ax, ay, az, gx, gy, gz, tc);
    }
}
