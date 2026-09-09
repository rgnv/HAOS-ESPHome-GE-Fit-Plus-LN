import re

import esphome.codegen as cg
from esphome.components import binary_sensor, ble_client, sensor, text_sensor, time
import esphome.config_validation as cv
from esphome.const import CONF_DISABLED_BY_DEFAULT, CONF_ID, CONF_TIME_ID

CODEOWNERS = ["@rgnv"]
DEPENDENCIES = ["ble_client", "api"]
AUTO_LOAD = ["sensor", "text_sensor", "binary_sensor"]

ge_scale_ns = cg.esphome_ns.namespace("ge_scale")
GEScale = ge_scale_ns.class_("GEScale", cg.Component, ble_client.BLEClientNode)

CONF_HEIGHT = "height"
CONF_SEX = "sex"
CONF_BIRTHDAY = "birthday"
CONF_AGE = "age"
CONF_EXPECTED_WEIGHT = "expected_weight"
CONF_WEIGHT_TOLERANCE = "weight_tolerance"
CONF_WRITE_BACK = "write_back"
CONF_FILTER_GUESTS = "filter_guests"

CONF_IS_GUEST = "is_guest"
CONF_WEIGHT_GUEST = "weight_guest"

OHM = "\u03a9"

# (config key, C++ setter, sensor_schema kwargs)
NUMERIC_SENSORS = [
    ("weight", "set_weight_sensor",
     dict(unit_of_measurement="kg", accuracy_decimals=2, device_class="weight", state_class="measurement")),
    ("weight_lb", "set_weight_lb_sensor",
     dict(unit_of_measurement="lb", accuracy_decimals=1, device_class="weight", state_class="measurement")),
    ("bmi", "set_bmi_sensor",
     dict(accuracy_decimals=1, state_class="measurement")),
    ("measurement_id", "set_measurement_id_sensor",
     dict(accuracy_decimals=0, state_class="measurement", entity_category="diagnostic")),
    ("body_fat_percent", "set_body_fat_sensor",
     dict(unit_of_measurement="%", accuracy_decimals=1, state_class="measurement")),
    ("body_water_percent", "set_body_water_sensor",
     dict(unit_of_measurement="%", accuracy_decimals=1, state_class="measurement")),
    ("protein_percent", "set_protein_sensor",
     dict(unit_of_measurement="%", accuracy_decimals=1, state_class="measurement")),
    ("bone_mass_percent", "set_bone_mass_sensor",
     dict(unit_of_measurement="%", accuracy_decimals=1, state_class="measurement")),
    ("muscle_mass_percent", "set_muscle_mass_sensor",
     dict(unit_of_measurement="%", accuracy_decimals=1, state_class="measurement")),
    ("skeletal_muscle_percent", "set_skeletal_muscle_sensor",
     dict(unit_of_measurement="%", accuracy_decimals=1, state_class="measurement")),
    ("fat_free_mass", "set_fat_free_mass_sensor",
     dict(unit_of_measurement="kg", accuracy_decimals=2, device_class="weight", state_class="measurement")),
    ("whole_body_impedance", "set_whole_body_impedance_sensor",
     dict(unit_of_measurement=OHM, accuracy_decimals=0, state_class="measurement", entity_category="diagnostic")),
]

TEXT_SENSORS = [
    ("subject", "set_subject_sensor"),
    ("source", "set_source_sensor"),
]

# Hidden (disabled-by-default, diagnostic) estimate entities used when no impedance is
# available. The data is captured but stays off the main entities; enable them in HA to
# start recording if you change your mind.
ESTIMATE_SENSORS = [
    ("body_fat_percent_estimate", "set_body_fat_estimate_sensor", "%", 1, None),
    ("body_water_percent_estimate", "set_body_water_estimate_sensor", "%", 1, None),
    ("protein_percent_estimate", "set_protein_estimate_sensor", "%", 1, None),
    ("bone_mass_percent_estimate", "set_bone_mass_estimate_sensor", "%", 1, None),
    ("muscle_mass_percent_estimate", "set_muscle_mass_estimate_sensor", "%", 1, None),
    ("skeletal_muscle_percent_estimate", "set_skeletal_muscle_estimate_sensor", "%", 1, None),
    ("fat_free_mass_estimate", "set_fat_free_mass_estimate_sensor", "kg", 2, "weight"),
]


def _estimate_schema(unit, acc, dclass):
    kw = dict(unit_of_measurement=unit, accuracy_decimals=acc, state_class="measurement",
              entity_category="diagnostic")
    if dclass:
        kw["device_class"] = dclass
    return sensor.sensor_schema(**kw)

IMPEDANCE_KWARGS = dict(
    unit_of_measurement=OHM, accuracy_decimals=1, state_class="measurement", entity_category="diagnostic"
)


def _birthday(value):
    value = cv.string_strict(value)
    if not re.match(r"^\d{4}-\d{2}-\d{2}$", value):
        raise cv.Invalid("birthday must be in YYYY-MM-DD format")
    return value


_schema = {
    cv.GenerateID(): cv.declare_id(GEScale),
    cv.Optional("recording_capacity", default=16): cv.int_range(min=1, max=32),
    cv.Optional(CONF_HEIGHT, default=1.78): cv.positive_float,
    cv.Optional(CONF_SEX, default="male"): cv.one_of("male", "female", lower=True),
    cv.Optional(CONF_BIRTHDAY): _birthday,
    cv.Optional(CONF_AGE, default=25.0): cv.positive_float,
    cv.Optional(CONF_EXPECTED_WEIGHT, default=74.0): cv.positive_float,
    cv.Optional(CONF_WEIGHT_TOLERANCE, default=6.0): cv.positive_float,
    cv.Optional(CONF_WRITE_BACK, default=False): cv.boolean,
    cv.Optional(CONF_FILTER_GUESTS, default=False): cv.boolean,
    cv.Optional(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
    cv.Optional(CONF_IS_GUEST): binary_sensor.binary_sensor_schema(entity_category="diagnostic"),
    cv.Optional(CONF_WEIGHT_GUEST): sensor.sensor_schema(
        unit_of_measurement="lb", accuracy_decimals=1, device_class="weight",
        state_class="measurement", entity_category="diagnostic"),
}
for _key, _setter, _kwargs in NUMERIC_SENSORS:
    _schema[cv.Optional(_key)] = sensor.sensor_schema(**_kwargs)
for _key, _setter, _unit, _acc, _dclass in ESTIMATE_SENSORS:
    _schema[cv.Optional(_key)] = _estimate_schema(_unit, _acc, _dclass)
for _i in range(1, 9):
    _schema[cv.Optional(f"impedance_{_i}")] = sensor.sensor_schema(**IMPEDANCE_KWARGS)
for _key, _setter in TEXT_SENSORS:
    _schema[cv.Optional(_key)] = text_sensor.text_sensor_schema()

CONFIG_SCHEMA = (
    cv.Schema(_schema).extend(cv.COMPONENT_SCHEMA).extend(ble_client.BLE_CLIENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)

    cg.add_define("USE_API_HOMEASSISTANT_SERVICES")
    cg.add_define("USE_API_HOMEASSISTANT_ACTION_RESPONSES")
    cg.add(var.set_recording_capacity(config["recording_capacity"]))
    cg.add(var.set_height(config[CONF_HEIGHT]))
    cg.add(var.set_sex_male(config[CONF_SEX] == "male"))
    cg.add(var.set_age_fallback(config[CONF_AGE]))
    cg.add(var.set_expected_weight(config[CONF_EXPECTED_WEIGHT]))
    cg.add(var.set_weight_tolerance(config[CONF_WEIGHT_TOLERANCE]))
    cg.add(var.set_write_back(config[CONF_WRITE_BACK]))
    cg.add(var.set_filter_guests(config[CONF_FILTER_GUESTS]))

    if CONF_BIRTHDAY in config:
        year, month, day = (int(p) for p in config[CONF_BIRTHDAY].split("-"))
        cg.add(var.set_birthday(year, month, day))
    if CONF_TIME_ID in config:
        rtc = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time(rtc))

    for key, setter, _kwargs in NUMERIC_SENSORS:
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(var, setter)(sens))
    for key, setter, _unit, _acc, _dclass in ESTIMATE_SENSORS:
        if key in config:
            config[key][CONF_DISABLED_BY_DEFAULT] = True  # always hidden by default
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(var, setter)(sens))
    for i in range(1, 9):
        key = f"impedance_{i}"
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(var.set_impedance_sensor(i - 1, sens))
    for key, setter in TEXT_SENSORS:
        if key in config:
            ts = await text_sensor.new_text_sensor(config[key])
            cg.add(getattr(var, setter)(ts))
    if CONF_IS_GUEST in config:
        bs = await binary_sensor.new_binary_sensor(config[CONF_IS_GUEST])
        cg.add(var.set_is_guest_sensor(bs))
    if CONF_WEIGHT_GUEST in config:
        gw = await sensor.new_sensor(config[CONF_WEIGHT_GUEST])
        cg.add(var.set_weight_guest_sensor(gw))
