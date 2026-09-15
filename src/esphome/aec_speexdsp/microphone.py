import esphome.codegen as cg
from esphome.components import audio, microphone
import esphome.config_validation as cv
from esphome.const import CONF_BITS_PER_SAMPLE, CONF_ID, CONF_NUM_CHANNELS, CONF_SAMPLE_RATE

from . import AECSpeexDspComponent, aec_speexdsp_ns

CONF_AEC_SPEEXDSP_ID = "aec_speexdsp_id"

AECSpeexDspMicrophone = aec_speexdsp_ns.class_(
    "AECSpeexDspMicrophone", microphone.Microphone, cg.Component
)


def _set_stream_limits(config):
    audio.set_stream_limits(
        min_bits_per_sample=16,
        max_bits_per_sample=16,
        min_channels=1,
        max_channels=1,
        min_sample_rate=16000,
        max_sample_rate=16000,
    )(config)
    return config


CONFIG_SCHEMA = cv.All(
    microphone.MICROPHONE_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(AECSpeexDspMicrophone),
            cv.GenerateID(CONF_AEC_SPEEXDSP_ID): cv.use_id(AECSpeexDspComponent),
            cv.Optional(CONF_BITS_PER_SAMPLE, default=16): cv.one_of(16),
            cv.Optional(CONF_NUM_CHANNELS, default=1): cv.one_of(1),
            cv.Optional(CONF_SAMPLE_RATE, default=16000): cv.one_of(16000),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _set_stream_limits,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await microphone.register_microphone(var, config)
    parent = await cg.get_variable(config[CONF_AEC_SPEEXDSP_ID])
    cg.add(var.set_parent(parent))
    cg.add(parent.set_microphone(var))
