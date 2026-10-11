"""Run the production API upload loop against deterministic ring/API stubs."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("g++"), "Requires a host g++ compiler")
class UploadTests(unittest.TestCase):
    def test_catchup_and_backpressure(self):
        source = (ROOT / "src/esphome/voice_assistant/voice_assistant.cpp").read_text()
        method = source.split("void VoiceAssistant::stream_api_audio_()", 1)[1].split(
            "void VoiceAssistant::handle_channel_stall_", 1)[0]
        code = r'''
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <functional>
#include <memory>
#include <vector>
#include <deque>
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
uint32_t now_ms=100;
uint32_t millis() { return now_ms; }
constexpr size_t SEND_BUFFER_SIZE=1024, BARGE_IN_PRE_ROLL_SIZE=16384;
constexpr size_t MAX_CATCHUP_PACKETS_PER_LOOP=8, MAX_API_AUDIO_PACKETS_PER_LOOP=2;
constexpr uint32_t BARGE_IN_SPEECH_HANGOVER_MS=1000;
enum class State {STREAMING_RESPONSE, STREAMING_MICROPHONE};
struct Ring {
 std::deque<uint8_t> pcm;
 size_t available() const { return pcm.size(); }
 void append(size_t n) { for(size_t i=0;i<n;i++) pcm.push_back(i%251); }
};
struct Source {
 std::shared_ptr<Ring> ring;
 std::vector<uint8_t> held;
 size_t offset=0;
 Source(std::shared_ptr<Ring> r):ring(r){}
 void fill(int,bool) {
  if(available()) return;
  held.clear(); offset=0;
  size_t n=std::min<size_t>(1024,ring->pcm.size());
  while(n--) { held.push_back(ring->pcm.front()); ring->pcm.pop_front(); }
 }
 size_t available() const { return held.size()-offset; }
 const uint8_t *data() const {return held.data()+offset;}
 void consume(size_t n) {offset+=n;}
};
namespace api {struct VoiceAssistantAudio {
 const uint8_t *data=nullptr,*data2=nullptr;
 size_t data_len=0,data2_len=0;
};}
struct API {
 bool accept=true;
 bool retired=false;
 void on_fatal_error() {retired=true;}
 int attempts=0;
 std::vector<uint8_t> sent;
 bool send_message(api::VoiceAssistantAudio m) {
  ++attempts;
  if(!accept) return false;
  sent.insert(sent.end(),m.data,m.data+m.data_len); return true;
 }
};
struct VoiceAssistant {
 bool barge_in_=true,barge_in_capture_gate_=true,barge_in_upload_gate_open_=false;
 bool awaiting_provider_response_=false;
 State state_=State::STREAMING_RESPONSE;
 uint32_t barge_in_speech_active_until_ms_=0,last_microphone_activity_ms_=0;
 uint32_t mic_retry_at_ms_=0,mic_blocked_since_ms_=0,audio_channel_stall_start_=0;
 uint32_t mic_diagnostic_start_=0,mic_diagnostic_packets_=0,mic_diagnostic_peak_=0,mic_diagnostic_drops_=0;
 size_t mic_diagnostic_bytes_=0,mic_diagnostic_dropped_bytes_=0;
 uint64_t mic_diagnostic_energy_=0;
 std::function<bool()> barge_in_audio_condition_{};
 std::shared_ptr<Ring> owned=std::make_shared<Ring>();
 std::weak_ptr<Ring> ring_buffer_=owned,ring_buffer2_;
 std::unique_ptr<Source> audio_source_=std::make_unique<Source>(owned),audio_source2_;
 API api;
 API *api_client_=&api;
 bool stopped=false;
 struct Error {void trigger(const char *,const char *) {}} error_trigger_;
 void request_stop() {stopped=true;}
 void defer(std::function<void()> fn) {fn();}
 void handle_channel_stall_(size_t,size_t) {}
 void stream_api_audio_();
};
'''
        code += "void VoiceAssistant::stream_api_audio_()" + method
        code += r'''
int main() {
 VoiceAssistant va;
 va.owned->append(16384);
 va.stream_api_audio_(); assert(va.api.sent.size()==8192);
 // Live audio arrives while history drains. Catch-up continues based on depth.
 va.owned->append(1024); va.stream_api_audio_();
 assert(va.api.sent.size()==16384);
 va.stream_api_audio_(); assert(va.api.sent.size()==17408);
 va.stream_api_audio_(); assert(va.api.sent.size()==17408); // closed capture sends nothing
 va.owned->append(1024); va.api.accept=false;
 va.stream_api_audio_(); int attempts=va.api.attempts;
 assert(va.audio_source_->available()==1024);
 va.stream_api_audio_(); assert(va.api.attempts==attempts); // bounded retries
 va.api.accept=true; now_ms+=16; va.stream_api_audio_();
 assert(va.api.sent.size()==18432 && va.audio_source_->available()==0);
 // A prolonged refusal aborts explicitly, without discarding the held packet.
 va.owned->append(1024); va.api.accept=false; va.stream_api_audio_();
 now_ms+=2001; va.stream_api_audio_(); assert(va.stopped);
 assert(va.api.retired); // A locally stopped pipeline must not survive on the blocked link.
 assert(va.audio_source_->available()==1024);
 // Legacy condition still gates uploads for devices without capture-side gating.
 VoiceAssistant legacy; legacy.barge_in_capture_gate_=false;
 bool speech=false; legacy.barge_in_audio_condition_=[&](){return speech;};
 legacy.owned->append(20000); legacy.stream_api_audio_(); assert(legacy.api.sent.empty());
 speech=true; legacy.stream_api_audio_();
 assert(legacy.api.sent.size()<=8192 && legacy.api.sent.front()==3616%251);
 while(legacy.owned->available() || legacy.audio_source_->available()) legacy.stream_api_audio_();
 assert(legacy.api.sent.size()==16384);
 // Non-barge-in keeps its two-packet loop budget.
 VoiceAssistant idle; idle.state_=State::STREAMING_MICROPHONE;
 idle.owned->append(4096); idle.stream_api_audio_(); // Large PCM peaks, no confirmed speech.
 assert(idle.last_microphone_activity_ms_==0 && !idle.awaiting_provider_response_);
 VoiceAssistant plain; plain.barge_in_=false; plain.owned->append(16384);
 plain.stream_api_audio_(); assert(plain.api.sent.size()==2048);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "test.cpp"
            exe = Path(directory) / "test"
            cpp.write_text(code)
            subprocess.run(["g++", "-std=c++17", "-fsanitize=address,undefined",
                            str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
