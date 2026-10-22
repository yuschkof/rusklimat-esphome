import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_CONFIG

from .. import CONF_RUSKLIMAT_ID, rusklimat_ns
from ..climate import RusklimatClimate

RusklimatSwitch = rusklimat_ns.class_(
    "RusklimatSwitch", switch.Switch, cg.Parented.template(RusklimatClimate)
)
SwitchKind = rusklimat_ns.enum("SwitchKind", is_class=True)

SWITCH_KINDS = {
    "display": SwitchKind.DISPLAY,
    "eco": SwitchKind.ECO,
    "ifeel": SwitchKind.IFEEL,
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_RUSKLIMAT_ID): cv.use_id(RusklimatClimate),
        **{
            # DISABLED: при загрузке ничего не шлём в кондиционер, состояние приходит из статуса
            cv.Optional(key): switch.switch_schema(
                RusklimatSwitch,
                entity_category=ENTITY_CATEGORY_CONFIG,
                default_restore_mode="DISABLED",
            )
            for key in SWITCH_KINDS
        },
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_RUSKLIMAT_ID])
    for key, kind in SWITCH_KINDS.items():
        if key not in config:
            continue
        sw = await switch.new_switch(config[key])
        await cg.register_parented(sw, parent)
        cg.add(sw.set_kind(kind))
        cg.add(getattr(parent, f"set_{key}_switch")(sw))
