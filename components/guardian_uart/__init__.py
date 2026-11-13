"""ESPHome configuration for the guardian_uart component.

This file is the Python half of the component. ESPHome runs it when it reads
guardian.yaml: it validates the ``guardian_uart:`` block and generates the C++
code that creates the GuardianUART object and its entities. The runtime
behaviour lives in guardian_uart.h / guardian_uart.cpp.

The component is a single hub that speaks the Bluelab Guardian sensor board's
serial protocol (docs/PROTOCOL.md) and exposes these optional entities:

  Readings            ``ec``, ``ph``, ``temperature`` (sensors)
  Alarm status        ``ec_alarm_status``, ``ph_alarm_status``,
                      ``temperature_alarm_status`` (text: ok / low / high)
  Reading status      ``ec_reading_status``, ``ph_reading_status``,
                      ``temperature_reading_status`` (text: ok / under range /
                      over range / disconnected / sensor fault / ...)
  Alarm thresholds    ``ec_low_alarm``, ``ec_high_alarm``, ``ph_low_alarm``,
                      ``ph_high_alarm``, ``temperature_low_alarm``,
                      ``temperature_high_alarm`` (writable numbers)
  Status              ``alarms_set``, ``ph_calibration_due``, ``link_connected``
                      (binary sensors) and ``device_mode`` (text)
  pH calibration      ``ph4_calibration``, ``ph7_calibration``,
                      ``ph10_calibration`` (text: calibrated / due / none) and
                      ``ph4_calibration_voltage``, ``ph7_calibration_voltage``,
                      ``ph10_calibration_voltage`` (sensors, V)
  Raw diagnostics     ``raw_status``, ``raw_calibration`` (text: the board's
                      state and calibration messages, for protocol work)

See README.md for what each entity means.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, number, sensor, text_sensor, uart
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_VOLTAGE,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_VOLT,
)

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "text_sensor", "binary_sensor", "number"]

# The C++ types this file generates code for.
guardian_uart_ns = cg.esphome_ns.namespace("guardian_uart")
GuardianUART = guardian_uart_ns.class_("GuardianUART", cg.Component, uart.UARTDevice)
GuardianNumber = guardian_uart_ns.class_("GuardianNumber", number.Number)

# The C++ SensorIndex enum: how the sensor board numbers its sensors.
SensorIndex = guardian_uart_ns.enum("SensorIndex")
SENSOR_EC = SensorIndex.SENSOR_EC
SENSOR_TEMPERATURE = SensorIndex.SENSOR_TEMPERATURE
SENSOR_PH = SensorIndex.SENSOR_PH

# Configuration keys, as written in guardian.yaml.
CONF_EC = "ec"
CONF_PH = "ph"
CONF_TEMPERATURE = "temperature"
CONF_EC_ALARM_STATUS = "ec_alarm_status"
CONF_PH_ALARM_STATUS = "ph_alarm_status"
CONF_TEMPERATURE_ALARM_STATUS = "temperature_alarm_status"
CONF_EC_READING_STATUS = "ec_reading_status"
CONF_PH_READING_STATUS = "ph_reading_status"
CONF_TEMPERATURE_READING_STATUS = "temperature_reading_status"
CONF_EC_LOW_ALARM = "ec_low_alarm"
CONF_EC_HIGH_ALARM = "ec_high_alarm"
CONF_PH_LOW_ALARM = "ph_low_alarm"
CONF_PH_HIGH_ALARM = "ph_high_alarm"
CONF_TEMPERATURE_LOW_ALARM = "temperature_low_alarm"
CONF_TEMPERATURE_HIGH_ALARM = "temperature_high_alarm"
CONF_ALARMS_SET = "alarms_set"
CONF_PH_CALIBRATION_DUE = "ph_calibration_due"
CONF_LINK_CONNECTED = "link_connected"
CONF_DEVICE_MODE = "device_mode"
CONF_PH4_CALIBRATION = "ph4_calibration"
CONF_PH7_CALIBRATION = "ph7_calibration"
CONF_PH10_CALIBRATION = "ph10_calibration"
CONF_PH4_CALIBRATION_VOLTAGE = "ph4_calibration_voltage"
CONF_PH7_CALIBRATION_VOLTAGE = "ph7_calibration_voltage"
CONF_PH10_CALIBRATION_VOLTAGE = "ph10_calibration_voltage"
CONF_RAW_STATUS = "raw_status"
CONF_RAW_CALIBRATION = "raw_calibration"

# The C++ PhBuffer enum: how the sensor board numbers its calibration records.
PhBuffer = guardian_uart_ns.enum("PhBuffer")
PH_BUFFER_4 = PhBuffer.PH_BUFFER_4
PH_BUFFER_7 = PhBuffer.PH_BUFFER_7
PH_BUFFER_10 = PhBuffer.PH_BUFFER_10

# Per-buffer calibration state entities: config key -> buffer.
CALIBRATION_STATE_SENSORS = {
    CONF_PH4_CALIBRATION: PH_BUFFER_4,
    CONF_PH7_CALIBRATION: PH_BUFFER_7,
    CONF_PH10_CALIBRATION: PH_BUFFER_10,
}

# Per-buffer calibration voltage entities: config key -> buffer.
CALIBRATION_VOLTAGE_SENSORS = {
    CONF_PH4_CALIBRATION_VOLTAGE: PH_BUFFER_4,
    CONF_PH7_CALIBRATION_VOLTAGE: PH_BUFFER_7,
    CONF_PH10_CALIBRATION_VOLTAGE: PH_BUFFER_10,
}

UNIT_MILLISIEMENS_PER_CM = "mS/cm"
ICON_ALARM = "mdi:alarm-light"
ICON_FLASK = "mdi:flask"
ICON_STATE_MACHINE = "mdi:state-machine"
ICON_CODE = "mdi:code-brackets"

# Fixed by the Guardian's hardware (docs/PROTOCOL.md §1).
BAUD_RATE = 57600

# Reading entities: config key -> sensor index.
READING_SENSORS = {
    CONF_EC: SENSOR_EC,
    CONF_PH: SENSOR_PH,
    CONF_TEMPERATURE: SENSOR_TEMPERATURE,
}

# Alarm status entities: config key -> sensor index.
ALARM_STATUS_SENSORS = {
    CONF_EC_ALARM_STATUS: SENSOR_EC,
    CONF_PH_ALARM_STATUS: SENSOR_PH,
    CONF_TEMPERATURE_ALARM_STATUS: SENSOR_TEMPERATURE,
}

# Reading status entities: config key -> sensor index.
READING_STATUS_SENSORS = {
    CONF_EC_READING_STATUS: SENSOR_EC,
    CONF_PH_READING_STATUS: SENSOR_PH,
    CONF_TEMPERATURE_READING_STATUS: SENSOR_TEMPERATURE,
}

# Alarm threshold entities: config key -> (sensor index, is_high_bound,
# minimum, maximum, step, unit). The minimum and maximum are the Guardian's
# measurement ranges (0-5.0 mS/cm, 0-14 pH, 0-50 C); they and the step only
# limit what Home Assistant offers, the sensor board stores whatever it is
# given.
ALARM_THRESHOLD_NUMBERS = {
    CONF_EC_LOW_ALARM: (SENSOR_EC, False, 0.0, 5.0, 0.1, UNIT_MILLISIEMENS_PER_CM),
    CONF_EC_HIGH_ALARM: (SENSOR_EC, True, 0.0, 5.0, 0.1, UNIT_MILLISIEMENS_PER_CM),
    CONF_PH_LOW_ALARM: (SENSOR_PH, False, 0.0, 14.0, 0.1, None),
    CONF_PH_HIGH_ALARM: (SENSOR_PH, True, 0.0, 14.0, 0.1, None),
    CONF_TEMPERATURE_LOW_ALARM: (SENSOR_TEMPERATURE, False, 0.0, 50.0, 0.5, UNIT_CELSIUS),
    CONF_TEMPERATURE_HIGH_ALARM: (SENSOR_TEMPERATURE, True, 0.0, 50.0, 0.5, UNIT_CELSIUS),
}


def _calibration_voltage_schema():
    """Returns the schema for one buffer's electrode voltage at calibration."""
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_VOLT,
        accuracy_decimals=4,
        device_class=DEVICE_CLASS_VOLTAGE,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    )


def _threshold_number_schema(unit):
    """Returns the schema for one writable alarm threshold."""
    options = {"entity_category": ENTITY_CATEGORY_CONFIG, "icon": ICON_ALARM}
    if unit is not None:
        options["unit_of_measurement"] = unit
    return number.number_schema(GuardianNumber, **options)


CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(GuardianUART),
            # Readings.
            cv.Optional(CONF_EC): sensor.sensor_schema(
                unit_of_measurement=UNIT_MILLISIEMENS_PER_CM,
                accuracy_decimals=2,
                icon=ICON_FLASK,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_PH): sensor.sensor_schema(
                accuracy_decimals=2,
                icon=ICON_FLASK,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=2,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            # Alarm status (ok / low / high).
            cv.Optional(CONF_EC_ALARM_STATUS): text_sensor.text_sensor_schema(),
            cv.Optional(CONF_PH_ALARM_STATUS): text_sensor.text_sensor_schema(),
            cv.Optional(CONF_TEMPERATURE_ALARM_STATUS): text_sensor.text_sensor_schema(),
            # Reading status (ok / under range / over range / disconnected / ...).
            cv.Optional(CONF_EC_READING_STATUS): text_sensor.text_sensor_schema(),
            cv.Optional(CONF_PH_READING_STATUS): text_sensor.text_sensor_schema(),
            cv.Optional(CONF_TEMPERATURE_READING_STATUS): text_sensor.text_sensor_schema(),
            # Alarm thresholds.
            cv.Optional(CONF_EC_LOW_ALARM): _threshold_number_schema(UNIT_MILLISIEMENS_PER_CM),
            cv.Optional(CONF_EC_HIGH_ALARM): _threshold_number_schema(UNIT_MILLISIEMENS_PER_CM),
            cv.Optional(CONF_PH_LOW_ALARM): _threshold_number_schema(None),
            cv.Optional(CONF_PH_HIGH_ALARM): _threshold_number_schema(None),
            cv.Optional(CONF_TEMPERATURE_LOW_ALARM): _threshold_number_schema(UNIT_CELSIUS),
            cv.Optional(CONF_TEMPERATURE_HIGH_ALARM): _threshold_number_schema(UNIT_CELSIUS),
            # Status.
            cv.Optional(CONF_ALARMS_SET): binary_sensor.binary_sensor_schema(),
            cv.Optional(CONF_PH_CALIBRATION_DUE): binary_sensor.binary_sensor_schema(),
            cv.Optional(CONF_LINK_CONNECTED): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_CONNECTIVITY,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_DEVICE_MODE): text_sensor.text_sensor_schema(
                icon=ICON_STATE_MACHINE,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            # Per-buffer pH calibration: state and voltage.
            cv.Optional(CONF_PH4_CALIBRATION): text_sensor.text_sensor_schema(icon=ICON_FLASK),
            cv.Optional(CONF_PH7_CALIBRATION): text_sensor.text_sensor_schema(icon=ICON_FLASK),
            cv.Optional(CONF_PH10_CALIBRATION): text_sensor.text_sensor_schema(icon=ICON_FLASK),
            cv.Optional(CONF_PH4_CALIBRATION_VOLTAGE): _calibration_voltage_schema(),
            cv.Optional(CONF_PH7_CALIBRATION_VOLTAGE): _calibration_voltage_schema(),
            cv.Optional(CONF_PH10_CALIBRATION_VOLTAGE): _calibration_voltage_schema(),
            # Raw board messages, for protocol work.
            cv.Optional(CONF_RAW_STATUS): text_sensor.text_sensor_schema(
                icon=ICON_CODE,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_RAW_CALIBRATION): text_sensor.text_sensor_schema(
                icon=ICON_CODE,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)

# Checks, after the whole configuration is loaded, that the UART bus the
# component is attached to is set up the way the sensor board needs.
FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "guardian_uart",
    baud_rate=BAUD_RATE,
    require_tx=True,
    require_rx=True,
    parity="NONE",
    stop_bits=1,
    data_bits=8,
)


async def to_code(config):
    """Generates the C++ code that builds the component and its entities."""
    hub = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(hub, config)
    await uart.register_uart_device(hub, config)

    for key, sensor_index in READING_SENSORS.items():
        if key in config:
            entity = await sensor.new_sensor(config[key])
            cg.add(hub.set_value_sensor(sensor_index, entity))

    for key, sensor_index in ALARM_STATUS_SENSORS.items():
        if key in config:
            entity = await text_sensor.new_text_sensor(config[key])
            cg.add(hub.set_alarm_status_sensor(sensor_index, entity))

    for key, sensor_index in READING_STATUS_SENSORS.items():
        if key in config:
            entity = await text_sensor.new_text_sensor(config[key])
            cg.add(hub.set_reading_status_sensor(sensor_index, entity))

    for key, spec in ALARM_THRESHOLD_NUMBERS.items():
        if key in config:
            sensor_index, is_high_bound, minimum, maximum, step, _unit = spec
            entity = await number.new_number(
                config[key], min_value=minimum, max_value=maximum, step=step
            )
            cg.add(entity.set_parent(hub))
            cg.add(entity.set_kind(sensor_index, is_high_bound))
            if is_high_bound:
                cg.add(hub.set_high_alarm_number(sensor_index, entity))
            else:
                cg.add(hub.set_low_alarm_number(sensor_index, entity))

    if CONF_ALARMS_SET in config:
        entity = await binary_sensor.new_binary_sensor(config[CONF_ALARMS_SET])
        cg.add(hub.set_alarms_set_sensor(entity))

    if CONF_PH_CALIBRATION_DUE in config:
        entity = await binary_sensor.new_binary_sensor(config[CONF_PH_CALIBRATION_DUE])
        cg.add(hub.set_ph_calibration_due_sensor(entity))

    if CONF_LINK_CONNECTED in config:
        entity = await binary_sensor.new_binary_sensor(config[CONF_LINK_CONNECTED])
        cg.add(hub.set_link_connected_sensor(entity))

    if CONF_DEVICE_MODE in config:
        entity = await text_sensor.new_text_sensor(config[CONF_DEVICE_MODE])
        cg.add(hub.set_device_mode_sensor(entity))

    for key, buffer in CALIBRATION_STATE_SENSORS.items():
        if key in config:
            entity = await text_sensor.new_text_sensor(config[key])
            cg.add(hub.set_calibration_state_sensor(buffer, entity))

    for key, buffer in CALIBRATION_VOLTAGE_SENSORS.items():
        if key in config:
            entity = await sensor.new_sensor(config[key])
            cg.add(hub.set_calibration_voltage_sensor(buffer, entity))

    if CONF_RAW_STATUS in config:
        entity = await text_sensor.new_text_sensor(config[CONF_RAW_STATUS])
        cg.add(hub.set_raw_status_sensor(entity))

    if CONF_RAW_CALIBRATION in config:
        entity = await text_sensor.new_text_sensor(config[CONF_RAW_CALIBRATION])
        cg.add(hub.set_raw_calibration_sensor(entity))
