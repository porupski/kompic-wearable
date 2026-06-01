/*
 * CO5300 2.06" AMOLED + CST9217 Touch — Bare Metal QSPI Driver
 * ESP32-S3-WROOM-1U-N16R8 | SLPIN power-toggle test
 *
 * ── ARDUINO IDE SETTINGS (test sketch only) ──────────────────────────
 *   Board:           ESP32S3 Dev Module
 *   Flash Size:      4MB (32Mb)
 *   Flash Mode:      QIO 80MHz
 *   PSRAM:           Disabled
 *   USB CDC On Boot: Enabled
 *   NOTE: setting Flash 16MB or PSRAM OPI here makes THIS sketch panic at
 *   boot (init_flash assert / octal_psram ID error). Production firmware is
 *   ESP-IDF with the real N16R8 config (16MB flash, OPI PSRAM).
 *
 * ── THIS BUILD ───────────────────────────────────────────────────────
 *   The BOOT button (GPIO0) toggles display power via QSPI commands:
 *     OFF: 0x28 DISPOFF -> 0x10 SLPIN   (OLED boost + source drivers down)
 *     ON : 0x11 SLPOUT  -> re-init regs -> 0x29 DISPON -> repaint
 *   Purpose: measure panel SYS/VBAT rail current in SLPIN vs active to
 *   decide whether a dedicated hardware OLED_EN GPIO is needed. If SLPIN
 *   current is low enough, OLED_EN is dropped and GPIO45 is freed on the PCB.
 *
 *   MEASURE: meter inline on the panel VBAT/SYS_Power line (connector pin 8),
 *   NOT the 3V3 logic line — SLPIN's value is shutting down the boost
 *   converter, which is fed from VBAT. Compare active vs SLPIN current there.
 *
 * Key findings:
 * - COLMOD 0x77 (RGB888) required — RGB565 QIO lane mapping broken on CO5300:
 *   the green channel bits straddle the byte boundary in a way the ESP32-S3
 *   QIO peripheral cannot reproduce. RGB888 sends one full byte per channel.
 * - Pixels packed manually as 3 bytes (R,G,B) expanded from RGB565 input.
 * - Commands via instruction 0x02 (1-wire), pixels via 0x32 (4-wire QIO).
 * - SLPIN forgets controller state (COLMOD/MADCTL/window/RAM). On wake you
 *   MUST re-run the full init sequence; SLPOUT+DISPON alone wakes blank or
 *   garbled. display_wake() re-runs display_regs() then repaints.
 * - SLPOUT ordering matters: SLPOUT first + 120ms settle BEFORE the rest.
 *   If it still wakes blank, bump the SLPOUT delay to 200ms.
 * - DISP_RST on GPIO3 (JTAG strapping pin) — acceptable for bench bring-up.
 *   RST pulse happens ONLY on cold boot, never per SLPIN/SLPOUT cycle.
 * - TE (tearing effect, panel pin 13) is NOT used in this sketch. Production
 *   design routes TE to a GPIO; software here does not sync to it.
 * - CST9217: 16-bit register 0xD000, ACK writeback 0xAB, 400kHz I2C.
 */

#include <Arduino.h>
#include <Wire.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>

// ════════════════════════════════════════════════════════════════════
//  PINS
// ════════════════════════════════════════════════════════════════════

///// TODO: STILL NEED TO SWAP TP_RST (was 5) to RX (44) on SuperMini due to schematic changes

#define QSPI_CS     10
#define QSPI_CLK    12
#define QSPI_D0     11   // MOSI  (SIO0)
#define QSPI_D1     13   // MISO  (SIO1)
#define QSPI_D2      9   // WP    (SIO2)
#define QSPI_D3     14   // HD    (SIO3)
#define DISP_RST     3   // strapping pin — bench OK, RST only on cold boot

#define BOOT_BTN     0   // WROOM BOOT button — display power toggle

#define TOUCH_SDA    1
#define TOUCH_SCL    2
#define TOUCH_INT    6
#define TOUCH_RST    5  // iv7.1 Schematics: this is now 44 (RX) pin
#define TOUCH_ADDR  0x5A

// ════════════════════════════════════════════════════════════════════
//  DISPLAY CONSTANTS
// ════════════════════════════════════════════════════════════════════

#define LCD_WIDTH    410
#define LCD_HEIGHT   502
#define COL_OFFSET    22   // hardware column offset for this panel
#define MAX_PIXELS  1024   // DMA chunk size in pixels

// RGB565 color constants — expanded to RGB888 internally before sending
#define BLACK    0x0000
#define WHITE    0xFFFF
#define RED      0xF800
#define GREEN    0x07E0
#define BLUE     0x001F
#define YELLOW   0xFFE0
#define CYAN     0x07FF
#define MAGENTA  0xF81F
#define ORANGE   0xFD20

// ════════════════════════════════════════════════════════════════════
//  STRUCTS
// ════════════════════════════════════════════════════════════════════

struct TouchPoint { bool pressed; uint16_t x, y; };
struct Stamp      { uint16_t x, y; uint32_t born; bool active; };

// ════════════════════════════════════════════════════════════════════
//  SPI STATE
// ════════════════════════════════════════════════════════════════════

static spi_device_handle_t  spi_dev;
static spi_transaction_ext_t spi_ext;
static spi_transaction_t    *g_spi_t;
static uint8_t              *dma_buf;

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
  bus.quadwp_io_num   = QSPI_D3;   // WP line maps to quadwp
  bus.quadhd_io_num   = QSPI_D2;   // HD line maps to quadhd
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
  dev.spics_io_num   = -1;          // manual CS
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

// Send command byte only (1-wire)
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

// Send command + 1 parameter byte (1-wire)
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

// Send command + 2x uint16 (for CASET/RASET window coords)
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
//  PIXEL OUTPUT
//
//  RGB565 input is expanded to RGB888 before sending.
//  COLMOD 0x77 (24-bit) is used because RGB565 QIO lane mapping is broken
//  on CO5300: the green channel bits straddle the byte boundary in a way
//  the ESP32-S3 QIO peripheral cannot reproduce. RGB888 sends one full byte
//  per channel — maps cleanly across lanes.
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
//  COLOR TEST SCREEN
// ════════════════════════════════════════════════════════════════════

static void draw_color_test() {
  fill_screen(BLACK);

  // Border
  fill_rect(0,            0,             LCD_WIDTH, 2,          WHITE);
  fill_rect(0,            LCD_HEIGHT-2,  LCD_WIDTH, 2,          WHITE);
  fill_rect(0,            0,             2,         LCD_HEIGHT,  WHITE);
  fill_rect(LCD_WIDTH-2,  0,             2,         LCD_HEIGHT,  WHITE);

  // 2x2 primary color grid
  int bw = 120, bh = 120, gap = 20;
  int gx = (LCD_WIDTH - 2*bw - gap) / 2;
  int gy = 40;
  fill_rect(gx,          gy,          bw, bh, RED);
  fill_rect(gx+bw+gap,   gy,          bw, bh, GREEN);
  fill_rect(gx,          gy+bh+gap,   bw, bh, BLUE);
  fill_rect(gx+bw+gap,   gy+bh+gap,   bw, bh, WHITE);

  // 3 secondary colors
  int sy = gy + 2*(bh+gap);
  int sw = 80, sh = 80;
  int sx = (LCD_WIDTH - 3*sw - 2*gap) / 2;
  fill_rect(sx,           sy, sw, sh, YELLOW);
  fill_rect(sx+sw+gap,    sy, sw, sh, CYAN);
  fill_rect(sx+2*(sw+gap),sy, sw, sh, MAGENTA);
}

// ════════════════════════════════════════════════════════════════════
//  DISPLAY INIT / POWER CONTROL
// ════════════════════════════════════════════════════════════════════

// Register init only — safe to re-run after SLPIN (no RST pulse).
// SLPOUT is issued FIRST with a 120ms settle before anything else; if the
// panel ever wakes blank, raise that delay to 200ms.
static void display_regs() {
  cmd(0x11);           delay(120);  // SLPOUT  (settle before rest)
  cmd8(0xFE, 0x00);                 // CMD page 0
  cmd8(0xC4, 0x80);                 // SPI write RAM enable (mandatory)
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

// Cold boot: hardware reset pulse THEN register init.
static void display_init() {
  pinMode(DISP_RST, OUTPUT);
  digitalWrite(DISP_RST, HIGH); delay(50);
  digitalWrite(DISP_RST, LOW);  delay(200);
  digitalWrite(DISP_RST, HIGH); delay(300);
  display_regs();
}

// Power DOWN: blank output, then sleep-in (kills OLED boost + drivers).
static void display_sleep() {
  cmd(0x28);            // DISPOFF
  delay(20);
  cmd(0x10);            // SLPIN
  delay(120);           // allow controller to enter sleep
}

// Power UP: sleep-out + full re-init (state was lost) + repaint.
static void display_wake() {
  display_regs();       // SLPOUT + full re-init + DISPON
  draw_color_test();    // repaint — RAM/state lost during SLPIN
}

// ════════════════════════════════════════════════════════════════════
//  TOUCH — CST9217
//  Protocol: write 16-bit register address 0xD000, read 12 bytes,
//  write back ACK byte 0xAB. Frame valid when buf[6]==0xAB.
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
  Wire.begin(TOUCH_SDA, TOUCH_SCL, 400000);
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

static bool display_on = true;

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== CO5300 + CST9217 — SLPIN power test ===");

  pinMode(BOOT_BTN, INPUT_PULLUP);   // BOOT idles HIGH, pressed = LOW

  spi_init();
  display_init();
  touch_init();
  draw_color_test();

  Serial.printf("Display: %dx%d, col_offset=%d\n", LCD_WIDTH, LCD_HEIGHT, COL_OFFSET);
  Serial.println("Press BOOT (GPIO0) to toggle display SLPIN / SLPOUT.");
  Serial.println("Measure current on the panel VBAT/SYS_Power rail in each state.");
  Serial.println("Touch screen to draw stamps. Orange corner = touch active.");
}

void loop() {
  // --- BOOT button: toggle display power (debounced) ---
  static bool     last_btn  = HIGH;
  static uint32_t last_edge = 0;
  bool btn = digitalRead(BOOT_BTN);
  if (btn != last_btn && (millis() - last_edge) > 200) {
    last_edge = millis();
    last_btn  = btn;
    if (btn == LOW) {                       // falling edge = press
      display_on = !display_on;
      if (display_on) {
        Serial.println("[DISP] WAKE  (SLPOUT + re-init + repaint)");
        display_wake();
      } else {
        Serial.println("[DISP] SLEEP (DISPOFF + SLPIN)");
        display_sleep();
      }
    }
  }

  // --- touch + stamps only while display is on ---
  if (display_on) {
    stamps_update();

    if (digitalRead(TOUCH_INT) == LOW) {
      fill_rect(LCD_WIDTH-30, 0, 30, 30, ORANGE);   // touch active indicator
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
  }

  delay(20);
}
