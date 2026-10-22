import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate, uart, switch, number, sensor
from esphome.const import CONF_ID
from . import rusklimat_ns

DEPENDENCIES = ["uart"]

CONF_DISPLAY_SWITCH = "display_switch"
CONF_IONIZER_SWITCH = "ionizer_switch"
CONF_SLEEP_SWITCH = "sleep_switch"
CONF_TURBO_SWITCH = "turbo_switch"
CONF_TIMER_NUMBER = "timer_number"
CONF_TIMER_REMAINING_SENSOR = "timer_remaining_sensor"

RusklimatClimate = rusklimat_ns.class_(
    "RusklimatClimate", climate.Climate, cg.PollingComponent, uart.UARTDevice
)

CONFIG_SCHEMA = (
    climate.climate_schema(RusklimatClimate)
    .extend(
        {
            cv.Optional(CONF_DISPLAY_SWITCH): cv.use_id(switch.Switch),
            cv.Optional(CONF_IONIZER_SWITCH): cv.use_id(switch.Switch),
            cv.Optional(CONF_SLEEP_SWITCH): cv.use_id(switch.Switch),
            cv.Optional(CONF_TURBO_SWITCH): cv.use_id(switch.Switch),
            cv.Optional(CONF_TIMER_NUMBER): cv.use_id(number.Number),
            cv.Optional(CONF_TIMER_REMAINING_SENSOR): cv.use_id(sensor.Sensor),
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.polling_component_schema("2s"))
)

# conf key -> setter компонента
_SWITCHES = {
    CONF_DISPLAY_SWITCH: "set_display_switch",
    CONF_IONIZER_SWITCH: "set_ionizer_switch",
    CONF_SLEEP_SWITCH: "set_sleep_switch",
    CONF_TURBO_SWITCH: "set_turbo_switch",
}


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await climate.register_climate(var, config)
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
