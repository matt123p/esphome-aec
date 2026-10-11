"""Exercise the production wake cue and its reference-tap placement."""
from pathlib import Path
import shutil
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.skipif(not shutil.which('g++'), reason='Requires host compiler')
def test_wake_beep_duration_mixing_and_cancel(tmp_path):
    code = r'''
#include "src/esphome/aec_speexdsp/wake_beep.h"
#include <cassert>
#include <vector>
using esphome::aec_speexdsp::WakeBeep;
int main() {
 for(unsigned channels : {1u,2u}) {
  WakeBeep beep;std::vector<int16_t> pcm(512*channels);
  assert(beep.render(pcm.data(),512,channels)==0);
  beep.start();assert(beep.active());size_t samples=0;std::vector<int16_t> complete;
  for(int block=0;block<4;++block) {
   std::fill(pcm.begin(),pcm.end(),0);
   size_t n=beep.render(pcm.data(),512,channels);samples+=n;
   for(size_t i=0;i<n;++i) {
    if(channels==2)assert(pcm[2*i]==pcm[2*i+1]);
    complete.push_back(pcm[i*channels]);
   }
   for(size_t i=n*channels;i<pcm.size();++i)assert(pcm[i]==0);
  }
  assert(samples==1600&&complete.front()==0&&complete.back()==0);
  assert(!beep.active());
  assert(*std::max_element(complete.begin(),complete.end())==8192);
  assert(*std::min_element(complete.begin(),complete.end())==-8192);
  assert(beep.render(pcm.data(),512,channels)==0);
  beep.start();beep.render(pcm.data(),512,channels);beep.stop();
  assert(!beep.active());
  assert(beep.render(pcm.data(),512,channels)==0);
  beep.start();std::fill(pcm.begin(),pcm.end(),32000);
  beep.render(pcm.data(),512,channels);
  assert(*std::max_element(pcm.begin(),pcm.end())==32767);
  beep.start();std::fill(pcm.begin(),pcm.end(),-32000);
  beep.render(pcm.data(),512,channels);
  assert(*std::min_element(pcm.begin(),pcm.end())==-32768);
 }
}
'''
    cpp=tmp_path/'beep.cpp';exe=tmp_path/'beep'
    cpp.write_text(code)
    subprocess.run(['g++','-std=c++17','-fsanitize=address,undefined','-I',str(ROOT),str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
    source=(ROOT/'src/esphome/aec_speexdsp/aec_speexdsp.cpp').read_text()
    playback=source.split('void AECSpeexDspComponent::run_playback_task_()',1)[1]
    assert playback.index('wake_beep_.render(')<playback.index('const float gain =')<playback.index('reference[frame] =')
