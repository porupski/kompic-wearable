/**
 * @file bq25619.h
 * @brief TI BQ25619 -- 1.5 A Li-ion charger + 1 A PMID boost, I2C bus 2.
 *
 * Hardware:
 *   Chip       : TI BQ25619RTWR (WQFN-24)
 *   I2C bus    : 2  (GPIO4 SDA, GPIO5 SCL, 400 kHz)
 *   I2C addr   : 0x6A (fixed)
 *   INT GPIO   : NONE -- status is polled via I2C (v7.2 line 237)
 *   QON pin    : dual-wired to GPIO16 (handled by boot_power.c, not this driver)
 *   BATSNS     : via I2C ADC readback (no external ADC, no GPIO)
 *   Boost (PMID): 5.0 V, mutually exclusive with charge; FW-gated via REG_MISC.
 *   TS         : 10 kΩ fixed (R14) + 10 kΩ 0402 NTC, battery-centred.
 *
 * Owns the broker_battery_data_t payload (Blueprint 4 §2 -- "fat driver").
 * data_broker.h will need its `#include "app_battery.h"` updated to
 * `#include "bq25619.h"` once boot_hw_init/data_broker integration lands.
 * Until then, this header is the single source of truth for the broker shape.
 *
 * Core 0 only. No LVGL.
 *
 * Datasheet: TI SLUSDH8 / SLUSDS3 (BQ25618 / BQ25619). Specific register
 * addresses below are family-conventional; each is tagged [DSV] (datasheet-verify)
 * in the porting .md and the .c file. Bench bring-up confirms.
 */

#ifndef BQ25619_H
#define BQ25619_H

#include "esp_err.h"
#include "driver/i2c.h"
#include <stdint.h>
#include <stdbool.h>

// -- Identity ------------------------------------------------------------------
const char *bq25619_get_chip_name(void);  // returns "BQ25619"
const char *bq25619_get_chip_desc(void);  // returns "Li-ion charger + PMID boost"

// -- I2C config ----------------------------------------------------------------
#define BQ25619_ADDR              0x6A

// -- Register map -- ALL ADDRESSES [DSV] except where noted --------------------
// Standard BQ256xx family layout. Bench bring-up confirms. See bq25619.c
// for the bit-level defines.
#define BQ25619_REG_IINDPM        0x00  // input current limit
#define BQ25619_REG_POC            0x01  // power-on config: watchdog rst, BOOST_EN, CHRG_CONFIG
#define BQ25619_REG_ICHG           0x02  // charge current
#define BQ25619_REG_PRECHRG        0x03  // pre-charge / termination current
#define BQ25619_REG_VREG           0x04  // charge voltage target
#define BQ25619_REG_TIMER          0x05  // termination + safety timer
#define BQ25619_REG_BOOSTV         0x06  // boost voltage + thermal reg
#define BQ25619_REG_MISC           0x07  // BATFET_DIS (ship-mode) lives here
#define BQ25619_REG_STATUS         0x08  // VBUS_STAT, CHRG_STAT, PG_STAT, ...
#define BQ25619_REG_FAULT          0x09  // WATCHDOG / BOOST / CHRG / BAT / NTC fault flags
#define BQ25619_REG_PART           0x0A  // [DSV] part number + device revision -- WHO_AM_I
#define BQ25619_REG_VBAT_ADC       0x0B  // [DSV] battery voltage ADC readback

// -- Bit positions (subset; expand in .c as datasheet review advances) ---------
#define BQ25619_MISC_BATFET_DIS    (1 << 5)   // [DSV] ship-mode entry bit
#define BQ25619_POC_BOOST_EN       (1 << 5)   // [DSV] enable PMID boost
#define BQ25619_POC_CHG_CONFIG     (1 << 4)   // [DSV] enable charging
#define BQ25619_STATUS_CHRG_MASK   (0x03 << 3)
#define BQ25619_STATUS_CHRG_SHIFT  3
#define BQ25619_STATUS_PG_GOOD     (1 << 2)   // [DSV] input power good

// -- Charge-state decode (REG_STATUS[4:3]) -------------------------------------
typedef enum {
    BQ25619_CHRG_IDLE        = 0,
    BQ25619_CHRG_PRE_CHARGE  = 1,
    BQ25619_CHRG_FAST_CHARGE = 2,
    BQ25619_CHRG_DONE        = 3,
} bq25619_charge_state_t;

// -- Broker payload (Blueprint 4 §2) -------------------------------------------
// Field names `voltage` / `percentage` / `charging` are preserved for existing
// consumers (lvgl_ui/system_tile.c, lvgl_ui/ui_main_screen.c).
typedef struct {
    float     voltage;        // battery volts (e.g. 3.85)
    uint8_t   percentage;     // 0..100, from voltage LUT (see bq25619_soc_from_mv)
    bool      charging;       // CHRG_STAT in PRE / FAST / DONE
    bool      power_good;     // VBUS present + PG_STAT = 1

    uint8_t   charge_state;   // bq25619_charge_state_t raw value
    uint8_t   fault;          // raw REG_FAULT (0 = no fault)
    bool      boost_enabled;  // PMID boost state (FW-controlled)

    // Mandatory bookkeeping
    uint32_t  last_update_ms;
    bool      enabled;
} broker_battery_data_t;

#define BROKER_BATTERY_TIMEOUT_MS  5000U

// -- Lifecycle ----------------------------------------------------------------

/**
 * @brief Probe BQ25619 by reading REG_PART. Logs the chip's part-number +
 *        revision bytes (used as WHO_AM_I once the datasheet bit positions
 *        are confirmed). Does NOT write any config -- the BQ comes up in a
 *        sensible default state, and overriding ICHG / VREG / IINDPM is the
 *        responsibility of a later configuration step.
 *
 *        Caller must NOT hold g_i2c_mutex.
 *
 *        Called from boot_hw_init.c after I2C bus 2 scan confirms ACK at 0x6A.
 *
 * @return ESP_OK on success.
 */
esp_err_t bq25619_init(i2c_port_t i2c_num);

/** @brief No-op for this chip. Reserved for symmetry with other drivers. */
void bq25619_deinit(void);

/**
 * @brief 1 Hz polling task. Pinned to Core 0 via boot_tasks.c (entry name
 *        `task_battery_fn`, kept for ABI compatibility with the carried-forward
 *        boot_tasks.c table). Reads REG_STATUS + REG_FAULT + VBAT_ADC, computes
 *        SoC via voltage LUT, writes the broker payload. Stack: 3072 bytes.
 */
void task_battery_fn(void *arg);

// -- Read helpers (caller holds g_i2c_mutex) -----------------------------------

esp_err_t bq25619_read_reg(i2c_port_t i2c_num, uint8_t reg, uint8_t *val);
esp_err_t bq25619_write_reg(i2c_port_t i2c_num, uint8_t reg, uint8_t val);

/**
 * @brief Read battery voltage (mV). Wraps REG_VBAT_ADC. [DSV] -- the exact
 *        conversion is datasheet-defined (often raw*step + offset_mV).
 *        Returns 0 on failure (and logs).
 */
esp_err_t bq25619_read_vbat_mv(i2c_port_t i2c_num, uint16_t *vbat_mv_out);

// -- Control --------------------------------------------------------------------

/**
 * @brief Enable / disable the PMID 5V boost. Mutually exclusive with charging
 *        per v7.2 line 237 -- the BQ25619 hardware enforces this.
 *        Caller must NOT hold g_i2c_mutex.
 */
esp_err_t bq25619_set_boost(i2c_port_t i2c_num, bool enable);

/**
 * @brief Trigger ship-mode by writing BATFET_DIS = 1 in REG_MISC. The BQ
 *        disconnects the battery from SYS, and the device shuts down within
 *        the chip's BATFET delay (~10 s typical, but configurable).
 *
 *        DO NOT CALL FROM A TEST HARNESS without a way back -- the only exit
 *        is a USB-C insert or a long press on GPIO16 (QON).
 *
 *        Caller must NOT hold g_i2c_mutex.
 */
esp_err_t bq25619_enter_ship_mode(i2c_port_t i2c_num);

// -- SoC helpers ---------------------------------------------------------------

/**
 * @brief Convert battery voltage in mV to SoC percent via a first-pass LUT
 *        (3.30 V = 0%, 4.20 V = 100%, with concave intermediate points for a
 *        typical 3.7 V LiPo discharge curve). Pure function, no side effects.
 */
uint8_t bq25619_soc_from_mv(uint16_t vbat_mv);

/**
 * @brief Self-learning hook (stub). Called by the polling task on every read.
 *        Today: tracks observed min/max vbat into static variables (read via
 *        `bq25619_soc_get_observed_extremes`). Tomorrow: NVS-persisted LUT
 *        tightening so the SoC curve adapts to the actual cell behaviour.
 *
 *        Pure, no I/O.
 */
void bq25619_soc_observe(uint16_t vbat_mv, bool is_charging);

/** @brief Read the running observed min/max vbat (mV). For diagnostics. */
void bq25619_soc_get_observed_extremes(uint16_t *min_mv, uint16_t *max_mv);

#endif // BQ25619_H
