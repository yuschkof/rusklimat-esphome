#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/number/number.h"
#include "esphome/components/sensor/sensor.h"
#include <vector>
#include <string>
#include <cstdio>
#include <cmath>
#include <cstring>

namespace esphome {
namespace rusklimat {

static const char *const TAG = "rusklimat";

// Байт [8] — битовая маска дополнительных функций. Биты независимы и могут стоять одновременно.
static constexpr uint8_t B8_SLEEP = 0x01;    // Сон
static constexpr uint8_t B8_IONIZER = 0x02;     // Листочек (ионизатор)
static constexpr uint8_t B8_QUIET = 0x04;    // Тихий (в HA — режим вентилятора QUIET)
static constexpr uint8_t B8_TURBO = 0x08;    // Турбо
static constexpr uint8_t B8_DISPLAY = 0x10;  // Подсветка дисплея
static constexpr uint8_t B8_KNOWN = B8_SLEEP | B8_IONIZER | B8_QUIET | B8_TURBO | B8_DISPLAY;

// Турбо и Сон показываем в HA как дополнительные скорости вентилятора (custom fan modes),
// поэтому они взаимоисключающие между собой и с Тихим.
static const char *const FAN_TURBO = "Турбо";
static const char *const FAN_SLEEP = "Сон";

// Пауза внутри кадра дольше этого времени считается обрывом: 22 байта на 9600 бод идут ~23 мс.
static constexpr uint32_t RX_TIMEOUT_MS = 100;

class RusklimatClimate : public climate::Climate, public PollingComponent, public uart::UARTDevice {
 public:
  RusklimatClimate() : PollingComponent(2000) {
    // Турбо и Сон — доп. скорости вентилятора (флаги 0x08 и 0x01 в байте [8])
    this->set_supported_custom_fan_modes({FAN_TURBO, FAN_SLEEP});
  }

  void set_display_switch(switch_::Switch *sw) { this->display_switch_ = sw; }
  void set_ionizer_switch(switch_::Switch *sw) { this->ionizer_switch_ = sw; }
  void set_sleep_switch(switch_::Switch *sw) { this->sleep_switch_ = sw; }
  void set_turbo_switch(switch_::Switch *sw) { this->turbo_switch_ = sw; }
  void set_timer_number(number::Number *num) { this->timer_number_ = num; }
  void set_timer_remaining_sensor(sensor::Sensor *sens) { this->timer_remaining_sensor_ = sens; }

  void setup() override {
    if (this->display_switch_ != nullptr) {
      this->display_switch_->publish_state(true);
    }
  }

  void update() override {
    uint8_t req[4] = {0xAA, 0x02, 0x01, 0xAD};
    this->write_array(req, 4);
  }

  void loop() override {
    const uint32_t now = millis();

    // Оборванный кадр: сбрасываем буфер, иначе байты следующего пакета "приклеятся" к хвосту старого.
    if (!this->rx_buffer_.empty() && (now - this->last_rx_ms_) > RX_TIMEOUT_MS) {
      ESP_LOGW(TAG, "RX timeout, dropping partial frame: %s",
               this->format_hex_(this->rx_buffer_.data(), this->rx_buffer_.size()).c_str());
      this->rx_buffer_.clear();
    }

    while (this->available()) {
      uint8_t c;
      if (!this->read_byte(&c)) {
        break;
      }
      this->last_rx_ms_ = now;

      if (this->rx_buffer_.empty() && c != 0xAA) {
        continue;
      }
      this->rx_buffer_.push_back(c);

      if (this->rx_buffer_.size() < 2) {
        continue;
      }
      const size_t expected_len = static_cast<size_t>(this->rx_buffer_[1]) + 2;
      if (expected_len < 4 || expected_len > 32) {
        this->rx_buffer_.clear();
        continue;
      }
      if (this->rx_buffer_.size() == expected_len) {
        this->process_packet_(this->rx_buffer_);
        this->rx_buffer_.clear();
      }
    }
  }

  climate::ClimateTraits traits() override {
    auto traits = climate::ClimateTraits();
    traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
    traits.set_visual_min_temperature(16.0f);
    traits.set_visual_max_temperature(32.0f);
    traits.set_visual_temperature_step(1.0f);

    traits.set_supported_modes({
        climate::CLIMATE_MODE_OFF,
        climate::CLIMATE_MODE_AUTO,
        climate::CLIMATE_MODE_COOL,
        climate::CLIMATE_MODE_DRY,
        climate::CLIMATE_MODE_HEAT,
        climate::CLIMATE_MODE_FAN_ONLY,
    });

    // QUIET, Турбо и Сон — флаги 0x04, 0x08 и 0x01 в байте [8] поверх обычной скорости из байта [9].
    traits.set_supported_fan_modes({
        climate::CLIMATE_FAN_AUTO,
        climate::CLIMATE_FAN_QUIET,
        climate::CLIMATE_FAN_LOW,
        climate::CLIMATE_FAN_MEDIUM,
        climate::CLIMATE_FAN_HIGH,
    });

    traits.set_supported_swing_modes({
        climate::CLIMATE_SWING_OFF,
        climate::CLIMATE_SWING_VERTICAL,
        climate::CLIMATE_SWING_HORIZONTAL,
        climate::CLIMATE_SWING_BOTH,
    });

    return traits;
  }

  void control(const climate::ClimateCall &call) override {
    if (call.get_mode().has_value()) {
      climate::ClimateMode m = *call.get_mode();
      this->mode = m;
      if (m != climate::CLIMATE_MODE_OFF) {
        this->last_active_mode_ = m;
      }
    }

    if (call.get_target_temperature().has_value()) {
      this->target_temperature = *call.get_target_temperature();
    }

    if (call.has_custom_fan_mode()) {
      // Турбо / Сон — только флаги, реальную скорость в [9] не трогаем.
      const char *m = call.get_custom_fan_mode().c_str();
      this->set_speed_flags_(false, strcmp(m, FAN_TURBO) == 0, strcmp(m, FAN_SLEEP) == 0);
    } else if (call.get_fan_mode().has_value()) {
      climate::ClimateFanMode fm = *call.get_fan_mode();
      // Тихий — тоже только флаг; обычная скорость сбрасывает все три флага.
      this->set_speed_flags_(fm == climate::CLIMATE_FAN_QUIET, false, false);
      if (fm != climate::CLIMATE_FAN_QUIET) {
        this->fan_raw_ = this->fan_to_raw_(fm, this->fan_raw_);
      }
    }
    this->update_fan_mode_();

    if (call.get_swing_mode().has_value()) {
      this->swing_mode = *call.get_swing_mode();
    }

    this->send_command_();
    this->publish_state();
  }

  // Подсветка дисплея (бит 0x10 в байте [8])
  void set_display(bool state) {
    this->display_active_ = state;
    this->apply_flag_(this->display_switch_, state);
  }

  // Ионизатор, значок листочка (бит 0x02 в байте [8])
  void set_ionizer(bool state) {
    this->ionizer_active_ = state;
    this->apply_flag_(this->ionizer_switch_, state);
  }

  // Сон (бит 0x01 в байте [8]); в HA также доступен как скорость вентилятора «Сон»
  void set_sleep(bool state) {
    this->set_speed_flags_(false, false, state);
    this->update_fan_mode_();
    this->publish_state();
    this->send_command_();
  }

  // Турбо (бит 0x08 в байте [8]); в HA также доступен как скорость вентилятора «Турбо»
  void set_turbo(bool state) {
    this->set_speed_flags_(false, state, false);
    this->update_fan_mode_();
    this->publish_state();
    this->send_command_();
  }

  // Установка таймера в часах (0 = выкл, 1..24 = часы)
  void set_timer_hours(float hours) {
    uint8_t h = (uint8_t) roundf(hours);
    if (h == 0) {
      this->timer_flag_ = 0x00;
      this->timer_hours_ = 0x00;
      this->timer_mins_ = 0x00;
    } else {
      if (h > 24)
        h = 24;
      this->timer_flag_ = 0x02;
      this->timer_hours_ = h;
      this->timer_mins_ = 0x00;
    }
    if (this->timer_number_ != nullptr) {
      this->timer_number_->publish_state(h);
    }
    if (this->timer_remaining_sensor_ != nullptr) {
      this->timer_remaining_sensor_->publish_state(h * 60);
    }
    this->send_command_();
  }

 protected:
  std::vector<uint8_t> rx_buffer_;
  std::vector<uint8_t> last_status_pkt_;
  uint32_t last_rx_ms_{0};
  climate::ClimateMode last_active_mode_{climate::CLIMATE_MODE_COOL};

  // Флаги байта [8]
  bool display_active_{true};
  bool ionizer_active_{false};
  bool sleep_active_{false};
  bool turbo_active_{false};
  bool quiet_active_{false};

  // Реальная скорость вентилятора из байта [9]: 0 = Auto, 1 = Low, 2 = Medium, 3 = High
  uint8_t fan_raw_{0x00};

  uint8_t timer_flag_{0x00};
  uint8_t timer_hours_{0x00};
  uint8_t timer_mins_{0x00};
  int last_published_timer_hours_{-1};
  int last_published_remaining_mins_{-1};

  switch_::Switch *display_switch_{nullptr};
  switch_::Switch *ionizer_switch_{nullptr};
  switch_::Switch *sleep_switch_{nullptr};
  switch_::Switch *turbo_switch_{nullptr};
  number::Number *timer_number_{nullptr};
  sensor::Sensor *timer_remaining_sensor_{nullptr};

  void apply_flag_(switch_::Switch *sw, bool state) {
    if (sw != nullptr) {
      sw->publish_state(state);
    }
    this->send_command_();
  }

  // Тихий / Турбо / Сон взаимоисключающие; синхронизируем и отдельные переключатели, если они есть.
  void set_speed_flags_(bool quiet, bool turbo, bool sleep) {
    this->quiet_active_ = quiet;
    this->turbo_active_ = turbo;
    this->sleep_active_ = sleep;
    if (this->turbo_switch_ != nullptr) {
      this->turbo_switch_->publish_state(turbo);
    }
    if (this->sleep_switch_ != nullptr) {
      this->sleep_switch_->publish_state(sleep);
    }
  }

  // Скорость для HA из флагов байта [8] и реальной скорости из [9]. Приоритет: Турбо, Сон, Тихий.
  void update_fan_mode_() {
    if (this->turbo_active_) {
      this->set_custom_fan_mode_(FAN_TURBO);
    } else if (this->sleep_active_) {
      this->set_custom_fan_mode_(FAN_SLEEP);
    } else if (this->quiet_active_) {
      this->set_fan_mode_(climate::CLIMATE_FAN_QUIET);
    } else {
      this->set_fan_mode_(raw_to_fan_(this->fan_raw_));
    }
  }

  static uint8_t fan_to_raw_(climate::ClimateFanMode fm, uint8_t fallback) {
    switch (fm) {
      case climate::CLIMATE_FAN_AUTO:
        return 0x00;
      case climate::CLIMATE_FAN_LOW:
        return 0x01;
      case climate::CLIMATE_FAN_MEDIUM:
        return 0x02;
      case climate::CLIMATE_FAN_HIGH:
        return 0x03;
      default:
        return fallback;
    }
  }

  static climate::ClimateFanMode raw_to_fan_(uint8_t raw) {
    switch (raw) {
      case 0x01:
        return climate::CLIMATE_FAN_LOW;
      case 0x02:
        return climate::CLIMATE_FAN_MEDIUM;
      case 0x03:
        return climate::CLIMATE_FAN_HIGH;
      default:
        return climate::CLIMATE_FAN_AUTO;
    }
  }

  std::string format_hex_(const uint8_t *data, size_t len) {
    std::string res;
    char buf[4];
    for (size_t i = 0; i < len; i++) {
      if (i > 0)
        res += ".";
      snprintf(buf, sizeof(buf), "%02X", data[i]);
      res += buf;
    }
    return res;
  }

  uint8_t calc_crc_(const uint8_t *data, size_t len) {
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i++) {
      sum += data[i];
    }
    return (uint8_t) (sum & 0xFF);
  }

  void process_packet_(const std::vector<uint8_t> &pkt) {
    std::string hex_str = this->format_hex_(pkt.data(), pkt.size());

    if (pkt.size() != 22) {
      ESP_LOGW(TAG, "Unknown packet size (%zu != 22): %s", pkt.size(), hex_str.c_str());
      return;
    }

    uint8_t crc = this->calc_crc_(pkt.data(), 21);
    if (crc != pkt[21]) {
      ESP_LOGW(TAG, "CRC Error (calc 0x%02X != pkt 0x%02X): %s", crc, pkt[21], hex_str.c_str());
      return;
    }

    if (pkt[2] == 0x02) {
      ESP_LOGD(TAG, "[AC ACK 0x02] %s", hex_str.c_str());
      return;
    }

    if (pkt[2] != 0x01) {
      ESP_LOGW(TAG, "Unknown packet type [2]=0x%02X: %s", pkt[2], hex_str.c_str());
      return;
    }

    // Diff-лог: пишем только реальные изменения статуса.
    bool changed = false;
    if (this->last_status_pkt_.size() == 22) {
      std::string diff;
      char dbuf[32];
      for (size_t i = 3; i < 21; i++) {
        if (pkt[i] != this->last_status_pkt_[i]) {
          snprintf(dbuf, sizeof(dbuf), "[%zu]:%02X->%02X ", i, this->last_status_pkt_[i], pkt[i]);
          diff += dbuf;
        }
      }
      if (!diff.empty()) {
        changed = true;
        ESP_LOGI(TAG, "[AC STATUS CHANGED] %s | Diff: %s", hex_str.c_str(), diff.c_str());
      }
    } else {
      changed = true;
      ESP_LOGI(TAG, "[AC INITIAL STATUS] %s", hex_str.c_str());
    }
    this->last_status_pkt_ = pkt;

    // [3] Питание, [7] Режим
    const bool power_on = (pkt[3] == 0x01);

    switch (pkt[7]) {
      case 0x00: this->last_active_mode_ = climate::CLIMATE_MODE_AUTO; break;
      case 0x01: this->last_active_mode_ = climate::CLIMATE_MODE_COOL; break;
      case 0x02: this->last_active_mode_ = climate::CLIMATE_MODE_DRY; break;
      case 0x03: this->last_active_mode_ = climate::CLIMATE_MODE_HEAT; break;
      case 0x04: this->last_active_mode_ = climate::CLIMATE_MODE_FAN_ONLY; break;
      default:
        if (changed) {
          ESP_LOGW(TAG, "Unknown mode [7]=0x%02X", pkt[7]);
        }
        break;
    }

    this->mode = power_on ? this->last_active_mode_ : climate::CLIMATE_MODE_OFF;

    // [8] Флаги: 0x10 Дисплей, 0x08 Турбо, 0x04 Тихий, 0x02 Листочек (ионизатор), 0x01 Сон
    this->sleep_active_ = (pkt[8] & B8_SLEEP) != 0;
    this->ionizer_active_ = (pkt[8] & B8_IONIZER) != 0;
    this->quiet_active_ = (pkt[8] & B8_QUIET) != 0;
    this->turbo_active_ = (pkt[8] & B8_TURBO) != 0;
    this->display_active_ = (pkt[8] & B8_DISPLAY) != 0;

    const uint8_t unknown_b8 = (uint8_t) (pkt[8] & ~B8_KNOWN);
    if (unknown_b8 != 0 && changed) {
      ESP_LOGW(TAG, "Unknown bits in byte [8]=0x%02X (mask: 0x%02X)", pkt[8], unknown_b8);
    }

    // [9] Скорость вентилятора; режим QUIET определяется флагом [8], а реальная скорость сохраняется
    if (pkt[9] <= 0x03) {
      this->fan_raw_ = pkt[9];
    } else if (changed) {
      ESP_LOGW(TAG, "Unknown fan speed [9]=0x%02X", pkt[9]);
    }
    this->update_fan_mode_();

    // [5] и [6] Жалюзи
    const bool v_swing = (pkt[5] == 0x00);
    const bool h_swing = (pkt[6] & 0x02) != 0;

    if (v_swing && h_swing) {
      this->swing_mode = climate::CLIMATE_SWING_BOTH;
    } else if (v_swing) {
      this->swing_mode = climate::CLIMATE_SWING_VERTICAL;
    } else if (h_swing) {
      this->swing_mode = climate::CLIMATE_SWING_HORIZONTAL;
    } else {
      this->swing_mode = climate::CLIMATE_SWING_OFF;
    }

    this->target_temperature = (float) pkt[10];
    this->current_temperature = (float) pkt[11];

    // [12..14] Таймер: [12]=0x02 (активен), [13]=часы, [14]=минуты
    this->timer_flag_ = pkt[12];
    this->timer_hours_ = pkt[13];
    this->timer_mins_ = pkt[14];

    const int active_hours = (pkt[12] == 0x02) ? (int) pkt[13] : 0;
    const int remaining_mins = (pkt[12] == 0x02) ? ((int) pkt[13] * 60 + (int) pkt[14]) : 0;

    if (this->timer_number_ != nullptr && active_hours != this->last_published_timer_hours_) {
      this->timer_number_->publish_state(active_hours);
      this->last_published_timer_hours_ = active_hours;
    }
    if (this->timer_remaining_sensor_ != nullptr && remaining_mins != this->last_published_remaining_mins_) {
      this->timer_remaining_sensor_->publish_state(remaining_mins);
      this->last_published_remaining_mins_ = remaining_mins;
    }

    // Публикация состояний переключателей в HA
    if (this->display_switch_ != nullptr) {
      this->display_switch_->publish_state(this->display_active_);
    }
    if (this->ionizer_switch_ != nullptr) {
      this->ionizer_switch_->publish_state(this->ionizer_active_);
    }
    if (this->sleep_switch_ != nullptr) {
      this->sleep_switch_->publish_state(this->sleep_active_);
    }
    if (this->turbo_switch_ != nullptr) {
      this->turbo_switch_->publish_state(this->turbo_active_);
    }

    this->publish_state();
  }

  void send_command_() {
    uint8_t pkt[22] = {0};
    pkt[0] = 0xAA;
    pkt[1] = 0x14;
    pkt[2] = 0x02;

    pkt[3] = (this->mode == climate::CLIMATE_MODE_OFF) ? 0x00 : 0x01;
    pkt[4] = 0x00;

    const bool v_swing = (this->swing_mode == climate::CLIMATE_SWING_VERTICAL ||
                          this->swing_mode == climate::CLIMATE_SWING_BOTH);
    const bool h_swing = (this->swing_mode == climate::CLIMATE_SWING_HORIZONTAL ||
                          this->swing_mode == climate::CLIMATE_SWING_BOTH);

    pkt[5] = v_swing ? 0x00 : 0x06;
    pkt[6] = 0x18 | (h_swing ? 0x02 : 0x00);

    climate::ClimateMode active_m =
        (this->mode == climate::CLIMATE_MODE_OFF) ? this->last_active_mode_ : this->mode;
    switch (active_m) {
      case climate::CLIMATE_MODE_AUTO: pkt[7] = 0x00; break;
      case climate::CLIMATE_MODE_COOL: pkt[7] = 0x01; break;
      case climate::CLIMATE_MODE_DRY: pkt[7] = 0x02; break;
      case climate::CLIMATE_MODE_HEAT: pkt[7] = 0x03; break;
      case climate::CLIMATE_MODE_FAN_ONLY: pkt[7] = 0x04; break;
      default: pkt[7] = 0x00; break;
    }

    // [8] Флаги
    uint8_t b8 = 0x00;
    if (this->display_active_) b8 |= B8_DISPLAY;
    if (this->turbo_active_) b8 |= B8_TURBO;
    if (this->quiet_active_) b8 |= B8_QUIET;
    if (this->ionizer_active_) b8 |= B8_IONIZER;
    if (this->sleep_active_) b8 |= B8_SLEEP;
    pkt[8] = b8;

    // [9] Реальная скорость вентилятора (QUIET её не меняет)
    pkt[9] = this->fan_raw_;

    uint8_t temp = (uint8_t) roundf(this->target_temperature);
    if (temp < 16) temp = 16;
    if (temp > 32) temp = 32;
    pkt[10] = temp;

    pkt[11] = 0x00;

    // [12..14] Передаем текущее состояние таймера, чтобы не сбрасывать его другими командами
    if (this->timer_flag_ == 0x02) {
      pkt[12] = 0x02;
      pkt[13] = this->timer_hours_;
      pkt[14] = this->timer_mins_;
    } else {
      pkt[12] = 0x00;
      pkt[13] = 0x00;
      pkt[14] = 0x00;
    }

    pkt[21] = this->calc_crc_(pkt, 21);

    ESP_LOGI(TAG, "[ESP TX 0x02] %s", this->format_hex_(pkt, 22).c_str());
    this->write_array(pkt, 22);
  }
};

}  // namespace rusklimat
}  // namespace esphome
