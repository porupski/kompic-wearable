// LED driver bench test for BC847C low-side switch on ESP32-C3 SuperMini + 72x40 OLED.
// Cycles STEPPER (10 x 10%, 2s hold) then GRADIENT (1% steps, 20s sweep) forever.

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

#define SDA_PIN     5
#define SCL_PIN     6
#define LED_PIN     2

#define PWM_FREQ    5000
#define PWM_RES     8
#define PWM_MAX     ((1 << PWM_RES) - 1)

#define STEP_COUNT      10
#define STEP_HOLD_MS    2000
#define SWEEP_MS        20000
#define SWEEP_STEP_MS   200

U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

enum Phase { PHASE_STEPPER, PHASE_GRADIENT };

static void setPwmPercent(uint8_t pct) {
    if (pct > 100) pct = 100;
    uint32_t duty = ((uint32_t)pct * PWM_MAX) / 100;
    ledcWrite(LED_PIN, duty);
}

static void drawStatus(Phase phase, uint8_t pct, uint32_t elapsedMs, uint32_t totalMs) {
    char line[16];
    u8g2.clearBuffer();

    u8g2.setFont(u8g2_font_04b_03_tr);
    u8g2.drawStr(0, 6, phase == PHASE_STEPPER ? "STEPPER" : "GRADIENT");

    u8g2.setFont(u8g2_font_logisoso16_tr);
    snprintf(line, sizeof(line), "%3u%%", pct);
    u8g2.drawStr(0, 26, line);

    u8g2.setFont(u8g2_font_04b_03_tr);
    snprintf(line, sizeof(line), "duty %u/%u", (unsigned)(((uint32_t)pct * PWM_MAX) / 100), PWM_MAX);
    u8g2.drawStr(0, 34, line);

    uint8_t sweepPct = totalMs ? (uint8_t)((elapsedMs * 100UL) / totalMs) : 0;
    if (sweepPct > 100) sweepPct = 100;
    snprintf(line, sizeof(line), "t %u%%", sweepPct);
    u8g2.drawStr(44, 34, line);

    // Bar for sweep position along the bottom row (72 wide, y=36..39)
    u8g2.drawFrame(0, 36, 72, 4);
    uint8_t barW = (uint8_t)((sweepPct * 70UL) / 100);
    if (barW) u8g2.drawBox(1, 37, barW, 2);

    u8g2.sendBuffer();
}

static void runStepper() {
    const uint32_t total = STEP_COUNT * STEP_HOLD_MS;
    for (uint8_t i = 1; i <= STEP_COUNT; ++i) {
        uint8_t pct = (uint8_t)((i * 100U) / STEP_COUNT);
        setPwmPercent(pct);
        uint32_t start = millis();
        while (millis() - start < STEP_HOLD_MS) {
            uint32_t elapsed = (i - 1) * STEP_HOLD_MS + (millis() - start);
            drawStatus(PHASE_STEPPER, pct, elapsed, total);
            delay(50);
        }
    }
}

static void runGradient() {
    uint32_t start = millis();
    while (true) {
        uint32_t elapsed = millis() - start;
        if (elapsed >= SWEEP_MS) break;
        uint8_t pct = (uint8_t)((elapsed * 100UL) / SWEEP_MS);
        setPwmPercent(pct);
        drawStatus(PHASE_GRADIENT, pct, elapsed, SWEEP_MS);
        delay(SWEEP_STEP_MS / 4);  // refresh faster than 1% cadence for smooth bar
    }
}

void setup() {
    Wire.begin(SDA_PIN, SCL_PIN);
    u8g2.begin();
    u8g2.setFontDirection(0);

    ledcAttach(LED_PIN, PWM_FREQ, PWM_RES);
    setPwmPercent(0);

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_04b_03_tr);
    u8g2.drawStr(0, 10, "LED DRV TEST");
    u8g2.drawStr(0, 20, "BC847C @ GPIO2");
    u8g2.drawStr(0, 30, "5kHz 8-bit");
    u8g2.sendBuffer();
    delay(1500);
}

void loop() {
    runStepper();
    setPwmPercent(0);
    delay(300);
    runGradient();
    setPwmPercent(0);
    delay(300);
}
