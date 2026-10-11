"""Production alignment: bounded frame work, acquisition and loss recovery."""
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.skipif(not shutil.which('g++'), reason='Requires host compiler')
def test_alignment_acquisition_tracking_and_recovery(tmp_path):
    code = r'''
#include "src/esphome/aec_speexdsp/reference_delay_tracker.h"
#include <cassert>
#include <random>
#include <vector>
using namespace esphome::aec_speexdsp;
int main() {
 std::mt19937 rng(9826); std::uniform_int_distribution<int> random(-12000,12000);
 constexpr int total=16000*24, frame=256;
 std::vector<int16_t> ref(total), mic(total*3);
 for(auto &v:ref) v=random(rng);
 ReferenceDelayTracker tracker; tracker.configure(7);
 auto feed=[&](int begin,int end,int delay,bool excited=true) {
  for(int n=begin;n<end;++n) mic[n*3]=n>=delay?int16_t(-.65f*ref[n-delay]):0;
  for(int n=begin;n+frame<=end;n+=frame) {
   tracker.process(mic.data()+n*3,3,ref.data()+n,frame,excited);
   assert(tracker.work().lags_last_call<=ReferenceDelayTracker::LAGS_PER_CALL);
  }
 };
 // Arbitrary full-range acquisition; raw microphone interleaving and inverted
 // polarity must not change the selected causal lag.
 feed(0,16000*3,73);
 assert(tracker.estimate().valid && tracker.estimate().delay_samples==73);
 assert(tracker.estimate().correlation>.99f);
 assert(tracker.work().local_searches>0 && tracker.work().full_searches>=1);
 const auto global_before=tracker.work().full_searches;
 // A one-sample path change stays in the tracking window, with no jump/reset.
 feed(16000*3,16000*5,74);
 assert(tracker.estimate().valid && tracker.estimate().delay_samples==74);
 assert(tracker.work().full_searches==global_before);
 // Local edge must expand, and large path changes must reacquire globally.
 feed(16000*5,16000*8,82);
 assert(tracker.estimate().valid && tracker.estimate().delay_samples>74 && tracker.estimate().delay_samples<=82);
 assert(tracker.work().full_searches>global_before);
 feed(16000*8,16000*11,210);
 assert(tracker.estimate().valid && tracker.estimate().delay_samples==210);
 // Continuing capture refreshes globally even when the local estimate is good.
 const auto refresh_before=tracker.work().full_searches;
 feed(16000*11,16000*21,210);
 assert(tracker.work().full_searches>refresh_before);
 // Silence cancels a partial scan and does no lag work; keep the applied delay.
 feed(16000*21,16000*22,210,false);
 assert(!tracker.estimate().valid && tracker.estimate().delay_samples==210);
 assert(tracker.work().lags_last_call==0);
 feed(16000*22,16000*24,5);
 assert(tracker.estimate().valid && tracker.estimate().delay_samples==5);
 assert(!tracker.process(nullptr,1,ref.data(),frame,true).valid);
 tracker.reset(7);
 assert(tracker.estimate().delay_samples==7 && !tracker.estimate().valid);
 assert(tracker.work().completed_searches==0);

 // Full-range endpoints, different frame/window sizes, reset configuration.
 for(int delay:{0,1,7,128,255,256}) for(int block:{128,256,512,1024}) {
  ReferenceDelayTracker t; t.configure(delay,256,1024,512);
  for(int n=0;n<16000*4;++n) mic[n*3]=n>=delay?ref[n-delay]:0;
  for(int n=0;n+block<=16000*4;n+=block) {
   t.process(mic.data()+n*3,3,ref.data()+n,block,true);
   assert(t.work().lags_last_call<=8);
  }
  assert(t.estimate().valid && t.estimate().delay_samples==delay);
 }
 // Independent near-end audio and periodic ambiguous material never acquire.
 for(int mode:{0,1}) {
  ReferenceDelayTracker t; t.configure(7,256,1024);
  for(int n=0;n<16000*2;++n) {
   if(mode==0) mic[n*3]=random(rng);
   else { ref[n]=int16_t(10000*std::sin(n*6.283185307179586/16)); mic[n*3]=ref[n]; }
  }
  for(int n=0;n+frame<=16000*2;n+=frame) t.process(mic.data()+n*3,3,ref.data()+n,frame,true);
  assert(!t.estimate().valid && t.estimate().delay_samples==7);
 }
 // Flat/DC input with an accidentally true excitation flag is rejected safely.
 ReferenceDelayTracker dc; dc.configure(7,256,1024);
 int16_t zeros[256]{};
 for(int n=0;n<160;++n) dc.process(zeros,1,zeros,256,true);
 assert(!dc.estimate().valid && dc.estimate().delay_samples==7);
}
'''
    cpp = tmp_path / 'alignment.cpp'
    exe = tmp_path / 'alignment'
    cpp.write_text(code)
    subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined,address', '-I', str(ROOT), str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
