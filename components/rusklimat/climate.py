import esphome.codegen as cg
from esphome.components import climate, uart
import esphome.config_validation as cv

from . import rusklimat_ns

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["switch"]

RusklimatClimate = rusklimat_ns.class_(
    "RusklimatClimate", climate.Climate, cg.PollingComponent, uart.UARTDevice
)

CONFIG_SCHEMA = (
    climate.climate_schema(RusklimatClimate)
    .extend(cv.polling_component_schema("2s"))
    .extend(uart.UART_DEVICE_SCHEMA)
)


async def to_code(config):
    var = await climate.new_climate(config)
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
