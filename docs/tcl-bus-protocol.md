# TCL indoor-unit bus — protocol notes

> Reverse-engineering notes behind the `tcl_bus` component. The original sniffer used a plain ESP32 with a separate isolated RS-485 module (RX = GPIO16, TX = GPIO17). The wall panel in this repo uses the Waveshare board's own RS-485 port on GPIO43/44 instead.

Reverse engineered Oct 2026 from an ESP32 + RS-485 sniffer.
Unit firmware string seen on the bus: `ACMCU/V9-R10ET53AC-FV001.001.015` (TCL).

## Physical / framing
- RS-485, **9600 baud, 8 data bits, EVEN parity, 1 stop (8E1)** — measured from pulse timings (bit ≈ 104 µs).
- Not Midea XYE (no `AA…55` frames at any baud).
- CRC-16/MODBUS (poly 0xA001 reflected, init 0xFFFF), **low byte first**, at the end of every frame.
- Poll (from master, the indoor board): `F1:<addr>:5A:<crc lo>:<crc hi>` — always 5 bytes.
- Response: `<class>:<addr>:<cmd>:00:<LEN>:<LEN payload bytes>:<crc lo>:<crc hi>` → total = LEN + 7.
  - Header rule: class = `X0`, addr = `Xn` (e.g. `10:13`, `20:23`, `80:81`).
- Polled addresses (~every 1.5–1.9 s): 11, 13, 1A, 1C, 21, 23, 2A, 2C, 2D, 81.
- **ESP32 UART pitfall:** with the default `rx_full_threshold` of 7, the last <7 bytes of a frame (and whole 5-byte polls) can sit in the UART for 80+ ms. Never frame by inter-byte timing; use header + length + CRC, and set `rx_full_threshold: 1`. A 60 ms "stale frame" timeout split valid 237-byte frames and lost polls.
- **Electrical glitches:** the idle bus floats (no fail-safe bias seen by the ESP's module), so `FF`/`FD` glitch bytes appear between frames — mainly in heat mode once the compressor/indoor fan run; a full cooling run showed none. A glitch burst running into a poll corrupts the poll's first byte (`F1` read as e.g. `C5`, `E2`, `7F`); the rest of the poll is intact. Original hardware: Waveshare TTL TO RS485 (B) isolated module — turn its 120R termination switch off and connect SGND to the unit's bus ground. Software fix: recognise polls by `<addr>:5A:<crc>` (CRC covers the F1).

## Temperature encoding (confirmed)
**raw = (°C + 30) × 10**, 16-bit big-endian. Confirmed by the indoor coil temperature reading 33 °C while heating and cooling to 15 °C after switch-off, and the controller room temperature matching the HA room sensor (17–18 °C).

## Who is who
- **Wall controller = `20:21, 20:23, 20:2A, 20:2C, 20:2D`** (all went offline when it was unplugged; F1 polls continued).
- `10:1x` = indoor unit status; `80:81` = unidentified (byte 10 counts slowly, byte 8 toggles).
- `10:11` byte 14 bit 7 = set while controller communication is lost.
- No write/command frames exist: the controller *reports* its settings when polled and the unit copies them (~1 s later into `10:13`).
- To control the unit: replace the controller and answer polls for its 5 addresses (replay learned replies, edit fields, recompute CRC). Real controller and emulator must never be on the bus together.
- In steady state the real controller's replies change only in: `20:23` bytes 72 (power/mode), 73–74 (setpoint), 75/79/80 (fan), 81–82 (room temp), 47 (brief 00→01→00 around a power change), 117 and 119–124 (briefly after reconnect). `20:21` byte 6 reads `98` after a reconnect (stayed `98` for 5+ minutes), `18` in the earliest captures.

## Frame 20:23 (controller settings, 143 bytes) — confirmed by button test
| Byte | Meaning |
|---|---|
| 47 | Pulses 00 → 01 → 00 around power on/off |
| 70 | Rolling counter 01 → 03 → 02 → … |
| 72 | bit 7 = power; low nibble = mode: 1 cool, 2 dry, 3 fan, 4 heat, 8 auto |
| 73–74 | Setpoint. One button press = 5 (0.5 °C). Per-mode defaults seen: cool/dry 510, fan 500, heat 520, auto 530 |
| 75 | Bits 7–5 = fan: 0 auto, 1 low, 2 mid, 3 high, 4 mute, 5 strong, 6 mid-low, 7 mid-high (low bits = 0x01) |
| 79 | bit 7 = silent (set with mute) |
| 80 | bit 3 = turbo (set with strong) |
| 81–82 | Controller room temperature, 1 °C resolution (flickers e.g. 17.0/18.0) |

## Frame 10:13 (indoor status, 237 bytes)
| Byte | Meaning |
|---|---|
| 51 | Compressor demand: 0 when off; ramps up after a call (heat → ~5E then ~4F; cool → ~40) |
| 70 | **Controller communication OK** (01); drops to 00 when the controller stops answering |
| 72 | Mirrors power/mode; in auto, low bits show the sub-mode actually running (0x8C = auto, heating). Reads power-off for a cycle when controller comms drop |
| 76 | 00 → 02/03 just before the indoor fan is released in heat |
| 85–86 | **Indoor coil temperature** (16-bit, standard encoding) — ~43 °C at full heat, ~3–4 °C in cool |
| 89–90 | Copy of the controller room temperature |
| 96 | Indoor fan output, 0 to ~0x70 (sensor shows ~85–99 when running) |
| 118, 120 | Unidentified (118 slowly varying; 120 small signed) |

## Unit control behaviour (measured 27 Oct 2026, wall controller in charge)
- **Heat:** fan held off until the coil reaches ~36–37 °C (~2 min after compressor start). Near the setpoint the unit throttles down (coil 44 → 30 °C over ~20 min) rather than cycling. **Thermo-off at ≈ setpoint + 1 °C** (controller sensor 19 °C, HA sensor 19.3 °C, setpoint 18.0 °C); compressor and indoor fan stop together.
- **Cool:** indoor fan starts immediately, compressor ~10 s later. **Thermo-off at ≈ setpoint − 1.5 to 2 °C** (controller 17–18 °C, HA 17.7 °C, setpoint 19.5 °C); indoor fan keeps running after the compressor stops (fan auto).
- **Restart delay** ~3 min after any compressor stop. Restart thresholds not yet measured.
- These decisions are made by the unit firmware, so behaviour is the same under ESP emulation.

`10:13` with cmd `0x23` (125 bytes) appears when the controller reconnects and contains Wi-Fi module identity / cloud keys — **don't post raw logs or template dumps publicly.**

## ESPHome implementation
`tcl_bus` external component (bus sniffer + controller emulator with template learning, saved to flash, collision guard, optional byte-change and unparsed-segment log, template dump at takeover; poll recovery for damaged CRC, destroyed leading F1, and polls queued behind unparseable data) plus a `climate` platform (thermostat entity). UART needs `rx_full_threshold: 1`. Keep `log_changes: false` for long runs (glitch bytes flood the log and can drop the log stream). Fan speeds use ESPHome's built-in fan modes where one exists (Auto, Quiet, Low, Medium, High, Focus) and custom names for Mid-Low and Mid-High — ESPHome matches built-in names first, so custom "Auto"/"Low"/"High" never arrive. Room temperature comes from HA `sensor.ac_temp_c`, which is also sent to the unit while emulating (switchable).
