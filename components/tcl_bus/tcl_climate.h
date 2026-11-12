#pragma once

// Thermostat-style climate entity for the TCL bus.
// Shows the unit's live state at all times; accepts changes only while the ESP is acting
// as the wall controller ("Controller Emulation" on). Otherwise changes are rejected and
// the entity snaps back to the real state.

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/climate/climate.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#include "tcl_bus.h"

#include <cmath>
#include <string>

namespace esphome {
namespace tcl_bus {

static const char *const CLIMATE_TAG = "tcl_bus.climate";

// Fan speeds. ESPHome matches incoming fan-mode names against its built-in modes first
// (case-insensitive), so speeds that have a built-in equivalent MUST use it; only the two
// with no equivalent are custom. Bus code = 20:23 byte 75 bits 7-5.
struct FanMap {
  uint8_t code;
  bool custom;
  climate::ClimateFanMode std_mode;
  const char *custom_name;
};
static const FanMap FAN_MAP[8] = {
    {0, false, climate::CLIMATE_FAN_AUTO, nullptr},    // Auto
    {4, false, climate::CLIMATE_FAN_QUIET, nullptr},   // Mute
    {1, false, climate::CLIMATE_FAN_LOW, nullptr},     // Low
    {6, true, climate::CLIMATE_FAN_AUTO, "Mid-Low"},   // Mid-Low
    {2, false, climate::CLIMATE_FAN_MEDIUM, nullptr},  // Mid
    {7, true, climate::CLIMATE_FAN_AUTO, "Mid-High"},  // Mid-High
    {3, false, climate::CLIMATE_FAN_HIGH, nullptr},    // High
    {5, false, climate::CLIMATE_FAN_FOCUS, nullptr},   // Strong (turbo)
};
static const char *const FAN_CUSTOM_NAMES[2] = {"Mid-Low", "Mid-High"};

class TclClimate : public climate::Climate, public Component {
 public:
  void set_bus(TclBus *bus) { this->bus_ = bus; }
#ifdef USE_SENSOR
  void set_room_sensor(sensor::Sensor *s, bool fahrenheit) {
    this->room_sensor_ = s;
    this->room_f_ = fahrenheit;
  }
#endif
  float get_setup_priority() const override { return setup_priority::DATA - 1.0f; }

  void setup() override {
    this->set_supported_custom_fan_modes(FAN_CUSTOM_NAMES);
    this->mode = climate::CLIMATE_MODE_OFF;
    this->action = climate::CLIMATE_ACTION_OFF;
    this->target_temperature = NAN;
    this->current_temperature = NAN;
#ifdef USE_SENSOR
    if (this->room_sensor_ != nullptr) {
      this->room_sensor_->add_on_state_callback([this](float x) { this->on_room_(x); });
      if (this->room_sensor_->has_state()) this->on_room_(this->room_sensor_->state);
    }
#endif
  }

  void loop() override {
    uint32_t now = millis();
    if (now - this->last_sync_ < 500) return;
    this->last_sync_ = now;
    this->sync_from_bus_();
  }

  climate::ClimateTraits traits() override {
    climate::ClimateTraits t{};
    t.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE | climate::CLIMATE_SUPPORTS_ACTION);
    t.set_supported_modes({climate::CLIMATE_MODE_OFF, climate::CLIMATE_MODE_HEAT_COOL, climate::CLIMATE_MODE_COOL,
                           climate::CLIMATE_MODE_HEAT, climate::CLIMATE_MODE_DRY, climate::CLIMATE_MODE_FAN_ONLY});
    for (const auto &m : FAN_MAP)
      if (!m.custom) t.add_supported_fan_mode(m.std_mode);
    t.set_visual_min_temperature(16.0f);
    t.set_visual_max_temperature(31.0f);
    t.set_visual_target_temperature_step(0.5f);
    t.set_visual_current_temperature_step(0.1f);
    return t;
  }

  void control(const climate::ClimateCall &call) override {
    if (!this->bus_->emulating()) {
      ESP_LOGW(CLIMATE_TAG, "Ignoring change: turn on \"Controller Emulation\" (wall controller unplugged) first");
      this->publish_state();  // snap the UI back to the real state
      return;
    }
    if (call.get_mode().has_value()) {
      auto m = *call.get_mode();
      if (m == climate::CLIMATE_MODE_OFF) {
        this->bus_->set_power(false);
      } else {
        this->bus_->set_mode(mode_to_bus_(m));
        this->bus_->set_power(true);
      }
      this->mode = m;
    }
    if (call.get_target_temperature().has_value()) {
      float c = *call.get_target_temperature();
      c = std::round(c * 2.0f) / 2.0f;  // the unit works in 0.5 °C steps
      this->bus_->set_setpoint_raw(TclBus::c_to_raw(c));
      this->target_temperature = c;
    }
    if (call.get_fan_mode().has_value()) {
      auto want = *call.get_fan_mode();
      bool matched = false;
      for (const auto &m : FAN_MAP) {
        if (!m.custom && m.std_mode == want) {
          this->bus_->set_fan(m.code);
          this->set_fan_mode_(want);
          matched = true;
        }
      }
      if (!matched) ESP_LOGW(CLIMATE_TAG, "Fan mode not supported by this unit");
    } else if (call.has_custom_fan_mode()) {
      std::string want(call.get_custom_fan_mode().c_str(), call.get_custom_fan_mode().size());
      for (const auto &m : FAN_MAP) {
        if (m.custom && want == m.custom_name) {
          this->bus_->set_fan(m.code);
          this->set_custom_fan_mode_(m.custom_name);
        }
      }
    }
    this->publish_state();
  }

 protected:
  static uint8_t mode_to_bus_(climate::ClimateMode m) {
    switch (m) {
      case climate::CLIMATE_MODE_COOL: return 1;
      case climate::CLIMATE_MODE_DRY: return 2;
      case climate::CLIMATE_MODE_FAN_ONLY: return 3;
      case climate::CLIMATE_MODE_HEAT: return 4;
      default: return 8;  // HEAT_COOL = the unit's auto mode
    }
  }
  static climate::ClimateMode bus_to_mode_(uint8_t b) {
    switch (b & 0x0F) {
      case 1: return climate::CLIMATE_MODE_COOL;
      case 2: return climate::CLIMATE_MODE_DRY;
      case 3: return climate::CLIMATE_MODE_FAN_ONLY;
      case 4: return climate::CLIMATE_MODE_HEAT;
      default: return climate::CLIMATE_MODE_HEAT_COOL;
    }
  }

  void on_room_(float x) {
    if (std::isnan(x)) return;
    float c = this->room_f_ ? (x - 32.0f) * 5.0f / 9.0f : x;
    this->room_c_ = c;
    this->bus_->set_room_temp_raw(TclBus::c_to_raw(c));  // used only while emulating
  }

  void sync_from_bus_() {
    bool changed = false;
    auto *s = this->bus_->frame(0x20, 0x23);  // controller settings (real controller or ours)
    if (s != nullptr && s->size() >= 85) {
      const auto &f = *s;
      bool power = f[72] & 0x80;
      climate::ClimateMode m = power ? bus_to_mode_(f[72]) : climate::CLIMATE_MODE_OFF;
      if (m != this->mode) {
        this->mode = m;
        changed = true;
      }
      float t = TclBus::raw_to_c((f[73] << 8) | f[74]);
      if (std::isnan(this->target_temperature) || std::fabs(t - this->target_temperature) > 0.01f) {
        this->target_temperature = t;
        changed = true;
      }
      uint8_t code = (f[75] >> 5) & 0x07;
      for (const auto &m : FAN_MAP) {
        if (m.code != code) continue;
        if (m.custom) {
          if (this->set_custom_fan_mode_(m.custom_name)) changed = true;
        } else {
          if (this->set_fan_mode_(m.std_mode)) changed = true;
        }
      }
      float room = std::isnan(this->room_c_) ? TclBus::raw_to_c((f[81] << 8) | f[82]) : this->room_c_;
      if (std::isnan(this->current_temperature) || std::fabs(room - this->current_temperature) > 0.05f) {
        this->current_temperature = room;
        changed = true;
      }
    }
    auto *st = this->bus_->frame(0x10, 0x13);  // indoor unit status
    if (st != nullptr && st->size() >= 123) {
      const auto &f = *st;
      climate::ClimateAction a;
      if (!(f[72] & 0x80)) {
        a = climate::CLIMATE_ACTION_OFF;
      } else {
        switch (f[72] & 0x07) {
          case 1: a = climate::CLIMATE_ACTION_COOLING; break;
          case 2: a = climate::CLIMATE_ACTION_DRYING; break;
          case 3: a = climate::CLIMATE_ACTION_FAN; break;
          case 4: a = climate::CLIMATE_ACTION_HEATING; break;
          default: a = climate::CLIMATE_ACTION_IDLE; break;
        }
        // Idle only when neither the compressor (byte 51) nor the indoor fan (byte 96) is running.
        // In heat the fan stays off for a few minutes while the coil warms up; that still counts as heating.
        if (f[51] == 0 && f[96] == 0 && a != climate::CLIMATE_ACTION_FAN) a = climate::CLIMATE_ACTION_IDLE;
      }
      if (a != this->action) {
        this->action = a;
        changed = true;
      }
    }
    if (changed) this->publish_state();
  }

  TclBus *bus_{nullptr};
#ifdef USE_SENSOR
  sensor::Sensor *room_sensor_{nullptr};
#endif
  bool room_f_{false};
  float room_c_{NAN};
  uint32_t last_sync_{0};
};

}  // namespace tcl_bus
}  // namespace esphome
