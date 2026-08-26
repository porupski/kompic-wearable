/*
 * 15_amoled_touch_test — CO5300 2.06" AMOLED + CST9217 touch bring-up
 * Target: Kompic Mk1 iv7.1 (ESP32-S3-WROOM-1U-N16R8)
 *
 * Purpose: first flash after the AMOLED FPC receptacle was hand-soldered onto
 * the iv7.1 prototype. Verifies that the panel powers up, QSPI init reaches
 * DISPON, and the touch controller ACKs. Same driver logic that ran on the
 * standalone breakout — pins re-aligned to the iv7.1 master pinout v20.
 *
 * ── Pin changes vs the old breakout sketch ──────────────────────────
 *   TOUCH_RST : 5  -> 44   (GPIO5 is now SCL_bus2 for the BQ25619)
 *   QSPI_TE   : 21 -> 45   (unused in this sketch either way)
 *   Everything else (QSPI 9..14, CS=10, DISP_RST=3, TOUCH_SDA=1/SCL=2/INT=6)
 *   matches — those pins were already correct.
 *
 * ── Arduino IDE settings ────────────────────────────────────────────
 *   Board:            ESP32S3 Dev Module
 *   Flash Size:       16MB (128Mb)
 *   Flash Mode:       QIO 80MHz
 *   PSRAM:            OPI PSRAM        (N16R8 module)
 *   USB CDC On Boot:  Enabled
 *   Upload Mode:      UART0
 *   (This sketch does not touch TinyUSB / USBMSC, so USB CDC on boot is safe
 *   here — no ARDUINO_USB_MODE guard needed.)
 *
 * ── Ship-mode entry (iv7.1 requirement) ─────────────────────────────
 *   Battery is permanently attached to the prototype. Double-click the user
 *   button (GPIO16 / BQ /QON) to drop BATFET via the BQ25619 (bus 2 @ 0x6A).
 *   Long-press (>1 s) on the button wakes the board again — handled by the BQ
 *   internal QON logic, no firmware needed to wake.
 *
 * ── Notes carried from the original bench sketch ────────────────────
 *   - COLMOD 0x77 (RGB888) — RGB565 QIO lane mapping is broken on CO5300.
 *     Pixels are packed R,G,B (3 bytes) and expanded from RGB565 input.
 *   - Commands go out via instruction 0x02 (1-wire), pixels via 0x32 (QIO).
 *   - GPIO3 (DISP_RST) is a JTAG-source strap; on iv7.1 the panel holds RST
 *     defined and firmware drives it as output. Confirmed cold-boot-clean
 *     per master pinout v20.
 *   - TE is not read here — production wiring is GPIO45, left unused.
 */

#include <Arduino.h>
#include <Wire.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>

// ════════════════════════════════════════════════════════════════════
//  PINS — iv7.1 master pinout v20
// ════════════════════════════════════════════════════════════════════

#define QSPI_CS     10
#define QSPI_CLK    12
#define QSPI_D0     11   // MOSI  (SIO0)
#define QSPI_D1     13   // MISO  (SIO1)
#define QSPI_D2      9   // WP    (SIO2)
#define QSPI_D3     14   // HD    (SIO3)
#define DISP_RST     3   // JTAG-src strap; safe on iv7.1 per master pinout
// #define QSPI_TE   45   // TE routed on iv7.1 but not used in this sketch

#define TOUCH_SDA    1   // bus 1 (shared with LSM, TMP117, MAX30101, RTC, ...)
#define TOUCH_SCL    2
#define TOUCH_INT    6
#define TOUCH_RST   44   // was 5 on the breakout; 5 is now SCL_bus2
#define TOUCH_ADDR  0x5A

#define BUTTON_PIN  16   // BQ_BUTTON, doubles as BQ /QON
#define SDA_BUS2     4   // BQ25619 lives here
#define SCL_BUS2     5
#define BQ_ADDR     0x6A

// ── BQ25619 ship-mode registers (same as 4_demo_mk1 / 12_lsm_full) ──
#define BQ_REG_MISC_OP        0x07
#define BQ_REG_STATUS         0x08
#define BQ_STATUS_PG          (1 << 2)
#define BQ_BATFET_DIS         (1 << 5)
#define BQ_BATFET_RST_WVBUS   (1 << 4)
#define BQ_BATFET_DLY         (1 << 3)
#define BQ_BATFET_RST_EN      (1 << 2)

// ── Button state machine timings ──
#define BTN_DEBOUNCE_MS      30
#define BTN_DOUBLE_GAP_MS   350

// ════════════════════════════════════════════════════════════════════
//  DISPLAY CONSTANTS
// ════════════════════════════════════════════════════════════════════

#define LCD_WIDTH    410
#define LCD_HEIGHT   502
#define COL_OFFSET    22
#define MAX_PIXELS  1024

#define BLACK    0x0000
#define WHITE    0xFFFF
#define RED      0xF800
#define GREEN    0x07E0
#define BLUE     0x001F
#define YELLOW   0xFFE0
#define CYAN     0x07FF
#define MAGENTA  0xF81F
#define ORANGE   0xFD20

struct TouchPoint { bool pressed; uint16_t x, y; };
struct Stamp      { uint16_t x, y; uint32_t born; bool active; };

// ════════════════════════════════════════════════════════════════════
//  SPI STATE
// ════════════════════════════════════════════════════════════════════

static spi_device_handle_t   spi_dev;
static spi_transaction_ext_t spi_ext;
static spi_transaction_t    *g_spi_t;
static uint8_t              *dma_buf;

static bool bq_ok = false;

static inline void CS_HIGH() { digitalWrite(QSPI_CS, HIGH); }
static inline void CS_LOW()  { digitalWrite(QSPI_CS, LOW);  }

// ════════════════════════════════════════════════════════════════════
//  SPI INIT
// ════════════════════════════════════════════════════════════════════

static void spi_init() {
  pinMode(QSPI_CS, OUTPUT);
  CS_HIGH();

  spi_bus_config_t bus = {};
  bus.mosi_io_num     = QSPI_D0;
  bus.miso_io_num     = QSPI_D1;
  bus.sclk_io_num     = QSPI_CLK;
  bus.quadwp_io_num   = QSPI_D3;
  bus.quadhd_io_num   = QSPI_D2;
  bus.data4_io_num    = -1;
  bus.data5_io_num    = -1;
  bus.data6_io_num    = -1;
  bus.data7_io_num    = -1;
  bus.max_transfer_sz = MAX_PIXELS * 3 * 8 + 8;
  bus.flags           = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS;
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

  spi_device_interface_config_t dev = {};
  dev.command_bits   = 8;
  dev.address_bits   = 24;
  dev.dummy_bits     = 0;
  dev.mode           = 0;
  dev.clock_speed_hz = 40000000;
  dev.spics_io_num   = -1;
  dev.queue_size     = 1;
  dev.flags          = SPI_DEVICE_HALFDUPLEX;
  ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &spi_dev));
  spi_device_acquire_bus(spi_dev, portMAX_DELAY);

  memset(&spi_ext, 0, sizeof(spi_ext));
  g_spi_t = (spi_transaction_t *)&spi_ext;

  dma_buf = (uint8_t *)heap_caps_aligned_alloc(16, MAX_PIXELS * 3, MALLOC_CAP_DMA);
  if (!dma_buf) { Serial.println("DMA alloc FAILED"); while(1); }
}

// ════════════════════════════════════════════════════════════════════
//  LOW-LEVEL SPI PRIMITIVES
// ════════════════════════════════════════════════════════════════════

static void cmd(uint8_t c) {
  CS_LOW();
  spi_ext.base.flags     = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
  spi_ext.base.cmd       = 0x02;
  spi_ext.base.addr      = (uint32_t)c << 8;
  spi_ext.base.tx_buffer = nullptr;
  spi_ext.base.length    = 0;
  spi_device_polling_start(spi_dev, g_spi_t, portMAX_DELAY);
  spi_device_polling_end(spi_dev, portMAX_DELAY);
  CS_HIGH();
}

static void cmd8(uint8_t c, uint8_t d) {
  CS_LOW();
  spi_ext.base.flags       = SPI_TRANS_USE_TXDATA | SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
  spi_ext.base.cmd         = 0x02;
  spi_ext.base.addr        = (uint32_t)c << 8;
  spi_ext.base.tx_data[0]  = d;
  spi_ext.base.length      = 8;
  spi_device_polling_start(spi_dev, g_spi_t, portMAX_DELAY);
  spi_device_polling_end(spi_dev, portMAX_DELAY);
  CS_HIGH();
}

static void cmd16x2(uint8_t c, uint16_t a, uint16_t b) {
  CS_LOW();
  spi_ext.base.flags       = SPI_TRANS_USE_TXDATA | SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
  spi_ext.base.cmd         = 0x02;
  spi_ext.base.addr        = (uint32_t)c << 8;
  spi_ext.base.tx_data[0]  = a >> 8;
  spi_ext.base.tx_data[1]  = a & 0xFF;
  spi_ext.base.tx_data[2]  = b >> 8;
  spi_ext.base.tx_data[3]  = b & 0xFF;
  spi_ext.base.length      = 32;
  spi_device_polling_start(spi_dev, g_spi_t, portMAX_DELAY);
  spi_device_polling_end(spi_dev, portMAX_DELAY);
  CS_HIGH();
}

// ════════════════════════════════════════════════════════════════════
//  PIXEL OUTPUT — RGB565 input, expanded to RGB888 (COLMOD 0x77)
// ════════════════════════════════════════════════════════════════════

static void set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  cmd16x2(0x2A, x + COL_OFFSET, x + COL_OFFSET + w - 1);
  cmd16x2(0x2B, y, y + h - 1);
  cmd(0x2C);  // RAMWR
}

static void fill_pixels(uint16_t color, uint32_t count) {
  uint8_t r = (color >> 11) << 3;
  uint8_t g = ((color >> 5) & 0x3F) << 2;
  uint8_t b = (color & 0x1F) << 3;

  uint32_t prefill = count < MAX_PIXELS ? count : MAX_PIXELS;
  for (uint32_t i = 0; i < prefill; i++) {
    dma_buf[i*3]   = r;
    dma_buf[i*3+1] = g;
    dma_buf[i*3+2] = b;
  }

  bool first = true;
  CS_LOW();
  while (count > 0) {
    uint32_t chunk = count < MAX_PIXELS ? count : MAX_PIXELS;

    if (first) {
      spi_ext.base.flags = SPI_TRANS_MODE_QIO;
      spi_ext.base.cmd   = 0x32;
      spi_ext.base.addr  = 0x003C00;
      first = false;
    } else {
      spi_ext.base.flags        = SPI_TRANS_MODE_QIO
                                | SPI_TRANS_VARIABLE_CMD
                                | SPI_TRANS_VARIABLE_ADDR
                                | SPI_TRANS_VARIABLE_DUMMY;
      spi_ext.command_bits  = 0;
      spi_ext.address_bits  = 0;
      spi_ext.dummy_bits    = 0;
    }
    spi_ext.base.tx_buffer = dma_buf;
    spi_ext.base.length    = chunk * 24;
    spi_device_polling_start(spi_dev, g_spi_t, portMAX_DELAY);
    spi_device_polling_end(spi_dev, portMAX_DELAY);
    count -= chunk;
  }
  CS_HIGH();
}

// ════════════════════════════════════════════════════════════════════
//  DRAWING PRIMITIVES
// ════════════════════════════════════════════════════════════════════

static void fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
  set_window(x, y, w, h);
  fill_pixels(color, (uint32_t)w * h);
}

static void fill_screen(uint16_t color) {
  fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, color);
}

// ════════════════════════════════════════════════════════════════════
//  DISPLAY INIT
// ════════════════════════════════════════════════════════════════════

static void display_init() {
  pinMode(DISP_RST, OUTPUT);
  digitalWrite(DISP_RST, HIGH); delay(50);
  digitalWrite(DISP_RST, LOW);  delay(200);
  digitalWrite(DISP_RST, HIGH); delay(300);

  cmd(0x11);           delay(120);  // SLPOUT
  cmd8(0xFE, 0x00);                 // CMD page 0
  cmd8(0xC4, 0x80);                 // SPI write RAM enable
  cmd8(0x3A, 0x77);                 // COLMOD: 24-bit RGB888
  cmd8(0x36, 0x00);                 // MADCTL: normal orientation, RGB
  cmd8(0x53, 0x20);                 // WRCTRLD: brightness control on
  cmd8(0x63, 0xFF);                 // HBM brightness max
  cmd(0x29);                        // DISPON
  cmd8(0x51, 0xD0);                 // brightness ~80%
  cmd8(0x58, 0x00);                 // sunlight enhancement off
  cmd(0x20);                        // INVOFF
  delay(20);
}

// ════════════════════════════════════════════════════════════════════
//  TOUCH — CST9217
// ════════════════════════════════════════════════════════════════════

static bool touch_read_reg(uint16_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(highByte(reg));
  Wire.write(lowByte(reg));
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)TOUCH_ADDR, len) < len) return false;
  for (int i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

static void touch_ack() {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0xD0); Wire.write(0x00); Wire.write(0xAB);
  Wire.endTransmission();
}

static TouchPoint touch_read() {
  TouchPoint tp = {false, 0, 0};
  uint8_t buf[12];
  if (!touch_read_reg(0xD000, buf, 12)) return tp;
  touch_ack();
  if (buf[0] == 0xAB || buf[6] != 0xAB) return tp;
  if ((buf[5] & 0x7F) == 0 || (buf[5] & 0x7F) > 2) return tp;
  if ((buf[0] & 0x0F) != 0x06) return tp;
  tp.x       = ((uint16_t)buf[1] << 4) | (buf[3] >> 4);
  tp.y       = ((uint16_t)buf[2] << 4) | (buf[3] & 0x0F);
  tp.pressed = true;
  return tp;
}

static void touch_init() {
  pinMode(TOUCH_RST, OUTPUT);
  pinMode(TOUCH_INT, INPUT_PULLUP);
  digitalWrite(TOUCH_RST, LOW);  delay(20);
  digitalWrite(TOUCH_RST, HIGH); delay(200);
  // Wire is started in setup() before this runs; touch shares bus 1 with the
  // other sensors on the iv7.1 board.
}

// ════════════════════════════════════════════════════════════════════
//  BQ25619 SHIP MODE (iv7.1 requirement — battery permanently attached)
// ════════════════════════════════════════════════════════════════════

static bool i2c_ping(TwoWire &bus, uint8_t addr) {
  bus.beginTransmission(addr);
  return bus.endTransmission() == 0;
}

static uint8_t i2c_read_reg(TwoWire &bus, uint8_t addr, uint8_t reg) {
  bus.beginTransmission(addr);
  bus.write(reg);
  if (bus.endTransmission(false) != 0) return 0xFF;
  if (bus.requestFrom(addr, (uint8_t)1) != 1) return 0xFF;
  return bus.read();
}

static void i2c_write_reg(TwoWire &bus, uint8_t addr, uint8_t reg, uint8_t val) {
  bus.beginTransmission(addr);
  bus.write(reg);
  bus.write(val);
  bus.endTransmission();
}

static void enter_ship_mode() {
  if (!bq_ok) {
    Serial.println("[BTN ] double-click -> ship mode, but BQ not alive; aborting");
    return;
  }
  Serial.println("[BTN ] double-click -> ship mode (BQ25619 BATFET off)");
  uint8_t r07 = i2c_read_reg(Wire1, BQ_ADDR, BQ_REG_MISC_OP);
  uint8_t r07_new = r07 | BQ_BATFET_DIS | BQ_BATFET_RST_WVBUS;
  r07_new &= ~BQ_BATFET_DLY;
  r07_new &= ~BQ_BATFET_RST_EN;
  Serial.printf("      REG07 0x%02X -> 0x%02X\n", r07, r07_new);
  Serial.flush();
  i2c_write_reg(Wire1, BQ_ADDR, BQ_REG_MISC_OP, r07_new);
  delay(50);
  uint8_t st = i2c_read_reg(Wire1, BQ_ADDR, BQ_REG_STATUS);
  if (st & BQ_STATUS_PG) {
    Serial.println("      USB present -- BATFET disabled; ship mode fires on unplug");
  } else {
    Serial.println("      BATFET off -- power gone");
    Serial.flush();
    while (1) delay(100);
  }
}

// ── Button state machine: detects double-click, fires ship mode ──
typedef enum {
  BTN_IDLE, BTN_PRESSED, BTN_WAIT_DBL, BTN_PRESSED_2
} BtnState;

static void button_poll() {
  static BtnState state    = BTN_IDLE;
  static uint32_t press_ms = 0;
  static uint32_t rel_ms   = 0;
  bool low = (digitalRead(BUTTON_PIN) == LOW);
  uint32_t now = millis();

  switch (state) {
    case BTN_IDLE:
      if (low) { press_ms = now; state = BTN_PRESSED; }
      break;
    case BTN_PRESSED:
      if (!low && (now - press_ms) >= BTN_DEBOUNCE_MS) {
        rel_ms = now; state = BTN_WAIT_DBL;
      }
      break;
    case BTN_WAIT_DBL:
      if (low) { press_ms = now; state = BTN_PRESSED_2; }
      else if ((now - rel_ms) > BTN_DOUBLE_GAP_MS) {
        Serial.println("[BTN ] single (ignored in test sketch)");
        state = BTN_IDLE;
      }
      break;
    case BTN_PRESSED_2:
      if (!low && (now - press_ms) >= BTN_DEBOUNCE_MS) {
        state = BTN_IDLE;
        enter_ship_mode();
      }
      break;
  }
}

// ════════════════════════════════════════════════════════════════════
//  COLOR TEST SCREEN
// ════════════════════════════════════════════════════════════════════

static void draw_color_test() {
  fill_screen(BLACK);

  fill_rect(0,            0,             LCD_WIDTH, 2,          WHITE);
  fill_rect(0,            LCD_HEIGHT-2,  LCD_WIDTH, 2,          WHITE);
  fill_rect(0,            0,             2,         LCD_HEIGHT,  WHITE);
  fill_rect(LCD_WIDTH-2,  0,             2,         LCD_HEIGHT,  WHITE);

  int bw = 120, bh = 120, gap = 20;
  int gx = (LCD_WIDTH - 2*bw - gap) / 2;
  int gy = 40;
  fill_rect(gx,          gy,          bw, bh, RED);
  fill_rect(gx+bw+gap,   gy,          bw, bh, GREEN);
  fill_rect(gx,          gy+bh+gap,   bw, bh, BLUE);
  fill_rect(gx+bw+gap,   gy+bh+gap,   bw, bh, WHITE);

  int sy = gy + 2*(bh+gap);
  int sw = 80, sh = 80;
  int sx = (LCD_WIDTH - 3*sw - 2*gap) / 2;
  fill_rect(sx,           sy, sw, sh, YELLOW);
  fill_rect(sx+sw+gap,    sy, sw, sh, CYAN);
  fill_rect(sx+2*(sw+gap),sy, sw, sh, MAGENTA);
}

// ════════════════════════════════════════════════════════════════════
//  TOUCH STAMP SYSTEM
// ════════════════════════════════════════════════════════════════════

#define STAMP_SIZE  20
#define STAMP_LIFE  2000
#define MAX_STAMPS  16

static Stamp stamps[MAX_STAMPS];

static void stamp_add(uint16_t x, uint16_t y) {
  for (int i = 0; i < MAX_STAMPS; i++) {
    if (!stamps[i].active) {
      stamps[i] = {x, y, (uint32_t)millis(), true};
      return;
    }
  }
}

static void stamps_update() {
  uint32_t now = millis();
  for (int i = 0; i < MAX_STAMPS; i++) {
    if (stamps[i].active && now - stamps[i].born > STAMP_LIFE) {
      fill_rect(stamps[i].x, stamps[i].y, STAMP_SIZE, STAMP_SIZE, BLACK);
      stamps[i].active = false;
    }
  }
}

// ════════════════════════════════════════════════════════════════════
//  MAIN
// ════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== 15_amoled_touch_test (iv7.1) ===");

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Bus 1: touch + everything else on the sensor bus
  Wire.begin (TOUCH_SDA, TOUCH_SCL, 400000);
  // Bus 2: BQ25619 only (ship mode)
  Wire1.begin(SDA_BUS2,  SCL_BUS2,  400000);

  if (i2c_ping(Wire1, BQ_ADDR)) {
    bq_ok = true;
    Serial.println("  BQ25619   0x6A : ACK  (ship mode armed on double-click)");
  } else {
    Serial.println("  BQ25619   0x6A : NO ACK  (ship mode UNAVAILABLE)");
  }
  if (i2c_ping(Wire, TOUCH_ADDR)) {
    Serial.println("  CST9217   0x5A : ACK");
  } else {
    Serial.println("  CST9217   0x5A : NO ACK  (touch will not work)");
  }

  spi_init();
  display_init();
  touch_init();
  draw_color_test();

  Serial.printf("Display: %dx%d, col_offset=%d\n", LCD_WIDTH, LCD_HEIGHT, COL_OFFSET);
  Serial.println("Touch the screen for yellow stamps. Orange corner = INT active.");
  Serial.println("Double-click button (GPIO16) to enter ship mode.");
}

void loop() {
  button_poll();
  stamps_update();

  if (digitalRead(TOUCH_INT) == LOW) {
    fill_rect(LCD_WIDTH-30, 0, 30, 30, ORANGE);
    TouchPoint tp = touch_read();
    if (tp.pressed) {
      uint16_t sx = (uint16_t)constrain((int)tp.x - STAMP_SIZE/2, 0, LCD_WIDTH  - STAMP_SIZE);
      uint16_t sy = (uint16_t)constrain((int)tp.y - STAMP_SIZE/2, 0, LCD_HEIGHT - STAMP_SIZE);
      fill_rect(sx, sy, STAMP_SIZE, STAMP_SIZE, YELLOW);
      stamp_add(sx, sy);
      Serial.printf("[TOUCH] x=%d y=%d\n", tp.x, tp.y);
    }
  } else {
    fill_rect(LCD_WIDTH-30, 0, 30, 30, BLACK);
  }

  delay(20);
}
