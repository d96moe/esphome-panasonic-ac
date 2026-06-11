# WLAN Protocol Investigation — finding the live working-mode / compressor state

**Goal (priority 1):** read the AC's *live working mode* over the DNSK-P11 WLAN link —
whether the unit is **idle / heating / cooling / defrosting** — so Home Assistant can show a
real `climate action` instead of a temperature-based guess.

This branch (`protocol-investigation`) adds **read-only protocol debugging** on top of the
existing `error-code-sensor` functionality. It changes only how we *interpret bytes the AC
already sends us*. It does not poll new keys by default and sends nothing new to the AC.

---

## Why this exists — what we already proved

On 2026-06-10/11 we ran a live test on the livingroom AC (`type: wlan`) using the Shelly power
meter (`sensor.heatpump_livingroom_power`) as ground truth, forcing the compressor ON (heat 28 °C,
up to 583 W) and OFF (heat 16 °C, ~3 W):

> **The compressor state is NOT in the 17 keys we poll.** Every unknown byte (44–79, incl.
> 76–79 `86 2E 2A 00` and 80–83 `00 0B 01 01`) was byte-identical at 3 W and 583 W. Only the
> indoor-temperature byte (62) moved.

See [reference_panasonic_wlan_packet] in the assistant memory and `controller.txt` (byte map).

So the signal must live in a **channel we currently drop**, or in a **poll key we don't request**.

---

## The three data channels

| Channel | Type | New fields without TX change? |
|---|---|---|
| **Poll response `0x10 0x89`** | Subscription — AC answers *only* the 17 keys in `CMD_POLL` | **No** — a new key must be added to the poll (a TX change; it is a *read*, key `0x10 0x09`) |
| **Report `0x10 0x0A`** | Push — AC sends *unsolicited* on state change, picks its own keys | Yes, pure RX |
| **Telemetry `0x11 0x03`** | Push — AC sends *periodically*, unsolicited (~160 bytes) | Yes, pure RX |

**Phase A** mines the two push channels (no TX change). **Phase B** (dormant) appends one extra
*read* key to the poll, only if Phase A is exhausted.

---

## What this branch adds (firmware)

All additions are optional in YAML; with none configured the build is behaviourally identical to
`error-code-sensor`.

### New debug text sensors (pure RX taps, raw hex)
| Sensor | Captures | Notes |
|---|---|---|
| `raw_packet` | `0x10 0x89` poll response | already existed |
| `debug_telemetry_1` | `0x11 0x03` bytes 0–124 | telemetry is ~160 B; split because HA caps a state at 255 chars |
| `debug_telemetry_2` | `0x11 0x03` bytes 125–end | |
| `debug_report` | `0x10 0x0A` report, dumped *before* parsing | pushed exactly on state change — prime channel |
| `debug_unknown` | any otherwise-dropped packet, incl. `0x3A`-header frames | |

Each publishes only on change (dedup); the full bytes also always go to the ESPHome DEBUG log.

### Protocol handling changes
- **`0x3A` header tap** (`esppac_wlan.cpp` loop): frames with the AC→controller header `0x3A`
  are normally dropped at the header check, so the working state machine has never processed
  them. We capture them to `debug_unknown` and drop them — **we do not feed them to
  `handle_packet`**, so counters/state/TX are byte-for-byte unchanged.
- **`0x11 0x03` branch** in `handle_packet`: capture telemetry to `debug_telemetry_*`.
- **Report dump**: `update_debug_report()` at the top of the existing `0x10 0x0A` branch
  (the report ACK we already send is unchanged).
- **Unknown dump**: `update_debug_unknown()` in the final `else`.
- **Relaxed size check**: query response `!= 125` → `< 70` (reject). A probe key or firmware
  variant can return a larger packet; the extra bytes are preserved in `raw_packet`.

### Phase B scaffolding — DORMANT
`probe_key` (YAML, hex uint8, default absent/0): when set, `build_poll_with_probe()` appends that
one key to the poll request so its echoed value lands in `raw_packet`. With `probe_key` absent the
original static `CMD_POLL` is sent verbatim (byte-identical — verified: builder reproduces it).

> **Safety:** polling is a *read* (`0x10 0x09`). It cannot write AC state. The dangerous
> service/calibration commands are *set* commands (`0x10 0x08`), which this branch never touches.
> **Caveat:** appending a key shifts the post-KV fields (error code 84–87 etc.) by +4, so during an
> active probe rely on `raw_packet`, not the parsed sensors. Probe **one key at a time**, supervised,
> then remove `probe_key`.

---

## Attack plan

### Phase A — RX mining (no TX change)
1. **Recon (free):** in the ESPHome log look for `Dropping invalid packet (header)` and
   `Received unknown packet` — presence proves telemetry/0x3A traffic is already arriving and
   being discarded.
2. **Build & deploy** to the lroom WLAN device (`esphome-web-044794`, `espthings-hvac-lroom`).
   Only lroom is relevant — hallway is CNT, not WLAN.
3. **Active cycles** (VT must be off — the scripts abort otherwise):
   - `script.panasonic_re_heat_cycle` — idle → heat 28 °C → idle (compressor ON at night).
   - `script.panasonic_re_cool_cycle` — idle → cool 16 °C → idle (run on a **warm afternoon**;
     cooling is blocked by the AC when outside temp is ~<10 °C — that's why the 8 °C cool test
     produced no compressor start).
   - Durations tunable via `input_number.protocol_test_{idle,heat,cool}_minutes`.
   - Each phase boundary writes a `Panasonic RE` logbook marker. The
     `Panasonic RE: Compressor START/STOP marker` automations add markers for any (also VT-driven)
     transition. Thresholds: >150 W = ON, <100 W for 2 min = OFF (fan alone ~75 W, crankcase ~4 W).
4. **Passive:** everything is recorded continuously (36-day retention), so spontaneous transitions
   are captured too.

### Phase B — poll probing (minimal TX, gated, only if Phase A fails)
Candidate keys (do **not** blind-scan 0x00–0xFF): keys the AC pushes in Reports; CNT keys from
`query.txt`; the handshake capability packet `0x10 0x81`. Set `probe_key: 0xNN` for one key, deploy,
observe its echoed value in `raw_packet` across compressor transitions, then remove it.

### Phase C — productify
Map the identified byte → `climate action` (a real `determine_action()`) and/or a
`compressor_active` binary sensor → merge to `error-code-sensor` → master → tag.

---

## Analysis pipeline
1. Find transition timestamps: `Panasonic RE` logbook markers / the START/STOP automations.
2. Pull the debug + `raw_packet` sensor states in a ±5 min window around each transition
   (recorder MariaDB; `mysql_query` MCP).
3. Position-wise hex diff across ≥3 cycles. Bytes that consistently track the compressor =
   candidates; bytes that drift continuously = temperatures/counters.
4. Confirm against at least one spontaneous (non-script) transition.

---

## Home Assistant objects created
- **Recorder** (`configuration.yaml`): glob `sensor.*panasonic_hvac_livingroom_debug_*`.
- **Helpers:** `input_number.protocol_test_{idle,heat,cool}_minutes`.
- **Scripts:** `script.panasonic_re_heat_cycle`, `script.panasonic_re_cool_cycle`.
- **Automations:** `Panasonic RE: Compressor START marker`, `… STOP marker`.

The snapshot automations and scripts only log / issue normal climate commands; nothing here can
reach raw protocol or service modes.

---

## Safety contract (the whole point)
This is **non-intrusive reverse engineering**, not control hacking.
- No new bytes are sent to the AC in Phase A. Phase B sends only *read* polls, one extra key, gated.
- No `set` (`0x10 0x08`) commands with unknown keys. No replay of captured command sequences.
- The only thing that changes AC behaviour is the test scripts issuing **normal** setpoint/mode
  commands — exactly what the remote/HA already do.
- Worst case (a parse bug reboots the ESP): the AC keeps running its own program; `serial_fault`
  flags it; rollback = point the device YAML back to `@error-code-sensor` and OTA.

---

## References
- beebop5 fork (genuine UART captures, `PROTOCOL_DOCUMENTATION.md`):
  https://github.com/beebop5/esphome-panasonic-ac
- Upstream: https://github.com/DomiStyle/esphome-panasonic-ac
- HA community (DNSK-P11 replacement): https://community.home-assistant.io/t/wifi-interface-for-panasonic-airconditioning-drop-in-dnsk-p11-replacement/326865
