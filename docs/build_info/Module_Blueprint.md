# Kompic Mk1 — Self-contained Module Blueprint

**Purpose.** Every hardware capability (sensor, actuator, LED, panel, radio,
audio path, ...) in this firmware is a **self-contained module** that exposes
the *same set of core functions* to two UIs at once:

- **CLI** (`fc_cli.c` verbs typed over USB-Serial-JTAG).
- **Screen** (LVGL tile + touch + eventually encoder click).

Both UIs are thin wrappers over the module's **command surface** — no logic
lives in the CLI dispatcher or the tile event handler that could not be
reached from the other side. Turning the flashlight on by tapping the button
on the LIGHT tile and typing `LIGHT ON` at the CLI hit the same
`flashlight_cmd_set(true)` code path.

**Why this discipline.**

- The bench is headless most of the time (iv7.1 display dead; Mk1b in fab).
  Every module must be testable via CLI alone — see
  `feedback_cli_first_testability.md`.
- Duplication (a tile that writes NVS directly, a CLI verb that writes NVS
  directly) is the #1 cause of two-way bugs: one path forgets to publish, or
  clamps differently, or persists to a slightly different key. Command
  surface is the single truth.
- Future outlets are cheap to add: a BLE app, a preset button, a scripted
  test rig — each is another thin wrapper over the same surface.

This document specifies the file layout, the command-surface contract, the
persistence + broker patterns, and the drop-in checklist for a new module.
Existing modules pre-date this blueprint; they will be migrated
opportunistically per `feedback_group_work_by_venue.md`, not in one big
sweep.

---

## 1. What counts as a module

One module = one hardware capability the user can meaningfully address.
Examples:

| Module    | Capability                       | Primary I/O           |
|:----------|:---------------------------------|:----------------------|
| `bq25619` | Battery charger + BATFET         | I2C0                  |
| `veml6030`| Ambient light + backlight auto   | I2C0                  |
| `drv2605` | Haptic driver                    | I2C0                  |
| `max_m10s`| GNSS receiver                    | UART + 1PPS GPIO      |
| `co5300`  | AMOLED display panel             | QSPI                  |
| `cst9217` | Capacitive touch controller      | I2C0 + INT GPIO       |
| `ws2812`  | Status LED strip                 | RMT peripheral        |
| `sdcard`  | Micro-SD storage                 | SDMMC                 |
| `mic_pdm` | PDM microphone                   | I2S                   |
| `pcf85063`| Battery-backed RTC               | I2C0                  |

Meta-modules (that own no hardware directly) still fit the pattern:

- `alarm`  — schedules haptic + notification events.
- `nvs_cfg` — system-wide flags (`batt_test`, `blackbox`, `lvgl_force`, ...).

---

## 2. File layout per module

```
components/<module>/
├── CMakeLists.txt
├── <module>.h          Public API — driver primitives + task fn + broker glue.
├── <module>.c          Driver implementation. Owns the hardware, publishes to
│                       data_broker, may spawn a task.
├── <module>_cmd.h      Command surface (THIS BLUEPRINT).  Same set of
│                       operations exposed to CLI + tile + any future outlet.
├── <module>_cmd.c      Command surface implementation. Thin — every command
│                       forwards to the driver or NVS, then returns.
├── <module>_tile.h     LVGL tile descriptor + tile-specific helpers.
├── <module>_tile.c     LVGL widgets. Event handlers call *_cmd_* functions;
│                       they DO NOT talk to the driver directly.
└── <module>_tests.c    (optional) Bench harness reachable via `TEST <module>`
                        CLI verb once the test infra lands.
```

**Naming.** `<module>_cmd_<verb>()`. Verb is lower_snake_case matching the CLI
token (`bq_cmd_ship_mode`, `light_cmd_auto_brightness_set`,
`gps_cmd_atomic_sync`). This keeps grep-ability high — one grep for
`<module>_cmd_` lists every operation the module supports.

---

## 3. Command surface — the contract

Every module MUST expose at least these categories in `<module>_cmd.h`:

| Category    | Signature shape                              | Purpose                                             |
|:------------|:---------------------------------------------|:----------------------------------------------------|
| **enable**  | `esp_err_t X_cmd_enable_set(bool on)`        | Power on/off, task start/stop, sensor arm/disarm.   |
|             | `bool      X_cmd_enable_get(void)`           | Live state (matches what STATUS + tile widget show).|
| **config**  | `esp_err_t X_cmd_<field>_set(T value)`       | One setter per user-facing config field.            |
|             | `T         X_cmd_<field>_get(void)`          | One getter per field. Return the currently ACTIVE   |
|             |                                              | value, not the requested one, if they differ.       |
| **action**  | `esp_err_t X_cmd_<action>(void)`             | One-shot verbs: `calibrate`, `sync`, `reset`,       |
|             |                                              | `zero_height`, `dump_regs`, `ship_mode`, ...        |
| **dump**    | `void X_cmd_dump(fc_print_fn print)`         | Human-readable state dump. Uses the caller-supplied |
|             |                                              | `print(const char *fmt, ...)` so the same routine   |
|             |                                              | feeds either serial or an LVGL text area.           |
| **status**  | `void X_cmd_status_summary(char *out,        | One-liner for the global STATUS printout.           |
|             |                            size_t max)`      |                                                     |

Return-code convention:

- `ESP_OK` — command accepted, state updated.
- `ESP_ERR_INVALID_ARG` — parse / range failure at the command boundary.
- `ESP_ERR_INVALID_STATE` — hardware not present, prerequisite unmet.
- `ESP_ERR_NOT_SUPPORTED` — command exists but not implemented on this HW.
- other `esp_err_t` — passed through from driver.

**No LVGL types in `<module>_cmd.h`.** The command surface is UI-agnostic;
adding an outlet must not require touching the surface. LVGL includes live
only in `<module>_tile.c`.

**No CLI parsing in `<module>_cmd.c`.** All arg parsing (string → int, ON/OFF
→ bool) happens in the CLI dispatcher. The command surface takes typed
arguments only.

---

## 4. Persistence — NVS pattern

If a config field must survive reboot:

1. `nvs_cfg.c` gets the key + getter + setter (unless the value is bulky, in
   which case the module opens its own NVS namespace).
2. `<module>_cmd_<field>_set(value)` writes NVS first, then applies to the
   driver. If the driver reject fails, the NVS write is left intact (next
   boot will retry the apply).
3. `<module>` init reads NVS on boot and pushes the value into the driver.
4. No other code writes to that key. CLI verb and tile handler both call
   `<module>_cmd_<field>_set()`.

See `nvs_cfg_sys_get/set_batt_test()` for the canonical shape.

---

## 5. Broker publish pattern (Blueprint 4)

Modules that produce continuous data (sensors) publish to `data_broker`:

- Driver task calls `broker_<module>_write(&payload)` once per acquisition
  cycle.
- Consumers (`_tile.c` refresh, `field_capture` CSV writers, `_cmd_status_*`
  routines) read via `broker_<module>_read(&out)`.
- `broker_<module>_hw_alive()` / `_get_status()` expose lifecycle state.
- The command surface never sits between driver → broker; it operates on
  the driver directly.

---

## 6. Two-outlet dispatch — concrete shape

### 6.1 CLI dispatch (`fc_cli.c`)

```c
if (startswith_ci(line, "LIGHT")) {
    const char *arg = line + 5;
    while (*arg == ' ' || *arg == '\t') arg++;

    if (*arg == 0 || startswith_ci(arg, "STATUS")) {
        light_cmd_dump(printf_wrapper);
        return;
    }
    if (startswith_ci(arg, "ON"))  { light_cmd_enable_set(true);  return; }
    if (startswith_ci(arg, "OFF")) { light_cmd_enable_set(false); return; }
    if (startswith_ci(arg, "AUTO")) {
        light_cmd_auto_brightness_set(true);
        return;
    }
    // ...
}
```

Rule of thumb: the CLI block is a parse-forward-print sandwich, never
> ~30 lines. Anything longer signals that logic is leaking into the CLI
that should live in `_cmd.c`.

### 6.2 Tile event dispatch (`<module>_tile.c`)

```c
static void cb_power_toggle(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool desired = lv_obj_has_state(sw, LV_STATE_CHECKED);
    esp_err_t r = light_cmd_enable_set(desired);
    if (r != ESP_OK) {
        // revert the switch visually so the widget mirrors reality
        lv_obj_clear_state(sw, LV_STATE_CHECKED);
        ESP_LOGW(TAG, "light enable rejected: %s", esp_err_to_name(r));
    }
}
```

The tile does not touch NVS, does not touch the driver, does not know about
the broker beyond reading through it for display. Every user-initiated
mutation goes through a `_cmd_` call.

### 6.3 Preset-button / future outlet

A preset button on the case wired to a GPIO would end up as:

```c
static void isr_preset_pressed(void *arg) {
    // ISR-safe: queue a request, the handler task calls
    // <module>_cmd_<action>() on the desired module.
}
```

Same surface. No changes to CLI, no changes to tile.

---

## 7. Global registration

- **CLI verb table**: `fc_cli.c` `rtc_cli_handle_line()` dispatcher. One
  `if (startswith_ci(...))` block per module. Update HELP + STATUS in the
  same edit (see `feedback_group_work_by_venue.md`).
- **Tile registry**: `components/lvgl_ui/tile_registry.c`. One line, one
  `&<module>_tile_desc` pointer. See `tile_registry.h` for the descriptor
  contract.
- **Task table**: `components/boot_logic/boot_tasks.c` for sensor tasks;
  LVGL-side tasks live in `lvgl_ui_display_start_tasks()`. Gate on hardware
  presence at spawn time.

---

## 8. Checklist — adding a new self-contained module `<X>`

1. `mkdir components/<X>/` and add `CMakeLists.txt`.
2. Driver: `<X>.h` + `<X>.c` — hardware primitives, task fn, broker publish
   if applicable. Version macro `<X>_DRIVER_VERSION` at top of `<X>.h`.
3. Command surface: `<X>_cmd.h` + `<X>_cmd.c` covering the categories in §3.
4. Broker glue: `broker_<X>_read/write/get_status/hw_alive/get_enabled/
   set_enabled` in `data_broker.{c,h}` (if a sensor).
5. NVS keys (if any): `nvs_cfg_sys_get/set_<X>_<field>()` in `nvs_cfg.{c,h}`
   OR own namespace in `<X>_cmd.c`.
6. Tile: `<X>_tile.h` + `<X>_tile.c` — exports `const tile_desc_t <X>_tile_desc`.
   Widgets call `<X>_cmd_*()` only. Tile lives in `components/<X>/`, not
   `components/lvgl_ui/`.
7. Register in `components/lvgl_ui/tile_registry.c` (`#include "<X>_tile.h"`,
   add pointer to `s_tiles[]`).
8. Register in `components/boot_logic/boot_tasks.c` if a task is needed
   (or `lvgl_ui_display_start_tasks()` for display-side tasks).
9. Register CLI verb in `components/field_capture/fc_cli.c`:
   - dispatcher block (parse → `_cmd_*()` → print)
   - HELP line
   - STATUS line (via `<X>_cmd_status_summary()` if the state fits on one row)
10. Update REQUIRES in whichever component's `CMakeLists.txt` needs to link
    against `<X>`.
11. Boot bring-up: add `<X>_init()` call in `boot_hw_init.c` (or
    equivalent phase). Boot log should print `<X>: driver v<version>` as
    the first line.
12. Provenance banners: driver `_init` first line uses `<X>_DRIVER_VERSION`;
    bump `KOMPIC_FW_VERSION` in `firmware_version.h`.
13. Test from the bench headless: `<VERB> STATUS` and each mutation returns
    a meaningful reply. If LVGL is up, `TILE <name>` reaches the widget too.

---

## 9. Migration status (2026-09-08)

None of the 10 tile modules yet expose `<X>_cmd.h` as specified. They all
directly touch broker + NVS + hardware from their tile event handlers.
This works today but violates §6.2 and creates the CLI/tile drift risk.

Migration approach:

- **No big-bang refactor.** Group per-venue: when a stage touches a module's
  files for another reason, extract its `_cmd.c` in the same pass. See
  `feedback_group_work_by_venue.md`.
- **New modules ship compliant.** From this blueprint's date forward,
  reviewers will bounce a PR that adds a module without a `_cmd.h`.
- **Priority migration order** (based on user-facing frequency):
  1. `bq25619` (SHIPMODE + batt_test already partially command-shaped in `nvs_cfg`).
  2. `veml6030` (backlight + auto-brightness — highest touch traffic).
  3. `pcf85063` (SET_TIME + GET_TIME + `rtc_cli_*` -- already close to shape).
  4. `drv2605` (haptic effect picker).
  5. Rest — as they get touched.

Track migration progress in each stage's `§8 Next steps` block.

---

## 10. Anti-patterns

- Tile event handler that writes NVS. → Move to `_cmd_<field>_set()`.
- CLI verb that computes a threshold before calling driver. → Move
  computation into `_cmd_*()`, keep CLI parse-only.
- Broker read inside `_cmd_*()`. → Broker is a data channel, `_cmd_*` is a
  control channel. If the command needs data, take it as an argument or
  read the driver's own state, not the broker snapshot.
- Two modules touching each other's NVS keys directly. → Add a
  `<other>_cmd_*` call.
- `<module>_tile.c` including anything from another `<module2>_tile.c`. →
  Cross-tile calls must go through the command surface, not through UI.

---

## References

- `feedback_cli_first_testability.md` — every module must be reachable via
  CLI on a headless bench.
- `feedback_group_work_by_venue.md` — migration bundling.
- `feedback_stage_log_workflow.md` — session cadence around stage docs.
- `components/lvgl_ui/tile_registry.h` — tile descriptor contract used in §7.
- `components/nvs_cfg/nvs_cfg.h` — NVS pattern used in §4.
- `components/field_capture/fc_cli.c` — CLI dispatcher used in §6.1.

This document supersedes the folkloric "Blueprint 3 §5 / Blueprint 5 §7"
references scattered through the codebase — those were a personal shorthand
never committed anywhere. The tile-descriptor + registry contract that they
implicitly named lives in `tile_registry.h`; the module-surface contract
that they *didn't* name lives here.
