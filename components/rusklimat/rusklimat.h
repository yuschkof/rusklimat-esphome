#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "esphome/components/climate/climate.h"
#include "esphome/components/number/number.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"

namespace esphome {
namespace rusklimat {

// Статус от кондиционера и команда от ESP — по 22 байта, см. таблицу протокола в README.
static constexpr size_t FRAME_SIZE = 22;
static constexpr size_t MAX_FRAME_SIZE = 32;

class RusklimatClimate : public climate::Climate, public PollingComponent, public uart::UARTDevice {
 public:
  RusklimatClimate() : PollingComponent(2000) {}

  void set_display_switch(switch_::Switch *sw) { this->display_switch_ = sw; }
  void set_ionizer_switch(switch_::Switch *sw) { this->ionizer_switch_ = sw; }
  void set_sleep_switch(switch_::Switch *sw) { this->sleep_switch_ = sw; }
  void set_quiet_switch(switch_::Switch *sw) { this->quiet_switch_ = sw; }
  void set_turbo_switch(switch_::Switch *sw) { this->turbo_switch_ = sw; }
  void set_timer_number(number::Number *num) { this->timer_number_ = num; }
  void set_timer_remaining_sensor(sensor::Sensor *sens) { this->timer_remaining_sensor_ = sens; }

  void dump_config() override;
  void update() override;
  void loop() override;

  // Вызываются из lambda template-сущностей в YAML.
  void set_display(bool state);
  void set_ionizer(bool state);
  void set_sleep(bool state);
  void set_quiet(bool state);
  void set_turbo(bool state);
  void set_timer_hours(float hours);

 protected:
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;

  void handle_frame_(const uint8_t *frame, size_t len);
  void apply_status_(const uint8_t *pkt, bool changed);
  void send_command_();
  bool ready_to_send_(const char *what);

  // Ставит флаги Тихий / Турбо / Сон и синхронизирует их переключатели.
  void set_flags_(bool quiet, bool turbo, bool sleep);
  void publish_switches_();
  void publish_timer_(int hours, int remaining_mins);

  // Приём
  std::array<uint8_t, MAX_FRAME_SIZE> rx_buf_{};
  size_t rx_len_{0};
  uint32_t last_rx_ms_{0};
  std::array<uint8_t, FRAME_SIZE> last_status_{};
  bool has_status_{false};

  // Состояние, которого нет в полях Climate
  climate::ClimateMode last_active_mode_{climate::CLIMATE_MODE_COOL};
  uint8_t fan_raw_{0x00};  // байт [9]: 0 = Auto, 1 = Low, 2 = Medium, 3 = High
  bool display_active_{true};
  bool ionizer_active_{false};
  bool sleep_active_{false};
  bool quiet_active_{false};
  bool turbo_active_{false};
  bool timer_active_{false};
  uint8_t timer_hours_{0};
  uint8_t timer_mins_{0};
  int published_timer_hours_{-1};
  int published_remaining_mins_{-1};

  switch_::Switch *display_switch_{nullptr};
  switch_::Switch *ionizer_switch_{nullptr};
  switch_::Switch *sleep_switch_{nullptr};
  switch_::Switch *quiet_switch_{nullptr};
  switch_::Switch *turbo_switch_{nullptr};
  number::Number *timer_number_{nullptr};
  sensor::Sensor *timer_remaining_sensor_{nullptr};
};

}  // namespace rusklimat
}  // namespace esphome
