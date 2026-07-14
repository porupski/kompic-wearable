# Kompic̄ Mk I — Flashlight LED Leakage Fault (Diagnostic Handoff)

**Purpose:** hardware fault diagnosis. This is one of two independent problems in
this package; a second section (magnetometer) may follow. Treat this as fully
self-contained. Full device context (the Kompic̄ system instructions) may or may
not accompany — everything needed to reason about *this* fault is below.

**What I want back:** the single most probable root cause, the minimal
test/repair sequence to confirm and fix it, anything in the evidence that
contradicts the leading hypothesis, a cheaper discriminating test than the ones
listed if one exists, and any failure mode not yet considered. Push back — do not
just ratify the current hypothesis.

---

## 1. Device context (minimal)

- Kompic̄ Mk I: ESP32-S3-WROOM-1U, 4-layer 1 mm PCB, **hand-reflow assembly**, all parts from LCSC.
- Flashlight = a single white LED on the **firmware-gated +5 V PMID boost rail** (from a BQ25619 charger IC). The same +5 V rail also feeds MAX30101 LED drive and a WS2812B. The boost is on during normal operation.
- GPS module (MAX-M10S) is **not yet installed**. u-blox forbids solvent cleaning of the M10S, so any solvent work on the board must be finished **before** the GPS goes on. This fault is the blocker for GPS install.

---

## 2. Circuit as-built (the working-except-for-the-leak version)

Low-side N-channel switch:

```
+5V ──[100R]──▶│─── (LED, white) ─── D
                                      │
                                   [BSS138W]  N-ch, SOT-323
                                      │
                                      S ──── GND

GPIO41 ──[47R]── GATE ──[10k]── GND
```

- LED: anode toward the 100R/5V side, cathode to the FET **drain**.
- FET: BSS138W (onsemi, SOT-323, logic-level N-ch). **Source to GND.**
- Gate: driven from **GPIO41** through a 47R series resistor; **10k pulldown** gate→GND.
- GPIO41 is the ESP32-S3 **MTDI** pin (has internal-pull behavior during the boot window; not a hard strap in this use). At runtime the pin is owned by the ESP32 **LEDC** peripheral (PWM, attached at 1 kHz / 8-bit).

**Prior dead-end, already resolved (not the current issue):** the first build put
the LED in the *source* leg (source-follower). Source degeneration collapsed
V_GS and the LED never lit. Moving the LED into the drain leg (above) fixed
that. The topology above is correct and is not in question.

---

## 3. Symptom (precise)

- **The moment the +5 V rail comes online, the LED shows a steady, clearly-visible white illumination.** Not a faint glow — a definite "this is ON" indicator level. Won't illuminate a surface, but unmistakably lit. Estimated **1–4 mA**.
- **Rail-triggered, not command-triggered.** Present at boot before any GPIO41 command and regardless of firmware state. Commanding the flashlight OFF does nothing. In flashlight mode, scrolling brightness to minimum lands at *the same* illumination — i.e. this leak is the floor, not a commanded PWM level.
- **Steady, not flickering.** It tracks the +5 V rail, not GPIO41.

For scale: full-on through the 100R from 5 V into a ~3 V white LED is ~20 mA.
1–4 mA is therefore *partial* conduction of the LED branch — a weakly-open
channel or a leak path passing a fraction of full current.

---

## 4. Firmware state (so it can be excluded)

- GPIO41 is driven by LEDC. In the off state `ledcWrite(pin, 0)` holds a hard push-pull **0 V** on the gate — this actively sinks anything an internal ~45k pull-up could source, so a floating-gate subthreshold glow cannot occur while LEDC is holding duty 0.
- Boot-window hardening was added before `ledcAttach`: `pinMode(OUTPUT)` + `digitalWrite(LOW)` + `gpio_set_pull_mode(FLOATING)`, to define the gate from reset until LEDC takes the pin.
- **None of the firmware changes moved the symptom.** Conclusion: this is **not** a gate-drive / firmware bug. The leak is passive and gated by the 5 V rail.

---

## 5. Measurements

| Measurement | Condition | Result | Expected |
|---|---|---|---|
| Gate voltage | off state, internal pull-up OFF + internal pull-down ON | **~1.0 V** | ~0 V |
| Gate → GND resistance | in-circuit, 5 V off | intermittently ~10k, sometimes steady **~22k** | ~10k |

**On the gate voltage:** BSS138W V_th(min) = 0.8 V, typ 1.3 V. A gate sitting at
~1.0 V is right at threshold — enough for weak channel conduction, which matches
the few-mA illumination. That the internal pull-down is ON and the gate *still*
reads ~1 V means an external **positive injection** is overpowering both the
internal pull-down (~45k) and the external pull-down.

Order-of-magnitude injection: to hold ~1 V across (~45k internal ∥ flaky ~10k
external), the injected current is on the order of **~100 µA**, sourced from a
node that is only live when +5 V is up.

**On the resistance anomaly:** the only DC paths gate→GND are the 10k pull-down
and the 47R into GPIO41 (the FET gate is DC-open; GPIO41 is high-Z when
unpowered / driven otherwise). Any genuine parallel path can only read **below**
10k. Reading **22k** therefore means the 10k pull-down is **not solidly
connected** (cold/intermittent joint), and the meter is seeing a higher-impedance
residual path instead of the resistor.

---

## 6. Ruled out

- **Overcurrent / thermal damage to the FET.** BSS138W: 210 mA continuous, 840 mA pulsed, 340 mW. Peak LED current ever run ≤ ~50 mA (~20 mW). The load never came close to stressing the part.
- **Firmware / gate drive** (Section 4): symptom is rail-gated, not command-gated; firmware changes had zero effect.
- **Topology / design error** (Section 2): low-side NMOS with the LED in the drain leg is the correct way to switch a 5 V LED rail from a 3.3 V logic pin. Not a schematic mistake.
- **Intrinsic device leakage.** BSS138W off-state I_DSS is in the **nA** range. Genuine device leakage cannot produce mA-scale illumination. mA off-current is external, not intrinsic to a healthy FET.

---

## 7. Assembly context that matters

- Hand-reflowed with **solder paste** applied around the FET; **heavy flux residue** in the area. Operator (me) considers it plausible that **solder pellets** were trapped under or beside the FET during reflow.
- 1–4 mA *steady and clearly on* leans more toward a **low-ohm metallic path (pellet)** than flux contamination — flux leakage usually presents as a fainter hundreds-of-kΩ trickle (tens of µA at the LED node), a dimmer glow. Not conclusive, but a lean.

---

## 8. Candidate root causes (current hypotheses — for pressure-testing, NOT settled)

1. **Pellet / bridge drain→gate under the FET.** Injects positive into the gate node, holds it at ~1 V, weakly opens the channel → few mA. Explains gate = 1 V *and* the rail-triggered behavior.
2. **Pellet / channel leak drain→source.** Passes LED current independent of the gate entirely. Would survive a gate-to-GND short.
3. **Cold joint on the 10k pull-down** (the 22k anomaly). Weakens the pull-down and lets whatever is injecting win more easily. Likely a **compounding** factor, not the sole cause.

**Leading read: (1) + (3) together.** But (2) is not excluded and would be the
worse outcome.

---

## 9. Decisive tests **not yet performed**

1. **Gate-to-source jumper.** Hard-wire gate directly to source/GND, 5 V online, flashlight commanded off.
   - **Glow dies** ⇒ gate-side leak (hypothesis 1, or the pin genuinely not driving low) → clean/reflow path.
   - **Glow persists** ⇒ drain–source leak (hypothesis 2) → FET must come off; clean/inspect/replace, no gate control can fix it.
   - This is one wire and it **halves the problem space**. Should be step 1.
2. **Reflow the 10k pull-down;** confirm a steady ~10k gate→GND afterward.
3. **Local IPA / flux-remover clean of the FET region** (see constraints, Section 11). **Not a board flood.**
4. **If cleaning fails:** lift the FET, inspect the pads and under-part region under magnification for a pellet, diode-test the FET (Section 10), reflow a fresh part (or the same one if it tests good).

---

## 10. Part facts — BSS138W (onsemi, SOT-323)

- N-ch logic-level. **V_th 0.8–1.5 V (typ 1.3 V).** R_DS(on) ~3.5 Ω @ V_GS 10 V, ~6 Ω @ 4.5 V.
- I_D 210 mA continuous, 840 mA pulsed. P_D 340 mW.
- Off-state I_DSS ~ **nA** — device leakage is orders of magnitude below the observed mA.
- **SOT-323 pinout: pin 1 = Gate, pin 2 = Source, pin 3 = Drain** (confirm against the package marking box before trusting probe placement).
- Diode-test of a good part (part removed): Gate–Source and Gate–Drain **open both directions** (insulated gate). Source→Drain body diode ~0.5–0.7 V one way, open the other. Any short/low reading across the gate = punched oxide = dead; low/shorted S–D both ways = channel leak.

---

## 11. Cleaning constraints (if a solvent step is recommended)

- **MSM261 MEMS mic port is on the board.** Kapton tape over the acoustic port, **no ultrasonic**, **no flooding** — IPA wicks into the cavity and wets the diaphragm.
- **BME688 gas port is on the board.** The MOX element treats IPA as a VOC (it is literally built to detect it); vapor contaminates it and it will read garbage gas values until a long bake-out. Tape the port, keep solvent out of that corner.
- **Local application only:** flux-remover pen or IPA on an acid brush at the FET area → agitate → wick with lint-free swabs → compressed-air dry → gentle warm pass to drive off residual solvent. GPS is absent, so no M10S solvent restriction yet — but finishing this *before* GPS install is the whole point.

---

## 12. Verification target (fault considered closed when)

+5 V online, flashlight commanded OFF: **gate reads ~0 V, LED fully dark, gate→GND
a solid ~10k.** If the gate still sits high after a confirmed-good pull-down and a
proper clean, the FET has an internal gate-drain leak → replace and re-verify.
