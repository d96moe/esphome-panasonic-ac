# Panasonic WLAN Protocol Investigation — Branch `protocol-investigation`

## Syfte

Denna branch innehåller debug-infrastruktur för att avkoda okända delar av
Panasonic DNSK-P11 WLAN-protokollet (0x89 CMD_POLL-svar).

**KRITISK SÄKERHETSBEGRÄNSNING:** Alla ändringar är passiva READ-operationer.
Inget 0x10 0x08 (SET), ingen replay, ingen blind-scan av skrivkommandon.
Protokollet kan innehålla kalibrerings- och servicekommandon som kan skada pumpen.

---

## Vad branchen lägger till (utöver `error-code-sensor`)

### Debug-sensorer (HA text sensors, dormanta utan YAML-config)
- `debug_telemetry_1/2` — fångar 0x11 0x03 telemetripaket (split pga HA 255-char-gräns)
- `debug_report` — dumpar 0x10 0x0A unsolicited reports före parsning
- `debug_unknown` — fångar 0x3A-header och okända pakettyper
- `probe_value` — visar svar på en probe-nyckel (Phase B)

### Phase B — KV-nyckel probe
- `probe_key` (HA number entity, 0–255) → lägger till en READ-nyckel i nästa poll
- Returnerar svar via `probe_value`-sensorn
- Används för att testa okända KV-nycklar utan att ändra AC-state

### Permanenta tillägg (finns även i denna branch)
- `key_0x85` — 4-byte KV-nyckel, pollad varje cykel
- `key_0x88` — 1-byte KV-nyckel, pollad varje cykel
- `raw_packet_2` — overflow-sensor (bytes 127+) pga HA 255-char-gräns
- CMD_POLL utökat från 17 till 19 nycklar

---

## Utförda tester och resultat

### Fullscan KV-nycklar 0x00–0xFF (2026-06-12) — KLAR

Alla 256 möjliga KV-nycklar provade via probe_key-mekanismen.
Utöver de 17 standard-nycklarna hittades **exakt 2 supportade nycklar**:

| Nyckel | Längd | Funktion |
|--------|-------|----------|
| 0x85   | 4 byte | Kumulativ energiräknare (se nedan) |
| 0x88   | 1 byte | Statiskt konfig-bitfield (se nedan) |

Alla övriga nycklar (0x30, 0x36–0x43, 0x50–0x52, 0x60, 0x70, 0x81–0x84,
0x87, 0xA2–0xA3, 0xA6–0xA7, 0xB1, 0xB3–0xB4, 0xBC–0xBD, 0xBF–0xC1,
och resten upp till 0xFF) returnerade "ej supportad" (128-byte svar).

---

### Key 0x85 — Kumulativ energiräknare (2026-06-13)

Verifierat mot heat/cool-cykler med extern Shelly 1PM-effektmätning.

**Byte-layout (4 byte, big-endian):**

```
Byte 0: 0x00  — konstant, high-order (ändras troligen aldrig i praktiken)
Byte 1: 0x35  — konstant i observation; ändras var ~65 kWh (månadsvis)
Byte 2-3:     — 16-bit lågordningsräknare, inkrement proportionellt mot last
```

**Räknartakt vs effekt (verifierat 2026-06-13):**

| Effekt (W) | Ticks/30s | Wh(calc) | Wh/tick |
|-----------|-----------|----------|---------|
| 4–7 (idle) | 1 / ~60s | 0.07–0.17 | basminimum |
| 130–180    | 1–2      | 1.1–1.5  | ~1.3    |
| 570        | 5        | 4.8      | 0.96    |
| 860–890    | 6–9      | 7.2–7.5  | ~1.0    |
| 1290–1550  | 13       | 10.8–12.9| ~1.0    |

**Slutsats:** Vid kompressordrift ≈ 1 Wh per tick.
Vid idle kvarstår en basminimumtakt (1 tick/min) oavsett effekt.

Total installationsenergi per 2026-06-13: ~3 478 kWh (räknare 0x003512XX).

**Byte 1 (0x35) är INTE kompressorfrekvens Hz.** Det är ett fruset
högreordningsbyte som ändras ungefär månadsvis.

**0x85 är INTE ett binärt kompressor-på/av-larm.**

---

### Key 0x88 — Statiskt konfig-bitfield (2026-06-13)

Konstant `0x42` = `0b01000010` igenom ALLA observerade driftlägen:
- AC AV / AC PÅ
- Idle (5W), kompressor låg (150W), kompressor hög (1550W)
- Kylläge, värmeläge

Oanvändbar för runtime-statusövervakning.
Möjlig tolkning: kapabilitets-/modellkod satt vid produktion.
**Defrost-test rekommenderas vintertid** (<0°C utomhus) — 0x42 kan
möjligen ändras under defrost-cykel.

---

### Byte-diff raw_packet IDLE vs COMPRESSOR (2026-06-13)

Enda bytes som skiljer sig mellan kompressor-idle och kompressor-aktiv:
- Byte 14 (key 0x80): 0x30=PÅ / 0x31=AV (power switch — inte kompressorstatus)
- Byte 18 (key 0xB0): 0x42=kyla / 0x43=värme (användarläge)
- Byte 22 (key 0x31): setpoint (satt av användaren)
- Byte 62 (key 0xBB): rumstemperatur (dynamisk)

**Inget okänt kompressor-aktivt-flag existerar i 0x89-svarets 19 KV-nycklar.**

---

### 0x11 0x03 telemetripaket — ALDRIG sett från denna enhet

Trots 60+ min test inkl. kompressordrift (bekräftat av beebop5-fork som
dokumenterar paketet för andra enheter): denna Panasonic-variant skickar
inte 0x11 0x03 spontant.

---

### 0x10 0x0A report-paket — avkodat

Skickas enbart vid användariniterade ändringar. Kompressorns ON/OFF-cykel
triggar inget report-paket.

KV-strukturen är identisk med 0x89-svarets KV-par.
Exempel debug_report: `5A55100A00090001300101023101289F`
= setpoint ändrat till 0x28/2 = 20°C.

---

### Comfort Cloud effektdata — troligen molnestimerat

Inga okända pakettyper fångades (debug_unknown tom).
Comfort Cloud visar effektdata men inget sådant värde finns i UART-protokollet.
Slutsats: Panasonics moln beräknar troligen effekten baserat på modell/mode/temp,
den mäts inte ur AC:n via UART.

---

## Öppen hypotes — okänd TX-kommandotyp

Det är möjligt att DNSK-P11-modulen skickar kommandon vi aldrig provat
(t.ex. `0x10 0x05` eller liknande) som får AC:n att returnera effekt-/kompressordata.

**För att verifiera:** Koppla spare ESP32-C3 som passiv UART-sniffer parallellt
med original DNSK-P11 (RX-only på båda linjerna, ingen TX). ESP32-C3 har 2
hardware-UART:ar — en per riktning. Logger på baud_rate: 0, output via MQTT.
Planeras som separat framtida projekt.

---

## Relaterade HA-objekt (skapade under investigation)

- `input_number.protocol_test_{idle,heat,cool}_minutes`
- `script.panasonic_re_heat_cycle` / `_cool_cycle`
- `automation.Panasonic RE: Compressor START/STOP marker`
- recorder-glob `sensor.*panasonic_hvac_livingroom_debug_*`

---

## Status (2026-06-13)

Investigation avslutad. Lroom återställd till `@error-code-sensor` + BT proxy på.
Denna branch (`protocol-investigation`) sparas intakt för:
- Defrost-test höst/vinter 2026
- Eventuell UART-sniffer-undersökning
