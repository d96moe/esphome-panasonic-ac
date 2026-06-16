# Panasonic WLAN Protocol Investigation — Branch `protocol-investigation`

## Purpose

This branch contains debug infrastructure for decoding unknown parts of the
Panasonic DNSK-P11 WLAN protocol (0x89 CMD_POLL response).

**CRITICAL SAFETY CONSTRAINT:** All changes are passive READ operations only.
No 0x10 0x08 (SET), no replay, no blind-scan of write commands.
The protocol may contain calibration and service commands that could damage the unit.

---

## What this branch adds (on top of `error-code-sensor`)

### Debug sensors (HA text sensors, dormant without YAML config)
- `debug_telemetry_1/2` — captures 0x11 0x03 telemetry packets (split due to HA 255-char limit)
- `debug_report` — dumps 0x10 0x0A unsolicited reports before parsing
- `debug_unknown` — captures 0x3A-header and unknown packet types
- `probe_value` — displays the response to a probe key (Phase B)

### Phase B — KV-key probe
- `probe_key` (HA number entity, 0–255) → adds one READ key to the next poll
- Returns response via the `probe_value` sensor
- Used to test unknown KV-keys without changing AC state

### Permanent additions (also present in this branch)
- `key_0x85` — 4-byte KV-key, polled every cycle
- `key_0x88` — 1-byte KV-key, polled every cycle
- `raw_packet_2` — overflow sensor (bytes 127+) due to HA 255-char limit
- CMD_POLL extended from 17 to 19 keys

---

## Tests performed and results

### KV-key scan (2026-06-12)

A systematic scan of KV-keys was performed via the probe_key mechanism,
covering candidates in the 0x00–0xFF range (excluding the 17 already-known
standard keys). An automated scan script was used but full coverage of all
256 keys cannot be confirmed with certainty.

Among all keys tested, exactly **2 additional supported keys** were found:

| Key  | Length | Function |
|------|--------|----------|
| 0x85 | 4 byte | Cumulative energy counter (see below) |
| 0x88 | 1 byte | Static config bitfield (see below) |

The following candidate keys were explicitly tested and returned "not supported"
(128-byte response): 0x30, 0x36–0x43, 0x50–0x52, 0x60, 0x70, 0x81–0x84,
0x87, 0xA2–0xA3, 0xA6–0xA7, 0xB1, 0xB3–0xB4, 0xBC–0xBD, 0xBF–0xC1.

---

### Key 0x85 — Unknown cumulative counter (2026-06-13)

Observed during heat/cool cycles with external Shelly 1PM power measurement.

**Byte layout (4 bytes, big-endian):**

```
Byte 0: 0x00  — constant, high-order
Byte 1: 0x35  — constant during observation; changes slowly over time (months?)
Byte 2-3:     — 16-bit counter, increments faster under load
```

**Tick rate vs. power (observed 2026-06-13):**

| Power (W)   | Ticks/30s |
|-------------|-----------|
| 4–7 (idle)  | 1 / ~60s  |
| 130–180     | 1–2       |
| 570         | 5         |
| 860–890     | 6–9       |
| 1290–1550   | 13        |

**Observation:** The counter increments faster at higher load. However, a
minimum tick rate persists even at near-zero idle load (1 tick/min at ~5 W),
which is inconsistent with a pure energy counter. The true meaning is unknown.

**Possible interpretations:**
- **Compressor runtime/load counter** — possibly counts compressor-minutes weighted by load or frequency; the 1 tick/min baseline at idle could be a heartbeat, with faster ticking under active compressor operation
- Cumulative energy (Wh) — correlation with power is approximate (~1 Wh/tick under compressor load), but the idle tick rate does not fit a pure energy interpretation
- Some other load-proportional operational metric

The runtime/load counter hypothesis is considered more likely given the idle behaviour.

**Counter value as of 2026-06-13:** 0x003512XX (bytes 2–3 changing).
Byte 1 (0x35) is stable but is NOT compressor frequency in Hz.

**0x85 is NOT a binary compressor on/off flag.**

---

### Key 0x88 — Static config bitfield (2026-06-13)

Constant `0x42` = `0b01000010` across ALL observed operating modes:
- AC OFF / AC ON
- Idle (5 W), compressor low (150 W), compressor high (1550 W)
- Cool mode, heat mode

Unusable for runtime status monitoring.
Possible interpretation: capability/model code set at production.
**Defrost test recommended in winter** (<0°C outdoor) — 0x42 may
possibly change during a defrost cycle.

---

### Raw packet byte-diff IDLE vs COMPRESSOR (2026-06-13)

The only bytes that differ between compressor-idle and compressor-active:
- Byte 14 (key 0x80): 0x30=ON / 0x31=OFF (power switch — not compressor status)
- Byte 18 (key 0xB0): 0x42=cool / 0x43=heat (user-selected mode)
- Byte 22 (key 0x31): setpoint (set by user)
- Byte 62 (key 0xBB): room temperature (dynamic)

**No unknown compressor-active flag exists in the 19 KV-keys of the 0x89 response.**

---

### 0x11 0x03 telemetry packet — NEVER seen from this unit

Despite 60+ minutes of testing including compressor operation (confirmed by the
beebop5 fork which documents the packet for other units): this Panasonic variant
does not send 0x11 0x03 spontaneously.

---

### 0x10 0x0A report packet — decoded

Sent only on user-initiated changes. Compressor ON/OFF cycles do not trigger
a report packet.

KV structure is identical to the 0x89 response KV-pairs.
Example debug_report: `5A55100A00090001300101023101289F`
= setpoint changed to 0x28/2 = 20°C.

---

### Comfort Cloud power data — likely cloud-estimated

No unknown packet types were captured (debug_unknown empty).
Comfort Cloud displays power data but no such value exists in the UART protocol.
Conclusion: Panasonic's cloud likely estimates power based on model/mode/temperatures;
it is not measured from the AC via UART.

---

## Open hypothesis — unknown TX command type

It is possible that the DNSK-P11 module sends commands we have never tried
(e.g. `0x10 0x05` or similar) that cause the AC to return power/compressor data.

**To verify:** Connect a spare ESP32-C3 as a passive UART sniffer in parallel with
the original DNSK-P11 (RX-only on both lines, no TX). The ESP32-C3 has 2 hardware
UARTs — one per direction. Logger on baud_rate: 0, output via MQTT.
Planned as a separate future project.

---

## Related HA objects (created during investigation)

- `input_number.protocol_test_{idle,heat,cool}_minutes`
- `script.panasonic_re_heat_cycle` / `_cool_cycle`
- `automation.Panasonic RE: Compressor START/STOP marker`
- recorder glob `sensor.*panasonic_hvac_livingroom_debug_*`

---

## Status (2026-06-13)

Investigation complete. Lroom restored to `@error-code-sensor` + BT proxy on.
This branch (`protocol-investigation`) is kept intact for:
- Defrost test autumn/winter 2026
- Possible UART sniffer investigation
