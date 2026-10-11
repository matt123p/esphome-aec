"""Compile production speaker event/state branches against deterministic stubs."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("g++"), "Requires a host g++ compiler")
class StateTests(unittest.TestCase):
    def test_stream_lifecycle(self):
        source = (ROOT / "src/esphome/voice_assistant/voice_assistant.cpp").read_text()
        events = source.split("void VoiceAssistant::on_event(", 1)[1]
        names = ["INTENT_END", "TTS_START", "TTS_END", "RUN_END", "TTS_STREAM_START", "TTS_STREAM_END"]
        cases = []
        for name in names:
            marker = "    case api::enums::VOICE_ASSISTANT_" + name + ":"
            cases.append(marker + events.split(marker, 1)[1].split("    case ", 1)[0])
        loop = source.split("void VoiceAssistant::loop()", 1)[1]
        loop = "    case State::STREAMING_RESPONSE:" + loop.split(
            "    case State::STREAMING_RESPONSE:", 1)[1].split("    default:", 1)[0]
        timeout = source.split("bool VoiceAssistant::barge_in_timed_out", 1)[1].split(
            "void VoiceAssistant::stream_api_audio_", 1)[0]
        start = source.split("void VoiceAssistant::request_start(", 1)[1].split(
            "void VoiceAssistant::request_stop()", 1)[0]
        event_guard = events.split('  switch (msg.event_type)', 1)[0]
        audio = source.split('void VoiceAssistant::on_audio(', 1)[1].split('void VoiceAssistant::on_timer_event', 1)[0]
        startup_timeout = 'this->set_timeout("pipeline-start"' + source.split(
            'this->set_timeout("pipeline-start"', 1)[1].split('      this->set_timeout("reset-conversation_id"', 1)[0]
        signal_stop = source.split('void VoiceAssistant::signal_stop_()', 1)[1].split('void VoiceAssistant::start_playback_timeout_', 1)[0]
        code = r'''
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <cstring>
#include <sys/socket.h>
#include <sys/types.h>
#define USE_SPEAKER
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGV(...) ((void)0)
#define API_LOG_MSG_DROPPED(...) ((void)0)
uint32_t now_ms=0;
uint32_t millis(){return now_ms;}
constexpr int AUDIO_MODE_API=1, AUDIO_MODE_UDP=0;
constexpr size_t RECEIVE_SIZE=1024;
enum class State {IDLE, START_MICROPHONE, STARTING_MICROPHONE, START_PIPELINE, STARTING_PIPELINE, STREAMING_MICROPHONE,
 STOP_MICROPHONE, STOPPING_MICROPHONE, AWAITING_RESPONSE, STREAMING_RESPONSE, RESPONSE_FINISHED};
namespace api {
namespace enums {enum Event {VOICE_ASSISTANT_INTENT_END,VOICE_ASSISTANT_TTS_START,
 VOICE_ASSISTANT_TTS_END,VOICE_ASSISTANT_RUN_END,VOICE_ASSISTANT_TTS_STREAM_START,
 VOICE_ASSISTANT_TTS_STREAM_END};}
struct Arg {std::string name,value;};
struct VoiceAssistantEventResponse {enums::Event event_type;std::vector<Arg> data;};
struct VoiceAssistantAudio {const uint8_t *data;size_t data_len;};
struct VoiceAssistantRequest {bool start;};
}
struct API {
 bool accept=true,retired=false;
 bool send_message(api::VoiceAssistantRequest){return accept;}
 void on_fatal_error(){retired=true;}
};
struct Trigger {int count=0;template<class... T> void trigger(T...) {++count;}};
struct Speaker {
 bool running=false,buffered=false;int stops=0,starts=0;
 bool is_running() const{return running;}
 bool has_buffered_data() const{return buffered;}
 void stop(){running=buffered=false;++stops;}
 void start(){running=true;++starts;}
};
struct Ring {size_t count=0;size_t available()const{return count;}};
struct Source {size_t count=0;size_t available()const{return count;}};
struct Mic {bool is_running()const{return true;}};
struct Socket {ssize_t read(void*,size_t){return 0;}};
struct VoiceAssistant {
 State state_=State::STREAMING_MICROPHONE;
 bool barge_in_=true,barge_in_capture_gate_=true,pipeline_run_ended_=false;
 bool speaker_stream_started_=false,awaiting_provider_response_=false;
 bool stream_ended_=false,wait_for_stream_end_=false,continue_conversation_=false,local_output_=true;
 bool near=false;
 API api;API *api_client_=&api;
 sockaddr_storage dest_addr_{};
 bool stopped=false;
 std::function<void()> startup_timeout;
 struct Error {int count=0;void trigger(const char*,const char*){++count;}} error_trigger_;
 bool continuous_=false,silence_detection_=false;
 uint32_t last_microphone_activity_ms_=0,last_playback_activity_ms_=0;
 size_t speaker_buffer_size_=0,speaker_buffer_index_=0,speaker_bytes_received_=0,speaker_buffer_capacity_=32768;
 uint8_t speaker_storage[32768]{};uint8_t *speaker_buffer_=speaker_storage;
 Speaker speaker;Speaker *speaker_=&speaker;
 Mic mic;Mic *mic_source_=&mic,*mic_source2_=nullptr;
 Socket socket;Socket *socket_=&socket;
 Source source;Source *audio_source_=&source;
 std::shared_ptr<Ring> owned=std::make_shared<Ring>();
 std::weak_ptr<Ring> ring_buffer_=owned;
 std::function<bool()> barge_in_audio_condition_=[this]{return near;};
 int audio_mode_=AUDIO_MODE_API,uploads=0;
 std::string conversation_id_;
 Trigger intent_end_trigger_,tts_start_trigger_,tts_end_trigger_,end_trigger_;
 Trigger tts_stream_start_trigger_,tts_stream_end_trigger_;
 void defer(std::function<void()> f){f();}
 void cancel_timeout(const char*){}
 void set_timeout(const char*,uint32_t ms,std::function<void()> f){assert(ms==10000);startup_timeout=f;}
 void request_stop(){stopped=true;signal_stop_();state_=State::STOP_MICROPHONE;}
 void signal_stop_();
 void start_playback_timeout_(){}
 void set_state_(State s,State){state_=s;}
 void reset_audio_activity(){last_microphone_activity_ms_=last_playback_activity_ms_=now_ms;}
 void clear_buffers_(){owned->count=source.count=0;}
 void write_speaker_(){speaker_buffer_size_=speaker_buffer_index_=0;}
 void stream_api_audio_(){++uploads;}
 bool is_running()const{return state_!=State::IDLE;}
 bool audio_silent_for(uint32_t ms)const{return now_ms-last_microphone_activity_ms_>=ms && now_ms-last_playback_activity_ms_>=ms;}
 bool barge_in_timed_out(uint32_t,uint32_t) const;
 void request_start(bool,bool);
 void on_event(const api::VoiceAssistantEventResponse&);
 void on_audio(const api::VoiceAssistantAudio&);
 void arm_start_timeout();
 void advance();
};
'''
        code += "bool VoiceAssistant::barge_in_timed_out" + timeout
        code += "void VoiceAssistant::request_start(" + start
        code += "void VoiceAssistant::signal_stop_()" + signal_stop
        code += "void VoiceAssistant::arm_start_timeout(){" + startup_timeout + "}\n"
        code += "void VoiceAssistant::on_audio(" + audio
        code += "void VoiceAssistant::on_event(" + event_guard + "switch(msg.event_type){\n"
        code += "".join(cases) + "}}\n"
        code += "void VoiceAssistant::advance(){switch(state_){\n" + loop + "default:break;}}\n"
        code += r'''
int main(){
 using namespace api::enums;
 VoiceAssistant va;
 va.on_event({VOICE_ASSISTANT_TTS_STREAM_START,{}});
 assert(va.state_==State::STREAMING_RESPONSE && va.wait_for_stream_end_);
 va.advance();assert(va.uploads==1); // Early output keeps microphone service alive.
 va.on_event({VOICE_ASSISTANT_TTS_STREAM_END,{}});
 va.advance();assert(va.state_==State::RESPONSE_FINISHED);
 va.advance();assert(va.state_==State::RESPONSE_FINISHED); // No RUN_END yet.
 va.on_event({VOICE_ASSISTANT_TTS_START,{{"text","late metadata"}}});
 assert(va.speaker.starts==0); // Do not resurrect drained playback.
 va.on_event({VOICE_ASSISTANT_TTS_END,{{"url","tts.wav"}}});
 assert(va.state_==State::RESPONSE_FINISHED);
 va.on_event({VOICE_ASSISTANT_INTENT_END,{{"continue_conversation","1"},{"conversation_id","same"}}});
 va.on_event({VOICE_ASSISTANT_RUN_END,{}});
 va.advance();assert(va.state_==State::START_PIPELINE && va.conversation_id_=="same");

 VoiceAssistant tail;
 tail.state_=State::RESPONSE_FINISHED;tail.continue_conversation_=true;
 tail.speaker.running=tail.speaker.buffered=true;tail.owned->count=16384;
 tail.advance();assert(tail.speaker.stops==0); // Trailing silence is not new speech.
 tail.near=true;tail.advance();
 assert(tail.speaker.stops==0 && tail.owned->count==16384);
 assert(tail.state_==State::RESPONSE_FINISHED); // Confirmation must not cut queued playback.
 tail.speaker.running=tail.speaker.buffered=false;tail.advance();
 assert(tail.state_==State::RESPONSE_FINISHED); // Wait for pipeline boundary.
 tail.on_event({VOICE_ASSISTANT_RUN_END,{}});tail.advance();
 assert(tail.state_==State::START_PIPELINE && tail.owned->count==16384);

 VoiceAssistant flush;
 flush.state_=State::RESPONSE_FINISHED;flush.stream_ended_=true;
 flush.speaker.running=true;flush.owned->count=4096;flush.source.count=512;
 flush.speaker_buffer_size_=1024;
 flush.on_event({VOICE_ASSISTANT_TTS_STREAM_START,{{"flush","1"}}});
 assert(flush.state_==State::STREAMING_RESPONSE && !flush.stream_ended_);
 assert(flush.speaker_buffer_size_==0 && flush.owned->count==4096 && flush.source.count==512);

 // Cancelled-response data cannot resurrect playback during idle/startup.
 for(auto state : {State::IDLE,State::STARTING_PIPELINE,State::STOP_MICROPHONE}) {
  VoiceAssistant stale;stale.state_=state;
  const uint8_t pcm[]={1,2,3,4};
  stale.on_audio({pcm,sizeof(pcm)});
  stale.on_event({VOICE_ASSISTANT_TTS_STREAM_START,{{"flush","1"}}});
  stale.on_event({VOICE_ASSISTANT_TTS_END,{{"url","old.wav"}}});
  stale.on_event({VOICE_ASSISTANT_TTS_STREAM_END,{}});
  assert(stale.state_==state && !stale.stream_ended_);
  assert(stale.speaker_bytes_received_==0 && stale.last_playback_activity_ms_==0);
  assert(stale.tts_stream_start_trigger_.count==0 && stale.tts_end_trigger_.count==0);
 }
 VoiceAssistant live;live.state_=State::STREAMING_RESPONSE;
 const uint8_t pcm[]={1,2,3,4};live.on_audio({pcm,sizeof(pcm)});
 assert(live.speaker_bytes_received_==sizeof(pcm));
 VoiceAssistant blocked;blocked.api.accept=false;blocked.signal_stop_();
 assert(blocked.api.retired);
 VoiceAssistant startup;startup.state_=State::STARTING_PIPELINE;startup.arm_start_timeout();
 startup.startup_timeout();
 assert(startup.stopped && startup.api.retired && startup.error_trigger_.count==1);
 VoiceAssistant started;started.state_=State::STARTING_PIPELINE;started.arm_start_timeout();
 started.state_=State::STREAMING_MICROPHONE;started.startup_timeout();
 assert(!started.stopped && !started.api.retired && started.error_trigger_.count==0);

 VoiceAssistant legacy;legacy.barge_in_=false;legacy.barge_in_capture_gate_=false;
 legacy.on_event({VOICE_ASSISTANT_TTS_STREAM_START,{}});
 assert(legacy.state_==State::STREAMING_MICROPHONE);
 legacy.on_event({VOICE_ASSISTANT_TTS_END,{{"url","tts.wav"}}});
 assert(legacy.state_==State::STREAMING_RESPONSE);
 legacy.state_=State::RESPONSE_FINISHED;legacy.advance();
 assert(legacy.state_==State::IDLE); // Legacy does not wait for new lifecycle flag.

 VoiceAssistant watch;
 now_ms=16000;assert(watch.barge_in_timed_out(15000,150000));
 watch.awaiting_provider_response_=true;
 assert(watch.barge_in_timed_out(15000,150000)); // Stale activity flag cannot extend listening.
 watch.state_=State::AWAITING_RESPONSE;
 assert(!watch.barge_in_timed_out(15000,150000));
 now_ms=121000;assert(!watch.barge_in_timed_out(15000,150000));
 now_ms=151000;assert(watch.barge_in_timed_out(15000,150000));
 watch.speaker.buffered=true;assert(!watch.barge_in_timed_out(15000,150000));
 watch.speaker.buffered=false;watch.reset_audio_activity();
 assert(!watch.barge_in_timed_out(15000,150000));
 // Local cancellation followed by a new wake must reset the old timestamps
 // immediately, before the deferred pipeline on_start automation.
 watch.state_=State::IDLE;
 watch.last_microphone_activity_ms_=watch.last_playback_activity_ms_=0;
 now_ms=2000000;
 watch.request_start(true,true);
 assert(watch.state_==State::START_MICROPHONE && watch.continuous_);
 assert(!watch.barge_in_timed_out(15000,150000));
 now_ms+=150001;
 assert(watch.barge_in_timed_out(15000,150000)); // Startup still has a failsafe.
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.cpp"
            binary = Path(directory) / "state"
            path.write_text(code)
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", str(path), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
