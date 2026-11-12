# TCL heat pump wall thermostat (ESPHome)

A touchscreen wall thermostat for a TCL ductless heat pump, built on a
**Waveshare ESP32-S3-Touch-LCD-4.3B**. It replaces the TCL wired wall
controller on the indoor unit's RS-485 bus and is fully controllable from
Home Assistant.

The unit has no command frames: it polls the wall controller and copies the
controller's settings from its replies. This project learns the controller's
replies, then answers the polls itself with the settings you choose. See
[`docs/tcl-bus-protocol.md`](docs/tcl-bus-protocol.md) for the protocol notes.

## Features

- **Main screen:** setpoint dial with − / + (°F, 61–88), power, mode and fan
  dropdowns (all 8 TCL fan speeds), room temperature, outdoor temperature with
  today's forecast high/low, clock, Home Assistant link status.
- **Temperature source:** dropdown under the setpoint showing and setting
  `input_select.ac_thermostat_temperature_selector` (1st Floor / Bedrooms /
  Whole House).
- **Background colour follows the mode:** blue cool, amber heat, violet
  auto, teal dry, slate fan.
- **Screen saver:** the room temperature bounces around a dimmed screen
  after an adjustable idle time; touch to wake.
- **Night screen off:** a schedule turns the screen fully off overnight.
- **Settings page:** brightness, screen-saver delay and brightness, night
  schedule, network info.
- **Home Assistant:** "Heat Pump" climate entity, decoded bus sensors (coil
  temperature, compressor demand, running mode…) and diagnostics.
- **Auto-start emulation** after a reboot once the templates are learned.

## Repository layout

| Path | What it is |
|---|---|
| `thermostat.yaml` | ESPHome config for the panel |
| `components/tcl_bus/` | External component: bus sniffer + wall-controller emulator + climate platform |
| `homeassistant/forecast-sensors.yaml` | HA template sensors for today's forecast high/low |
| `docs/tcl-bus-protocol.md` | Reverse-engineered bus protocol |
| `secrets.yaml.example` | Template for your `secrets.yaml` (never commit the real one) |

## Hardware

- Waveshare **ESP32-S3-Touch-LCD-4.3B** (the "B" version: screw terminals,
  RS-485 on GPIO43/44). The plain 4.3 has RS-485 on different pins.
- Wiring to the indoor unit's controller bus:
  - **A → A, B → B** (if no frames arrive, swap them; it's harmless)
  - **Bus signal ground → board GND**
  - Do **not** connect any power wire from the bus.
  - Leave the board's RS-485 termination switch **off**.
- The board's RS-485 port is not isolated: power it from an ordinary USB wall
  adapter, not a computer, while it's on the bus.

## Setup

1. Copy `secrets.yaml.example` to `secrets.yaml` and fill it in.
2. Make sure `components/tcl_bus/` contains the component's `.h`/`.cpp` files
   as well as the Python files.
3. Copy the folder into your ESPHome config directory and install
   `thermostat.yaml`.
4. In Home Assistant: Settings → Devices & services → ESPHome → Thermostat →
   Configure → turn on **Allow the device to perform Home Assistant actions**
   (needed for the temperature-source dropdown).
5. Add `homeassistant/forecast-sensors.yaml` to `configuration.yaml` (under
   `template:`), change the weather entity if needed, and restart HA.

### First-time learning (once per board)

1. With the **wall controller connected**, install the firmware and wait
   until **Templates Learned = 5** (about 30 s).
2. Press **Save Controller Templates**.
3. Unplug the wall controller; wait until **Wall Controller Present = off**.
4. Turn on **Controller Emulation**. After that it turns on automatically at
   every boot (switch **Auto-start Emulation** off to prevent that).

The real controller and the panel must never answer on the bus at the same
time; auto-start only engages when no controller is detected.

## Home Assistant entities used

| Entity | Purpose |
|---|---|
| `sensor.ac_temp_c` | Room temperature (°C) shown on the panel and sent to the unit |
| `input_select.ac_thermostat_temperature_selector` | Which area the room temperature is taken from |
| `sensor.bresser_6in1_0_542115398_temperature` | Outdoor temperature |
| `sensor.forecast_today_high` / `_low` | Created by `homeassistant/forecast-sensors.yaml` |

Change these in the `substitutions:` block of `thermostat.yaml` to match
your setup.

## Known limits

- **No Bluetooth proxy on this panel.** With the 800×480 display running,
  the ESP32-S3 doesn't have enough internal RAM left to start Bluetooth; it
  crashes at boot (and ESPHome rolls back). Use a separate ESP32 as a proxy.
- **Brightness is software dimming.** The board's backlight is on/off only,
  so brightness levels are a dark overlay.
- **Display vs Wi-Fi.** The RGB display's DMA can starve Wi-Fi ("Auth
  Expired"). The IRAM `sdkconfig_options` and `power_save_mode: NONE` in the
  config fix this; keep the pixel clock at 16 MHz (14 MHz leaves this panel
  blank).

## Safety

This replaces a controller on live HVAC equipment. Work with the power off
when wiring, and keep the original wall controller so you can go back to it.
Don't publish raw bus logs or template dumps: one of the unit's frames
contains its Wi-Fi module identity and cloud keys.
