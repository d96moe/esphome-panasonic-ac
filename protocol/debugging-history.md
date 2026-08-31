# Panasonic Protocol Debugging History

Consolidated findings from reverse-engineering both the WLAN (DNSK-P11) and CNT
(CZ-TACG1/CN-CNT) protocols against real hardware, across multiple sessions.
Complements the raw byte maps in `protocol/query.txt` / `protocol/controller.txt`
and the external community doc
[ssjoholm/panasonic-cn-cnt](https://github.com/ssjoholm/panasonic-cn-cnt) with
what was actually found/fixed/tested on these specific units.

## Current Status (2026-08-31)

**Hardware** (see full table below): 3 units, 2 protocols.
**Deployed branch**: `heat-8-15-and-error-code` — all 3 units now on it.

| Area | Status |
|---|---|
| CNT real `hvac_action` (byte 12) | ✅ Working, validated live on hallway + cabin |
| WLAN error/fault codes | ✅ Working (0x89 response, ASCII, e.g. "H099") |
| CNT error/fault codes | ❌ Unknown — no byte identified. Detection tooling deployed to catch it live next time (anomaly_sensor, uart debug, mode-mismatch check) |
| CNT humidity (byte 20) | ❌ Not real on these units — constant `0xFF` on both hallway and cabin |
| CNT coil/room temp (byte 21) | ⚠️ Disputed — "coil temp" vs "duplicate of byte 18", not fully confirmed |
| CNT slot mechanism (byte 31-33) | ⚠️ Model-dependent — cabin (new) implements it, hallway (old) doesn't |
| `determine_action()` overwrite bug | ✅ Fixed 2026-08-31 (was silently discarding byte-12 hvac_action on every deployed branch since 2026-08-20) |
| `traits()` unconditional 8-30°C range | ✅ Fixed 2026-08-31 (now only widens when `heat_8_15_preset: true`) |
| Upstream PR #194 (byte-12 hvac_action) | 🕓 Open, no maintainer response yet (opened 2026-08-20) |
| Report to ssjoholm (byte 20/31 findings, CNT fault-code question) | 🕓 PR open: https://github.com/ssjoholm/panasonic-cn-cnt/pull/1 |

---

## Hardware

| Location | Indoor | Outdoor | Protocol | ESPHome device |
|---|---|---|---|---|
| Livingroom | CS-NZ25VKE | CU-NZ25VKE | WLAN (built-in factory WiFi, DNSK-P11) | `espthings-hvac-lroom` |
| Hallway | CS-LZ25TKE | CU-LZ25TKE | CNT (older unit) | `espthings-hvac-hallway` |
| Cabin | CS-CZ25ZKE | CU-CZ25ZKE | CNT (much newer) + built-in factory WiFi (Comfort Cloud, unused by ESPHome, runs in parallel) | `espthings-hvac-cabin` |

---

## WLAN protocol (DNSK-P11) — livingroom

### Key scanning (0x00-0xFF fullscan)

WLAN's poll response has a queryable per-key KV structure, which made a systematic
key-by-key scan possible (unlike CNT's single fixed-format packet). Full scan
completed; two new keys found beyond what the base component already decoded:

- **Key 0x85**: len=4, increments roughly once per 48s. Byte 1 = `0x35` constant,
  bytes 2-3 = 16-bit counter. Candidate for compressor runtime tracking — not
  fully confirmed.
- **Key 0x88**: len=1, value `0x42` (66 decimal) observed. Purpose unclear
  (bitfield? Hz? °C?) — needs an ON/OFF comparison to narrow down, not yet done.

Also investigated key 0x11/0x03 (a telemetry packet type referenced in some
docs) — **never observed from this AC at all**, ruled out as not applicable to
this unit.

### Error/fault codes — SOLVED

Directly readable as ASCII in the 0x89 poll response, byte offset 84-87. E.g.
`"H099"` = evaporator frost. This is how a real fault was diagnosed live: turned
out to be clogged dust filters, not a communication/UART issue as first
suspected. Implemented in the `error-code-sensor` feature (`error_code`,
`error_description`, `error_active`).

---

## CNT protocol (CZ-TACG1) — hallway + cabin

### Why CNT got less exploration than WLAN

CNT's poll is a single fixed command (`CMD_POLL`, 10 zero bytes) — there's no
way to "ask for" additional fields the way WLAN's per-key architecture allows.
This made systematic scanning impossible; the only way to find more in CNT is
to force the AC through many real operational states live and diff captured
packets by hand — much slower and more hands-on than WLAN's mechanical scan.
In hindsight, the extensive live state-cycling that *was* done (heat/cool/dry/
off, repeatedly) happened to be spent on livingroom (WLAN) mostly out of
convenience (main/most-accessible unit), not a deliberate protocol choice —
CNT would very likely have yielded its own findings sooner with the same
attention.

### Byte 12 — real `hvac_action` state machine

Implemented 2026-08-20, replacing the crude temperature-delta heuristic
(`determine_action()`) that WLAN still uses. Mapping:

```cpp
if (byte12 == 0x00) action = OFF;
else if ((byte12 & 0xF0) == 0x40) action = (byte12 == 0x40) ? IDLE : HEATING;
else if ((byte12 & 0xF0) == 0x30) action = (byte12 == 0x30) ? IDLE : COOLING;
else action = IDLE;  // 0x04/0x08 transient
```

All active states (`0x48`/`0x4C` heat start/run, `0x38`/`0x3C` cool start/run)
empirically confirmed live on real hardware, including the first-ever live
observation of `0x48` anywhere (previously only theorized). Only `0x44` (a
theorized heat sub-state) remains unobserved.

**Note**: [ssjoholm/panasonic-cn-cnt](https://github.com/ssjoholm/panasonic-cn-cnt)
independently discovered this same state machine back in **2025-12-13** — about
8 months before this fork implemented it. Their doc wasn't found until later
(via an unrelated search for heat-exchanger telemetry), which is the main
reason for the gap.

### `determine_action()` overwrite bug (found + fixed 2026-08-31)

`set_data()` had an *unconditional* `this->action = this->determine_action();`
at the very end of the function, running **after** the correct byte-12-derived
action was already set — silently discarding it on every single packet, on
every branch this was merged into (`error-code-sensor`,
`heat-8-15-and-error-code`) from the 2026-08-20 merge until this fix. Only the
original isolated PR branch, `hvac-action-from-state-byte`, never had this bug.
Reintroduced at least once by an upstream-sync merge before being caught and
fixed for good.

### Byte 20 — Humidity? Not on this hardware

ssjoholm's doc claims byte 20 = humidity %, externally validated via an
independent Zigbee sensor on their test unit. On both hallway and cabin here,
it reads a **constant `0xFF`** (255%, impossible) regardless of AC state.
Matches DomiStyle's own original `protocol/query.txt`, which labels this byte
"Marker" rather than humidity. Best guess: these specific units simply have no
physical humidity sensor, and `0xFF` is a not-populated placeholder. Reported
upstream: https://github.com/ssjoholm/panasonic-cn-cnt/pull/1

### Byte 21 — coil temp, or a duplicate of byte 18? Not fully resolved

Two disagreeing sources:
- **DomiStyle's own `query.txt`**: labels it "Current temperature" (i.e. a
  backup/duplicate of byte 18) — one captured sample packet showed identical
  values for both.
- **ssjoholm**: "Indoor coil/piping temperature", externally validated, notes
  Panasonic's own app shows byte 21 = byte 18 + 2°C (a consistent offset, not
  identical — would explain DomiStyle's single sample if captured at idle,
  when coil ≈ room temp).

A live divergence between byte 18 and 21 was observed on the cabin unit
(favors the coil-temp theory — true duplicates shouldn't diverge). Exposed as
`coil_temperature_sensor` on that basis, not fully confirmed either way.
Relevant to the still-open defrost/icing investigation (indoor coil temp may
be a leading indicator of outdoor coil icing).

### Byte 31-33 — multiplexed slot, model-dependent implementation

ssjoholm documents three rotating slot values: `0x80` (Slot 1, static
unit/model ID), `0xC0` (Slot 2, dynamic — NanoE-X on/off), `0xC1` (Slot 3,
static series ID). Confirmed on two more real units 2026-08-31:

- **Cabin (CS-CZ25ZKE, new)**: `0x80 0x53 0x01` — real Slot 1, very close to
  ssjoholm's own CS-HZ35ZKE row (`0x80 0x52 0x01`, differs by 1 in byte 32).
- **Hallway (CS-LZ25TKE, old)**: stable `0x00 0x00 0x00` across every packet
  captured (5 packets over 22s, live, while every other byte varied normally —
  ruling out noise). Not one of the three documented slots.

**Conclusion**: the multiplexed-slot mechanism itself appears to be
model/generation-dependent — cabin (newer) implements it, hallway (older)
doesn't (its `0x00` is an always-empty placeholder, not a real 4th slot).
Reported upstream alongside the byte 20 finding.

### `anomaly_sensor` — behavioral fault detection, no known error byte needed

Since no fault-code byte has been identified on CNT, built a different
approach: watch bytes documented as static/reserved (8, 9, 16, 17) — baselined
per-unit at boot, flagged on any deviation — plus byte 12/14 checked against
their known-value sets, plus byte 31 checked against a known-slot set (see
above). Also a behavioral check independent of any specific byte: if the AC
stops acknowledging mode/power commands for >5 minutes (`last_commanded_mode_byte_`
vs the reported `data[0]`), flags it — a plausible symptom of a fault-locked
unit, decoupled from decoding anything specific.

Deliberately silent during normal operation (low HA recorder noise) — only
publishes a full packet hex dump when something genuinely novel is seen.
Triggered by an H99 fault on hallway (2026-08-31) that went uncaptured because
none of this tooling existed yet.

Iteration note: the first deploy whitelisted only ssjoholm's three documented
slot values for byte 31, which fired on *every single packet* on hallway
(since hallway's real value, `0x00`, was never in that list) — fixed by adding
`0x00` after confirming via live capture it was a stable, real value and not
noise, rather than just disabling the check.

### `traits()` unconditional 8-30°C range bug (found + fixed 2026-08-31)

`traits()` set the visual temperature range to a static 8-30°C to accommodate
the `heat_8_15_preset` custom preset (8-15°C heat mode for winter house
shutdown) — but did so **unconditionally**, even on units with
`heat_8_15_preset: false` (the default), which had no reason to ever show a
widened, misleading range. Root cause of *why* it's static at all: ESPHome's
`visual_min/max_temperature` is only sent once, in `ListEntitiesClimateResponse`
at entity registration — never in regular state updates — so there's no
supported way to dynamically narrow/widen the slider based on which preset is
currently active without a full client reconnect (confirmed via live testing
in an earlier attempt, see fork commit history `fbe9aca`). Fix: gate the
widened minimum on `this->heat_8_15_preset_enabled_`.

---

## Dead ends — commercial/service interfaces investigated for CNT telemetry

Extensive research into whether any *other* documented Panasonic interface
exposes raw outdoor heat-exchanger temperature (for predictive defrost/icing
detection) or fault codes on CNT specifically. Four independent, correctly-
targeted sources all converged on the same negative result:

1. **PAC Checker Tool** (`ACC-CR-USB`) — connects via U1/U2 terminals (VRF
   central-control bus), wrong target class (30-circuit commercial framing).
2. **CZ-RTC5 wired remote** — same CN-CNT bus already used, but its documented
   installer codes are all user-facing config, not diagnostic readouts.
3. **Intesis `INMBSPAN001R000`** Modbus gateway — R1/R2 bus, wrong Panasonic
   product line (ECOi/PACi commercial/VRF, not the residential Etherea line
   these units belong to).
4. **Intesis `PA-AC-MBS-1`** — explicitly for the **Etherea line** (matches
   these units), and *does* connect via CN-CNT. Its Modbus register map
   proves useful data exists in the byte stream that's never been decoded
   publicly by anyone:
   - Register 53 "Compressor Status": 0 Off / 1 To off / 2 To on / 3 On
   - Register 58 "Demand Response (DRM level)": 0 Normal / 1-3 throttled —
     directly relevant to existing peak-shedding automation if ever cracked
   - Detailed fault-code table naming the actual sensors (H28/H32 = outdoor
     heat exchanger temp sensor 1/2), but only as pass/fail codes, never a
     live numeric register — same "flags only, no raw telemetry" pattern as
     everywhere else.

**Bottom line**: even the one gateway targeting the exact right product class
and physical port only exposes fault codes, never live sensor values. A
predictive (not just reactive) defrost-risk signal is very likely unobtainable
through any standard/documented Panasonic interface for this product line —
Compressor Status / DRM level remain a live lead (proven to exist, byte
offsets unknown) if anyone ever wants to dig further.

---

## Branch/PR reference

- **Production branch**: `heat-8-15-and-error-code` — combines error-code-sensor
  (WLAN), heat_8_15 preset (both protocols), byte-12 hvac_action (CNT),
  anomaly detection, humidity/coil sensors. Deployed to all 3 units.
- **Upstream PR #194** (DomiStyle/esphome-panasonic-ac): byte-12 hvac_action,
  source branch `hvac-action-from-state-byte` (NOT `cnt-action-from-state-byte`
  — a similarly-named but different, stale branch with the overwrite bug still
  present — always verify via `gh pr view 194 --json headRefName` rather than
  trusting a remembered name).
- **Weekly auto-sync**: Jenkins job `esphome-panasonic-ac-sync`
  (`ci/Jenkinsfile.sync`), Mondays 04:00, merges upstream `master` into fork
  branches including `heat-8-15-and-error-code`. This is the mechanism that
  reintroduced the `determine_action()` bug at least once — any manual fix to
  a synced branch should be double-checked after the next sync run.
