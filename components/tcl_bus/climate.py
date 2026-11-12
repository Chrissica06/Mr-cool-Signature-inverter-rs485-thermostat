import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate, sensor

from . import TclBus, tcl_bus_ns

DEPENDENCIES = ["tcl_bus"]

CONF_TCL_BUS_ID = "tcl_bus_id"
CONF_ROOM_TEMPERATURE_SENSOR = "room_temperature_sensor"
CONF_ROOM_SENSOR_FAHRENHEIT = "room_sensor_fahrenheit"

TclClimate = tcl_bus_ns.class_("TclClimate", climate.Climate, cg.Component)

CONFIG_SCHEMA = (
    climate.climate_schema(TclClimate)
    .extend(
        {
            cv.GenerateID(CONF_TCL_BUS_ID): cv.use_id(TclBus),
            # Optional: a room temperature to show as "current temperature" and, while the
            # ESP is acting as the wall controller, to send to the unit in its place.
            cv.Optional(CONF_ROOM_TEMPERATURE_SENSOR): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_ROOM_SENSOR_FAHRENHEIT, default=False): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await climate.new_climate(config)
    await cg.register_component(var, config)
    bus = await cg.get_variable(config[CONF_TCL_BUS_ID])
    cg.add(var.set_bus(bus))
    if CONF_ROOM_TEMPERATURE_SENSOR in config:
        sens = await cg.get_variable(config[CONF_ROOM_TEMPERATURE_SENSOR])
        cg.add(var.set_room_sensor(sens, config[CONF_ROOM_SENSOR_FAHRENHEIT]))
