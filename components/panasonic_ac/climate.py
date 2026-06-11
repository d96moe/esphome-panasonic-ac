from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_POWER,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_WATT,
)
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart, climate, sensor, select, switch, text_sensor, binary_sensor

AUTO_LOAD = ["switch", "sensor", "select", "text_sensor", "binary_sensor"]
DEPENDENCIES = ["uart"]

panasonic_ac_ns = cg.esphome_ns.namespace("panasonic_ac")
PanasonicAC = panasonic_ac_ns.class_(
    "PanasonicAC", cg.Component, uart.UARTDevice, climate.Climate
)
panasonic_ac_cnt_ns = panasonic_ac_ns.namespace("CNT")
PanasonicACCNT = panasonic_ac_cnt_ns.class_("PanasonicACCNT", PanasonicAC)
panasonic_ac_wlan_ns = panasonic_ac_ns.namespace("WLAN")
PanasonicACWLAN = panasonic_ac_wlan_ns.class_("PanasonicACWLAN", PanasonicAC)

PanasonicACSwitch = panasonic_ac_ns.class_(
    "PanasonicACSwitch", switch.Switch, cg.Component
)
PanasonicACSelect = panasonic_ac_ns.class_(
    "PanasonicACSelect", select.Select, cg.Component
)


CONF_HORIZONTAL_SWING_SELECT = "horizontal_swing_select"
CONF_VERTICAL_SWING_SELECT = "vertical_swing_select"
CONF_OUTSIDE_TEMPERATURE = "outside_temperature"
CONF_OUTSIDE_TEMPERATURE_OFFSET = "outside_temperature_offset"
CONF_CURRENT_TEMPERATURE_SENSOR = "current_temperature_sensor"
CONF_CURRENT_TEMPERATURE_OFFSET = "current_temperature_offset"
CONF_NANOEX_SWITCH = "nanoex_switch"
CONF_ECO_SWITCH = "eco_switch"
CONF_ECONAVI_SWITCH = "econavi_switch"
CONF_MILD_DRY_SWITCH = "mild_dry_switch"
CONF_CURRENT_POWER_CONSUMPTION = "current_power_consumption"
CONF_ERROR_CODE = "error_code"
CONF_RAW_PACKET = "raw_packet"
CONF_DEFROST_SENSOR = "defrost_sensor"
CONF_SERIAL_FAULT = "serial_fault"
CONF_WLAN = "wlan"
CONF_CNT = "cnt"

# Protocol-investigation debug sensors + dormant Phase B probe key
CONF_DEBUG_TELEMETRY_1 = "debug_telemetry_1"
CONF_DEBUG_TELEMETRY_2 = "debug_telemetry_2"
CONF_DEBUG_REPORT = "debug_report"
CONF_DEBUG_UNKNOWN = "debug_unknown"
CONF_PROBE_KEY = "probe_key"

HORIZONTAL_SWING_OPTIONS = ["auto", "left", "left_center", "center", "right_center", "right"]

VERTICAL_SWING_OPTIONS = ["swing", "auto", "up", "up_center", "center", "down_center", "down"]

SWITCH_SCHEMA = switch._SWITCH_SCHEMA.extend(cv.COMPONENT_SCHEMA).extend(
    {cv.GenerateID(): cv.declare_id(PanasonicACSwitch)}
)
SELECT_SCHEMA = select._SELECT_SCHEMA.extend(
    {cv.GenerateID(CONF_ID): cv.declare_id(PanasonicACSelect)}
)

PANASONIC_COMMON_SCHEMA = {
    cv.Optional(CONF_HORIZONTAL_SWING_SELECT): SELECT_SCHEMA,
    cv.Optional(CONF_VERTICAL_SWING_SELECT): SELECT_SCHEMA,
    cv.Optional(CONF_OUTSIDE_TEMPERATURE): sensor.sensor_schema(
        unit_of_measurement=UNIT_CELSIUS,
        accuracy_decimals=0,
        device_class=DEVICE_CLASS_TEMPERATURE,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    cv.Optional(CONF_NANOEX_SWITCH): SWITCH_SCHEMA,
}

SCHEMA = climate._CLIMATE_SCHEMA.extend(
    {
        cv.Optional(CONF_HORIZONTAL_SWING_SELECT): SELECT_SCHEMA,
        cv.Optional(CONF_VERTICAL_SWING_SELECT): SELECT_SCHEMA,
        cv.Optional(CONF_OUTSIDE_TEMPERATURE): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_NANOEX_SWITCH): SWITCH_SCHEMA,
        cv.Optional(CONF_ERROR_CODE): text_sensor.text_sensor_schema(
            icon="mdi:alert-circle-outline",
        ),
        cv.Optional(CONF_DEFROST_SENSOR): binary_sensor.binary_sensor_schema(
            device_class="cold",
        ),
    }
).extend(uart.UART_DEVICE_SCHEMA)

CONFIG_SCHEMA = cv.typed_schema(
    {
        CONF_WLAN: SCHEMA.extend(
            {
                cv.GenerateID(): cv.declare_id(PanasonicACWLAN),
                cv.Optional(CONF_RAW_PACKET): text_sensor.text_sensor_schema(
                    icon="mdi:code-brackets",
                ),
                # Protocol-investigation debug text sensors (raw hex dumps)
                cv.Optional(CONF_DEBUG_TELEMETRY_1): text_sensor.text_sensor_schema(
                    icon="mdi:code-brackets",
                ),
                cv.Optional(CONF_DEBUG_TELEMETRY_2): text_sensor.text_sensor_schema(
                    icon="mdi:code-brackets",
                ),
                cv.Optional(CONF_DEBUG_REPORT): text_sensor.text_sensor_schema(
                    icon="mdi:code-brackets",
                ),
                cv.Optional(CONF_DEBUG_UNKNOWN): text_sensor.text_sensor_schema(
                    icon="mdi:code-brackets",
                ),
                # Dormant Phase B: append one extra READ key to the poll request.
                # Omit (or set 0) for byte-identical default polling.
                cv.Optional(CONF_PROBE_KEY): cv.hex_uint8_t,
            }
        ),
        CONF_CNT: SCHEMA.extend(
            {
                cv.GenerateID(): cv.declare_id(PanasonicACCNT),
                cv.Optional(CONF_ECO_SWITCH): SWITCH_SCHEMA,
                cv.Optional(CONF_ECONAVI_SWITCH): SWITCH_SCHEMA,
                cv.Optional(CONF_MILD_DRY_SWITCH): SWITCH_SCHEMA,
                cv.Optional(CONF_CURRENT_TEMPERATURE_SENSOR): cv.use_id(sensor.Sensor),
                cv.Optional(CONF_CURRENT_POWER_CONSUMPTION): sensor.sensor_schema(
                  unit_of_measurement=UNIT_WATT,
                  accuracy_decimals=0,
                  device_class=DEVICE_CLASS_POWER,
                  state_class=STATE_CLASS_MEASUREMENT,
              ),
                cv.Optional(CONF_SERIAL_FAULT): binary_sensor.binary_sensor_schema(
                    device_class="problem",
                ),
            }
        ),
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await climate.register_climate(var, config)
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    if CONF_HORIZONTAL_SWING_SELECT in config:
        conf = config[CONF_HORIZONTAL_SWING_SELECT]
        swing_select = await select.new_select(conf, options=HORIZONTAL_SWING_OPTIONS)
        await cg.register_component(swing_select, conf)
        cg.add(var.set_horizontal_swing_select(swing_select))

    if CONF_VERTICAL_SWING_SELECT in config:
        conf = config[CONF_VERTICAL_SWING_SELECT]
        swing_select = await select.new_select(conf, options=VERTICAL_SWING_OPTIONS)
        await cg.register_component(swing_select, conf)
        cg.add(var.set_vertical_swing_select(swing_select))

    if CONF_OUTSIDE_TEMPERATURE in config:
        sens = await sensor.new_sensor(config[CONF_OUTSIDE_TEMPERATURE])
        cg.add(var.set_outside_temperature_sensor(sens))

    if CONF_OUTSIDE_TEMPERATURE_OFFSET in config:
        cg.add(var.set_outside_temperature_offset(config[CONF_OUTSIDE_TEMPERATURE_OFFSET]))

    for s in [CONF_ECO_SWITCH, CONF_NANOEX_SWITCH, CONF_MILD_DRY_SWITCH, CONF_ECONAVI_SWITCH]:
        if s in config:
            conf = config[s]
            a_switch = cg.new_Pvariable(conf[CONF_ID])
            await cg.register_component(a_switch, conf)
            await switch.register_switch(a_switch, conf)
            cg.add(getattr(var, f"set_{s}")(a_switch))

    if CONF_CURRENT_TEMPERATURE_SENSOR in config:
        sens = await cg.get_variable(config[CONF_CURRENT_TEMPERATURE_SENSOR])
        cg.add(var.set_current_temperature_sensor(sens))

    if CONF_CURRENT_TEMPERATURE_OFFSET in config:
        cg.add(var.set_current_temperature_offset(config[CONF_CURRENT_TEMPERATURE_OFFSET]))

    if CONF_CURRENT_POWER_CONSUMPTION in config:
        sens = await sensor.new_sensor(config[CONF_CURRENT_POWER_CONSUMPTION])
        cg.add(var.set_current_power_consumption_sensor(sens))

    if CONF_ERROR_CODE in config:
        ts = await text_sensor.new_text_sensor(config[CONF_ERROR_CODE])
        cg.add(var.set_error_code_text_sensor(ts))

    if CONF_RAW_PACKET in config:
        ts = await text_sensor.new_text_sensor(config[CONF_RAW_PACKET])
        cg.add(var.set_raw_packet_text_sensor(ts))

    if CONF_DEBUG_TELEMETRY_1 in config:
        ts = await text_sensor.new_text_sensor(config[CONF_DEBUG_TELEMETRY_1])
        cg.add(var.set_debug_telemetry_1_text_sensor(ts))

    if CONF_DEBUG_TELEMETRY_2 in config:
        ts = await text_sensor.new_text_sensor(config[CONF_DEBUG_TELEMETRY_2])
        cg.add(var.set_debug_telemetry_2_text_sensor(ts))

    if CONF_DEBUG_REPORT in config:
        ts = await text_sensor.new_text_sensor(config[CONF_DEBUG_REPORT])
        cg.add(var.set_debug_report_text_sensor(ts))

    if CONF_DEBUG_UNKNOWN in config:
        ts = await text_sensor.new_text_sensor(config[CONF_DEBUG_UNKNOWN])
        cg.add(var.set_debug_unknown_text_sensor(ts))

    if CONF_PROBE_KEY in config:
        cg.add(var.set_probe_key(config[CONF_PROBE_KEY]))

    if CONF_DEFROST_SENSOR in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_DEFROST_SENSOR])
        cg.add(var.set_defrost_sensor(sens))

    if CONF_SERIAL_FAULT in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_SERIAL_FAULT])
        cg.add(var.set_serial_fault_sensor(sens))
