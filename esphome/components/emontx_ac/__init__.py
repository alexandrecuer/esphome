import logging

import esphome.codegen as cg
from esphome.components import mqtt, uart
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_NAME, CONF_RX_BUFFER_SIZE, CONF_UART_ID
import esphome.final_validate as fv

CODEOWNERS = ["@FredM67"]
CONF_EMONTX_ID = "emontx_id"
CONF_TAG_NAME = "tag_name"
CONF_BASE_TOPIC = "base_topic"
CONF_MQTT_ID = "mqtt_id"
CONF_MQTT_AUTODISCOVERY = "mqtt_autodiscovery"
DEPENDENCIES = ["uart"]
_LOGGER = logging.getLogger(__name__)
# Ensure UART RX buffer size is large enough to handle data bursts from firmware
# The firmware can send ~2KB of configuration data in bursts which would
# overflow the default 256-byte buffer causing data loss and corruption
MINIMUM_RX_BUFFER_SIZE = 2048

emontx_ns = cg.esphome_ns.namespace("emontx")
EmonTx = emontx_ns.class_("EmonTx", cg.PollingComponent, uart.UARTDevice)


# Main configuration schema
CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(EmonTx),
            cv.Optional(CONF_NAME, default="emontx"): cv.string,
            cv.Optional(CONF_BASE_TOPIC, default="emon/emontx"): cv.string,
            cv.Optional(CONF_MQTT_AUTODISCOVERY, default=False): cv.boolean,
            cv.Optional(CONF_MQTT_ID): cv.use_id(mqtt.MQTTClientComponent),
        }
    )
    .extend(cv.polling_component_schema("10s"))
    .extend(uart.UART_DEVICE_SCHEMA)
)


def final_validate(config):

    full_config = fv.full_config.get()

    # Find the UART component config and ensure rx_buffer_size is adequate
    for uart_conf in full_config["uart"]:
        if uart_conf[CONF_ID] == config[CONF_UART_ID]:
            current_buffer_size = uart_conf.get(CONF_RX_BUFFER_SIZE, 256)
            if current_buffer_size < MINIMUM_RX_BUFFER_SIZE:
                uart_id_str = str(config[CONF_UART_ID])

                # If value is exactly 256 (UART default), assume it's the default and auto-upgrade
                # If it's any other value below 2048, assume user explicitly set it and raise error
                if current_buffer_size == 256:
                    # Using default value - auto-upgrade with info message
                    uart_conf[CONF_RX_BUFFER_SIZE] = MINIMUM_RX_BUFFER_SIZE
                    _LOGGER.info(
                        "emontx: Automatically setting UART rx_buffer_size to %d bytes (minimum required by emonTx firmware)",
                        MINIMUM_RX_BUFFER_SIZE,
                    )
                else:
                    # User explicitly configured a non-default value that's too small
                    raise cv.Invalid(
                        f"Component emontx requires UART '{uart_id_str}' to have "
                        f"rx_buffer_size of at least {MINIMUM_RX_BUFFER_SIZE} bytes "
                        f"(currently set to {current_buffer_size} bytes). "
                        f"The emonTx firmware sends ~2KB configuration bursts that would overflow smaller buffers. "
                        f"Please increase rx_buffer_size in your uart configuration.",
                        path=[CONF_UART_ID],
                    )
            break

    # Validate UART settings
    schema = uart.final_validate_device_schema(
        "emontx",
        baud_rate=115200,
        require_rx=True,
        data_bits=8,
        parity="NONE",
        stop_bits=1,
    )
    return schema(config)


FINAL_VALIDATE_SCHEMA = final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    if CONF_NAME in config:
        cg.add(var.set_name(config[CONF_NAME]))
    if CONF_MQTT_ID in config:
        mqtt_ = await cg.get_variable(config[CONF_MQTT_ID])
        cg.add(var.set_mqtt_client(mqtt_))

    if CONF_BASE_TOPIC in config:
        cg.add(var.set_base_topic(config[CONF_BASE_TOPIC]))
    if CONF_MQTT_AUTODISCOVERY in config:
        cg.add(var.set_mqtt_autodiscovery(config[CONF_MQTT_AUTODISCOVERY]))
