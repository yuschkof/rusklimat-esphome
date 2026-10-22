#include "rusklimat.h"

#include <cmath>

#include "esphome/core/log.h"

namespace esphome {
namespace rusklimat {

static const char *const TAG = "rusklimat";

static const uint8_t PACKET_START = 0xAA;
static const size_t PACKET_SIZE = 22;
static const size_t MAX_PACKET_SIZE = 32;

void RusklimatClimate::dump_config() {
  LOG_CLIMATE("", "Rusklimat Climate", this);
  LOG_UPDATE_INTERVAL(this);
}

void RusklimatClimate::update() {
  uint8_t req[4] = {0xAA, 0x02, 0x01, 0xAD};
  this->write_array(req, 4);
}

void RusklimatClimate::loop() {
  while (this->available()) {
    uint8_t c;
    this->read_byte(&c);

    if (this->rx_buffer_.empty() && c != PACKET_START) {
      continue;  // Ждем стартовый байт 0xAA
    }

    this->rx_buffer_.push_back(c);

    // Проверяем длину пакета по второму байту
    if (this->rx_buffer_.size() >= 2) {
      size_t expected_len = this->rx_buffer_[1] + 2;  // +2 (AA и CRC)
      if (expected_len > MAX_PACKET_SIZE) {
        this->rx_buffer_.clear();
        continue;
      }
      if (this->rx_buffer_.size() == expected_len) {
        this->process_packet_(this->rx_buffer_);
        this->rx_buffer_.clear();
      }
    }
  }
}

climate::ClimateTraits RusklimatClimate::traits() {
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

  traits.set_supported_fan_modes({
      climate::CLIMATE_FAN_AUTO,
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

  traits.set_supported_presets({
      climate::CLIMATE_PRESET_NONE,
      climate::CLIMATE_PRESET_BOOST,  // Турбо (бит 0x08 в байте 8)
      climate::CLIMATE_PRESET_SLEEP,  // Ночной (бит 0x01 в байте 8)
  });

  return traits;
}

void RusklimatClimate::control(const climate::ClimateCall &call) {
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

  if (call.get_fan_mode().has_value()) {
    this->fan_mode = *call.get_fan_mode();
  }

  if (call.get_swing_mode().has_value()) {
    this->swing_mode = *call.get_swing_mode();
  }

  if (call.get_preset().has_value()) {
    this->preset = *call.get_preset();
  }

  this->send_command_();
  this->publish_state();
}

void RusklimatClimate::set_display(bool state) {
  if (state) {
    this->byte6_base_ |= 0x10;
  } else {
    this->byte6_base_ &= ~0x10;
  }
  if (this->display_switch_ != nullptr)
    this->display_switch_->publish_state(state);
  this->send_command_();
}

void RusklimatClimate::set_eco(bool state) {
  // Задел под байт [12] (или бит в байте 8, уточняется по логам)
  this->byte12_ = state ? 0x02 : 0x00;
  if (this->eco_switch_ != nullptr)
    this->eco_switch_->publish_state(state);
  this->send_command_();
}

void RusklimatClimate::set_ifeel(bool state) {
  // Задел под iFeel
  if (state) {
    this->byte8_base_ |= 0x04;
  } else {
    this->byte8_base_ &= ~0x04;
  }
  if (this->ifeel_switch_ != nullptr)
    this->ifeel_switch_->publish_state(state);
  this->send_command_();
}

uint8_t RusklimatClimate::calc_crc_(const uint8_t *data, size_t len) {
  uint16_t sum = 0;
  for (size_t i = 0; i < len; i++) {
    sum += data[i];
  }
  return (uint8_t) (sum & 0xFF);
}

void RusklimatClimate::process_packet_(const std::vector<uint8_t> &pkt) {
  if (pkt.size() != PACKET_SIZE)
    return;

  uint8_t crc = this->calc_crc_(pkt.data(), PACKET_SIZE - 1);
  if (crc != pkt[PACKET_SIZE - 1]) {
    ESP_LOGW(TAG, "CRC Error: calc 0x%02X != pkt 0x%02X", crc, pkt[PACKET_SIZE - 1]);
    return;
  }

  // Парсим только пакеты статуса от кондиционера (байт [2] == 0x01)
  if (pkt[2] != 0x01)
    return;

  ESP_LOGV(TAG, "Status: %s", format_hex_pretty(pkt).c_str());

  bool power_on = (pkt[3] == 0x01);

  // Сохраняем выбранный режим даже если кондиционер выключен
  switch (pkt[7]) {
    case 0x00: this->last_active_mode_ = climate::CLIMATE_MODE_AUTO; break;
    case 0x01: this->last_active_mode_ = climate::CLIMATE_MODE_COOL; break;
    case 0x02: this->last_active_mode_ = climate::CLIMATE_MODE_DRY; break;
    case 0x03: this->last_active_mode_ = climate::CLIMATE_MODE_HEAT; break;
    case 0x04: this->last_active_mode_ = climate::CLIMATE_MODE_FAN_ONLY; break;
    default: break;
  }

  this->mode = power_on ? this->last_active_mode_ : climate::CLIMATE_MODE_OFF;

  // Скорость вентилятора (байт [9])
  switch (pkt[9]) {
    case 0x00: this->fan_mode = climate::CLIMATE_FAN_AUTO; break;
    case 0x01: this->fan_mode = climate::CLIMATE_FAN_LOW; break;
    case 0x02: this->fan_mode = climate::CLIMATE_FAN_MEDIUM; break;
    case 0x03: this->fan_mode = climate::CLIMATE_FAN_HIGH; break;
    default: break;
  }

  // Жалюзи: вертикальные в [5] (0x00 = вкл, 0x06 = выкл), горизонтальные в [6] (бит 0x02)
  bool v_swing = (pkt[5] == 0x00);
  bool h_swing = (pkt[6] & 0x02) != 0;
  this->byte6_base_ = pkt[6] & ~0x02;  // Сохраняем остальные биты байта [6]

  if (v_swing && h_swing) {
    this->swing_mode = climate::CLIMATE_SWING_BOTH;
  } else if (v_swing) {
    this->swing_mode = climate::CLIMATE_SWING_VERTICAL;
  } else if (h_swing) {
    this->swing_mode = climate::CLIMATE_SWING_HORIZONTAL;
  } else {
    this->swing_mode = climate::CLIMATE_SWING_OFF;
  }

  // Пресеты в байте [8]: 0x08 — Турбо, 0x01 — Ночной
  this->byte8_base_ = pkt[8] & ~(0x08 | 0x01);
  if (pkt[8] & 0x08) {
    this->preset = climate::CLIMATE_PRESET_BOOST;
  } else if (pkt[8] & 0x01) {
    this->preset = climate::CLIMATE_PRESET_SLEEP;
  } else {
    this->preset = climate::CLIMATE_PRESET_NONE;
  }

  // Температуры (байты [10] и [11])
  this->target_temperature = (float) pkt[10];
  this->current_temperature = (float) pkt[11];

  // Обновляем статус доп. свитчей
  if (this->display_switch_ != nullptr) {
    this->display_switch_->publish_state((pkt[6] & 0x10) != 0);
  }

  this->publish_state();
}

void RusklimatClimate::send_command_() {
  uint8_t pkt[PACKET_SIZE] = {0};
  pkt[0] = PACKET_START;
  pkt[1] = PACKET_SIZE - 2;
  pkt[2] = 0x02;  // Команда от ESP

  // [3] Питание: 0x00 = Выкл, 0x01 = Вкл
  pkt[3] = (this->mode == climate::CLIMATE_MODE_OFF) ? 0x00 : 0x01;
  pkt[4] = 0x00;

  // [5] и [6] Жалюзи
  bool v_swing = (this->swing_mode == climate::CLIMATE_SWING_VERTICAL ||
                  this->swing_mode == climate::CLIMATE_SWING_BOTH);
  bool h_swing = (this->swing_mode == climate::CLIMATE_SWING_HORIZONTAL ||
                  this->swing_mode == climate::CLIMATE_SWING_BOTH);

  pkt[5] = v_swing ? 0x00 : 0x06;
  pkt[6] = this->byte6_base_ | (h_swing ? 0x02 : 0x00);

  // [7] Режим работы
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

  // [8] Пресеты (Турбо / Сон)
  uint8_t b8 = this->byte8_base_;
  if (this->preset.has_value()) {
    if (*this->preset == climate::CLIMATE_PRESET_BOOST)
      b8 |= 0x08;
    if (*this->preset == climate::CLIMATE_PRESET_SLEEP)
      b8 |= 0x01;
  }
  pkt[8] = b8;

  // [9] Скорость вентилятора
  pkt[9] = 0x00;
  if (this->fan_mode.has_value()) {
    switch (*this->fan_mode) {
      case climate::CLIMATE_FAN_LOW: pkt[9] = 0x01; break;
      case climate::CLIMATE_FAN_MEDIUM: pkt[9] = 0x02; break;
      case climate::CLIMATE_FAN_HIGH: pkt[9] = 0x03; break;
      default: pkt[9] = 0x00; break;
    }
  }

  // [10] Целевая температура
  int temp = (int) roundf(this->target_temperature);
  if (temp < 16)
    temp = 16;
  if (temp > 32)
    temp = 32;
  pkt[10] = (uint8_t) temp;

  // [11] В команде всегда 0x00
  pkt[11] = 0x00;

  // [12..14] Доп. функции (по умолчанию в команде 0x00)
  pkt[12] = this->byte12_;
  pkt[13] = 0x00;
  pkt[14] = 0x00;

  // [21] Контрольная сумма
  pkt[PACKET_SIZE - 1] = this->calc_crc_(pkt, PACKET_SIZE - 1);

  ESP_LOGD(TAG, "Command: %s", format_hex_pretty(pkt, PACKET_SIZE).c_str());
  this->write_array(pkt, PACKET_SIZE);
}

void RusklimatSwitch::write_state(bool state) {
  switch (this->kind_) {
    case SwitchKind::DISPLAY: this->parent_->set_display(state); break;
    case SwitchKind::ECO: this->parent_->set_eco(state); break;
    case SwitchKind::IFEEL: this->parent_->set_ifeel(state); break;
  }
}

}  // namespace rusklimat
}  // namespace esphome
