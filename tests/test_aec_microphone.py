"""Run the actual microphone implementation against host FreeRTOS stubs."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("g++"), "Requires a host g++ compiler")
class MicrophoneTests(unittest.TestCase):
    def test_handoff_lifecycle(self):
        header = (ROOT / "src/esphome/aec_speexdsp/aec_speexdsp.h").read_text()
        declaration = header.split("class AECSpeexDspMicrophone :", 1)[1].split(
            "class AECSpeexDspSpeaker :", 1
        )[0]
        source = (ROOT / "src/esphome/aec_speexdsp/aec_speexdsp.cpp").read_text()
        methods = source.split("AECSpeexDspMicrophone::~", 1)[1].split(
            "void AECSpeexDspSpeaker::setup()", 1
        )[0]
        harness = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <functional>
using SemaphoreHandle_t = void *;
constexpr int portMAX_DELAY = 0, MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
void *xSemaphoreCreateMutex() { return reinterpret_cast<void *>(1); }
void vSemaphoreDelete(void *) {}
void xSemaphoreTake(void *, int) {}
void xSemaphoreGive(void *) {}
void *heap_caps_malloc(size_t n, int) { return malloc(n); }
void heap_caps_free(void *p) { free(p); }
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGCONFIG(...) ((void)0)
#define YESNO(x) ((x) ? "YES" : "NO")
uint32_t clock_ms = 0;
uint32_t millis() { return clock_ms; }
constexpr int SAMPLE_RATE = 16000;
namespace audio { struct AudioStreamInfo { AudioStreamInfo(int=0,int=0,int=0) {} }; }
struct Component {
 virtual void setup() {} virtual void loop() {} virtual void dump_config() {}
 bool failed = false;
 bool is_failed() { return failed; } void mark_failed() { failed=true; }
};
namespace microphone {
enum { STATE_RUNNING, STATE_STOPPED };
struct Microphone {
 virtual void start() {} virtual void stop() {}
 int state_ = STATE_STOPPED;
 audio::AudioStreamInfo audio_stream_info_;
 struct Callbacks {
  std::vector<uint8_t> output;
  void call(const std::vector<uint8_t> &v) { output.insert(output.end(),v.begin(),v.end()); }
 } data_callbacks_;
};
}
class AECSpeexDspComponent;
'''
        harness += "class AECSpeexDspMicrophone :" + declaration
        harness += "AECSpeexDspMicrophone::~" + methods
        harness += r'''
struct Mic : AECSpeexDspMicrophone {
 size_t queued() { return buffered_bytes_; }
 size_t dropped() { return dropped_bytes_; }
 void tick() { clock_ms += 16; loop(); }
 void frame(uint8_t value) { std::vector<uint8_t> v(512,value); publish(v.data(),v.size()); }
 auto &output() { return data_callbacks_.output; }
};
int main() {
 // The detector fires after command onset: retain 100 ms before delivery,
 // including a wrapped partial frame, then the post-detection continuation.
 {
  Mic delayed; delayed.setup(); delayed.start();
  std::vector<uint8_t> speech(65024);
  for (size_t i=0;i<speech.size();i++) speech[i]=(i/128)%251;
  for (size_t i=0;i<speech.size();i+=512) { delayed.publish(speech.data()+i,512); delayed.tick(); }
  delayed.output().clear();
  // These samples have been captured but have not reached the detector.
  std::vector<uint8_t> pending(2048,251);
  delayed.publish(pending.data(),pending.size());
  delayed.request_pre_roll(); delayed.stop();
  std::vector<uint8_t> continuation(2048,252);
  delayed.publish(continuation.data(),continuation.size());
  delayed.start(); delayed.begin_pre_roll_replay();
  while (delayed.queued()) delayed.tick();
  std::vector<uint8_t> expected(speech.end()-3200,speech.end());
  expected.insert(expected.end(),pending.begin(),pending.end());
  expected.insert(expected.end(),continuation.begin(),continuation.end());
  assert(delayed.output()==expected);
 }
 Mic m; m.setup();
 for (int i=0;i<10000;i++) m.frame(99);
 assert(m.queued()==0); // No idle accumulation without listeners.
 m.start();
 for (int i=0;i<10000;i++) m.frame(99);
 assert(m.queued()<=2048 && m.dropped()==0);
 m.request_pre_roll(); m.stop(); // MWW listener disappears before VA starts.
 for (int i=0;i<100;i++) { m.frame(i); m.tick(); }
 assert(m.queued()==54400 && m.output().empty());
 m.start(); m.begin_pre_roll_replay();
 for (int i=0;i<100;i++) {
   m.frame(100+i);
   size_t before=m.output().size(); m.tick();
   assert(m.output().size()-before<=16*1024);
 }
 while (m.queued()) m.tick();
 assert(m.output().size()==3200+200*512);
 for (int i=0;i<3200;i++) assert(m.output()[i]==99);
 for (int i=0;i<200;i++)
   for (int j=0;j<512;j++) assert(m.output()[3200+i*512+j]==i);
 // Exercise wrapping beyond the physical capacity across a long utterance.
 for (int i=0;i<1000;i++) { m.frame(i%256); m.tick(); }
 assert(m.dropped()==0);
 for (int i=0;i<300;i++) m.frame(7);
 assert(m.queued()==128000 && m.dropped()==25600);
 m.discard_pending_audio(); assert(m.queued()==0);
 m.stop(); m.start(); m.output().clear(); m.frame(42); m.tick();
 assert(m.output()==std::vector<uint8_t>(512,42));
 // Short TTS must not enter live audio or future wake-word history.
 m.set_response_playing(true); m.output().clear();
 for (int i=0;i<10;i++) { m.frame(55); m.tick(); }
 assert(m.queued()==0 && m.output().empty());
 m.set_response_playing(false); m.frame(56); m.tick();
 assert(m.queued()==0 && m.output().empty());
 clock_ms+=300; m.frame(57); m.tick();
 assert(m.output()==std::vector<uint8_t>(512,57));
 m.output().clear(); m.request_pre_roll(); m.begin_pre_roll_replay();
 while (m.queued()) m.tick();
 assert(m.output()==std::vector<uint8_t>(512,57));
 // Capture gating is frame aligned, retains pre-roll once, and survives a
 // provider flush while the interruption utterance is still being spoken.
 {
  Mic gated; gated.setup(); gated.start();
  assert(gated.set_barge_in_active(true));
  auto frame = [&](int value, bool near) {
    std::vector<uint8_t> v(512,value); gated.publish(v.data(),v.size(),near); gated.tick();
  };
  for (int i=0;i<40;i++) frame(i,false);
  assert(gated.output().empty());
  for (int i=40;i<45;i++) frame(i,true);
  assert(!gated.set_barge_in_active(true)); // flush/replacement start is idempotent
  for (int i=45;i<108;i++) frame(i,false); // ceil(1000/16) trailing frames
  assert(gated.output().size()==99*512);
  for (int i=9;i<108;i++)
    for (int j=0;j<512;j++) assert(gated.output()[(i-9)*512+j]==i);
  for (int i=108;i<118;i++) frame(i,false);
  assert(gated.output().size()==99*512);
  frame(118,true); // overlapping pre-roll must not duplicate the preceding tail
  assert(gated.output().size()==110*512);
  for (int i=9;i<119;i++)
    for (int j=0;j<512;j++) assert(gated.output()[(i-9)*512+j]==i);
  // Block downstream delivery, including more than the old 64 ms idle limit.
  size_t budget=0;
  gated.set_delivery_condition([&](size_t n) { if(n>budget) return false; budget-=n; return true; });
  gated.discard_pending_audio();
  for (int i=0;i<20;i++) frame(120,true);
  assert(gated.queued()==20*512);
  budget=2048; gated.tick(); assert(gated.queued()==20*512-2048);
  budget=128000; while(gated.queued()) gated.tick();
  assert(gated.dropped()==0);
 }
 // Below-threshold syllables stay byte-exact through hangover and a provider flush.
 {
  Mic speech; speech.setup(); speech.start(); speech.set_barge_in_active(true);
  auto frame = [&](uint8_t value, bool near, bool rejected=true) {
    std::vector<uint8_t> v(512,value); speech.publish(v.data(),v.size(),near,rejected&&!near); speech.tick();
  };
  for(int i=0;i<3;i++) frame(40,false,false); // onset candidates before confirmation
  frame(41,true);
  for(int i=0;i<20;i++) frame(42,false); // weak "tell me about", not proven echo
  assert(!speech.set_barge_in_active(true)); // provider flush/replacement callback
  for(int i=0;i<5;i++) frame(43,true); // "Sunday"
  assert(speech.output().size()==29*512);
  assert(std::all_of(speech.output().begin()+4*512,speech.output().begin()+24*512,[](auto v){return v==42;}));
  assert(std::all_of(speech.output().begin()+24*512,speech.output().end(),[](auto v){return v==43;}));
 }
 // Closed-gate rejected pre-roll stays silent; open-gate PCM is continuous.
 {
  Mic echo; echo.setup(); echo.start(); echo.set_barge_in_active(true);
  auto frame = [&](uint8_t value, bool near, bool rejected) {
    std::vector<uint8_t> v(512,value); echo.publish(v.data(),v.size(),near,rejected); echo.tick();
  };
  for(int i=0;i<40;i++) frame(99,false,true);
  assert(echo.output().empty());
  for(int i=0;i<3;i++) frame(42,false,false); // Onset before 64 ms confirmation.
  frame(42,true,false);
  assert(echo.is_near_end_speech());
  assert(echo.output().size()==32*512);
  assert(std::all_of(echo.output().begin(),echo.output().begin()+28*512,[](auto v){return v==0;}));
  assert(std::all_of(echo.output().begin()+28*512,echo.output().end(),[](auto v){return v==42;}));
  for(int i=0;i<63;i++) frame(99,false,true);
  assert(!echo.is_near_end_speech());
  assert(echo.output().size()==95*512);
  assert(std::all_of(echo.output().begin()+32*512,echo.output().end(),[](auto v){return v==99;}));
  frame(99,false,true);assert(echo.output().size()==95*512);
  // Outside the barge-in gate, wake-word and ordinary capture stay untouched.
  echo.set_barge_in_active(false);frame(99,false,true);
  assert(echo.output().back()==99);
 }
 // Replacement playback after real silence cancels only the inherited tail.
 {
  Mic restart; restart.setup(); restart.start(); restart.set_barge_in_active(true);
  restart.set_delivery_condition([](size_t){return false;});
  auto frame = [&](uint8_t value, bool near, bool playing) {
    std::vector<uint8_t> v(512,value);
    restart.publish(v.data(),v.size(),near,playing&&!near,playing); restart.tick();
  };
  frame(41,true,true);
  for(int i=0;i<19;i++) frame(42,false,false); // 304 ms of reference silence
  frame(43,true,false); // speech immediately before the quick replacement
  const auto queued=restart.queued();
  frame(99,false,true);
  assert(restart.queued()==queued); // no replacement echo; retained speech survives
  frame(99,false,true); assert(restart.queued()==queued);
  frame(44,true,true); assert(restart.queued()>queued); // fresh speech reopens
  restart.set_delivery_condition([](size_t){return true;});
  while(restart.queued()) restart.tick();
  assert(std::count(restart.output().begin(),restart.output().end(),uint8_t{99})==0);
  assert(std::count(restart.output().begin(),restart.output().end(),uint8_t{43})==512);
  assert(std::count(restart.output().begin(),restart.output().end(),uint8_t{44})==512);
  assert(restart.dropped()==0);
 }
 // Short gaps inside the same reply must not cut off weak user syllables.
 {
  Mic pause; pause.setup(); pause.start(); pause.set_barge_in_active(true);
  auto frame = [&](uint8_t value, bool near, bool playing) {
    std::vector<uint8_t> v(512,value);
    pause.publish(v.data(),v.size(),near,playing&&!near,playing); pause.tick();
  };
  frame(41,true,true);
  for(int i=0;i<4;i++) frame(42,false,false); // 64 ms
  frame(43,false,true);
  assert(pause.output().size()==6*512);
  assert(pause.output().back()==43);
 }
 // Protection previews the SAME capture position/restart rule as delivery.
 // Wall-clock stalls and repeated provider starts cannot extend/truncate it.
 {
  Mic protect; protect.setup(); protect.start();
  assert(protect.get_barge_in_speech_protection(.9f,true,true)==0.f);
  protect.set_barge_in_active(true);
  auto frame = [&](bool near, bool playing) {
    std::vector<uint8_t> v(512,42);
    protect.publish(v.data(),v.size(),near,!near,playing);
  };
  assert(protect.get_barge_in_speech_protection(.7f,false,true)==.7f); // onset
  assert(protect.get_barge_in_speech_protection(.9f,true,true)==1.f); // first confirmation
  frame(true,true);
  clock_ms+=5000; // hold is measured in capture samples, not elapsed wall time
  assert(!protect.set_barge_in_active(true)); // flush is idempotent
  for(int i=0;i<63;i++) {
    assert(protect.get_barge_in_speech_protection(.01f,false,true)==1.f);
    frame(false,true);
  }
  const auto queued=protect.queued();
  assert(protect.get_barge_in_speech_protection(.01f,false,true)==.01f);
  frame(false,true);assert(protect.queued()==queued); // exact same expiry frame
  frame(true,true);
  for(int i=0;i<4;i++) frame(false,false);
  assert(protect.get_barge_in_speech_protection(.02f,false,true)==1.f); // short gap
  frame(false,true);
  for(int i=0;i<19;i++) frame(false,false); // 304 ms: replacement cancels hold
  const auto before_restart=protect.queued();
  assert(protect.get_barge_in_speech_protection(.02f,false,true)==.02f);
  frame(false,true);assert(protect.queued()==before_restart);
  assert(protect.get_barge_in_speech_protection(.9f,true,true)==1.f);
  frame(true,true); // fresh confirmation reopens
  protect.set_barge_in_active(false);
  assert(protect.get_barge_in_speech_protection(.9f,true,true)==0.f);
  frame(false,true); // ordinary capture is no longer gated
  protect.set_barge_in_active(true);
  assert(protect.get_barge_in_speech_protection(.02f,false,true)==.02f); // no stale hold
  protect.set_response_playing(true);
  assert(protect.get_barge_in_speech_protection(.9f,true,true)==0.f);
  protect.set_response_playing(false);
  assert(protect.get_barge_in_speech_protection(.9f,true,true)==0.f); // acoustic tail
  clock_ms+=300;
  assert(protect.get_barge_in_speech_protection(.9f,true,true)==1.f);
 }
 // Cancel an armed handoff and verify later wake-word delivery resumes.
 m.request_pre_roll(); m.frame(8); m.discard_pending_audio();
 m.output().clear(); m.frame(43); m.tick();
 assert(m.output()==std::vector<uint8_t>(512,43));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "test.cpp"
            exe = Path(directory) / "test"
            cpp.write_text(harness)
            subprocess.run(["g++", "-std=c++17", "-fsanitize=undefined", "-g",
                            str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
