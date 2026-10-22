#pragma once

#include <vector>

#include "esphome/components/climate/climate.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace rusklimat {

class RusklimatClimate : public climate::Climate, public PollingComponent, public uart::UARTDevice {
 public:
  // Привязка кастомных переключателей для доп. функций
  void set_display_switch(switch_::Switch *sw) { display_switch_ = sw; }
  void set_eco_switch(switch_::Switch *sw) { eco_switch_ = sw; }
  void set_ifeel_switch(switch_::Switch *sw) { ifeel_switch_ = sw; }

  void dump_config() override;
  // Каждые 2 секунды (update_interval) отправляем запрос статуса AA.02.01.AD
  void update() override;
  void loop() override;

  // Управление доп. функциями из YAML-свитчей
  void set_display(bool state);
  void set_eco(bool state);
  void set_ifeel(bool state);

 protected:
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;

  uint8_t calc_crc_(const uint8_t *data, size_t len);
  void process_packet_(const std::vector<uint8_t> &pkt);
  void send_command_();

  std::vector<uint8_t> rx_buffer_;
  climate::ClimateMode last_active_mode_{climate::CLIMATE_MODE_COOL};

  uint8_t byte6_base_{0x18};  // 0x10 (дисплей) + 0x08 (базовый флаг/пищалка)
  uint8_t byte8_base_{0x10};  // Базовый ниббл 0x10 в байте 8
  uint8_t byte12_{0x00};

  switch_::Switch *display_switch_{nullptr};
  switch_::Switch *eco_switch_{nullptr};
  switch_::Switch *ifeel_switch_{nullptr};
};

enum class SwitchKind : uint8_t { DISPLAY, ECO, IFEEL };

class RusklimatSwitch : public switch_::Switch, public Parented<RusklimatClimate> {
 public:
  void set_kind(SwitchKind kind) { kind_ = kind; }

 protected:
  void write_state(bool state) override;

  SwitchKind kind_{SwitchKind::DISPLAY};
};

}  // namespace rusklimat
}  // namespace esphome
