from esphome import pins
import esphome.codegen as cg
from esphome.components import audio_adc
from esphome.components.esp32 import add_idf_component
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@matt"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["audio", "microphone", "ring_buffer", "speaker"]

CONF_AEC_MODE = "aec_mode"
CONF_AGC = "agc"
CONF_AFE_INPUT_FORMAT = "afe_input_format"
CONF_AUDIO_ADC = "audio_adc"
CONF_BCLK_PIN = "bclk_pin"
CONF_DIAGNOSTIC_RAW_SLOT = "diagnostic_raw_slot"
CONF_DIN_PIN = "din_pin"
CONF_DOUT_PIN = "dout_pin"
CONF_FILTER_LENGTH = "filter_length"
CONF_I2S_PORT = "i2s_port"
CONF_LRCLK_PIN = "lrclk_pin"
CONF_MCLK_PIN = "mclk_pin"
CONF_MICROPHONE_SLOTS = "microphone_slots"
CONF_METERS = "meters"
CONF_NLP_LEVEL = "nlp_level"
CONF_REFERENCE_SLOT = "reference_slot"
CONF_REFERENCE_SOURCE = "reference_source"
CONF_REFERENCE_DELAY_SAMPLES = "reference_delay_samples"
CONF_RESAMPLER = "resampler"
CONF_TDM_SLOTS = "tdm_slots"
CONF_TX_SLOTS = "tx_slots"

CONF_NOISE_SUPPRESSION = "noise_suppression"
CONF_SPEECH_ENHANCEMENT = "speech_enhancement"
CONF_WAKENET = "wakenet"

aec_audio_ns = cg.esphome_ns.namespace("aec_audio")
AECAudioComponent = aec_audio_ns.class_("AECAudioComponent", cg.Component)
AECAudioMetersComponent = aec_audio_ns.class_("AECAudioMetersComponent", cg.Component)

AECAudioReferenceSource = aec_audio_ns.enum("AECAudioReferenceSource")
REFERENCE_SOURCES = {
    "analog_slot": AECAudioReferenceSource.AEC_AUDIO_REFERENCE_ANALOG_SLOT,
    "playback": AECAudioReferenceSource.AEC_AUDIO_REFERENCE_PLAYBACK,
}

AECAudioMode = aec_audio_ns.enum("AECAudioMode")
AEC_MODES = {
    "fd_low_cost": AECAudioMode.AEC_AUDIO_MODE_FD_LOW_COST,
    "fd_high_perf": AECAudioMode.AEC_AUDIO_MODE_FD_HIGH_PERF,
}

AECAudioNlpLevel = aec_audio_ns.enum("AECAudioNlpLevel")
NLP_LEVELS = {
    "normal": AECAudioNlpLevel.AEC_AUDIO_NLP_NORMAL,
    "aggressive": AECAudioNlpLevel.AEC_AUDIO_NLP_AGGRESSIVE,
    "very_aggressive": AECAudioNlpLevel.AEC_AUDIO_NLP_VERY_AGGRESSIVE,
}


def _validate_slots(config):
    slots = config[CONF_TDM_SLOTS]
    selected = config[CONF_MICROPHONE_SLOTS]
    reference = config[CONF_REFERENCE_SLOT]
    if any(slot >= slots for slot in selected + [reference] + config[CONF_TX_SLOTS]):
        raise cv.Invalid("All selected slots must be lower than tdm_slots")
    if len(set(selected)) != len(selected):
        raise cv.Invalid("Microphone slots must be distinct")
    if config[CONF_REFERENCE_SOURCE] == "analog_slot" and reference in selected:
        raise cv.Invalid("The analog reference slot must not also be a microphone slot")
    if (
        config[CONF_REFERENCE_SOURCE] == "analog_slot"
        and config[CONF_REFERENCE_DELAY_SAMPLES] != 0
    ):
        raise cv.Invalid("reference_delay_samples must be zero when using an analog reference slot")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(AECAudioComponent),
            cv.Required(CONF_AUDIO_ADC): cv.use_id(audio_adc.AudioAdc),
            cv.Required(CONF_MCLK_PIN): pins.internal_gpio_output_pin_number,
            cv.Required(CONF_BCLK_PIN): pins.internal_gpio_output_pin_number,
            cv.Required(CONF_LRCLK_PIN): pins.internal_gpio_output_pin_number,
            cv.Required(CONF_DIN_PIN): pins.internal_gpio_input_pin_number,
            cv.Required(CONF_DOUT_PIN): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_I2S_PORT, default=0): cv.int_range(min=0, max=2),
            cv.Optional(CONF_TDM_SLOTS, default=4): cv.int_range(min=4, max=4),
            cv.Optional(CONF_MICROPHONE_SLOTS, default=[0, 1]): cv.All(
                cv.ensure_list(cv.int_range(min=0, max=3)), cv.Length(min=2, max=2)
            ),
            cv.Optional(CONF_REFERENCE_SLOT, default=2): cv.int_range(min=0, max=3),
            cv.Optional(CONF_REFERENCE_SOURCE, default="analog_slot"): cv.one_of(
                *REFERENCE_SOURCES, lower=True
            ),
            cv.Optional(CONF_TX_SLOTS, default=[0, 1]): cv.All(
                cv.ensure_list(cv.int_range(min=0, max=3)), cv.Length(min=2, max=2)
            ),
            cv.Optional(CONF_DIAGNOSTIC_RAW_SLOT): cv.int_range(min=0, max=3),
            cv.Optional(CONF_AEC_MODE, default="fd_low_cost"): cv.enum(AEC_MODES, lower=True),
            cv.Optional(CONF_AGC, default=True): cv.boolean,
            cv.Optional(CONF_AFE_INPUT_FORMAT, default="mmnr"): cv.one_of("mmr", "mmnr", lower=True),
            cv.Optional(CONF_NLP_LEVEL, default="aggressive"): cv.enum(
                NLP_LEVELS, lower=True
            ),
            cv.Optional(CONF_FILTER_LENGTH, default=4): cv.int_range(min=1, max=16),
            cv.Optional(CONF_REFERENCE_DELAY_SAMPLES, default=0): cv.int_range(min=0, max=4000),
            cv.Optional(CONF_RESAMPLER, default=False): cv.boolean,
            cv.Optional(CONF_NOISE_SUPPRESSION, default=True): cv.boolean,
            cv.Optional(CONF_SPEECH_ENHANCEMENT, default=True): cv.boolean,
            cv.Optional(CONF_WAKENET, default=False): cv.boolean,
            cv.Optional(CONF_METERS): cv.Schema(
                {cv.GenerateID(): cv.declare_id(AECAudioMetersComponent)}
            ).extend(cv.COMPONENT_SCHEMA),
            cv.Optional("telemetry", default=False): cv.boolean,
            cv.Optional("slot_logs", default=False): cv.boolean,
            cv.Optional("diagnostics", default=False): cv.boolean,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_slots,
)


async def to_code(config):
    add_idf_component(name="espressif/esp-sr", ref="2.4.6")

    if config.get("telemetry", False):
        cg.add_define("USE_AEC_AUDIO_TELEMETRY")
    if config.get("slot_logs", False):
        cg.add_define("USE_AEC_AUDIO_SLOT_LOGS")
    if config.get("diagnostics", False):
        cg.add_define("USE_AEC_AUDIO_DIAGNOSTICS")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    adc = await cg.get_variable(config[CONF_AUDIO_ADC])
    cg.add(var.set_audio_adc(adc))
    cg.add(var.set_pins(
        config[CONF_MCLK_PIN],
        config[CONF_BCLK_PIN],
        config[CONF_LRCLK_PIN],
        config[CONF_DIN_PIN],
        config[CONF_DOUT_PIN],
    ))
    cg.add(var.set_i2s_port(config[CONF_I2S_PORT]))
    cg.add(var.set_tdm_slots(config[CONF_TDM_SLOTS]))
    cg.add(var.set_microphone_slots(*config[CONF_MICROPHONE_SLOTS]))
    cg.add(var.set_reference_slot(config[CONF_REFERENCE_SLOT]))
    cg.add(var.set_reference_source(REFERENCE_SOURCES[config[CONF_REFERENCE_SOURCE]]))
    cg.add(var.set_tx_slots(*config[CONF_TX_SLOTS]))
    if CONF_DIAGNOSTIC_RAW_SLOT in config:
        cg.add(var.set_diagnostic_raw_slot(config[CONF_DIAGNOSTIC_RAW_SLOT]))
    cg.add(var.set_aec_mode(config[CONF_AEC_MODE]))
    cg.add(var.set_agc_enabled(config[CONF_AGC]))
    cg.add(var.set_afe_include_unused_channel(config[CONF_AFE_INPUT_FORMAT] == "mmnr"))
    cg.add(var.set_nlp_level(config[CONF_NLP_LEVEL]))
    cg.add(var.set_filter_length(config[CONF_FILTER_LENGTH]))
    if config[CONF_RESAMPLER]:
        cg.add_define("USE_AEC_AUDIO_PLAYBACK_RESAMPLER")
    cg.add(var.set_reference_delay_samples(config[CONF_REFERENCE_DELAY_SAMPLES]))
    cg.add(var.set_noise_suppression_enabled(config[CONF_NOISE_SUPPRESSION]))
    cg.add(var.set_speech_enhancement_enabled(config[CONF_SPEECH_ENHANCEMENT]))
    cg.add(var.set_wakenet_enabled(config[CONF_WAKENET]))

    if meters_config := config.get(CONF_METERS):
        cg.add_define("USE_AEC_AUDIO_METERS")
        meters = cg.new_Pvariable(meters_config[CONF_ID])
        await cg.register_component(meters, meters_config)
        cg.add(var.register_meters_callback(meters))
