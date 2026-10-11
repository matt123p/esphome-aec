"""Optional diagnostic sensors for the two independent speech probabilities."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_ID, ENTITY_CATEGORY_DIAGNOSTIC, STATE_CLASS_MEASUREMENT, UNIT_PERCENT,
)

from . import AECSpeexDspComponent, aec_speexdsp_ns

DEPENDENCIES = ["aec_speexdsp"]
CONF_PARENT = "aec_speexdsp_id"
CONF_SPEEX = "speex_vad_probability"
CONF_GRU = "gru_probability"
ProbabilitySensors = aec_speexdsp_ns.class_("AECSpeexDspProbabilitySensors", cg.PollingComponent)

PROBABILITY_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_PERCENT,
    accuracy_decimals=2,
    state_class=STATE_CLASS_MEASUREMENT,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
)

CONFIG_SCHEMA = cv.All(
    cv.Schema({
        cv.GenerateID(): cv.declare_id(ProbabilitySensors),
        cv.GenerateID(CONF_PARENT): cv.use_id(AECSpeexDspComponent),
        cv.Optional(CONF_SPEEX): PROBABILITY_SCHEMA,
        cv.Optional(CONF_GRU): PROBABILITY_SCHEMA,
    }).extend(cv.polling_component_schema("500ms")),
    cv.has_at_least_one_key(CONF_SPEEX, CONF_GRU),
)


async def to_code(config):
    cg.add_define("USE_AEC_SPEEXDSP_PROBABILITY_SENSORS")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_PARENT])
    cg.add(var.set_parent(parent))
    for key in (CONF_SPEEX, CONF_GRU):
        if key in config:
            value = await sensor.new_sensor(config[key])
            cg.add(getattr(var, f"set_{key}_sensor")(value))
