import esphome.codegen as cg
from esphome.components import climate, number, sensor, switch, uart
import esphome.config_validation as cv

from . import rusklimat_ns

DEPENDENCIES = ["uart"]

CONF_DISPLAY_SWITCH = "display_switch"
CONF_IONIZER_SWITCH = "ionizer_switch"
CONF_SLEEP_SWITCH = "sleep_switch"
CONF_QUIET_SWITCH = "quiet_switch"
CONF_TURBO_SWITCH = "turbo_switch"
CONF_TIMER_NUMBER = "timer_number"
CONF_TIMER_REMAINING_SENSOR = "timer_remaining_sensor"

RusklimatClimate = rusklimat_ns.class_(
    "RusklimatClimate", climate.Climate, cg.PollingComponent, uart.UARTDevice
)

# Необязательные ссылки на template-сущности: компонент публикует в них состояние из статуса кондиционера.
_SWITCHES = {
    CONF_DISPLAY_SWITCH: "set_display_switch",
    CONF_IONIZER_SWITCH: "set_ionizer_switch",
    CONF_SLEEP_SWITCH: "set_sleep_switch",
    CONF_QUIET_SWITCH: "set_quiet_switch",
    CONF_TURBO_SWITCH: "set_turbo_switch",
}

CONFIG_SCHEMA = (
    climate.climate_schema(RusklimatClimate)
    .extend(
        {
            **{cv.Optional(key): cv.use_id(switch.Switch) for key in _SWITCHES},
            cv.Optional(CONF_TIMER_NUMBER): cv.use_id(number.Number),
            cv.Optional(CONF_TIMER_REMAINING_SENSOR): cv.use_id(sensor.Sensor),
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.polling_component_schema("2s"))
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "rusklimat",
    baud_rate=9600,
    require_tx=True,
    require_rx=True,
    data_bits=8,
    parity="NONE",
    stop_bits=1,
)


async def to_code(config):
    var = await climate.new_climate(config)
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    for key, setter in _SWITCHES.items():
        if key in config:
            sw = await cg.get_variable(config[key])
            cg.add(getattr(var, setter)(sw))

    if CONF_TIMER_NUMBER in config:
        num = await cg.get_variable(config[CONF_TIMER_NUMBER])
        cg.add(var.set_timer_number(num))

    if CONF_TIMER_REMAINING_SENSOR in config:
        sens = await cg.get_variable(config[CONF_TIMER_REMAINING_SENSOR])
        cg.add(var.set_timer_remaining_sensor(sens))
