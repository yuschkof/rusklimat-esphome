#include "rusklimat.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace rusklimat {

static const char *const TAG = "rusklimat";

static constexpr uint8_t FRAME_START = 0xAA;
static constexpr uint8_t TYPE_STATUS = 0x01;   // статус от кондиционера
static constexpr uint8_t TYPE_COMMAND = 0x02;  // команда от ESP; кондиционер отвечает на неё ACK того же типа

// Пауза внутри кадра дольше этого времени считается обрывом: 22 байта на 9600 бод идут ~23 мс.
static constexpr uint32_t RX_TIMEOUT_MS = 100;

// Байт [5]: вертикальные жалюзи
static constexpr uint8_t V_SWING_ON = 0x00;
static constexpr uint8_t V_SWING_OFF = 0x06;

// Байт [6]: в команде всегда базовые биты 0x18, горизонтальные жалюзи — 0x02
static constexpr uint8_t B6_BASE = 0x18;
static constexpr uint8_t B6_H_SWING = 0x02;

// Байт [8]: независимые флаги дополнительных функций
static constexpr uint8_t B8_SLEEP = 0x01;
static constexpr uint8_t B8_IONIZER = 0x02;
static constexpr uint8_t B8_QUIET = 0x04;
static constexpr uint8_t B8_TURBO = 0x08;
static constexpr uint8_t B8_DISPLAY = 0x10;
static constexpr uint8_t B8_KNOWN = B8_SLEEP | B8_IONIZER | B8_QUIET | B8_TURBO | B8_DISPLAY;

// Байты [12..14]: таймер выключения
static constexpr uint8_t TIMER_ACTIVE = 0x02;
static constexpr uint8_t TIMER_MAX_HOURS = 24;

static constexpr uint8_t MIN_TEMP = 16;
static constexpr uint8_t MAX_TEMP = 32;

static uint8_t checksum(const uint8_t *data, size_t len) {
  uint8_t sum = 0;
  for (size_t i = 0; i < len; i++)
    sum += data[i];
  return sum;
}

// Hex-дамп кадра в стековый буфер, без кучи
struct HexDump {
  char buf[format_hex_pretty_size(MAX_FRAME_SIZE)];
  HexDump(const uint8_t *data, size_t len) { format_hex_pretty_to(this->buf, data, len, '.'); }
  const char *c_str() const { return this->buf; }
};

static uint8_t fan_to_raw(climate::ClimateFanMode fm, uint8_t fallback) {
  switch (fm) {
    case climate::CLIMATE_FAN_AUTO: return 0x00;
    case climate::CLIMATE_FAN_LOW: return 0x01;
    case climate::CLIMATE_FAN_MEDIUM: return 0x02;
    case climate::CLIMATE_FAN_HIGH: return 0x03;
    default: return fallback;
  }
}

static climate::ClimateFanMode raw_to_fan(uint8_t raw) {
  switch (raw) {
    case 0x01: return climate::CLIMATE_FAN_LOW;
    case 0x02: return climate::CLIMATE_FAN_MEDIUM;
    case 0x03: return climate::CLIMATE_FAN_HIGH;
    default: return climate::CLIMATE_FAN_AUTO;
  }
}

static uint8_t mode_to_raw(climate::ClimateMode mode) {
  switch (mode) {
    case climate::CLIMATE_MODE_COOL: return 0x01;
    case climate::CLIMATE_MODE_DRY: return 0x02;
    case climate::CLIMATE_MODE_HEAT: return 0x03;
    case climate::CLIMATE_MODE_FAN_ONLY: return 0x04;
    default: return 0x00;  // AUTO
  }
}

static bool raw_to_mode(uint8_t raw, climate::ClimateMode *mode) {
  switch (raw) {
    case 0x00: *mode = climate::CLIMATE_MODE_AUTO; return true;
    case 0x01: *mode = climate::CLIMATE_MODE_COOL; return true;
    case 0x02: *mode = climate::CLIMATE_MODE_DRY; return true;
    case 0x03: *mode = climate::CLIMATE_MODE_HEAT; return true;
    case 0x04: *mode = climate::CLIMATE_MODE_FAN_ONLY; return true;
    default: return false;
  }
}

void RusklimatClimate::dump_config() {
  LOG_CLIMATE("", "Rusklimat Climate", this);
  LOG_UPDATE_INTERVAL(this);
}

void RusklimatClimate::update() {
  static const uint8_t STATUS_REQUEST[] = {FRAME_START, 0x02, TYPE_STATUS, 0xAD};
  this->write_array(STATUS_REQUEST, sizeof(STATUS_REQUEST));
}

void RusklimatClimate::loop() {
  const uint32_t now = millis();

  // Оборванный кадр: сбрасываем буфер, иначе байты следующего кадра «приклеятся» к хвосту старого.
  if (this->rx_len_ != 0 && (now - this->last_rx_ms_) > RX_TIMEOUT_MS) {
    ESP_LOGW(TAG, "RX timeout, dropping partial frame: %s", HexDump(this->rx_buf_.data(), this->rx_len_).c_str());
    this->rx_len_ = 0;
  }

  uint8_t c;
  while (this->available() && this->read_byte(&c)) {
    this->last_rx_ms_ = now;

    if (this->rx_len_ == 0 && c != FRAME_START)
      continue;
    this->rx_buf_[this->rx_len_++] = c;
    if (this->rx_len_ < 2)
      continue;

    // [1] — число байт после него без CRC
    const size_t expected_len = static_cast<size_t>(this->rx_buf_[1]) + 2;
    if (expected_len < 4 || expected_len > MAX_FRAME_SIZE) {
      this->rx_len_ = 0;
      continue;
    }
    if (this->rx_len_ == expected_len) {
      this->handle_frame_(this->rx_buf_.data(), this->rx_len_);
      this->rx_len_ = 0;
    }
  }
}

climate::ClimateTraits RusklimatClimate::traits() {
  auto traits = climate::ClimateTraits();
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
  traits.set_visual_min_temperature(MIN_TEMP);
  traits.set_visual_max_temperature(MAX_TEMP);
  traits.set_visual_temperature_step(1.0f);

  traits.set_supported_modes({
      climate::CLIMATE_MODE_OFF,
      climate::CLIMATE_MODE_AUTO,
      climate::CLIMATE_MODE_COOL,
      climate::CLIMATE_MODE_DRY,
      climate::CLIMATE_MODE_HEAT,
      climate::CLIMATE_MODE_FAN_ONLY,
  });

  // Тихий, Турбо и Сон — не скорости, а флаги байта [8]; они вынесены в отдельные переключатели.
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

  return traits;
}

void RusklimatClimate::control(const climate::ClimateCall &call) {
  if (!this->ready_to_send_("climate call")) {
    this->publish_state();  // вернуть в HA прежнее состояние
    return;
  }

  if (call.get_mode().has_value()) {
    this->mode = *call.get_mode();
    if (this->mode != climate::CLIMATE_MODE_OFF)
      this->last_active_mode_ = this->mode;
  }
  if (call.get_target_temperature().has_value())
    this->target_temperature = *call.get_target_temperature();
  if (call.get_fan_mode().has_value()) {
    this->fan_raw_ = fan_to_raw(*call.get_fan_mode(), this->fan_raw_);
    this->fan_mode = raw_to_fan(this->fan_raw_);
  }
  if (call.get_swing_mode().has_value())
    this->swing_mode = *call.get_swing_mode();

  this->send_command_();
  this->publish_state();
}

void RusklimatClimate::set_display(bool state) {
  if (this->ready_to_send_("display"))
    this->display_active_ = state;
  this->publish_switches_();
  this->send_command_();
}

void RusklimatClimate::set_ionizer(bool state) {
  if (this->ready_to_send_("ionizer"))
    this->ionizer_active_ = state;
  this->publish_switches_();
  this->send_command_();
}

// Сон совместим с Тихим, но выключает Турбо
void RusklimatClimate::set_sleep(bool state) {
  if (this->ready_to_send_("sleep"))
    this->set_flags_(this->quiet_active_, state ? false : this->turbo_active_, state);
  this->publish_switches_();
  this->send_command_();
}

// Тихий совместим со Сном, но выключает Турбо
void RusklimatClimate::set_quiet(bool state) {
  if (this->ready_to_send_("quiet"))
    this->set_flags_(state, state ? false : this->turbo_active_, this->sleep_active_);
  this->publish_switches_();
  this->send_command_();
}

// Турбо выключает Тихий и Сон
void RusklimatClimate::set_turbo(bool state) {
  if (this->ready_to_send_("turbo"))
    this->set_flags_(state ? false : this->quiet_active_, state, state ? false : this->sleep_active_);
  this->publish_switches_();
  this->send_command_();
}

// 0 — выключить таймер, 1..24 — выключить кондиционер через столько часов
void RusklimatClimate::set_timer_hours(float hours) {
  if (!this->ready_to_send_("timer")) {
    this->published_timer_hours_ = -1;  // форсировать повторную публикацию прежнего значения
    this->publish_timer_(this->timer_active_ ? this->timer_hours_ : 0,
                         this->timer_active_ ? this->timer_hours_ * 60 + this->timer_mins_ : 0);
    return;
  }

  const int h = std::max(0, std::min<int>(lroundf(hours), TIMER_MAX_HOURS));
  this->timer_active_ = h > 0;
  this->timer_hours_ = h;
  this->timer_mins_ = 0;
  this->publish_timer_(h, h * 60);
  this->send_command_();
}

// До первого статуса ESP не знает, в каком состоянии кондиционер. Команда из значений по умолчанию
// (режим OFF) выключила бы его, поэтому до этого момента команды не отправляются.
bool RusklimatClimate::ready_to_send_(const char *what) {
  if (this->has_status_)
    return true;
  ESP_LOGW(TAG, "Ignoring %s: no status from the AC yet", what);
  return false;
}

void RusklimatClimate::set_flags_(bool quiet, bool turbo, bool sleep) {
  this->quiet_active_ = quiet;
  this->turbo_active_ = turbo;
  this->sleep_active_ = sleep;
}

void RusklimatClimate::publish_switches_() {
  if (this->display_switch_ != nullptr)
    this->display_switch_->publish_state(this->display_active_);
  if (this->ionizer_switch_ != nullptr)
    this->ionizer_switch_->publish_state(this->ionizer_active_);
  if (this->sleep_switch_ != nullptr)
    this->sleep_switch_->publish_state(this->sleep_active_);
  if (this->quiet_switch_ != nullptr)
    this->quiet_switch_->publish_state(this->quiet_active_);
  if (this->turbo_switch_ != nullptr)
    this->turbo_switch_->publish_state(this->turbo_active_);
}

void RusklimatClimate::publish_timer_(int hours, int remaining_mins) {
  if (this->timer_number_ != nullptr && hours != this->published_timer_hours_) {
    this->timer_number_->publish_state(hours);
    this->published_timer_hours_ = hours;
  }
  if (this->timer_remaining_sensor_ != nullptr && remaining_mins != this->published_remaining_mins_) {
    this->timer_remaining_sensor_->publish_state(remaining_mins);
    this->published_remaining_mins_ = remaining_mins;
  }
}

void RusklimatClimate::handle_frame_(const uint8_t *frame, size_t len) {
  if (len != FRAME_SIZE) {
    ESP_LOGW(TAG, "Unexpected frame size %u: %s", (unsigned) len, HexDump(frame, len).c_str());
    return;
  }

  const uint8_t crc = checksum(frame, FRAME_SIZE - 1);
  if (crc != frame[FRAME_SIZE - 1]) {
    ESP_LOGW(TAG, "CRC error (calc 0x%02X, got 0x%02X): %s", crc, frame[FRAME_SIZE - 1],
             HexDump(frame, len).c_str());
    return;
  }

  if (frame[2] == TYPE_COMMAND) {
    ESP_LOGV(TAG, "ACK: %s", HexDump(frame, len).c_str());
    return;
  }
  if (frame[2] != TYPE_STATUS) {
    ESP_LOGW(TAG, "Unknown frame type 0x%02X: %s", frame[2], HexDump(frame, len).c_str());
    return;
  }

  // Пишем в лог только изменения статуса, побайтно: «[8]:10->14»
  bool changed = false;
  if (!this->has_status_) {
    changed = true;
    ESP_LOGI(TAG, "Initial status: %s", HexDump(frame, len).c_str());
  } else {
    char diff[FRAME_SIZE * 12] = {0};
    size_t pos = 0;
    for (size_t i = 3; i < FRAME_SIZE - 1; i++) {
      if (frame[i] != this->last_status_[i] && pos < sizeof(diff)) {
        pos += snprintf(diff + pos, sizeof(diff) - pos, "[%u]:%02X->%02X ", (unsigned) i, this->last_status_[i],
                        frame[i]);
      }
    }
    if (pos != 0) {
      changed = true;
      ESP_LOGD(TAG, "Status changed: %s | %s", HexDump(frame, len).c_str(), diff);
    }
  }
  std::copy(frame, frame + FRAME_SIZE, this->last_status_.begin());
  this->has_status_ = true;

  this->apply_status_(frame, changed);
}

void RusklimatClimate::apply_status_(const uint8_t *pkt, bool changed) {
  // [3] питание, [7] режим; режим храним и при выключенном кондиционере, чтобы включать в нём же
  climate::ClimateMode mode;
  if (raw_to_mode(pkt[7], &mode)) {
    this->last_active_mode_ = mode;
  } else if (changed) {
    ESP_LOGW(TAG, "Unknown mode [7]=0x%02X", pkt[7]);
  }
  this->mode = (pkt[3] == 0x01) ? this->last_active_mode_ : climate::CLIMATE_MODE_OFF;

  // [5], [6] жалюзи
  const bool v_swing = pkt[5] == V_SWING_ON;
  const bool h_swing = (pkt[6] & B6_H_SWING) != 0;
  if (v_swing && h_swing) {
    this->swing_mode = climate::CLIMATE_SWING_BOTH;
  } else if (v_swing) {
    this->swing_mode = climate::CLIMATE_SWING_VERTICAL;
  } else if (h_swing) {
    this->swing_mode = climate::CLIMATE_SWING_HORIZONTAL;
  } else {
    this->swing_mode = climate::CLIMATE_SWING_OFF;
  }

  // [8] флаги
  this->sleep_active_ = (pkt[8] & B8_SLEEP) != 0;
  this->ionizer_active_ = (pkt[8] & B8_IONIZER) != 0;
  this->quiet_active_ = (pkt[8] & B8_QUIET) != 0;
  this->turbo_active_ = (pkt[8] & B8_TURBO) != 0;
  this->display_active_ = (pkt[8] & B8_DISPLAY) != 0;
  if ((pkt[8] & ~B8_KNOWN) != 0 && changed)
    ESP_LOGW(TAG, "Unknown bits in [8]=0x%02X", pkt[8]);

  // [9] скорость вентилятора
  if (pkt[9] <= 0x03) {
    this->fan_raw_ = pkt[9];
  } else if (changed) {
    ESP_LOGW(TAG, "Unknown fan speed [9]=0x%02X", pkt[9]);
  }
  this->fan_mode = raw_to_fan(this->fan_raw_);

  // [10] уставка, [11] температура в комнате
  this->target_temperature = pkt[10];
  this->current_temperature = pkt[11];

  // [12..14] таймер; [13]/[14] могут хранить старое значение и при выключенном таймере
  this->timer_active_ = pkt[12] == TIMER_ACTIVE;
  this->timer_hours_ = pkt[13];
  this->timer_mins_ = pkt[14];
  this->publish_timer_(this->timer_active_ ? pkt[13] : 0, this->timer_active_ ? pkt[13] * 60 + pkt[14] : 0);

  this->publish_switches_();
  this->publish_state();
}

void RusklimatClimate::send_command_() {
  if (!this->has_status_)
    return;

  uint8_t pkt[FRAME_SIZE] = {0};
  pkt[0] = FRAME_START;
  pkt[1] = FRAME_SIZE - 2;
  pkt[2] = TYPE_COMMAND;

  pkt[3] = this->mode == climate::CLIMATE_MODE_OFF ? 0x00 : 0x01;

  const bool v_swing =
      this->swing_mode == climate::CLIMATE_SWING_VERTICAL || this->swing_mode == climate::CLIMATE_SWING_BOTH;
  const bool h_swing =
      this->swing_mode == climate::CLIMATE_SWING_HORIZONTAL || this->swing_mode == climate::CLIMATE_SWING_BOTH;
  pkt[5] = v_swing ? V_SWING_ON : V_SWING_OFF;
  pkt[6] = B6_BASE | (h_swing ? B6_H_SWING : 0);

  pkt[7] = mode_to_raw(this->mode == climate::CLIMATE_MODE_OFF ? this->last_active_mode_ : this->mode);

  uint8_t b8 = 0;
  if (this->display_active_)
    b8 |= B8_DISPLAY;
  if (this->turbo_active_)
    b8 |= B8_TURBO;
  if (this->quiet_active_)
    b8 |= B8_QUIET;
  if (this->ionizer_active_)
    b8 |= B8_IONIZER;
  if (this->sleep_active_)
    b8 |= B8_SLEEP;
  pkt[8] = b8;

  pkt[9] = this->fan_raw_;  // флаги [8] реальную скорость не меняют
  pkt[10] = std::max<int>(MIN_TEMP, std::min<int>(MAX_TEMP, lroundf(this->target_temperature)));
  // [11] в команде всегда 0

  // Передаём текущий таймер, иначе любая команда его сбросит
  if (this->timer_active_) {
    pkt[12] = TIMER_ACTIVE;
    pkt[13] = this->timer_hours_;
    pkt[14] = this->timer_mins_;
  }

  pkt[FRAME_SIZE - 1] = checksum(pkt, FRAME_SIZE - 1);

  ESP_LOGD(TAG, "TX: %s", HexDump(pkt, FRAME_SIZE).c_str());
  this->write_array(pkt, FRAME_SIZE);
}

}  // namespace rusklimat
}  // namespace esphome
