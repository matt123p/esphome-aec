#include "neural_barge_in.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include "speex_preprocess.h"
using esphome::aec_speexdsp::NeuralBargeIn;
int main(int argc, char **argv) {
  // Neural speech protection starts without waiting for event confirmation.
  auto *pre=speex_preprocess_state_init(256,16000);
  if (!pre) return 3;
  int16_t silence[256]{};
  float protect=1.f;
  if (speex_preprocess_ctl(pre,SPEEX_PREPROCESS_SET_NEURAL_PROTECT_PROB,&protect)) return 4;
  speex_preprocess_run(pre,silence);
  SpeexNearEndDiagnostics diagnostics{};
  speex_preprocess_ctl(pre,SPEEX_PREPROCESS_GET_NEAR_END_DIAGNOSTICS,&diagnostics);
  if (diagnostics.protected_probability != 1.f) return 5;
  protect=std::numeric_limits<float>::quiet_NaN();
  if (!speex_preprocess_ctl(pre,SPEEX_PREPROCESS_SET_NEURAL_PROTECT_PROB,&protect)) return 6;
  speex_preprocess_state_destroy(pre);
  NeuralBargeIn detector;
  if (!detector.init()) return 1;
  const float threshold=argc>1?std::strtof(argv[1],nullptr):esphome::aec_speexdsp::barge_model::threshold;
  uint32_t flags[2]; int16_t pcm[3][256];
  while (std::fread(flags,sizeof(flags),1,stdin)==1) {
    if (std::fread(pcm,sizeof(pcm),1,stdin)!=1) return 2;
    detector.set_enabled((flags[1] & 2) == 0);
    auto r=detector.process(pcm[0],pcm[1],1,pcm[2],flags[0],flags[1] & 1,threshold,3,
                            (flags[1] & 4) == 0);
    std::printf("%.9g %d %d %d %d",r.probability,r.ready,r.candidate,r.confirmed,r.event);
    for (unsigned i=0;i<84;++i) std::printf(" %.9g",detector.features()[i]);
    for (unsigned i=0;i<84;++i) std::printf(" %.9g",detector.normalized()[i]);
    std::puts("");
  }
  return 0;
}
