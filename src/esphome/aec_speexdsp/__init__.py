from esphome import pins
import esphome.codegen as cg
from esphome.components import audio_adc
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@matt"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["audio", "microphone", "ring_buffer", "speaker"]

CONF_AGC = "agc"
CONF_AUDIO_ADC = "audio_adc"
CONF_BCLK_PIN = "bclk_pin"
CONF_BEAMFORMING = "beamforming"
CONF_DIAGNOSTIC_RAW_SLOT = "diagnostic_raw_slot"
CONF_DIN_PIN = "din_pin"
CONF_DOUT_PIN = "dout_pin"
CONF_ECHO_SUPPRESS_ACTIVE_DB = "echo_suppress_active_db"
CONF_ECHO_SUPPRESS_DB = "echo_suppress_db"
CONF_FILTER_LENGTH = "filter_length"
CONF_FRAME_SIZE = "frame_size"
CONF_I2S_PORT = "i2s_port"
CONF_LRCLK_PIN = "lrclk_pin"
CONF_MCLK_PIN = "mclk_pin"
CONF_MICROPHONE_SLOTS = "microphone_slots"
CONF_METERS = "meters"
CONF_NOISE_SUPPRESSION = "noise_suppression"
CONF_NOISE_SUPPRESSION_LEVEL_DB = "noise_suppression_level_db"
CONF_OUTPUT_CHANNEL = "output_channel"
CONF_PLAYBACK_GAIN_DB = "playback_gain_db"
CONF_PROFILING = "profiling"
CONF_REFERENCE_SLOT = "reference_slot"
CONF_REFERENCE_SOURCE = "reference_source"
CONF_REFERENCE_DELAY_SAMPLES = "reference_delay_samples"
CONF_RESAMPLER = "resampler"
CONF_TDM_SLOTS = "tdm_slots"
CONF_TX_SLOTS = "tx_slots"
CONF_VAD = "vad"
CONF_VAD_THRESHOLD = "vad_threshold"

aec_speexdsp_ns = cg.esphome_ns.namespace("aec_speexdsp")
AECSpeexDspComponent = aec_speexdsp_ns.class_("AECSpeexDspComponent", cg.Component)
AECSpeexDspMetersComponent = aec_speexdsp_ns.class_("AECSpeexDspMetersComponent", cg.Component)

AECSpeexDspReferenceSource = aec_speexdsp_ns.enum("AECSpeexDspReferenceSource")
REFERENCE_SOURCES = {
    "analog_slot": AECSpeexDspReferenceSource.AEC_SPEEXDSP_REFERENCE_ANALOG_SLOT,
    "playback": AECSpeexDspReferenceSource.AEC_SPEEXDSP_REFERENCE_PLAYBACK,
}

AECSpeexDspOutputChannel = aec_speexdsp_ns.enum("AECSpeexDspOutputChannel")
OUTPUT_CHANNELS = {
    "first": AECSpeexDspOutputChannel.AEC_SPEEXDSP_OUTPUT_FIRST,
    "second": AECSpeexDspOutputChannel.AEC_SPEEXDSP_OUTPUT_SECOND,
    "mixed": AECSpeexDspOutputChannel.AEC_SPEEXDSP_OUTPUT_MIXED,
}


def _validate(config):
    agc_gate = config[CONF_AGC]["gate"]
    for opening, closing in [("open_rms", "close_rms"), ("reference_open_rms", "reference_close_rms")]:
        if agc_gate[opening] <= agc_gate[closing]:
            raise cv.Invalid(f"agc.gate.{opening} must be greater than {closing}")
    if agc_gate["enabled"] and not config[CONF_AGC]["enabled"]:
        raise cv.Invalid("agc.gate requires agc.enabled")
    slots = config[CONF_TDM_SLOTS]
    selected = config[CONF_MICROPHONE_SLOTS]
    reference = config[CONF_REFERENCE_SLOT]
    if any(slot >= slots for slot in selected + [reference] + config[CONF_TX_SLOTS]):
        raise cv.Invalid("All selected slots must be lower than tdm_slots")
    if len(set(selected)) != len(selected):
        raise cv.Invalid("Microphone slots must be distinct")
    if config[CONF_REFERENCE_SOURCE] == "analog_slot" and reference in selected:
        raise cv.Invalid("The analog reference slot must not also be a microphone slot")
    if config[CONF_REFERENCE_SOURCE] == "analog_slot" and config[CONF_REFERENCE_DELAY_SAMPLES] > 256:
        # The analog reference delay runs through the runtime history buffer;
        # the larger software delay only applies to the playback reference.
        raise cv.Invalid("reference_delay_samples supports 0-256 with an analog reference slot")
    if config[CONF_FRAME_SIZE] % 64 != 0:
        raise cv.Invalid("frame_size must be a multiple of 64")
    if config[CONF_FRAME_SIZE] & (config[CONF_FRAME_SIZE] - 1):
        raise cv.Invalid(
            "frame_size must be a power of two when using the ESP-DSP FFT "
            "(128, 256, 512, or 1024)"
        )
    if config[CONF_FILTER_LENGTH] < config[CONF_FRAME_SIZE]:
        raise cv.Invalid("filter_length must be at least frame_size")
    if config[CONF_OUTPUT_CHANNEL] != "first" and len(selected) < 2:
        raise cv.Invalid(
            "output_channel 'second' and 'mixed' require two microphone_slots"
        )
    if config[CONF_BEAMFORMING]["enabled"] and len(selected) < 2:
        raise cv.Invalid("beamforming requires at least two microphone_slots")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(AECSpeexDspComponent),
            cv.Required(CONF_AUDIO_ADC): cv.use_id(audio_adc.AudioAdc),
            cv.Required(CONF_MCLK_PIN): pins.internal_gpio_output_pin_number,
            cv.Required(CONF_BCLK_PIN): pins.internal_gpio_output_pin_number,
            cv.Required(CONF_LRCLK_PIN): pins.internal_gpio_output_pin_number,
            cv.Required(CONF_DIN_PIN): pins.internal_gpio_input_pin_number,
            cv.Required(CONF_DOUT_PIN): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_I2S_PORT, default=0): cv.int_range(min=0, max=2),
            cv.Optional(CONF_TDM_SLOTS, default=4): cv.int_range(min=4, max=4),
            cv.Optional(CONF_MICROPHONE_SLOTS, default=[0, 1]): cv.All(
                cv.ensure_list(cv.int_range(min=0, max=3)), cv.Length(min=1, max=4)
            ),
            cv.Optional(CONF_REFERENCE_SLOT, default=2): cv.int_range(min=0, max=3),
            cv.Optional(CONF_REFERENCE_SOURCE, default="analog_slot"): cv.one_of(
                *REFERENCE_SOURCES, lower=True
            ),
            cv.Optional(CONF_TX_SLOTS, default=[0, 1]): cv.All(
                cv.ensure_list(cv.int_range(min=0, max=3)), cv.Length(min=2, max=2)
            ),
            cv.Optional(CONF_DIAGNOSTIC_RAW_SLOT): cv.int_range(min=0, max=3),
            cv.Optional(CONF_FRAME_SIZE, default=256): cv.int_range(min=128, max=1024),
            cv.Optional(CONF_FILTER_LENGTH, default=2048): cv.int_range(min=256, max=16384),
            cv.Optional(CONF_OUTPUT_CHANNEL, default="first"): cv.one_of(
                *OUTPUT_CHANNELS, lower=True
            ),
            cv.Optional(CONF_BEAMFORMING, default={}): cv.Schema(
                {
                    cv.Optional("enabled", default=False): cv.boolean,
                    cv.Optional("max_lag", default=3): cv.int_range(min=1, max=8),
                    cv.Optional("update_frames", default=8): cv.int_range(min=1, max=64),
                    cv.Optional("min_rms", default=120): cv.int_range(min=1, max=32767),
                    cv.Optional("min_correlation_percent", default=50): cv.int_range(min=1, max=99),
                    cv.Optional("min_peak_dominance_percent", default=5): cv.int_range(min=0, max=50),
                }
            ),
            cv.Optional(CONF_NOISE_SUPPRESSION, default=True): cv.boolean,
            cv.Optional(CONF_NOISE_SUPPRESSION_LEVEL_DB, default=15): cv.int_range(min=5, max=60),
            cv.Optional(CONF_AGC, default={}): cv.Schema({
              cv.Optional("enabled", default=True): cv.boolean,
              cv.Optional("max_gain", default=12): cv.int_range(min=0, max=60),
              cv.Optional("target_level", default=0.25): cv.float_range(min=0.01, max=1.0),
              cv.Optional("gate", default={}): cv.Schema({
                cv.Optional("enabled", default=False): cv.boolean,
                cv.Optional("reference_open_rms", default=200): cv.float_range(min=0.01, max=32768),
                cv.Optional("reference_close_rms", default=100): cv.float_range(min=0.01, max=32768),
                cv.Optional("open_rms", default=64): cv.float_range(min=0.01, max=32768),
                cv.Optional("close_rms", default=32): cv.float_range(min=0.01, max=32768),
                cv.Optional("open_delay_ms", default=32): cv.int_range(min=0, max=60000),
                cv.Optional("hold_ms", default=250): cv.int_range(min=0, max=60000),
                cv.Optional("tail_ms", default=250): cv.int_range(min=0, max=60000),
                cv.Optional("release_ms", default=150): cv.int_range(min=0, max=60000),
                cv.Optional("startup_guard_ms", default=200): cv.int_range(min=0, max=60000),
              }),
            }),
            cv.Optional(CONF_VAD, default=True): cv.boolean,
            # Speex keeps its speech-continue probability at 20%; a start
            # threshold below that would never reset, so clamp the range.
            cv.Optional(CONF_VAD_THRESHOLD, default=35): cv.int_range(min=20, max=90),
            cv.Optional(CONF_ECHO_SUPPRESS_DB, default=40): cv.int_range(min=5, max=60),
            cv.Optional(CONF_ECHO_SUPPRESS_ACTIVE_DB, default=15): cv.int_range(min=5, max=60),
            cv.Optional(CONF_PLAYBACK_GAIN_DB, default=0.0): cv.float_range(min=-60.0, max=0.0),
            cv.Optional(CONF_REFERENCE_DELAY_SAMPLES, default=0): cv.int_range(min=0, max=4000),
            cv.Optional(CONF_RESAMPLER, default=False): cv.boolean,
            cv.Optional(CONF_METERS): cv.Schema(
                {
                    cv.GenerateID(): cv.declare_id(AECSpeexDspMetersComponent),
                    cv.Optional("enabled", default=True): cv.boolean,
                }
            ).extend(cv.COMPONENT_SCHEMA),
            cv.Optional("telemetry", default=False): cv.boolean,
            cv.Optional(CONF_PROFILING, default=False): cv.boolean,
            cv.Optional("slot_logs", default=False): cv.boolean,
            cv.Optional("diagnostics", default=False): cv.boolean,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate,
)


async def to_code(config):
    # Required by the no-fallback SIMD FFT backend selected in config.h.
    from esphome.components.esp32 import add_idf_component
    add_idf_component(name="espressif/esp-dsp", ref="1.8.2")

    # SpeexDSP is vendored inside this component (speexdsp/), so no IDF
    # managed component needs to be added here.

    if config.get("telemetry", False):
        cg.add_define("USE_AEC_SPEEXDSP_TELEMETRY")
    if config.get(CONF_PROFILING, False):
        cg.add_define("USE_AEC_SPEEXDSP_PROFILE")
    if config.get("slot_logs", False):
        cg.add_define("USE_AEC_SPEEXDSP_SLOT_LOGS")
    if config.get("diagnostics", False):
        cg.add_define("USE_AEC_SPEEXDSP_DIAGNOSTICS")

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
    cg.add(var.set_microphone_slots(config[CONF_MICROPHONE_SLOTS]))
    cg.add(var.set_reference_slot(config[CONF_REFERENCE_SLOT]))
    cg.add(var.set_reference_source(REFERENCE_SOURCES[config[CONF_REFERENCE_SOURCE]]))
    cg.add(var.set_tx_slots(*config[CONF_TX_SLOTS]))
    if CONF_DIAGNOSTIC_RAW_SLOT in config:
        cg.add(var.set_diagnostic_raw_slot(config[CONF_DIAGNOSTIC_RAW_SLOT]))
    cg.add(var.set_frame_size(config[CONF_FRAME_SIZE]))
    cg.add(var.set_filter_length(config[CONF_FILTER_LENGTH]))
    cg.add(var.set_output_channel(OUTPUT_CHANNELS[config[CONF_OUTPUT_CHANNEL]]))
    beamforming = config[CONF_BEAMFORMING]
    cg.add(var.configure_beamforming(
        beamforming["enabled"],
        beamforming["max_lag"],
        beamforming["update_frames"],
        beamforming["min_rms"],
        beamforming["min_correlation_percent"],
        beamforming["min_peak_dominance_percent"],
    ))
    cg.add(var.set_noise_suppression_enabled(config[CONF_NOISE_SUPPRESSION]))
    cg.add(var.set_noise_suppression_level_db(config[CONF_NOISE_SUPPRESSION_LEVEL_DB]))
    agc = config[CONF_AGC]
    cg.add(var.set_agc_enabled(agc["enabled"]))
    cg.add(var.set_agc_target_level(agc["target_level"]))
    cg.add(var.set_agc_max_gain_db(agc["max_gain"]))
    gate = agc["gate"]
    cg.add(var.configure_agc_gate(gate["enabled"], gate["reference_open_rms"], gate["reference_close_rms"],
        gate["open_rms"], gate["close_rms"], gate["open_delay_ms"], gate["hold_ms"], gate["tail_ms"], gate["release_ms"],
        gate["startup_guard_ms"]))
    cg.add(var.set_vad_enabled(config[CONF_VAD]))
    cg.add(var.set_vad_threshold(config[CONF_VAD_THRESHOLD]))
    cg.add(var.set_echo_suppress_db(config[CONF_ECHO_SUPPRESS_DB]))
    cg.add(var.set_echo_suppress_active_db(config[CONF_ECHO_SUPPRESS_ACTIVE_DB]))
    cg.add(var.set_playback_gain_db(config[CONF_PLAYBACK_GAIN_DB]))
    if config[CONF_RESAMPLER]:
        cg.add_define("USE_AEC_SPEEXDSP_PLAYBACK_RESAMPLER")
    cg.add(var.set_reference_delay_samples(config[CONF_REFERENCE_DELAY_SAMPLES]))

    if meters_config := config.get(CONF_METERS):
        # The define makes the meter class available to generated UI lambdas.
        # Registration controls whether it runs in the audio hot path.
        cg.add_define("USE_AEC_SPEEXDSP_METERS")
        meters = cg.new_Pvariable(meters_config[CONF_ID])
        await cg.register_component(meters, meters_config)
        if meters_config["enabled"]:
            cg.add(var.register_meters_callback(meters))
