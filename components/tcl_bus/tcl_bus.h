#pragma once

// TCL indoor-unit bus (9600 8E1, Modbus-style CRC) — sniffer + wall-controller emulator.
//
// Bus facts (reverse engineered from captures):
//   poll      F1:<addr>:5A:<crc lo>:<crc hi>                (indoor board = master)
//   response  <class>:<addr>:<cmd>:00:<LEN>:<LEN bytes>:<crc lo>:<crc hi>
//   Wall controller answers addresses 0x21, 0x23, 0x2A, 0x2C, 0x2D (class 0x20).
//   Frame 20:23 carries the user settings:
//     [70] rolling counter 01>03>02   [72] bit7 power, low nibble mode 1 cool 2 dry 3 fan 4 heat 8 auto
//     [73..74] setpoint (raw)         [75] bits7-5 fan 0 auto 1 low 2 mid 3 high 4 mute 5 strong 6 midlow 7 midhigh
//     [79] bit7 silent                [80] bit3 turbo     [81..82] controller room temperature (raw)
//
// The emulator only ever transmits when explicitly enabled, only after it has learned a
// real reply for every controller address, and only while the real controller is silent.

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "esphome/components/uart/uart.h"

#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <vector>

namespace esphome {
namespace tcl_bus {

static const char *const TAG = "tcl_bus";

class TclBus : public Component, public uart::UARTDevice {
 public:
  static constexpr size_t MAX_FRAME = 260;
  static constexpr size_t MAX_TPL = 6;
  static constexpr size_t TPL_BYTES = 160;

  void set_de_pin(GPIOPin *pin) { this->de_pin_ = pin; }
  void set_reply_delay_us(uint32_t us) { this->reply_delay_us_ = us; }
  void set_log_changes(bool on) { this->log_changes_ = on; }
  bool log_changes() const { return this->log_changes_; }
  // Whether the HA room temperature replaces 20:23 bytes 81-82 while emulating.
  void set_send_room_temp(bool on) { this->send_room_ = on; }
  bool send_room_temp() const { return this->send_room_; }
  float get_setup_priority() const override { return setup_priority::DATA; }

  void setup() override {
    if (this->de_pin_ != nullptr) {
      this->de_pin_->setup();
      this->de_pin_->digital_write(false);
    }
    this->pref_ = global_preferences->make_preference<Saved>(0x7C1B0A23UL);
    Saved s;
    if (this->pref_.load(&s)) {
      for (size_t i = 0; i < MAX_TPL; i++) {
        if (s.len[i] >= 7 && s.len[i] <= TPL_BYTES) {
          this->templates_[s.addr[i]] = std::vector<uint8_t>(s.data[i], s.data[i] + s.len[i]);
          this->ctrl_addrs_.insert(s.addr[i]);
        }
      }
      ESP_LOGI(TAG, "Loaded %u saved controller templates", (unsigned) this->templates_.size());
    }
    this->high_freq_.start();  // run loop() continuously so polls are answered quickly
  }

  void dump_config() override {
    ESP_LOGCONFIG(TAG, "TCL bus:");
    LOG_PIN("  DE pin: ", this->de_pin_);
    ESP_LOGCONFIG(TAG, "  Reply delay: %u us", (unsigned) this->reply_delay_us_);
    ESP_LOGCONFIG(TAG, "  Saved templates: %u", (unsigned) this->templates_.size());
  }

  void loop() override {
    // Read whatever the UART driver has. Note: the ESP32 driver hands bytes over in bursts
    // (RX FIFO threshold), so gaps of ~8 ms *inside* a frame are normal here. Frame boundaries
    // come from the header + length byte + CRC, not from timing.
    uint8_t c;
    bool got = false;
    while (this->available() && this->read_byte(&c)) {
      this->rx_.push_back(c);
      got = true;
    }
    if (got) {
      this->last_byte_us_ = micros();
      this->parse_();
    } else if (!this->rx_.empty() && (micros() - this->last_byte_us_) > 1000000) {
      // Nothing new for 1 s and a partial frame is still pending: it will never complete.
      // (Must be long: the UART can hold the last few bytes of a frame back for 80+ ms.)
      this->skip_(this->rx_.size());
      this->flush_skipped_();
    }
  }

  // ---------- read access for YAML lambdas ----------
  const std::vector<uint8_t> *frame(uint8_t cls, uint8_t addr) const {
    auto it = this->frames_.find((uint16_t) cls << 8 | addr);
    return it == this->frames_.end() ? nullptr : &it->second;
  }
  bool controller_present() const {
    return this->last_ctrl_ms_ != 0 && (millis() - this->last_ctrl_ms_) < 5000;
  }
  bool templates_complete() const {
    if (this->ctrl_addrs_.empty()) return false;
    for (auto a : this->ctrl_addrs_)
      if (!this->templates_.count(a)) return false;
    return this->templates_.count(0x23) && this->templates_.at(0x23).size() >= 85;
  }
  size_t template_count() const { return this->templates_.size(); }
  bool emulating() const { return this->emulate_; }

  // ---------- desired state (used while emulating) ----------
  bool want_power() const { return this->want_power_; }
  uint8_t want_mode() const { return this->want_mode_; }
  uint16_t want_setpoint_raw() const { return this->want_set_; }
  uint8_t want_fan() const { return this->want_fan_; }
  void set_power(bool on) { this->want_power_ = on; }
  void set_mode(uint8_t m) {
    if (m == 1 || m == 2 || m == 3 || m == 4 || m == 8) this->want_mode_ = m;
  }
  void set_setpoint_raw(uint16_t raw) { this->want_set_ = raw; }
  void set_fan(uint8_t f) { this->want_fan_ = f & 0x07; }
  void set_room_temp_raw(uint16_t raw) {
    this->room_raw_ = raw;
    this->room_valid_ = true;
  }
  void clear_room_temp() { this->room_valid_ = false; }

  bool save_templates() {
    if (this->templates_.empty()) return false;
    Saved s;
    memset(&s, 0, sizeof(s));
    size_t i = 0;
    for (auto &kv : this->templates_) {
      if (i >= MAX_TPL || kv.second.size() > TPL_BYTES) continue;
      s.addr[i] = kv.first;
      s.len[i] = kv.second.size();
      memcpy(s.data[i], kv.second.data(), kv.second.size());
      i++;
    }
    bool ok = this->pref_.save(&s) && global_preferences->sync();
    ESP_LOGI(TAG, "Saved %u controller templates: %s", (unsigned) i, ok ? "ok" : "FAILED");
    return ok;
  }

  // Returns the resulting state.
  bool set_emulate(bool on) {
    if (!on) {
      if (this->emulate_) ESP_LOGW(TAG, "Controller emulation OFF");
      this->emulate_ = false;
      return false;
    }
    if (this->emulate_) return true;
    if (this->controller_present()) {
      ESP_LOGE(TAG, "Refusing to emulate: the real wall controller is still answering. Unplug it first.");
      return false;
    }
    if (!this->templates_complete()) {
      ESP_LOGE(TAG, "Refusing to emulate: no complete set of controller replies learned yet "
                    "(have %u). Run with the wall controller connected first.",
               (unsigned) this->templates_.size());
      return false;
    }
    // Start from exactly what the controller last said, so takeover changes nothing.
    auto &t = this->templates_[0x23];
    this->want_power_ = t[72] & 0x80;
    this->want_mode_ = t[72] & 0x0F;
    this->want_set_ = (uint16_t) t[73] << 8 | t[74];
    this->want_fan_ = (t[75] >> 5) & 0x07;
    this->dump_templates_();
    this->emulate_ = true;
    ESP_LOGW(TAG, "Controller emulation ON (power=%d mode=%u set=%u fan=%u)", this->want_power_,
             this->want_mode_, this->want_set_, this->want_fan_);
    return true;
  }

  // Temperature encoding used for setpoint and room temperature.
  // ASSUMPTION (fits all captures, unconfirmed against the display): raw = (°C + 30) * 10.
  static float raw_to_c(uint16_t raw) { return raw / 10.0f - 30.0f; }
  static uint16_t c_to_raw(float c) { return (uint16_t) lroundf((c + 30.0f) * 10.0f); }

  uint32_t frames_ok{0};
  uint32_t frames_bad{0};
  uint32_t replies_sent{0};
  uint32_t noisy_polls{0};  // polls answered despite a noise-damaged CRC

 protected:
  struct Saved {
    uint8_t addr[MAX_TPL];
    uint16_t len[MAX_TPL];
    uint8_t data[MAX_TPL][TPL_BYTES];
  };

  static uint16_t crc16_(const uint8_t *d, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
      c ^= d[i];
      for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xA001 : (c >> 1);
    }
    return c;
  }
  static bool header_ok_(uint8_t a, uint8_t b) {
    if (a == 0xF1) return true;
    return a != 0 && (a & 0x0F) == 0 && (b & 0xF0) == a;
  }

  void parse_() {
    while (!this->rx_.empty()) {
      if (this->rx_.size() >= 2 && !header_ok_(this->rx_[0], this->rx_[1])) {
        // A poll whose leading F1 was destroyed by a glitch (seen when the indoor fan runs):
        // the remaining <addr>:5A:<crc lo>:<crc hi> still identifies it, because the CRC covers F1.
        if (this->emulate_ && this->rx_.size() >= 4 && this->rx_[1] == 0x5A && (this->rx_[0] & 0xF0) == 0x20 &&
            this->templates_.count(this->rx_[0])) {
          const uint8_t q[3] = {0xF1, this->rx_[0], 0x5A};
          uint16_t c = crc16_(q, 3);
          if (this->rx_[2] == (c & 0xFF) && this->rx_[3] == (c >> 8)) {
            uint8_t addr = this->rx_[0];
            this->flush_skipped_();
            this->rx_.erase(this->rx_.begin(), this->rx_.begin() + 4);
            this->noisy_polls++;
            this->reply_(addr);
            continue;
          }
        }
        if (this->rx_.size() < 4 && this->emulate_ && (this->rx_[0] & 0xF0) == 0x20 && this->rx_[1] == 0x5A)
          return;  // could be a damaged poll; wait for its CRC bytes
        this->skip_(1);
        continue;
      }
      if (this->rx_.size() < 5) return;
      size_t len = (this->rx_[0] == 0xF1) ? 5 : (size_t) this->rx_[4] + 7;
      if (this->rx_[0] == 0xF1 && this->rx_[2] != 0x5A) len = 0;
      if (len < 5 || len > MAX_FRAME) {
        this->skip_(1);
        continue;
      }
      if (this->rx_.size() < len) {
        // Waiting for the rest of a frame. If a complete, valid poll is already sitting further
        // along in the buffer, what we are waiting on is not a real frame: drop it and serve the
        // poll now instead of missing it.
        size_t p = this->find_poll_(1);
        if (p != 0) {
          this->skip_(p);
          continue;
        }
        return;
      }
      uint16_t c = crc16_(this->rx_.data(), len - 2);
      if (this->rx_[len - 2] != (c & 0xFF) || this->rx_[len - 1] != (c >> 8)) {
        // A poll whose first three bytes are intact (F1:<our addr>:5A) but with a damaged CRC is
        // still clearly a poll for us; answer it rather than let the "controller" drop out.
        if (len == 5 && this->emulate_ && (this->rx_[1] & 0xF0) == 0x20 && this->templates_.count(this->rx_[1])) {
          uint8_t addr = this->rx_[1];
          this->flush_skipped_();
          this->rx_.erase(this->rx_.begin(), this->rx_.begin() + 5);
          this->noisy_polls++;
          this->reply_(addr);
          continue;
        }
        this->skip_(1);
        continue;
      }
      this->flush_skipped_();
      std::vector<uint8_t> f(this->rx_.begin(), this->rx_.begin() + len);
      this->rx_.erase(this->rx_.begin(), this->rx_.begin() + len);
      this->frames_ok++;
      this->handle_(f);
    }
  }

  // Move n bytes we could not frame into skipped_ (logged as one UNPARSED segment later).
  void skip_(size_t n) {
    if (this->skipped_.empty()) this->frames_bad++;  // one count per unparsed segment
    for (size_t i = 0; i < n && !this->rx_.empty(); i++) {
      if (this->skipped_.size() < 400) this->skipped_.push_back(this->rx_[0]);
      this->rx_.erase(this->rx_.begin());
    }
  }

  // Index of the first complete poll with a valid CRC at or after `from`, or 0 if none.
  size_t find_poll_(size_t from) const {
    for (size_t p = from; p + 5 <= this->rx_.size(); p++) {
      if (this->rx_[p] != 0xF1 || this->rx_[p + 2] != 0x5A) continue;
      uint16_t c = crc16_(&this->rx_[p], 3);
      if (this->rx_[p + 3] == (c & 0xFF) && this->rx_[p + 4] == (c >> 8)) return p;
    }
    return 0;
  }

  // Log a segment of bytes that did not form a valid frame (40 bytes per line).
  void flush_skipped_() {
    if (this->skipped_.empty()) return;
    if (this->log_changes_) {
      const auto &s = this->skipped_;
      for (size_t off = 0; off < s.size(); off += 40) {
        char line[40 * 3 + 1];
        size_t n = 0;
        for (size_t k = off; k < s.size() && k < off + 40; k++)
          n += snprintf(line + n, sizeof(line) - n, k == off ? "%02X" : ":%02X", s[k]);
        ESP_LOGD(TAG, "UNPARSED %u bytes @%3u %s", (unsigned) s.size(), (unsigned) off, line);
      }
    }
    this->skipped_.clear();
  }

  void handle_(const std::vector<uint8_t> &f) {
    if (f[0] == 0xF1) {
      uint8_t addr = f[1];
      if (this->emulate_ && (addr & 0xF0) == 0x20 && this->templates_.count(addr)) this->reply_(addr);
      return;
    }
    uint16_t key = (uint16_t) f[0] << 8 | f[1];
    if (f[0] == 0x20) {
      if (this->emulate_) {
        if (f == this->last_tx_) return;  // our own echo
        ESP_LOGE(TAG, "Another device answered as the wall controller (%02X:%02X). "
                      "Emulation OFF to avoid bus collisions.", f[0], f[1]);
        this->emulate_ = false;
        this->last_ctrl_ms_ = millis();
      } else {
        this->last_ctrl_ms_ = millis();
        this->ctrl_addrs_.insert(f[1]);
        if (f.size() <= TPL_BYTES) this->templates_[f[1]] = f;  // learn the real controller's reply
      }
    }
    if (this->log_changes_) this->log_diff_(key, f);
    this->frames_[key] = f;
  }

  // Print each learned controller reply (40 bytes per line so log lines aren't truncated).
  void dump_templates_() {
    for (auto &kv : this->templates_) {
      const auto &f = kv.second;
      for (size_t off = 0; off < f.size(); off += 40) {
        char line[40 * 3 + 1];
        size_t n = 0;
        for (size_t k = off; k < f.size() && k < off + 40; k++)
          n += snprintf(line + n, sizeof(line) - n, k == off ? "%02X" : ":%02X", f[k]);
        ESP_LOGI(TAG, "TPL %02X:%02X @%3u %s", f[0], f[1], (unsigned) off, line);
      }
    }
  }

  // Log which payload bytes changed since the previous frame from the same device.
  void log_diff_(uint16_t key, const std::vector<uint8_t> &f) {
    auto it = this->frames_.find(key);
    if (it == this->frames_.end() || it->second.size() != f.size()) {
      ESP_LOGI(TAG, "NEW %02X:%02X len=%u", f[0], f[1], (unsigned) f.size());
      return;
    }
    char buf[400];
    size_t n = 0;
    for (size_t k = 5; k + 2 < f.size() && n < sizeof(buf) - 16; k++) {
      if (key == 0x2023 && k == 70) continue;  // rolling counter
      if (it->second[k] != f[k])
        n += snprintf(buf + n, sizeof(buf) - n, " [%u]%02X>%02X", (unsigned) k, it->second[k], f[k]);
    }
    if (n) ESP_LOGI(TAG, "CHG %02X:%02X%s%s", f[0], f[1], buf, this->emulate_ ? "" : "  (controller)");
  }

  void reply_(uint8_t addr) {
    std::vector<uint8_t> f = this->templates_[addr];
    if (addr == 0x23 && f.size() >= 85) {
      static const uint8_t seq[3] = {0x01, 0x03, 0x02};
      this->counter_ = (this->counter_ + 1) % 3;
      f[70] = seq[this->counter_];
      f[72] = (this->want_power_ ? 0x80 : 0x00) | (this->want_mode_ & 0x0F);
      f[73] = this->want_set_ >> 8;
      f[74] = this->want_set_ & 0xFF;
      f[75] = (uint8_t) (this->want_fan_ << 5) | (f[75] & 0x1F);
      f[79] = (this->want_fan_ == 4) ? (f[79] | 0x80) : (f[79] & 0x7F);
      f[80] = (this->want_fan_ == 5) ? (f[80] | 0x08) : (f[80] & ~0x08);
      if (this->room_valid_ && this->send_room_) {
        f[81] = this->room_raw_ >> 8;
        f[82] = this->room_raw_ & 0xFF;
      }
    }
    uint16_t c = crc16_(f.data(), f.size() - 2);
    f[f.size() - 2] = c & 0xFF;
    f[f.size() - 1] = c >> 8;

    if (this->reply_delay_us_) delayMicroseconds(this->reply_delay_us_);
    if (this->de_pin_ != nullptr) this->de_pin_->digital_write(true);
    this->write_array(f);
    this->flush();  // wait until the last bit is out before releasing the driver
    if (this->de_pin_ != nullptr) this->de_pin_->digital_write(false);
    this->last_tx_ = f;
    this->frames_[(uint16_t) f[0] << 8 | f[1]] = f;
    this->replies_sent++;
  }

  GPIOPin *de_pin_{nullptr};
  uint32_t reply_delay_us_{3000};
  HighFrequencyLoopRequester high_freq_;
  ESPPreferenceObject pref_;

  std::vector<uint8_t> rx_;
  uint32_t last_byte_us_{0};
  std::map<uint16_t, std::vector<uint8_t>> frames_;
  std::map<uint8_t, std::vector<uint8_t>> templates_;
  std::set<uint8_t> ctrl_addrs_;
  uint32_t last_ctrl_ms_{0};
  std::vector<uint8_t> last_tx_;

  bool emulate_{false};
  bool log_changes_{false};
  bool send_room_{true};
  std::vector<uint8_t> skipped_;
  bool want_power_{false};
  uint8_t want_mode_{8};
  uint16_t want_set_{520};
  uint8_t want_fan_{0};
  uint16_t room_raw_{0};
  bool room_valid_{false};
  uint8_t counter_{0};
};

}  // namespace tcl_bus
}  // namespace esphome
