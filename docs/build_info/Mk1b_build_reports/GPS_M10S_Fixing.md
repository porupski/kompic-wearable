---
Date opened: 2026-09-18
Date closed: 2026-09-20
Status: RESOLVED — 18b/c sketch defaults had RX/TX inverted relative to the master pinout; wiring GPS TX→GPIO18 and GPS RX→GPIO17 (per master pinout IOMUX: U1RXD on 18, U1TXD on 17) produces a clean ~289-370 B/s NMEA stream on the Mk1b. No bodge required.
---

# MAX-M10S GPS UART Silence — Debug Findings & Next Steps
Kompic Mk1 / Mk1b, ESP32-S3-WROOM-1U-N16R8

## Symptom
GPS module (u-blox MAX-M10S) receives correctly and transmits valid NMEA/UBX
at 9600 baud when confirmed alive on a bench ESP32-C3 SuperMini. Wired the
same way (VCC/GND/TX/RX only) into either ESP32-S3 board in this project,
essentially zero bytes are ever decoded by firmware — exactly ONE stray
byte in `total`, right after the UART reopen/reconfigure, then permanent
silence. The GPS is meanwhile spamming ~487 B/s of NMEA into the pin (the
scope sees it) but the UART peripheral never registers a single receive.

## Confirmed good (do not re-test these)
- **Module itself is alive.** Full NMEA burst + valid UBX-MON-VER response
  on the C3 bench rig, ~487 B/s, SW='ROM SPG 5.10 (7b202e)'.
- **Signal reaches the S3 pin cleanly.** Oscilloscope on GPIO17 (Mk1b)
  shows a clean, valid squarewave, no noise, no clipping.
- **Bit timing is correct 9600 baud.** Measured pulse width ~100 µs,
  matches the expected 104.17 µs bit period.
- **RX/TX pin-role assignment is correct.** Scope confirms the incoming
  waveform (module TX) lands on the pin configured as UART RX.
- **Not a power problem.** Isolating the module onto a separate known-good
  3.3 V (off the Mk1b's own 3V2 rail) made no difference. Even sourcing
  3V3 to the module from the C3 while wiring TX/RX to the S3 (grounds
  tied) — still nothing on the S3.
- **Not a cold solder joint.** Wires soldered directly to the WROOM
  module's castellated pads — no PCB trace in the loop. Same result at
  the GPS pads and at the WROOM castellations.
- **Not a full-flash-erase issue.** Arduino IDE → *Erase All Flash Before
  Sketch Upload* enabled, or `esptool.py erase_flash`, then reflashed the
  18b sketch — same silent result. This rules out stale ESP-IDF
  bootloader / partition table / NVS content.
- **Not a specific GPS chip.** Same module works on C3, is silent on both
  S3 boards. Two independent WROOM modules, two independent PCBs, one
  GPS module — the constant is the S3 side.
- **Not a specific board.** Failure identical on Mk1 (older, iv7.x) and
  Mk1b (iv8.0). Both are ESP32-S3-WROOM-1U-N16R8.

## Ruled out, with evidence
| Hypothesis | Test | Result |
|---|---|---|
| RX/TX swapped | Swapped both directions | No change |
| Power delivery / 3V2 rail sag | Isolated bench 3.3 V supply | No change |
| Signal integrity / wiring | Oscilloscope on the pin | Clean signal confirmed |
| Cold joint under WROOM | Direct-to-castellation wiring | No change |
| Specific pin (17/18) damaged | Moved to spare QSPI pins 9/10 on same board | Still silent |
| UART peripheral instance | Swapped `HardwareSerial(1)` → `HardwareSerial(2)`, same pins | No change |
| Fixed-ratio baud/clock error (2×/0.5×/4×) | Swept 4800 / 19200 / 38400 / 9600 via CLI | No real data at any rate — only isolated 1-byte glitches right after reopen, consistent with reconfigure noise, not a shifted-but-decodable stream |
| Single dead chip | Moved same GPS module to the **older Mk1 board**, GPIO17/18 | Also silent |
| Stale ESP-IDF flash content | Full erase-all-flash then reflash Arduino sketch | No change |

Two independently fabricated WROOM-1 modules failing identically, with a
proven-good external signal, rules out a coincidental double hardware
failure. The common factor is either **arduino-esp32 core behavior on S3**
or **persistent silicon state** on the S3s (not flash).

---

## New evidence — 2026-09-20

### Bench monitor: C3 (working) vs S3 (silent)

**C3 bench rig — module ALIVE, UART decoding perfectly:**
```
========================================================
  MAX-M10S liveness diagnostic (ESP32-C3 SuperMini)
========================================================
  UART1: RX=GPIO4  TX=GPIO5  baud=9600
  Wire: C3.3V3 -> M10.VCC, C3.GND -> M10.GND
        C3.GPIO4 -> M10.TX, C3.GPIO5 -> M10.RX
  SAFEBOOT_N: leave FLOATING (do NOT tie high)
  Type HELP for CLI commands.
========================================================
[SEND] UBX cls=0x0A id=0x04 len=0  ck=0E 34
[UBX ] cls=0x0A id=0x04 len=190 OK  52 4F 4D 20 53 50 47 20 35 2E 31 30 20 28 37 62 32 30 32 65 29 00 00 00 00 00 00 00 00 00 30 30 ...
       SW='ROM SPG 5.10 (7b202e)'  HW='000A0000'
[NMEA] $GNRMC,,V,,,,,,,,,,N,V*37
[NMEA] $GNVTG,,,,,,,,,N*2E
[NMEA] $GNGGA,,,,,,0,00,99.99,,,,,,*56
[NMEA] $GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,1*33
[NMEA] $GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,3*31
[NMEA] $GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,4*36
[NMEA] $GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,5*37
[NMEA] $GNGLL,,,,,,V,N*7A
[STAT] total=487 B  window=487 B/s (last 1000 ms)  NMEA=8 UBX=1 err=0  idle=268 ms  baud=9600  hex=0
```

**Both S3 boards (Mk1 and Mk1b) — module SILENT:**
```
========================================================
  MAX-M10S liveness diagnostic (Mk1b iv8.0, ESP32-S3)
========================================================
  UART1: RX=GPIO17  TX=GPIO18  baud=9600
  Type HELP for CLI commands.
========================================================
[SEND] UBX cls=0x0A id=0x04 len=0  ck=0E 34
[STAT] total=1 B  window=1 B/s (last 1000 ms)  NMEA=0 UBX=0 err=0  idle=791  ms baud=9600  hex=0
[STAT] total=1 B  window=0 B/s (last 1000 ms)  NMEA=0 UBX=0 err=0  idle=1791 ms baud=9600  hex=0
[STAT] total=1 B  window=0 B/s (last 1000 ms)  NMEA=0 UBX=0 err=0  idle=2791 ms baud=9600  hex=0
... (total stays pinned at 1 forever)
```

Note the pattern: **`total` reaches 1 within the first STATS window and
then never budges again.** That one byte is almost certainly a glitch
from the pin's transition into UART-input mode during `begin()` — the
GPS is streaming ~500 B/s but the UART never sees a real byte after the
initial transient.

### The 18c "clock source" sketch does not build

Ran the follow-up sketch that tries to force `uart_set_clock_source()`
directly. Compilation fails on the installed arduino-esp32 core:
```
'uart_set_clock_source' was not declared in this scope
```
That symbol is an **IDF v5.0+** public API. The core installed in this
Arduino 1.8.19 setup is an arduino-esp32 **v2.x** (IDF v4.4 under the
hood), which does not expose that call. To force the clock source in
v2.x you have to use `uart_param_config()` with `.source_clk =
UART_SCLK_APB` — after `uart_driver_delete()` / re-install, since
HardwareSerial has already installed the driver by the time you'd want
to change it. (See Next steps.)

The compile failure ALSO tells us something on its own: this Arduino
install ships an older arduino-esp32 core. Older cores have known S3
UART quirks — see Analysis below.

---

## Analysis — where the actual break is

The scope + `total=1` combination is diagnostic.

- Scope: the GPS's 9600-baud waveform is physically present on the ESP
  input pin, clean, at correct amplitude.
- Firmware counter: `total` is pinned at 1. Not 1 per second — 1
  forever. The UART peripheral is not receiving edges.

That means the fault is **not** on the wire, **not** in the pin's
electrical state, and **not** in the baud divider (a wrong divider would
still produce byte events — even if framing-errored or garbage). The
break is **between the pin and the UART peripheral input**: the signal
lands on the pin, but the routing from that GPIO into `U1RXD` (or into
whatever UART instance is opened) is not present.

On ESP32-S3, that routing goes through the GPIO matrix by
`esp_rom_gpio_connect_in_signal(pin, U1RXD_IN_IDX, false)` inside the
driver. If that call is missing, wrong, or being *undone* by another
piece of code, the peripheral sees a floating input regardless of what
the pin physically shows.

### Leading candidates for what's severing the routing

**(A) arduino-esp32 v2.x HardwareSerial regression on ESP32-S3 for
non-IOMUX pin choices.**
`HardwareSerial::begin(baud, config, rxPin, txPin)` in v2.x has a known
history of doing IOMUX vs. GPIO-matrix path selection incorrectly on the
S3 (the S3 was added late in the v2 line). When the requested RX pin is
not the IOMUX default for that UART instance, the driver takes the
matrix path — and on some v2.x releases the matrix in-signal is either
not set, or is set for `UART*_RXD_IN_IDX` of the wrong instance. The
symptoms match exactly: TX may or may not appear (also matrix-out), RX
goes nowhere, moving to another non-IOMUX pin pair (9/10) makes no
difference, moving to another UART instance makes no difference (the
same faulty branch runs).

The C3 works because on the C3 arduino-esp32 v2.x, the HardwareSerial
S3 quirks don't apply — and/or the C3 pins 4/5 happen to be routed
correctly.

**(B) Persistent RTC / LP_IO state on GPIO 17/18 (and 9/10) from prior
ESP-IDF firmware, surviving reset and erase_flash.**
On the S3, GPIO 0–21 are RTC/LP_IO capable — that includes every pin
tried in this test (9, 10, 17, 18). If an earlier ESP-IDF app on either
board called `gpio_hold_en()` or `rtc_gpio_hold_en()` on these pins
(for a deep-sleep scenario, or accidentally as part of another driver),
the *entire* pin state — direction, PU/PD, and routing enable — gets
latched inside the RTC domain. That latch:
- survives `esp_restart()` / NRST — normal digital reset does not clear
  the RTC domain
- survives `esptool.py erase_flash` — that only clears flash, not the
  RTC/eFuse silicon state
- is only cleared by an actual **power cycle** of the RTC domain, i.e.
  battery disconnect or a full BQ ship-mode → wake

The Mk1b has a permanently-attached battery
([[feedback_sketches_need_shipmode]]), so a reset button press is *not*
a power cycle. Unless ship-mode was actually triggered between the last
IDF flash and this test, RTC hold state persists indefinitely.

A hold that leaves the pin as "input enabled, no PU/PD" would show
exactly what the scope shows (external signal present on the pin) while
disconnecting the pin from the UART matrix — exactly what `total=1`
suggests. And it would apply to every pin the previous IDF app happened
to have configured, which is why 17/18 AND 9/10 both fail.

**(C) A less-likely but still open possibility:** default UART clock
source being wrong (the reason 18c was written). Given (A) and (B), and
given that the sweep across 4800/9600/19200/38400 landed on no working
divider, this drops down the list — but hasn't been directly tested
because 18c won't build.

The 1-byte glitch on boot is also more consistent with (A)/(B) — a
brief moment where the pin is momentarily readable during the reconfig,
then routing breaks — than with a clock-source mismatch (which would
produce a steady stream of framing errors, not silence).

---

## Next steps, in order

1. **Sanity: full power cycle first.** Trigger BQ ship-mode (double-click
   per `[[feedback_sketches_need_shipmode]]`), wait a beat, wake the
   board, re-flash 18b, watch. This is the cheapest test that
   distinguishes (B) from (A): if the board comes up and immediately
   starts decoding NMEA, hypothesis (B) was it (RTC hold cleared by
   power-cycle).

2. **If still silent, force GPIO reset at the top of `setup()`** before
   `GPS.begin()` in the 18b sketch:
   ```c
   #include "driver/gpio.h"
   #include "driver/rtc_io.h"
   ...
   void setup() {
       Serial.begin(115200);
       delay(400);

       // Clear any lingering RTC/hold/route state on the GPS pins
       // (belt-and-suspenders; only matters if hypothesis B is live)
       rtc_gpio_hold_dis((gpio_num_t)GPS_RX_PIN);
       rtc_gpio_hold_dis((gpio_num_t)GPS_TX_PIN);
       gpio_hold_dis    ((gpio_num_t)GPS_RX_PIN);
       gpio_hold_dis    ((gpio_num_t)GPS_TX_PIN);
       gpio_reset_pin   ((gpio_num_t)GPS_RX_PIN);
       gpio_reset_pin   ((gpio_num_t)GPS_TX_PIN);

       GPS.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
       pinMode(GPS_RX_PIN, INPUT_PULLUP);
       ...
   }
   ```
   This proves/disproves (B) without needing a physical power cycle.

3. **If still silent, bypass HardwareSerial entirely** using the raw IDF
   v4.4 API (no `uart_set_clock_source` needed — use `uart_config_t
   .source_clk`). Skeleton to drop into a fresh sketch:
   ```c
   #include "driver/uart.h"
   #define UART_PORT UART_NUM_1

   static void gps_uart_init_raw(void) {
       uart_config_t cfg = {
           .baud_rate  = 9600,
           .data_bits  = UART_DATA_8_BITS,
           .parity     = UART_PARITY_DISABLE,
           .stop_bits  = UART_STOP_BITS_1,
           .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
           .rx_flow_ctrl_thresh = 0,
           .source_clk = UART_SCLK_APB,   // <-- force APB, no HardwareSerial
       };
       // Just in case HardwareSerial has already claimed it
       uart_driver_delete(UART_PORT);
       ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
       ESP_ERROR_CHECK(uart_set_pin(UART_PORT,
                                    GPS_TX_PIN, GPS_RX_PIN,
                                    UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
       ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 2048, 0, 0, NULL, 0));
   }
   ```
   Read with `uart_read_bytes(UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(10))`
   in `loop()`.
   - This eliminates (A) entirely: if raw-IDF works and HardwareSerial
     doesn't on the same pins/board, arduino-esp32 v2.x's S3 UART path
     is confirmed as the culprit.
   - It also gives us the `.source_clk` knob without needing v5-only
     `uart_set_clock_source`, so we can try `UART_SCLK_REF_TICK` /
     `UART_SCLK_RTC` too.

4. **If STILL silent after (3),** the last-resort control is to test on
   a genuinely never-flashed ESP32-S3 module (bare, from reel) — same
   sketch, same pins. If a virgin S3 also fails, it's an arduino-esp32
   pin/matrix issue for the S3-N16R8 variant; if a virgin S3 works, it
   confirms our two S3s are stuck in a silicon-persistent state that no
   flash operation clears.

5. **Longer-term fix regardless of root cause:** the Mk1b's real
   `max_m10s` component under ESP-IDF v5.5 (which is what will run on
   Mk1b post-integration) uses raw `uart_driver_install()` + explicit
   `.source_clk = UART_SCLK_APB`. Once step (3) works, port the same
   init into `components/max_m10s/max_m10s.c` so this class of bug never
   reappears when we move off the Arduino test rig.

## Notes on the 18c sketch (for future reference)
- The `#include "driver/uart.h"` is fine; only the `uart_set_clock_source`
  call is v5-only.
- Under v2.x (IDF v4.4) the substitute is `uart_param_config()` with
  `.source_clk` set — but that requires `uart_driver_delete()` first
  because HardwareSerial has already installed the driver. HardwareSerial's
  API doesn't expose a way to change `.source_clk` after install, which
  is why the raw sketch in step (3) is the cleanest way to test the
  clock-source hypothesis on this Arduino installation.
- ESP32-S3 UART only exposes `UART_SCLK_APB / UART_SCLK_XTAL / UART_SCLK_RTC`
  — `UART_SCLK_REF_TICK` is a C3/older-SoC option and doesn't compile for S3.

---

## Root cause found — 2026-09-20 bench session

**GPIO17 input buffer on the S3-WROOM is physically dead / stuck-high.
NOT a code or clock-source issue.**

Confirmed by running 18c on Mk1b:

1. On the currently-installed pins (17 = RX, 18 = TX), **all three clock
   sources (APB / XTAL / RTC) produce silence**. `total` stays at
   0–3 bytes forever (the couple of bytes that do land are reconfig
   glitches during `uart_init`, not real data).

2. `PROBE` on GPIO17 — which reads the pin as a plain GPIO input for
   10 ms while the GPS is bursting NMEA at ~500 B/s — reports:
   ```
   [PROBE] samples: hi=7652 lo=0 edges=0
   ```
   The oscilloscope shows a clean 9600-baud swing on the same pad.
   `edges=0` means the ESP's digital input path is **not seeing** any
   of the transitions the scope is showing on the pin. The GPS is
   driving the pad, the scope confirms it, and the ESP's input latches
   solid high anyway.

3. `PINS 9 10` on the exact same sketch, exact same GPS, exact same
   wiring topology — **instant success**:
   ```
   [STAT] total=5482 B  window=418 B/s  NMEA=153 UBX=1 err=0
          rx=GPIO9 tx=GPIO10 baud=9600 clk=APB
   ```
   Full NMEA burst + `UBX=1` (MON-VER response) at ~420 B/s, matching
   the C3 bench rig. Software / GPS / wiring are all fine. It's
   specifically 17 (and, on Mk1's earlier test, 17/18) that don't
   receive.

The failure is **identical on Mk1 and Mk1b** — two independently
fabricated boards, two independent WROOM-1 modules, both silent on
GPIO17. Two possible interpretations:

- (a) Both WROOMs took the same ESD/over-voltage insult during assembly
  and coincidentally on the same pin. Coincidence is uncomfortable but
  possible — both were built in the same session with the same tooling.
- (b) There's an untraced short/clamp on the Mk1b design that pulls
  GPIO17 high enough to defeat the CMOS input threshold. Pinout says
  nothing else is on the net; a schematic re-audit is warranted before
  respinning boards, but not urgent for this bench triage.

Either way, GPIO17 is unusable on **this hardware, right now**, and we
have to route the GPS somewhere else.

## Fix plan

Order of preference (cheapest first):

### Tier 1 — firmware-only pin swap (if GPIO18 input is alive)
Run `PINS 18 17` in the 18c sketch. If bytes flow:
- GPS stays on the existing 17/18 PCB pads/traces.
- Firmware just swaps which pin is RX and which is TX for UART1.
- Add a per-unit override (NVS flag `gps_pins_swapped`) in the real
  ESP-IDF `components/max_m10s/max_m10s.c` so both variants coexist
  in one binary.

### Tier 2 — single bodge, keep TX on GPIO18 (if GPIO18 output is alive)
If Tier 1 fails, run `PINS 9 18` + `PING` in the 18c sketch. If the
`UBX=` counter increments, GPIO18's output driver works — only RX is
broken. In that case:
- Keep GPS TX pad → GPIO18 (existing PCB trace, no change).
- **Bodge wire: GPS TX pad → GPIO7 (MAX_INT) castellation.**
- Sacrifice MAX30101 interrupt; poll the FIFO instead. Fine for daytime
  HR; slightly worse for sleep-HR power but MAX_INT was already the
  lowest-priority wake source.
- GPIO7 is RTC-capable, so future wake-on-GPS-byte remains an option.

### Tier 3 — full double bodge (both 17 and 18 dead)
If GPIO18 also fails both tests:
- GPS RX → GPIO7 (MAX_INT), same trade as Tier 2.
- GPS TX → GPIO15 (RTC_INT). Losing scheduled alarm-wake stings less
  than losing LSM raise-to-wake (GPIO8) or the crown (GPIO21). RTC is
  still readable via I²C bus 2 by polling.
- Two bodge wires per board; document per-unit.

### Rejected alternatives (why)
- **GPIO0 (DRV_EN):** boot-mode strap, must be HIGH at reset. UART line
  glitches during boot could drop it low and land us in download mode
  on cold boot. Not worth on a wearable.
- **GPIO46 (TimePulse) or GPIO45 (QSPI_TE):** both are strap pins that
  must idle LOW at reset. UART TX/RX idles HIGH — direct strap
  violation.
- **GPIO41 (FLASHLIGHT):** non-RTC (loses wake) AND shares the
  flashlight LED driver — brownouts on RX every strobe.
- **Move GPS to I²C (DDC, 0x42):** MAX-M10S supports it, but it eats
  bus-2 bandwidth we deliberately keep quiet for touch/PMIC, requires
  polling (no INT line wired), and hides the underlying "why is GPIO17
  dead" question that will bite us again on the next pin if not
  understood.

## Follow-ups after Mk1b GPS is live
1. Schematic re-audit on GPIO17 net: is there anything hidden on the
   net besides the GPS TX pad? If yes → design fix for Mk2. If no → the
   damage was assembly-side and Mk2 needs stricter ESD handling.
2. Port the working `uart_init()` block from 18c into
   `components/max_m10s/max_m10s.c`: explicit `.source_clk =
   UART_SCLK_APB`, `nuke_pin_state()` on GPS pins before install, and
   NVS-selectable pin pair for per-unit overrides.
3. Add `PROBE` equivalent to the ESP-IDF CLI (temporarily detach a pin
   from its peripheral, sample as plain input, report edges) — the
   single test that would have found this in one line if we'd had it
   from the start.
4. Log this class of failure in [[reference_mk2_intel_folder]] so the
   Mk2 respin gets a pre-reflow "PROBE each I/O against a squarewave"
   step, not a post-integration one.

---

## RESOLUTION — 2026-09-20

**Root cause: the 18b/c sketch `#define`s had GPS_RX_PIN and GPS_TX_PIN
inverted relative to the Mk1b master pinout.**

Master pinout (`0_Kompic_Pinout_MASTER_v20_iv7.1.md`):
- GPIO17 → `U1TXD` IOMUX → this is the ESP's UART TX (drives GPS RXD)
- GPIO18 → `U1RXD` IOMUX → this is the ESP's UART RX (from GPS TXD)

Sketch defaults (both 18b and initial 18c):
```c
#define GPS_TX_PIN       18    // ESP TX -> MAX-M10S RXD (pin 3)   [WRONG]
#define GPS_RX_PIN       17    // ESP RX <- MAX-M10S TXD (pin 2)   [WRONG]
```
Exactly inverted. So when Ivan wired the GPS to what the sketch called
"GPS_TX_PIN 18" (ESP TX in sketch), he was actually connecting GPS TXD
to an ESP output pin — no data could ever flow. Every earlier "swapped
both directions" test moved the wires but never simultaneously
recompiled the firmware, so no combination ever ended up
correctly-aligned.

Once the pin roles were aligned with the master pinout (via `PINS 18 17`
in the CLI, i.e. rx=18, tx=17), the GPS immediately produced clean
NMEA at ~289-370 B/s on the Mk1b — the first successful stream on any
S3 board in this project.

### About the earlier "GPIO17 input damage" hypothesis
That was the correct diagnosis for the *symptom* (PROBE saw hi=7652,
lo=0, edges=0 on GPIO17 while the GPS was streaming), but it may or
may not be an actual damage. Both interpretations fit the PROBE data:

- **Damage interpretation:** GPIO17's input buffer is genuinely dead
  (stuck-high), which is why PROBE saw no edges when a live GPS TX
  was on the wire. The Mk1 board's earlier failure is consistent, but
  might also be a repeat of the same pin-role mistake without ever
  actually testing the correct combination.
- **Miswiring interpretation:** the wire Ivan believed was on GPIO17
  during PROBE might in fact have been on the GPS's RXD pin (an input
  from the GPS's perspective), which the GPS does not drive — so
  no edges is the expected reading.

Either way, the master pinout convention (GPIO17=TX, GPIO18=RX) works
today on this Mk1b, so no bodge is needed and no pin is being
sacrificed. If we later need to prove GPIO17 input is or isn't dead,
run `PINS 17 18` (rx=17, tx=18) on the current bench rig with GPS TXD
wired to GPIO17 — if silent, damage confirmed; if bytes flow, the
earlier PROBE was misinterpreted.

### Correct output — reference snippet
Bench log at the moment of resolution, sketch 18c on the Mk1b, GPS on
GPIO17/18 per the master pinout:
```
[STAT] total=947 B  window=289 B/s (last 1000 ms)  NMEA=28 UBX=0 err=0
       idle=418 ms  rx=GPIO18 tx=GPIO17 baud=9600 clk=APB hex=0
[NMEA] $GNRMC,,V,,,,,,,,,,N,V*37
[NMEA] $GNVTG,,,,,,,,,N*2E
[NMEA] $GNGGA,,,,,,0,00,99.99,,,,,,*56
[NMEA] $GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,1*33
[NMEA] $GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,3*31
[NMEA] $GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,4*36
[NMEA] $GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,5*37
[NMEA] $GPGSV,1,1,00,0*65
[NMEA] $GAGSV,1,1,00,0*74
[NMEA] $GBGSV,1,1,00,0*77
[NMEA] $GQGSV,1,1,00,0*64
[NMEA] $GNGLL,,,,,,V,N*7A
```
Rate steady at ~289 B/s (up to 370 B/s during GSV bursts), zero errors,
zero fix (GPGGA fix=0 is correct — no antenna, indoors, cold module).

### Final assembly wiring (2026-09-20, "flying wires with tape" build)
| MAX-M10S pin | Wire to | Note |
|---|---|---|
| VCC | 3V3 | main supply |
| GND | GND | shared with RF ground |
| V_BCKP | supercap `+` | supercap other leg → GND; enables warm/hot start |
| TXD | **ESP GPIO18** | ESP U1RXD (RX in) |
| RXD | **ESP GPIO17** | ESP U1TXD (TX out) |
| TIMEPULSE | **ESP GPIO46** | 1PPS; wired now, unused until time-sync fw lands |
| RF_IN | antenna | 50 Ω pigtail / antenna trace; keep short |
| SAFEBOOT_N | **FLOAT** | never tie high (fights internal 1 kΩ to TIMEPULSE) |
| RESET_N | **FLOAT** | self-reset on power-up |
| VIO_SEL / D_SEL | **FLOAT** | float = 3V3 UART default |
| EXTINT, SDA, SCL | **FLOAT** | unused (UART mode) |

### Locked-in firmware changes for the real driver
When porting into `components/max_m10s/max_m10s.c`:
- `GPS_RX_PIN = 18`, `GPS_TX_PIN = 17` (matches master pinout, matches
  the working bench config).
- `uart_config_t.source_clk = UART_SCLK_APB` explicitly.
- Call `nuke_pin_state()` (from 18c) on both GPS pins before
  `uart_driver_install()` — cheap insurance against any future
  RTC-hold surprise.
- Consider updating the `#define` names to `GPS_RXD_ON_ESP18` /
  `GPS_TXD_ON_ESP17` or similar to make the direction unambiguous
  and prevent this exact bug from recurring.

### Loose end
`clkname()` in the current 18c still has REF_TICK in its CLI help
strings (both `cli_help()` and the `[HB]` heartbeat) even though the
enum entry is gone. Cosmetic; fix on next touch of the file.
