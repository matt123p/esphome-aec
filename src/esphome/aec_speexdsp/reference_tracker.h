#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace esphome::aec_speexdsp {
struct ReferenceFrame {
  bool active{false}, startup{false}, quiet{true}, excited{false};
};
// Advance once per 16 kHz input frame BEFORE preprocessing. All consumers
// receive the same snapshot and cannot advance its timers independently.
class ReferenceTracker {
 public:
  void configure(float open, float close, uint32_t tail_ms, uint32_t startup_ms) {
    open_ = open; close_ = close; tail_ = tail_ms * 16; startup_ = startup_ms * 16;
    active_ = false; quiet_ = remaining_ = 0;
  }
  ReferenceFrame process(float rms, uint32_t samples) {
    if (!std::isfinite(rms) || rms < 0 || samples == 0 || samples > 1024)
      return {true, true, false, false};
    if (rms >= open_) {
      if (!active_) remaining_ = startup_;
      active_ = true; quiet_ = 0;
    } else if (active_) {
      quiet_ = rms < close_ ? std::min(quiet_ + samples, tail_) : 0;
      if (rms < close_ && quiet_ >= tail_) active_ = false;
    }
    ReferenceFrame result{active_, active_ && remaining_ != 0, rms < close_, rms >= open_};
    remaining_ = samples >= remaining_ ? 0 : remaining_ - samples;
    return result;
  }
 private:
  float open_{200}, close_{100};
  uint32_t tail_{4000}, startup_{3200}, quiet_{0}, remaining_{0};
  bool active_{false};
};
}
