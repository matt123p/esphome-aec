"""Compile the production frame gate, VAD selection and PCM routing with stubs."""
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.skipif(not shutil.which('g++'), reason='Requires host C++ compiler')
def test_playback_assistant_truth_table_and_transitions(tmp_path):
    header = (ROOT / 'src/esphome/aec_speexdsp/aec_speexdsp.h').read_text()
    source = (ROOT / 'src/esphome/aec_speexdsp/aec_speexdsp.cpp').read_text()
    controls = header.split('  bool neural_controls_audio_() const {', 1)[1].split('  bool near_end_enabled_', 1)[0]
    gate = '    this->neural_reference_active_ = reference_frame.active;' + source.split('    this->neural_reference_active_ = reference_frame.active;', 1)[1].split('    const int64_t neural_start', 1)[0]
    select = source.split('#ifdef USE_AEC_SPEEXDSP_NEURAL_BARGE_IN\n    if (this->neural_controls_audio_())', 1)[1].split('#ifdef USE_AEC_SPEEXDSP_TELEMETRY', 1)[0]
    select = '#ifdef USE_AEC_SPEEXDSP_NEURAL_BARGE_IN\n    if (this->neural_controls_audio_())' + select
    publish = source.split('void AECSpeexDspComponent::publish_frame_', 1)[1].split('void AECSpeexDspComponent::capture_frame_', 1)[0]
    code = r'''
#define USE_AEC_SPEEXDSP_NEURAL_BARGE_IN
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <vector>
using spx_int32_t = int;
constexpr int SPEEX_PREPROCESS_SET_NEAR_END_HINT=0, SPEEX_PREPROCESS_SET_NEAR_END_PROTECTION=1,
 SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE=2, SPEEX_PREPROCESS_GET_PROB=3;
int speex_preprocess_ctl(void *, int request, void *value) {
 if(request==SPEEX_PREPROCESS_GET_PROB) *static_cast<int *>(value)=37;
 return 0;
}
int millis() { return 0; }
struct Resettable { void reset() {} void invalidate() {} bool reference_active() const { return true; }
 bool candidate() const { return false; } };
struct Detector {
 bool enabled=true; unsigned resets=0;
 void set_enabled(bool value) { if(value!=enabled) { enabled=value; ++resets; } }
};
struct Microphone {
 bool vad=false, rejected=false; size_t bytes=0;
 void publish(const uint8_t *, size_t n, bool v, bool r) { bytes+=n; vad=v; rejected=r; }
};
struct AECSpeexDspComponent {
 Detector detector; Detector *neural_barge_=&detector;
 Microphone mic; Microphone *microphone_=&mic;
 std::atomic<bool> neural_enabled_requested_{true}, voice_assistant_active_{false};
 bool neural_enabled_=true, neural_barge_active_=false, near_end_enabled_=true, neural_reference_active_=false;
 bool soft_double_talk_enabled_=true; int echo_suppress_active_db_=25;
 Resettable near_end_detector_, near_end_correlation_, cancellation_quality_, soft_double_talk_;
 std::vector<void *> preprocess_state_{reinterpret_cast<void *>(1)};
 bool near_end_speech_=false, vad_enabled_=true;
 std::atomic<bool> vad_state_{false}; std::atomic<float> vad_probability_{0}, speex_vad_probability_{0};
'''
    code += 'bool neural_controls_audio_() const {' + controls
    code += 'void publish_frame_(const int16_t *mono, size_t frames);\n'
    code += 'void frame(bool playing, bool assistant, bool speech_detected, bool confirmed, int diagnostic_raw_slot=-1) {\n'
    code += 'voice_assistant_active_=assistant; struct { bool active; } reference_frame{playing};\n'
    code += 'unsigned neural_time_us=0, neural_frames=0, neural_peak_us=0, neural_last_log=0;\n'
    code += gate
    code += 'struct { bool confirmed; float probability; } neural{confirmed,.91f};\n' + select
    code += 'int16_t pcm[256]{}; publish_frame_(pcm,256); }\n};\n'
    code += 'void AECSpeexDspComponent::publish_frame_' + publish
    code += r'''
int main() {
 AECSpeexDspComponent c;
 // Speex wins outside the conjunction, regardless of stale neural confirmation.
 for(bool playback : {false,true}) for(bool assistant : {false,true}) {
  const bool use_neural=playback&&assistant;
  c.frame(playback,assistant,true,false);
  assert(c.neural_barge_active_==use_neural);
  assert(c.vad_state_==!use_neural);
  assert(c.mic.vad==c.vad_state_);
  assert(c.mic.rejected==use_neural);
  assert(c.vad_probability_==(use_neural?.91f:.37f));
  assert(c.speex_vad_probability_==.37f);
  c.frame(playback,assistant,false,true);
  assert(c.vad_state_==use_neural);
  assert(!c.mic.rejected);
 }
 // Scope transitions preserve the normalizer/context; manual disable resets it.
 c.frame(true,true,true,true); auto resets=c.detector.resets;
 c.frame(true,false,false,true); assert(c.detector.resets==resets);
 assert(!c.vad_state_ && !c.mic.rejected);
 c.frame(true,false,false,true); assert(c.detector.resets==resets);
 c.frame(true,true,false,false); assert(c.detector.resets==resets);
 c.neural_enabled_requested_=false; c.frame(true,true,true,false);
 assert(c.vad_state_ && !c.mic.rejected);
 c.neural_enabled_requested_=true; c.frame(true,true,true,false);
 assert(!c.vad_state_ && c.mic.rejected);
 c.frame(true,true,true,true,0); assert(!c.vad_state_);
 assert(c.mic.bytes>0);
}
'''
    cpp = tmp_path / 'routing.cpp'
    exe = tmp_path / 'routing'
    cpp.write_text(code)
    subprocess.run(['g++', '-std=c++17', '-fsanitize=undefined', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


@pytest.mark.skipif(not shutil.which('g++'), reason='Requires host C++ compiler')
def test_probability_sensors_publish_independent_percentages(tmp_path):
    header = (ROOT / 'src/esphome/aec_speexdsp/aec_speexdsp.h').read_text()
    implementation = 'class AECSpeexDspProbabilitySensors' + header.split(
        'class AECSpeexDspProbabilitySensors', 1)[1].split('#endif', 1)[0]
    code = r'''
#include <cassert>
#include <cmath>
namespace sensor { struct Sensor {
 float state=-1; unsigned calls=0;
 void publish_state(float value) { state=value; ++calls; }
}; }
struct PollingComponent { virtual void update() {} };
struct AECSpeexDspComponent {
 float speex=.37f, gru=.9995f;
 float get_speex_vad_probability() const { return speex; }
 float get_neural_barge_probability() const { return gru; }
};
'''
    code += implementation
    code += r'''
int main() {
 AECSpeexDspComponent parent; sensor::Sensor speex, gru;
 AECSpeexDspProbabilitySensors both;
 both.update(); // No parent: safe before setup.
 both.set_parent(&parent); both.set_speex_vad_probability_sensor(&speex);
 both.set_gru_probability_sensor(&gru); both.update();
 assert(std::abs(speex.state-37.f)<.0001f);
 assert(std::abs(gru.state-99.95f)<.0001f);
 parent.gru=0; both.update(); // Inactive GRU must not leave a stale reading.
 assert(gru.state==0 && speex.state==37);
 AECSpeexDspProbabilitySensors only_speex; only_speex.set_parent(&parent);
 only_speex.set_speex_vad_probability_sensor(&speex); only_speex.update();
 assert(speex.calls==3 && gru.calls==2);
 AECSpeexDspProbabilitySensors only_gru; only_gru.set_parent(&parent);
 only_gru.set_gru_probability_sensor(&gru); only_gru.update();
 assert(speex.calls==3 && gru.calls==3);
}
'''
    cpp = tmp_path / 'sensors.cpp'
    exe = tmp_path / 'sensors'
    cpp.write_text(code)
    subprocess.run(['g++', '-std=c++17', '-fsanitize=undefined', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
