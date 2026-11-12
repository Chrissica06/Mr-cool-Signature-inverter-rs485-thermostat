import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import uart
from esphome.const import CONF_ID

DEPENDENCIES = ["uart"]
CODEOWNERS = []

CONF_DE_PIN = "de_pin"
CONF_REPLY_DELAY = "reply_delay"
CONF_LOG_CHANGES = "log_changes"

tcl_bus_ns = cg.esphome_ns.namespace("tcl_bus")
TclBus = tcl_bus_ns.class_("TclBus", cg.Component, uart.UARTDevice)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(TclBus),
            # RS-485 driver enable (DE, tie RE to it). Omit for auto-direction modules.
            cv.Optional(CONF_DE_PIN): pins.gpio_output_pin_schema,
            # Silence to leave after a poll before answering.
            cv.Optional(CONF_REPLY_DELAY, default="3ms"): cv.positive_time_period_microseconds,
            # Log every byte that changes in frames from the unit (for troubleshooting).
            cv.Optional(CONF_LOG_CHANGES, default=False): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    if CONF_DE_PIN in config:
        pin = await cg.gpio_pin_expression(config[CONF_DE_PIN])
        cg.add(var.set_de_pin(pin))
    cg.add(var.set_reply_delay_us(config[CONF_REPLY_DELAY].total_microseconds))
    cg.add(var.set_log_changes(config[CONF_LOG_CHANGES]))
