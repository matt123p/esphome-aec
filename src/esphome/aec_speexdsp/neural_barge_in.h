#pragma once
#if !defined(USE_ESP32) || defined(USE_AEC_SPEEXDSP_NEURAL_BARGE_IN)
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "fftwrap.h"
#include "barge_in_model.h"

namespace esphome::aec_speexdsp {
static_assert(barge_model::format_version == 1 && barge_model::feature_order_version == 1 &&
              barge_model::sample_rate == 16000 && barge_model::frame_samples == 256 &&
              barge_model::context_frames == 16 && barge_model::inputs == 84 &&
              barge_model::dense_units == 32 && barge_model::gru_units == 24,
              "Incompatible neural barge-in model metadata");
// Feature order v1: (mic0,mic1,ref) per Bark band, then four spatial
// channels per region. Matches checkpoint names, not the recipe's suggestion.
class NeuralBargeIn {
 public:
  NeuralBargeIn() = default;
  NeuralBargeIn(const NeuralBargeIn &) = delete;
  NeuralBargeIn &operator=(const NeuralBargeIn &) = delete;
  struct Result { float probability{0}; bool ready{false}, candidate{false}, confirmed{false}, event{false}; };
  ~NeuralBargeIn() { if (fft_) spx_fft_destroy(fft_); }
  bool init() {
    if (!fft_) fft_ = spx_fft_init(256);
    for (unsigned i=0;i<256;++i) window_[i]=.5f-.5f*std::cos(6.283185307179586f*i/255);
    reset(); return fft_ != nullptr && barge_model::format_version == 1;
  }
  void reset() {
    ++resets_;
    std::memset(features_, 0, sizeof(features_));
    std::memset(dense_valid_, 0, sizeof(dense_valid_));
    std::memcpy(mean_,barge_model::initial_mean,sizeof(mean_));
    std::memcpy(var_,barge_model::initial_var,sizeof(var_));
    std::memcpy(spatial_,barge_model::spatial_initial,sizeof(spatial_));
    count_=head_=consecutive_=0; result_={};
  }
  const Result &result() const { return result_; }
  void set_enabled(bool enabled) {
    if (enabled_ != enabled) { enabled_ = enabled; reset(); }
  }
  unsigned context_frames() const { return count_; }
  unsigned consecutive_frames() const { return consecutive_; }
  uint32_t resets() const { return resets_; }
  const float *normalizer_variance() const { return var_; }
  // Statistics advance in EVERY state; neural arithmetic only during playback.
  Result process(const int16_t *mic0, const int16_t *mic1, unsigned stride,
                 const int16_t *ref, bool playing, bool valid, float threshold, unsigned confirmation,
                 bool barge_in_possible = true) {
    if (!enabled_) return result_;
    if (!valid || !fft_) { reset(); return result_; }
    for (unsigned c=0;c<3;++c) {
      for (unsigned i=0;i<256;++i)
        scratch_[i]=(c==2?ref[i]:(c==0?mic0:mic1)[i*stride])*window_[i];
      spx_fft(fft_,scratch_,spectra_[c]);
      // Speex forward FFT is normalized; training uses unscaled numpy rfft.
      for (float &v:spectra_[c]) v*=256;
    }
    float compact[12]{}; unsigned regions[3]{};
    for (unsigned b=0;b<24;++b) {
      float power[3]{}, re=0, im=0;
      const unsigned first=barge_model::band_first[b], last=barge_model::band_last[b];
      for (unsigned k=first;k<=last;++k) {
        float r[3], j[3];
        for (unsigned c=0;c<3;++c) {
          r[c]=spectra_[c][k==128?255:k==0?0:2*k-1];
          j[c]=(k==0||k==128)?0:spectra_[c][2*k];
          power[c]+=r[c]*r[c]+j[c]*j[c];
        }
        re+=r[0]*r[1]+j[0]*j[1]; im+=j[0]*r[1]-r[0]*j[1];
      }
      const float bins=last-first+1;
      for (unsigned c=0;c<3;++c) { power[c]/=bins; features_[b*3+c]=std::log1p(power[c]); }
      const float normal=std::sqrt(power[0]*power[1])+1e-8f;
      re/=bins*normal; im/=bins*normal;
      const unsigned region=barge_model::band_region[b]; ++regions[region];
      compact[4*region]+=std::log((power[0]+1)/(power[1]+1));
      compact[4*region+1]+=std::sqrt(re*re+im*im);
      compact[4*region+2]+=re; compact[4*region+3]+=im;
    }
    for (unsigned i=0;i<12;++i) {
      const float current=compact[i]/regions[i/4];
      features_[72+i]=(current-spatial_[i])/barge_model::spatial_scale[i];
      if (playing) spatial_[i]+=.02f*(current-spatial_[i]);
    }
    for (unsigned i=0;i<84;++i) {
      const float delta=features_[i]-mean_[i];
      context_[head_][i]=std::clamp(delta/std::sqrt(std::max(var_[i],.0025f)),-12.f,12.f);
      mean_[i]+=.01f*delta; var_[i]=.99f*var_[i]+.01f*delta*delta;
      if (!std::isfinite(context_[head_][i]) || !std::isfinite(var_[i])) { reset(); return result_; }
    }
    // The normalized frame is immutable until this ring slot is overwritten.
    // Cache its Dense32 projection lazily so idle frames still skip neural work.
    dense_valid_[head_]=false;
    head_=(head_+1)%16; count_=std::min(count_+1,16u);
    Result next{};
    next.ready=count_==16;
    if (playing && barge_in_possible && next.ready) {
      float hidden[24]{};
      for (unsigned t=0;t<16;++t) {
        float gates[72], recurrent[72];
        const unsigned slot=(head_+t)%16;
        const float *dense=dense_context_[slot];
        if (!dense_valid_[slot]) {
          const float *input=context_[slot];
          for (unsigned o=0;o<32;++o) {
            float v=barge_model::input_0_bias[o];
            for (unsigned i=0;i<84;++i) v+=barge_model::input_0_weight[o*84+i]*input[i];
            dense_context_[slot][o]=std::max(0.f,v);
          }
          dense_valid_[slot]=true;
        }
        for (unsigned o=0;o<72;++o) {
          gates[o]=barge_model::core_bias_ih_l0[o]; recurrent[o]=barge_model::core_bias_hh_l0[o];
          for (unsigned i=0;i<32;++i) gates[o]+=barge_model::core_weight_ih_l0[o*32+i]*dense[i];
          for (unsigned i=0;i<24;++i) recurrent[o]+=barge_model::core_weight_hh_l0[o*24+i]*hidden[i];
        }
        // PyTorch gate order r,z,n, reset applied AFTER recurrent affine.
        for (unsigned i=0;i<24;++i) {
          float r=sigmoid(gates[i]+recurrent[i]), z=sigmoid(gates[24+i]+recurrent[24+i]);
          float n=std::tanh(gates[48+i]+r*recurrent[48+i]);
          hidden[i]=(1-z)*n+z*hidden[i];
        }
      }
      float logit=barge_model::output_bias[0];
      for (unsigned i=0;i<24;++i) logit+=hidden[i]*barge_model::output_weight[i];
      next.probability=sigmoid(logit); next.candidate=next.probability>=threshold;
      consecutive_=next.candidate?std::min(consecutive_+1,confirmation):0;
      next.confirmed=consecutive_>=confirmation; next.event=next.confirmed&&!result_.confirmed;
    } else consecutive_=0;
    result_=next; return result_;
  }
  const float *features() const { return features_; }
  const float *normalized() const { return context_[(head_+15)%16]; }
 private:
  static float sigmoid(float x) { return 1.f/(1.f+std::exp(-x)); }
  void *fft_{nullptr};
  float window_[256]{}, scratch_[256]{}, spectra_[3][256]{};
  float features_[84]{}, mean_[84]{}, var_[84]{}, spatial_[12]{}, context_[16][84]{};
  float dense_context_[16][32]{};
  bool dense_valid_[16]{};
  unsigned count_{0}, head_{0}, consecutive_{0}; Result result_{};
  uint32_t resets_{0};
  bool enabled_{true};
};
}
#endif
