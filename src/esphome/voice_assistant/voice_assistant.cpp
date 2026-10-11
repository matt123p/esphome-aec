#include "voice_assistant.h"
#include "esphome/core/defines.h"

#ifdef USE_VOICE_ASSISTANT

#include "esphome/components/socket/socket.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>

namespace esphome::voice_assistant {

static const char *const TAG = "voice_assistant";

#ifdef SAMPLE_RATE_HZ
#undef SAMPLE_RATE_HZ
#endif

static const size_t SAMPLE_RATE_HZ = 16000;

// Bounded upload staging. The local capture-side gate uses backpressure to
// retain the rest of a long utterance in its FIFO without overwriting its head.
static const size_t RING_BUFFER_SAMPLES = 2000 * SAMPLE_RATE_HZ / 1000;  // 2 s at 16 kHz
static const size_t RING_BUFFER_SIZE = RING_BUFFER_SAMPLES * sizeof(int16_t);
static const size_t BARGE_IN_PRE_ROLL_SIZE = 512 * SAMPLE_RATE_HZ / 1000 * sizeof(int16_t);
static const size_t SEND_BUFFER_SAMPLES = 32 * SAMPLE_RATE_HZ / 1000;  // 32ms * 16kHz / 1000ms
static const size_t SEND_BUFFER_SIZE = SEND_BUFFER_SAMPLES * sizeof(int16_t);
// The producer's sample clock supplies the live rate. Catch up using queue
// depth, not a pre-roll byte counter or a wall-clock packet interval.
static const uint32_t BARGE_IN_SPEECH_HANGOVER_MS = 1000;
static const size_t MAX_API_AUDIO_PACKETS_PER_LOOP = 2;
static const size_t MAX_CATCHUP_PACKETS_PER_LOOP = 8;
static const size_t RECEIVE_SIZE = 1024;
// If one microphone channel keeps producing audio while another configured channel produces none for this
// long, treat the silent channel as failed and stop the stream. A working microphone exposes a chunk every
// SEND_BUFFER_SAMPLES (32 ms), so this is far longer than any legitimate gap between chunks.
static const uint32_t AUDIO_CHANNEL_STALL_TIMEOUT_MS = 2000;

VoiceAssistant::VoiceAssistant() { global_voice_assistant = this; }

void VoiceAssistant::setup() {
  this->mic_source_->add_data_callback([this](const std::vector<uint8_t> &data) {
    std::shared_ptr<ring_buffer::RingBuffer> temp_ring_buffer = this->ring_buffer_.lock();
    if (temp_ring_buffer != nullptr) {
      temp_ring_buffer->write((void *) data.data(), data.size());
    }
  });

  // Second microphone channel
  if (this->mic_source2_ != nullptr) {
    this->mic_source2_->add_data_callback([this](const std::vector<uint8_t> &data) {
      std::shared_ptr<ring_buffer::RingBuffer> temp_ring_buffer = this->ring_buffer2_.lock();
      if (temp_ring_buffer != nullptr) {
        temp_ring_buffer->write((void *) data.data(), data.size());
      }
    });
  }

#ifdef USE_MEDIA_PLAYER
  if (this->media_player_ != nullptr) {
    this->media_player_->add_on_state_callback([this](media_player::MediaPlayerState state) {
      switch (state) {
        case media_player::MediaPlayerState::MEDIA_PLAYER_STATE_ANNOUNCING:
          if (this->media_player_response_state_ == MediaPlayerResponseState::URL_SENT) {
            // State changed to announcing after receiving the url
            this->media_player_response_state_ = MediaPlayerResponseState::PLAYING;
          }
          break;
        default:
          if (this->media_player_response_state_ == MediaPlayerResponseState::PLAYING) {
            // No longer announcing the TTS response
            this->media_player_response_state_ = MediaPlayerResponseState::FINISHED;
          }
          break;
      }
    });
  }
#endif
}

float VoiceAssistant::get_setup_priority() const { return setup_priority::AFTER_CONNECTION; }

bool VoiceAssistant::start_udp_socket_() {
  this->socket_ = socket::socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (this->socket_ == nullptr) {
    ESP_LOGE(TAG, "Could not create socket");
    this->mark_failed();
    return false;
  }
  int enable = 1;
  int err = this->socket_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int));
  if (err != 0) {
    ESP_LOGW(TAG, "Socket unable to set reuseaddr: errno %d", err);
    // we can still continue
  }
  err = this->socket_->setblocking(false);
  if (err != 0) {
    ESP_LOGE(TAG, "Socket unable to set nonblocking mode: errno %d", err);
    this->mark_failed();
    return false;
  }

#ifdef USE_SPEAKER
  if (this->speaker_ != nullptr) {
    struct sockaddr_storage server;

    socklen_t sl = socket::set_sockaddr_any((struct sockaddr *) &server, sizeof(server), 6055);
    if (sl == 0) {
      ESP_LOGE(TAG, "Socket unable to set sockaddr: errno %d", errno);
      this->mark_failed();
      return false;
    }

    err = this->socket_->bind((struct sockaddr *) &server, sizeof(server));
    if (err != 0) {
      ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
      this->mark_failed();
      return false;
    }
  }
#endif
  this->udp_socket_running_ = true;
  return true;
}

bool VoiceAssistant::allocate_buffers_() {
#ifdef USE_SPEAKER
  if ((this->speaker_ != nullptr) && (this->speaker_buffer_ == nullptr)) {
    RAMAllocator<uint8_t> speaker_allocator;
    this->speaker_buffer_ = speaker_allocator.allocate(this->speaker_buffer_capacity_);
    if (this->speaker_buffer_ == nullptr) {
      ESP_LOGW(TAG, "Could not allocate speaker buffer");
      return false;
    }
  }
#endif

  if (this->audio_source_ == nullptr) {
    std::shared_ptr<ring_buffer::RingBuffer> temp_ring_buffer = ring_buffer::RingBuffer::create(RING_BUFFER_SIZE);
    if (temp_ring_buffer == nullptr) {
      ESP_LOGE(TAG, "Could not allocate ring buffer");
      return false;
    }
    // Zero-copy source that reads directly from the ring buffer; frame-aligned to never split an int16 sample.
    this->audio_source_ = audio::RingBufferAudioSource::create(temp_ring_buffer, SEND_BUFFER_SIZE, sizeof(int16_t));
    if (this->audio_source_ == nullptr) {
      ESP_LOGE(TAG, "Could not allocate audio source");
      return false;
    }
    this->ring_buffer_ = temp_ring_buffer;
  }

  // Second microphone channel
  if ((this->mic_source2_ != nullptr) && (this->audio_source2_ == nullptr)) {
    std::shared_ptr<ring_buffer::RingBuffer> temp_ring_buffer = ring_buffer::RingBuffer::create(RING_BUFFER_SIZE);
    if (temp_ring_buffer == nullptr) {
      ESP_LOGE(TAG, "Could not allocate second ring buffer");
      return false;
    }
    this->audio_source2_ = audio::RingBufferAudioSource::create(temp_ring_buffer, SEND_BUFFER_SIZE, sizeof(int16_t));
    if (this->audio_source2_ == nullptr) {
      ESP_LOGE(TAG, "Could not allocate second audio source");
      return false;
    }
    this->ring_buffer2_ = temp_ring_buffer;
  }

  return true;
}

void VoiceAssistant::clear_buffers_() {
  if (this->audio_source_ != nullptr) {
    this->audio_source_->clear_buffered_data();
  }

  // Second microphone channel
  if (this->audio_source2_ != nullptr) {
    this->audio_source2_->clear_buffered_data();
  }

  // Reset the multi-channel stall watchdog (see audio_channel_stall_start_).
  this->audio_channel_stall_start_ = 0;

  this->mic_diagnostic_start_ = 0;
  this->mic_diagnostic_bytes_ = 0;
  this->mic_diagnostic_packets_ = 0;
  this->mic_diagnostic_dropped_bytes_ = 0;
  this->mic_diagnostic_energy_ = 0;
  this->mic_diagnostic_peak_ = 0;
  this->mic_diagnostic_drops_ = 0;
  this->mic_retry_at_ms_ = 0;
  this->mic_blocked_since_ms_ = 0;
  this->barge_in_speech_active_until_ms_ = 0;
  this->barge_in_upload_gate_open_ = false;

#ifdef USE_SPEAKER
  if ((this->speaker_ != nullptr) && (this->speaker_buffer_ != nullptr)) {
    memset(this->speaker_buffer_, 0, this->speaker_buffer_capacity_);

    this->speaker_buffer_size_ = 0;
    this->speaker_buffer_index_ = 0;
    this->speaker_bytes_received_ = 0;
  }
#endif
}

void VoiceAssistant::deallocate_buffers_() {
  // Destroying each source releases its ring buffer; the matching weak_ptr then expires automatically.
  this->audio_source_.reset();

  // Second microphone channel
  this->audio_source2_.reset();

#ifdef USE_SPEAKER
  if ((this->speaker_ != nullptr) && (this->speaker_buffer_ != nullptr)) {
    RAMAllocator<uint8_t> speaker_deallocator;
    speaker_deallocator.deallocate(this->speaker_buffer_, this->speaker_buffer_capacity_);
    this->speaker_buffer_ = nullptr;
  }
#endif
}

void VoiceAssistant::reset_conversation_id() {
  this->conversation_id_ = "";
  ESP_LOGD(TAG, "reset conversation ID");
}

void VoiceAssistant::set_barge_in_capture_gate(bool enabled) {
  this->barge_in_capture_gate_ = enabled;
}

void VoiceAssistant::reset_microphone_upload() {
  if (this->audio_source_) this->audio_source_->clear_buffered_data();
  if (this->audio_source2_) this->audio_source2_->clear_buffered_data();
  this->mic_retry_at_ms_ = 0;
  this->mic_blocked_since_ms_ = 0;
}

bool VoiceAssistant::can_accept_microphone_bytes(size_t bytes) const {
  if (!this->mic_source_->is_running()) return true;  // Do not stall the wake-word listener.
  auto ring = this->ring_buffer_.lock();
  auto ring2 = this->ring_buffer2_.lock();
  return (!ring || ring->free() >= bytes) && (!ring2 || ring2->free() >= bytes);
}

bool VoiceAssistant::barge_in_timed_out(uint32_t idle_ms, uint32_t response_ms) const {
  if (!this->barge_in_ || !this->is_running()) return false;
#ifdef USE_SPEAKER
  if (this->speaker_ && (this->speaker_buffer_size_ != 0 || this->speaker_->has_buffered_data() ||
                         this->speaker_->is_running())) return false;
#endif
  // Listening remains an idle-listening state, even after microphone activity.
  // Only pipeline/output state warrants the long provider-response deadline.
  const bool waiting = this->state_ != State::STREAMING_MICROPHONE;
  return this->audio_silent_for(waiting ? response_ms : idle_ms);
}

void VoiceAssistant::stream_api_audio_() {
  const bool gate_response = this->barge_in_ && this->state_ == State::STREAMING_RESPONSE;
  // Legacy sources can still supply a delivery condition. The local AEC path
  // gates PCM on the capture timeline and must not be gated a second time here.
  if (gate_response && !this->barge_in_capture_gate_ && this->barge_in_audio_condition_) {
    const uint32_t now = millis();
    if (this->barge_in_audio_condition_()) {
      this->barge_in_speech_active_until_ms_ = now + BARGE_IN_SPEECH_HANGOVER_MS;
      this->last_microphone_activity_ms_ = now;
    } else if (this->barge_in_speech_active_until_ms_ == 0 ||
               static_cast<int32_t>(now - this->barge_in_speech_active_until_ms_) >= 0) {
      this->barge_in_upload_gate_open_ = false;
      return;
    }
    if (!this->barge_in_upload_gate_open_) {
      auto trim = [](auto &source, auto &weak_ring) {
        auto ring = weak_ring.lock();
        if (!source || !ring) return;
        const size_t buffered = ring->available() + source->available();
        size_t discard = buffered > BARGE_IN_PRE_ROLL_SIZE ? buffered - BARGE_IN_PRE_ROLL_SIZE : 0;
        while (discard != 0) {
          source->fill(0, false);
          const size_t count = std::min(discard, source->available());
          if (count == 0) break;
          source->consume(count);
          discard -= count;
        }
      };
      trim(this->audio_source_, this->ring_buffer_);
      trim(this->audio_source2_, this->ring_buffer2_);
      this->barge_in_upload_gate_open_ = true;
      ESP_LOGD(TAG, "Barge-in upload gate opened");
    }
  }
  if (this->mic_retry_at_ms_ != 0 && static_cast<int32_t>(millis() - this->mic_retry_at_ms_) < 0) return;

  if (this->barge_in_) {
    const uint32_t now = millis();
    if (this->mic_diagnostic_start_ == 0)
      this->mic_diagnostic_start_ = now;
    const uint32_t elapsed = now - this->mic_diagnostic_start_;
    if (elapsed >= 5000) {
      const size_t samples = this->mic_diagnostic_bytes_ / sizeof(int16_t);
      const float rms = samples ? std::sqrt(static_cast<double>(this->mic_diagnostic_energy_) / samples) : 0.0f;
      const size_t attempted_bytes = this->mic_diagnostic_bytes_ + this->mic_diagnostic_dropped_bytes_;
      ESP_LOGI(TAG, "Mic upload: sent=%u bytes/%" PRIu32 " packets dropped=%u bytes/%" PRIu32
                    " packets attempted=%.1f B/s delivered=%.1f B/s window=%" PRIu32
                    " ms rms=%.1f peak=%" PRIu32 " state=%u",
               static_cast<unsigned>(this->mic_diagnostic_bytes_), this->mic_diagnostic_packets_,
               static_cast<unsigned>(this->mic_diagnostic_dropped_bytes_), this->mic_diagnostic_drops_,
               elapsed ? attempted_bytes * 1000.0f / elapsed : 0.0f,
               elapsed ? this->mic_diagnostic_bytes_ * 1000.0f / elapsed : 0.0f, elapsed, rms,
               this->mic_diagnostic_peak_, static_cast<unsigned>(this->state_));
      this->mic_diagnostic_start_ = now;
      this->mic_diagnostic_bytes_ = 0;
      this->mic_diagnostic_packets_ = 0;
      this->mic_diagnostic_dropped_bytes_ = 0;
      this->mic_diagnostic_energy_ = 0;
      this->mic_diagnostic_peak_ = 0;
      this->mic_diagnostic_drops_ = 0;
    }
  }
  // Both microphone channels are sent together, if configured. Home Assistant feeds one of the
  // channels to its speech-to-text stream and treats an empty payload on that channel as
  // end-of-stream, and the device cannot know which channel it picked, so only send once every
  // configured channel has audio exposed, and always send them together. We don't target any
  // particular message size: Home Assistant re-chunks the audio, and each fill() exposes at most
  // SEND_BUFFER_SIZE bytes.
  auto ring = this->ring_buffer_.lock();
  const size_t backlog = (ring ? ring->available() : 0) + this->audio_source_->available();
  const size_t packet_limit = this->barge_in_ && backlog > 2 * SEND_BUFFER_SIZE
                                  ? MAX_CATCHUP_PACKETS_PER_LOOP : MAX_API_AUDIO_PACKETS_PER_LOOP;
  size_t packets_sent = 0;
  while (packets_sent < packet_limit) {
    // fill() exposes a new chunk, or returns 0 if a previous chunk is still exposed; available()
    // reports the currently exposed bytes either way.
    this->audio_source_->fill(0, false);
    size_t available = this->audio_source_->available();
    size_t available2 = 0;
    if (this->audio_source2_ != nullptr) {
      this->audio_source2_->fill(0, false);
      available2 = this->audio_source2_->available();
    }

    const bool channel_empty = (available == 0);
    const bool channel2_empty = (this->audio_source2_ != nullptr) && (available2 == 0);
    if (channel_empty || channel2_empty) {
      // A configured channel has no audio yet, so keep any chunk exposed on the other channel for the
      // next pass rather than sending an empty payload.
      this->handle_channel_stall_(available, available2);
      break;
    }

    // Both channels have audio exposed; clear any in-progress stall timer.
    this->audio_channel_stall_start_ = 0;

    api::VoiceAssistantAudio msg;
    // Zero-copy: send_message() copies the data out before we consume it.
    msg.data = this->audio_source_->data();
    msg.data_len = available;
    if (this->audio_source2_ != nullptr) {
      msg.data2 = this->audio_source2_->data();
      msg.data2_len = available2;
    }

    if (!this->api_client_->send_message(msg)) {
      // Retain the exposed packet, with a bounded retry rate. Dropping it here
      // loses phonemes and advances the sample timeline without sending them.
      const uint32_t now = millis();
      this->mic_retry_at_ms_ = now + 16;
      if (this->mic_blocked_since_ms_ == 0) this->mic_blocked_since_ms_ = now;
      if (now - this->mic_blocked_since_ms_ >= 2000) {
        ESP_LOGE(TAG, "Microphone upload blocked for 2 s; stopping instead of dropping speech");
        this->request_stop();
        // A stop packet can fail on the same full TCP buffer. Reconnect so
        // Core cannot keep the cancelled pipeline alive on this connection.
        if (this->api_client_ != nullptr) this->api_client_->on_fatal_error();
        this->defer([this]() { this->error_trigger_.trigger("microphone-backpressure", "Audio upload stalled"); });
      }
      return;
    }
    this->mic_retry_at_ms_ = 0;
    this->mic_blocked_since_ms_ = 0;
    if (this->barge_in_capture_gate_ && this->state_ == State::STREAMING_RESPONSE) {
      this->last_microphone_activity_ms_ = millis();  // Confirmed quiet speech also counts as activity.
      this->awaiting_provider_response_ = true;
    }

    if (this->barge_in_) {
      this->mic_diagnostic_bytes_ += available;
      this->mic_diagnostic_packets_++;
      for (size_t offset = 0; offset + sizeof(int16_t) <= available; offset += sizeof(int16_t)) {
        int16_t pcm;
        memcpy(&pcm, this->audio_source_->data() + offset, sizeof(pcm));
        const int32_t sample = pcm;
        const uint32_t magnitude = sample < 0 ? -sample : sample;
        this->mic_diagnostic_energy_ += static_cast<uint64_t>(magnitude) * magnitude;
        this->mic_diagnostic_peak_ = std::max(this->mic_diagnostic_peak_, magnitude);
        // Capture-gated sources supply confirmed speech through the condition
        // in loop(). A single boosted/noisy PCM peak must not keep them alive.
        if (!this->barge_in_capture_gate_ && magnitude >= 600) {
          this->last_microphone_activity_ms_ = millis();
        }
      }
    }
    this->audio_source_->consume(available);
    if (this->audio_source2_ != nullptr) {
      this->audio_source2_->consume(available2);
    }
    packets_sent++;
  }
}

void VoiceAssistant::handle_channel_stall_(size_t available, size_t available2) {
  // Called when at least one configured channel has no audio exposed. When one channel has data and the
  // other does not, watch how long the empty channel stays starved: Home Assistant has no stream timeout
  // and would never tell us to stop, so a channel that fails outright would otherwise hang streaming
  // forever with the live channel's chunk held. Stop the stream with an error after a prolonged imbalance.
  if ((available == 0) && (available2 == 0)) {
    // Both channels are idle (no audio buffered yet); normal, not a stalled channel.
    this->audio_channel_stall_start_ = 0;
    return;
  }

  const uint32_t now = App.get_loop_component_start_time();
  if (this->audio_channel_stall_start_ == 0) {
    this->audio_channel_stall_start_ = now;
  } else if ((now - this->audio_channel_stall_start_) >= AUDIO_CHANNEL_STALL_TIMEOUT_MS) {
    ESP_LOGW(TAG, "Mic channel %d stalled, stopping stream", (available == 0) ? 0 : 1);
    this->audio_channel_stall_start_ = 0;
    this->signal_stop_();
    this->set_state_(State::STOP_MICROPHONE, State::IDLE);
    this->defer([this]() {
      this->error_trigger_.trigger("mic-channel-stalled", "A microphone channel stopped producing audio");
    });
  }
}

void VoiceAssistant::loop() {
  if (this->barge_in_ && this->barge_in_capture_gate_ && this->is_running() &&
      this->barge_in_audio_condition_ && this->barge_in_audio_condition_()) {
    this->last_microphone_activity_ms_ = millis();
  }
  if (this->api_client_ == nullptr && this->state_ != State::IDLE && this->state_ != State::STOP_MICROPHONE &&
      this->state_ != State::STOPPING_MICROPHONE) {
    if (this->mic_source_->is_running() || (this->mic_source2_ && this->mic_source2_->is_running()) ||
        this->state_ == State::STARTING_MICROPHONE) {
      this->set_state_(State::STOP_MICROPHONE, State::IDLE);
    } else {
      this->set_state_(State::IDLE, State::IDLE);
    }
    this->continuous_ = false;
    this->signal_stop_();
    this->clear_buffers_();
    return;
  }
  switch (this->state_) {
    case State::IDLE: {
      if (this->continuous_ && this->desired_state_ == State::IDLE) {
        this->idle_trigger_.trigger();
        this->set_state_(State::START_MICROPHONE, State::START_PIPELINE);
      } else {
        this->deallocate_buffers_();
      }
      break;
    }
    case State::START_MICROPHONE: {
      ESP_LOGD(TAG, "Starting Microphone");
      if (!this->allocate_buffers_()) {
        this->status_set_error(LOG_STR("Failed to allocate buffers"));
        return;
      }
      if (this->status_has_error()) {
        this->status_clear_error();
      }
      this->clear_buffers_();

      this->mic_source_->start();
      if (this->mic_source2_) {
        this->mic_source2_->start();
      }
      this->set_state_(State::STARTING_MICROPHONE);
      break;
    }
    case State::STARTING_MICROPHONE: {
      if (this->mic_source_->is_running() && (!this->mic_source2_ || this->mic_source2_->is_running())) {
        this->set_state_(this->desired_state_);
      }
      break;
    }
    case State::START_PIPELINE: {
      ESP_LOGD(TAG, "Requesting start");
      this->pipeline_run_ended_ = false;
      this->speaker_stream_started_ = false;
      this->awaiting_provider_response_ = false;
      this->reset_audio_activity();
      uint32_t flags = 0;
      if (!this->continue_conversation_ && this->use_wake_word_)
        flags |= api::enums::VOICE_ASSISTANT_REQUEST_USE_WAKE_WORD;
      // Live providers negotiate requires_external_vad=false even on the
      // first request; conventional STT providers can still use Core's VAD.
      if (this->silence_detection_ && (!this->barge_in_ || !this->continue_conversation_))
        flags |= api::enums::VOICE_ASSISTANT_REQUEST_USE_VAD;
      api::VoiceAssistantAudioSettings audio_settings;
      audio_settings.noise_suppression_level = this->noise_suppression_level_;
      audio_settings.auto_gain = this->auto_gain_;
      audio_settings.volume_multiplier = this->volume_multiplier_;

      api::VoiceAssistantRequest msg;
      msg.start = true;
      msg.conversation_id = StringRef(this->conversation_id_);
      msg.flags = flags;
      msg.audio_settings = audio_settings;
      msg.wake_word_phrase = StringRef(this->wake_word_);

      // Reset media player state tracking
#ifdef USE_MEDIA_PLAYER
      if (this->media_player_ != nullptr) {
        this->media_player_response_state_ = MediaPlayerResponseState::IDLE;
      }
#endif

      if (this->api_client_ == nullptr || !this->api_client_->send_message(msg)) {
        ESP_LOGW(TAG, "Could not request start");
        this->error_trigger_.trigger("not-connected", "Could not request start");
        this->continuous_ = false;
        this->set_state_(State::STOP_MICROPHONE, State::IDLE);
        break;
      }
      this->set_state_(State::STARTING_PIPELINE);
      this->set_timeout("pipeline-start", 10000, [this]() {
        if (this->state_ != State::STARTING_PIPELINE) return;
        ESP_LOGE(TAG, "Pipeline start acknowledgement timed out; reconnecting API");
        this->request_stop();
        if (this->api_client_ != nullptr) this->api_client_->on_fatal_error();
        this->defer([this]() { this->error_trigger_.trigger("pipeline-start-timeout", "Pipeline did not start"); });
      });
      this->set_timeout("reset-conversation_id", this->conversation_timeout_,
                        [this]() { this->reset_conversation_id(); });
      break;
    }
    case State::STARTING_PIPELINE: {
      break;  // State changed when udp server port received
    }
    case State::STREAMING_MICROPHONE: {
      // pre_shift is ignored by RingBufferAudioSource (no intermediate transfer buffer to compact).
      if (this->audio_mode_ == AUDIO_MODE_API) {
        this->stream_api_audio_();
      } else {
        // UDP (will eventually be deprecated)
        // Only the primary microphone channel is used
        while (true) {
          this->audio_source_->fill(0, false);
          size_t available = this->audio_source_->available();
          if (available == 0) {
            break;
          }
          if (!this->udp_socket_running_) {
            if (!this->start_udp_socket_()) {
              this->set_state_(State::STOP_MICROPHONE, State::IDLE);
              break;
            }
          }
          this->socket_->sendto(this->audio_source_->data(), available, 0, (struct sockaddr *) &this->dest_addr_,
                                sizeof(this->dest_addr_));
          this->audio_source_->consume(available);
        }
      }  // audio mode
      break;
    }
    case State::STOP_MICROPHONE: {
      // Check both microphone channels
      bool is_running = this->mic_source_->is_running();
      bool is_running2 = false;
      if (this->mic_source2_) {
        is_running2 = this->mic_source2_->is_running();
      }
      if (is_running || is_running2) {
        if (is_running) {
          this->mic_source_->stop();
        }
        if (is_running2) {
          this->mic_source2_->stop();
        }
        this->set_state_(State::STOPPING_MICROPHONE);
      } else {
        this->set_state_(this->desired_state_);
      }
      break;
    }
    case State::STOPPING_MICROPHONE: {
      // Check both microphone channels
      bool is_stopped = this->mic_source_->is_stopped();
      bool is_stopped2 = true;
      if (this->mic_source2_) {
        is_stopped2 = this->mic_source2_->is_stopped();
      }
      if (is_stopped && is_stopped2) {
        this->set_state_(this->desired_state_);
      }
      break;
    }
    case State::AWAITING_RESPONSE: {
      break;  // State changed by events
    }
    case State::STREAMING_RESPONSE: {
      // The live-model STT provider consumes PCM for the complete pipeline,
      // including while response audio is being played.
      if (this->barge_in_ && this->audio_mode_ == AUDIO_MODE_API && this->mic_source_->is_running() &&
          (!this->mic_source2_ || this->mic_source2_->is_running())) {
        this->stream_api_audio_();
      }
      bool playing = false;
#ifdef USE_SPEAKER
      if (this->speaker_ != nullptr) {
        ssize_t received_len = 0;
        if (this->audio_mode_ == AUDIO_MODE_UDP) {
          if (this->speaker_buffer_index_ + RECEIVE_SIZE < this->speaker_buffer_capacity_) {
            received_len = this->socket_->read(this->speaker_buffer_ + this->speaker_buffer_index_, RECEIVE_SIZE);
            if (received_len > 0) {
              this->speaker_buffer_index_ += received_len;
              this->speaker_buffer_size_ += received_len;
              this->speaker_bytes_received_ += received_len;
            }
          } else {
            ESP_LOGD(TAG, "Receive buffer full");
          }
        }
        // Build a small buffer of audio before sending to the speaker
        bool end_of_stream = this->stream_ended_ && (this->audio_mode_ == AUDIO_MODE_API || received_len < 0);
        if (this->speaker_bytes_received_ > RECEIVE_SIZE * 4 || end_of_stream)
          this->write_speaker_();
        if (this->wait_for_stream_end_) {
          this->cancel_timeout("playing");
          if (end_of_stream) {
            ESP_LOGD(TAG, "End of audio stream received");
            this->cancel_timeout("speaker-timeout");
            this->set_state_(State::RESPONSE_FINISHED, State::RESPONSE_FINISHED);
          }
          break;  // We dont want to timeout here as the STREAM_END event will take care of that.
        }
        playing = this->speaker_->is_running();
      }
#endif
#ifdef USE_MEDIA_PLAYER
      if (this->media_player_ != nullptr) {
        playing = (this->media_player_response_state_ == MediaPlayerResponseState::PLAYING);

        if (this->media_player_response_state_ == MediaPlayerResponseState::FINISHED) {
          this->media_player_response_state_ = MediaPlayerResponseState::IDLE;
          this->cancel_timeout("playing");
          ESP_LOGD(TAG, "Announcement finished playing");
          this->set_state_(State::RESPONSE_FINISHED, State::RESPONSE_FINISHED);

          api::VoiceAssistantAnnounceFinished msg;
          msg.success = true;
          if (!this->api_client_->send_message(msg)) {
            API_LOG_MSG_DROPPED(TAG, "Announce-finished");
          }
          break;
        }
      }
#endif
      if (playing) {
        this->start_playback_timeout_();
      }
      break;
    }
    case State::RESPONSE_FINISHED: {
#ifdef USE_SPEAKER
      if (this->speaker_ != nullptr) {
        // Core has ended the stream: let its queued reply drain. A detector
        // confirmation alone cannot distinguish speech from residual echo and
        // must not truncate the reply. Retain microphone audio for the next
        // pipeline; active-stream interruptions remain driven by Core's flush.
        if (this->speaker_buffer_size_ > 0) {
          this->write_speaker_();
          break;
        }
        if (this->speaker_->has_buffered_data() || this->speaker_->is_running()) {
          break;
        }
        // Early PCM can finish before INTENT_END supplies the continue flag.
        // Wait for RUN_END before starting another pipeline on this API link.
        if (this->barge_in_ && !this->pipeline_run_ended_) break;
        ESP_LOGD(TAG, "Speaker has finished outputting all audio");
        this->speaker_->stop();
        this->cancel_timeout("speaker-timeout");
        this->cancel_timeout("playing");

        // A user may have started speaking while the last TTS buffers drained.
        // Preserve gated capture for the next pipeline in a live conversation.
        if (!(this->barge_in_capture_gate_ && this->continue_conversation_))
          this->clear_buffers_();
        this->speaker_buffer_size_ = this->speaker_buffer_index_ = this->speaker_bytes_received_ = 0;

        this->wait_for_stream_end_ = false;
        this->stream_ended_ = false;

        this->tts_stream_end_trigger_.trigger();
      }
#endif
      if (this->barge_in_) {
        if (this->continue_conversation_) {
          // RUN_END has closed the completed Home Assistant pipeline even though
          // the live conversation continues. Open its next pipeline with the
          // same conversation ID, without stopping the already-running mic.
          this->set_state_(State::START_PIPELINE, State::START_PIPELINE);
        } else {
          this->set_state_(State::STOP_MICROPHONE, State::IDLE);
        }
      } else {
        if (this->continue_conversation_) {
          this->set_state_(State::START_MICROPHONE, State::START_PIPELINE);
        } else {
          this->set_state_(State::IDLE, State::IDLE);
        }
      }
      break;
    }
    default:
      break;
  }
}

#ifdef USE_SPEAKER
void VoiceAssistant::write_speaker_() {
  if ((this->speaker_ != nullptr) && (this->speaker_buffer_ != nullptr)) {
    if (this->speaker_buffer_size_ > 0) {
      size_t write_chunk = std::min<size_t>(this->speaker_buffer_size_, 4 * 1024);
      size_t written = this->speaker_->play(this->speaker_buffer_, write_chunk);
      if (written > 0) {
        this->last_playback_activity_ms_ = millis();
        memmove(this->speaker_buffer_, this->speaker_buffer_ + written, this->speaker_buffer_size_ - written);
        this->speaker_buffer_size_ -= written;
        this->speaker_buffer_index_ -= written;
        this->set_timeout("speaker-timeout", 5000, [this]() { this->speaker_->stop(); });
      } else {
        ESP_LOGV(TAG, "Speaker buffer full, trying again next loop");
      }
    }
  }
}
#endif

void VoiceAssistant::client_subscription(api::APIConnection *client, bool subscribe) {
  if (!subscribe) {
    if (this->api_client_ == nullptr || client != this->api_client_) {
      ESP_LOGE(TAG, "Client attempting to unsubscribe that is not the current API Client");
      return;
    }
    this->api_client_ = nullptr;
    this->client_disconnected_trigger_.trigger();
    return;
  }

  if (this->api_client_ != nullptr) {
    char current_peername[socket::SOCKADDR_STR_LEN];
    char new_peername[socket::SOCKADDR_STR_LEN];
    ESP_LOGE(TAG,
             "Multiple API Clients attempting to connect to Voice Assistant\n"
             "  Current client: %s (%s)\n"
             "  New client: %s (%s)",
             this->api_client_->get_name(), this->api_client_->get_peername_to(current_peername), client->get_name(),
             client->get_peername_to(new_peername));
    return;
  }

  this->api_client_ = client;
  this->client_connected_trigger_.trigger();
}

static const LogString *voice_assistant_state_to_string(State state) {
  switch (state) {
    case State::IDLE:
      return LOG_STR("IDLE");
    case State::START_MICROPHONE:
      return LOG_STR("START_MICROPHONE");
    case State::STARTING_MICROPHONE:
      return LOG_STR("STARTING_MICROPHONE");
    case State::WAIT_FOR_VAD:
      return LOG_STR("WAIT_FOR_VAD");
    case State::WAITING_FOR_VAD:
      return LOG_STR("WAITING_FOR_VAD");
    case State::START_PIPELINE:
      return LOG_STR("START_PIPELINE");
    case State::STARTING_PIPELINE:
      return LOG_STR("STARTING_PIPELINE");
    case State::STREAMING_MICROPHONE:
      return LOG_STR("STREAMING_MICROPHONE");
    case State::STOP_MICROPHONE:
      return LOG_STR("STOP_MICROPHONE");
    case State::STOPPING_MICROPHONE:
      return LOG_STR("STOPPING_MICROPHONE");
    case State::AWAITING_RESPONSE:
      return LOG_STR("AWAITING_RESPONSE");
    case State::STREAMING_RESPONSE:
      return LOG_STR("STREAMING_RESPONSE");
    case State::RESPONSE_FINISHED:
      return LOG_STR("RESPONSE_FINISHED");
    default:
      return LOG_STR("UNKNOWN");
  }
};

void VoiceAssistant::set_state_(State state) {
  State old_state = this->state_;
  if (old_state == State::STARTING_PIPELINE && state != old_state)
    this->cancel_timeout("pipeline-start");
  this->state_ = state;
  if (this->barge_in_ && old_state != State::STREAMING_RESPONSE && state == State::STREAMING_RESPONSE) {
    this->barge_in_speech_active_until_ms_ = 0;
    this->barge_in_upload_gate_open_ = false;
  }
  ESP_LOGD(TAG, "State changed from %s to %s", LOG_STR_ARG(voice_assistant_state_to_string(old_state)),
           LOG_STR_ARG(voice_assistant_state_to_string(state)));
}

void VoiceAssistant::set_state_(State state, State desired_state) {
  this->set_state_(state);
  this->desired_state_ = desired_state;
  ESP_LOGD(TAG, "Desired state set to %s", LOG_STR_ARG(voice_assistant_state_to_string(desired_state)));
}

void VoiceAssistant::failed_to_start() {
  ESP_LOGE(TAG, "Failed to start server. See Home Assistant logs for more details.");
  this->error_trigger_.trigger("failed-to-start", "Failed to start server. See Home Assistant logs for more details.");
  this->set_state_(State::STOP_MICROPHONE, State::IDLE);
}

void VoiceAssistant::start_streaming() {
  if (this->state_ != State::STARTING_PIPELINE) {
    this->signal_stop_();
    return;
  }

  ESP_LOGD(TAG, "Client started, streaming microphone");
  this->audio_mode_ = AUDIO_MODE_API;

  // Both microphone channels
  if (this->mic_source_->is_running() && (!this->mic_source2_ || this->mic_source2_->is_running())) {
    this->set_state_(State::STREAMING_MICROPHONE, State::STREAMING_MICROPHONE);
  } else {
    this->set_state_(State::START_MICROPHONE, State::STREAMING_MICROPHONE);
  }
}

void VoiceAssistant::start_streaming(struct sockaddr_storage *addr, uint16_t port) {
  if (this->state_ != State::STARTING_PIPELINE) {
    this->signal_stop_();
    return;
  }

  ESP_LOGD(TAG, "Client started, streaming microphone");
  this->audio_mode_ = AUDIO_MODE_UDP;

  if (this->mic_source2_ != nullptr) {
    ESP_LOGW(TAG, "UDP audio mode does not support a second microphone channel; only the primary will be streamed");
  }

  memcpy(&this->dest_addr_, addr, sizeof(this->dest_addr_));
  if (this->dest_addr_.ss_family == AF_INET) {
    ((struct sockaddr_in *) &this->dest_addr_)->sin_port = htons(port);
  }
#if LWIP_IPV6
  else if (this->dest_addr_.ss_family == AF_INET6) {
    ((struct sockaddr_in6 *) &this->dest_addr_)->sin6_port = htons(port);
  }
#endif
  else {
    ESP_LOGW(TAG, "Unknown address family: %d", this->dest_addr_.ss_family);
    return;
  }

  // Only primary microphone channel over UDP
  if (this->mic_source_->is_running()) {
    this->set_state_(State::STREAMING_MICROPHONE, State::STREAMING_MICROPHONE);
  } else {
    this->set_state_(State::START_MICROPHONE, State::STREAMING_MICROPHONE);
  }
}

void VoiceAssistant::request_start(bool continuous, bool silence_detection) {
  if (this->api_client_ == nullptr) {
    ESP_LOGE(TAG, "No API client connected");
    this->set_state_(State::IDLE, State::IDLE);
    this->continuous_ = false;
    return;
  }
  if (this->state_ == State::IDLE) {
    this->continuous_ = continuous;
    this->silence_detection_ = silence_detection;
    // A new local wake/button request must not inherit the previous session's
    // expired watchdog while microphone startup and deferred on_start run.
    this->reset_audio_activity();
    this->set_state_(State::START_MICROPHONE, State::START_PIPELINE);
  }
}

void VoiceAssistant::request_stop() {
  this->continuous_ = false;
  this->continue_conversation_ = false;

  switch (this->state_) {
    case State::IDLE:
      break;
    case State::START_MICROPHONE:
    case State::STARTING_MICROPHONE:
    case State::WAIT_FOR_VAD:
    case State::WAITING_FOR_VAD:
    case State::START_PIPELINE:
      this->set_state_(State::STOP_MICROPHONE, State::IDLE);
      break;
    case State::STARTING_PIPELINE:
    case State::STREAMING_MICROPHONE:
      this->signal_stop_();
      this->set_state_(State::STOP_MICROPHONE, State::IDLE);
      break;
    case State::STOP_MICROPHONE:
    case State::STOPPING_MICROPHONE:
      this->desired_state_ = State::IDLE;
      break;
    case State::AWAITING_RESPONSE:
      this->signal_stop_();
      this->set_state_(State::STOP_MICROPHONE, State::IDLE);
      break;
    case State::STREAMING_RESPONSE:
      this->signal_stop_();
#ifdef USE_MEDIA_PLAYER
      // Stop any ongoing media player announcement
      if (this->media_player_ != nullptr) {
        this->media_player_->make_call()
            .set_command(media_player::MEDIA_PLAYER_COMMAND_STOP)
            .set_announcement(true)
            .perform();
      }
#endif
#ifdef USE_SPEAKER
      if (this->speaker_ != nullptr) {
        this->speaker_->stop();
        this->speaker_buffer_size_ = 0;
        this->speaker_buffer_index_ = 0;
        this->speaker_bytes_received_ = 0;
        this->wait_for_stream_end_ = false;
        this->stream_ended_ = false;
      }
#endif
      this->set_state_(State::STOP_MICROPHONE, State::IDLE);
      break;
    case State::RESPONSE_FINISHED:
      if (this->barge_in_ && !this->pipeline_run_ended_) this->signal_stop_();
#ifdef USE_SPEAKER
      if (this->speaker_ != nullptr) {
        this->speaker_->stop();
        this->speaker_buffer_size_ = 0;
        this->speaker_buffer_index_ = 0;
        this->speaker_bytes_received_ = 0;
        this->wait_for_stream_end_ = false;
        this->stream_ended_ = false;
      }
#endif
      this->set_state_(State::STOP_MICROPHONE, State::IDLE);
      break;
  }
}

void VoiceAssistant::signal_stop_() {
  memset(&this->dest_addr_, 0, sizeof(this->dest_addr_));
  if (this->api_client_ == nullptr) {
    return;
  }
  ESP_LOGD(TAG, "Signaling stop");
  api::VoiceAssistantRequest msg;
  msg.start = false;
  if (!this->api_client_->send_message(msg)) {
    API_LOG_MSG_DROPPED(TAG, "Stop request");
    this->api_client_->on_fatal_error();
  }
}

void VoiceAssistant::start_playback_timeout_() {
  this->set_timeout("playing", 2000, [this]() {
    this->cancel_timeout("speaker-timeout");
    this->set_state_(State::RESPONSE_FINISHED, State::RESPONSE_FINISHED);

    if (this->api_client_ == nullptr)
      return;
    api::VoiceAssistantAnnounceFinished msg;
    msg.success = true;
    if (!this->api_client_->send_message(msg)) {
      API_LOG_MSG_DROPPED(TAG, "Announce-finished");
    }
  });
}

void VoiceAssistant::on_event(const api::VoiceAssistantEventResponse &msg) {
  ESP_LOGD(TAG, "Event Type: %" PRId32, msg.event_type);
  if (this->barge_in_ &&
      (this->state_ == State::IDLE || this->state_ == State::START_MICROPHONE ||
       this->state_ == State::STARTING_MICROPHONE || this->state_ == State::START_PIPELINE ||
       this->state_ == State::STARTING_PIPELINE || this->state_ == State::STOP_MICROPHONE ||
       this->state_ == State::STOPPING_MICROPHONE) &&
      (msg.event_type == api::enums::VOICE_ASSISTANT_TTS_START ||
       msg.event_type == api::enums::VOICE_ASSISTANT_TTS_END ||
       msg.event_type == api::enums::VOICE_ASSISTANT_TTS_STREAM_START ||
       msg.event_type == api::enums::VOICE_ASSISTANT_TTS_STREAM_END)) {
    ESP_LOGD(TAG, "Ignoring TTS event outside an active pipeline");
    return;
  }
  switch (msg.event_type) {
    case api::enums::VOICE_ASSISTANT_RUN_START:
      ESP_LOGD(TAG, "Assist Pipeline running");
#ifdef USE_MEDIA_PLAYER
      this->started_streaming_tts_ = false;
      for (const auto &arg : msg.data) {
        if (arg.name == "url") {
          this->tts_response_url_ = arg.value;
        }
      }
#endif
      this->defer([this]() { this->start_trigger_.trigger(); });
      break;
    case api::enums::VOICE_ASSISTANT_WAKE_WORD_START:
      break;
    case api::enums::VOICE_ASSISTANT_WAKE_WORD_END: {
      ESP_LOGD(TAG, "Wake word detected");
      this->defer([this]() { this->wake_word_detected_trigger_.trigger(); });
      break;
    }
    case api::enums::VOICE_ASSISTANT_STT_START:
      ESP_LOGD(TAG, "STT started");
      this->defer([this]() { this->listening_trigger_.trigger(); });
      break;
    case api::enums::VOICE_ASSISTANT_STT_END: {
      std::string text;
      for (const auto &arg : msg.data) {
        if (arg.name == "text") {
          text = arg.value;
        }
      }
      if (text.empty()) {
        ESP_LOGW(TAG, "No text in STT_END event");
        return;
      } else if (text.length() > 500) {
        text.resize(497);
        text += "...";
      }
      ESP_LOGD(TAG, "Speech recognised as: \"%s\"", text.c_str());
      this->defer([this, text]() { this->stt_end_trigger_.trigger(text); });
      break;
    }
    case api::enums::VOICE_ASSISTANT_INTENT_START:
      ESP_LOGD(TAG, "Intent started");
      this->defer([this]() { this->intent_start_trigger_.trigger(); });
      break;
    case api::enums::VOICE_ASSISTANT_INTENT_PROGRESS: {
      ESP_LOGD(TAG, "Intent progress");
      std::string tts_url_for_trigger;
#ifdef USE_MEDIA_PLAYER
      if (this->media_player_ != nullptr) {
        for (const auto &arg : msg.data) {
          if ((arg.name == "tts_start_streaming") && (arg.value == "1") && !this->tts_response_url_.empty()) {
            this->media_player_response_state_ = MediaPlayerResponseState::URL_SENT;

            this->media_player_->make_call().set_media_url(this->tts_response_url_).set_announcement(true).perform();

            this->started_streaming_tts_ = true;
            this->start_playback_timeout_();

            tts_url_for_trigger = this->tts_response_url_;
            this->tts_response_url_.clear();  // Reset streaming URL
            this->set_state_(State::STREAMING_RESPONSE, State::STREAMING_RESPONSE);
          }
        }
      }
#endif
      this->defer([this, tts_url_for_trigger]() { this->intent_progress_trigger_.trigger(tts_url_for_trigger); });
      break;
    }
    case api::enums::VOICE_ASSISTANT_INTENT_END: {
      for (const auto &arg : msg.data) {
        if (arg.name == "conversation_id") {
          this->conversation_id_ = arg.value;
        } else if (arg.name == "continue_conversation") {
          this->continue_conversation_ = (arg.value == "1");
        }
      }
      this->defer([this]() { this->intent_end_trigger_.trigger(); });
      break;
    }
    case api::enums::VOICE_ASSISTANT_TTS_START: {
      std::string text;
      for (const auto &arg : msg.data) {
        if (arg.name == "text") {
          text = arg.value;
        }
      }
      if (text.empty()) {
        ESP_LOGW(TAG, "No text in TTS_START event");
        return;
      }
      if (text.length() > 500) {
        text.resize(497);
        text += "...";
      }
      ESP_LOGD(TAG, "Response: \"%s\"", text.c_str());
      this->defer([this, text]() {
        this->tts_start_trigger_.trigger(text);
#ifdef USE_SPEAKER
        if (this->speaker_ != nullptr && !(this->barge_in_ && this->speaker_stream_started_)) {
          this->speaker_->start();
        }
#endif
      });
      break;
    }
    case api::enums::VOICE_ASSISTANT_TTS_END: {
      std::string url;
      for (const auto &arg : msg.data) {
        if (arg.name == "url") {
          url = arg.value;
        }
      }
      if (url.empty()) {
        ESP_LOGW(TAG, "No url in TTS_END event");
        return;
      }
      ESP_LOGD(TAG, "Response URL: \"%s\"", url.c_str());
      this->defer([this, url]() {
#ifdef USE_MEDIA_PLAYER
        if ((this->media_player_ != nullptr) && (!this->started_streaming_tts_)) {
          this->media_player_response_state_ = MediaPlayerResponseState::URL_SENT;

          this->media_player_->make_call().set_media_url(url).set_announcement(true).perform();

          this->start_playback_timeout_();
        }
        this->started_streaming_tts_ = false;  // Helps indicate reaching the TTS_END stage
#endif
        this->tts_end_trigger_.trigger(url);
      });
      State new_state = this->local_output_ ? State::STREAMING_RESPONSE : State::IDLE;
      if (this->barge_in_ && this->speaker_stream_started_)
        break;  // PCM events, not late TTS metadata, own the speaker state.
      if (new_state != this->state_) {
        // Don't needlessly change the state. The intent progress stage may have already changed the state to
        // streaming response.
        this->set_state_(new_state, new_state);
      }
      break;
    }
    case api::enums::VOICE_ASSISTANT_RUN_END: {
      ESP_LOGD(TAG, "Assist Pipeline ended");
      this->pipeline_run_ended_ = true;
      if ((this->state_ == State::START_PIPELINE) || (this->state_ == State::STARTING_PIPELINE) ||
          (this->state_ == State::STREAMING_MICROPHONE)) {
        // Microphone is running, stop it
        this->set_state_(State::STOP_MICROPHONE, State::IDLE);
      } else if (this->state_ == State::AWAITING_RESPONSE) {
        // No TTS start event ("nevermind")
        this->set_state_(State::IDLE, State::IDLE);
      }
      this->defer([this]() { this->end_trigger_.trigger(); });
      break;
    }
    case api::enums::VOICE_ASSISTANT_ERROR: {
      std::string code;
      std::string message;
      for (const auto &arg : msg.data) {
        if (arg.name == "code") {
          code = arg.value;
        } else if (arg.name == "message") {
          message = arg.value;
        }
      }
      if (code == "wake-word-timeout" || code == "wake_word_detection_aborted" || code == "no_wake_word") {
        // Don't change state here since either the "tts-end" or "run-end" events will do it.
        return;
      } else if (code == "wake-provider-missing" || code == "wake-engine-missing") {
        // Wake word is not set up or not ready on Home Assistant so stop and do not retry until user starts again.
        this->defer([this, code, message]() {
          this->request_stop();
          this->error_trigger_.trigger(code, message);
        });
        return;
      }
      ESP_LOGE(TAG, "Error: %s - %s", code.c_str(), message.c_str());
      if (this->state_ != State::IDLE) {
        this->signal_stop_();
        this->set_state_(State::STOP_MICROPHONE, State::IDLE);
      }
      this->defer([this, code, message]() { this->error_trigger_.trigger(code, message); });
      break;
    }
    case api::enums::VOICE_ASSISTANT_TTS_STREAM_START: {
#ifdef USE_SPEAKER
      if (this->speaker_ != nullptr) {
        bool flush = false;
        for (const auto &arg : msg.data) {
          if (arg.name == "flush" && arg.value == "1") {
            flush = true;
            break;
          }
        }
        if (flush) {
          // This is an in-place interruption boundary, not a new response.
          // stop() synchronously purges the aec_speexdsp playback and AEC
          // reference queues; clear the voice-assistant staging buffer here
          // too, before any later API audio packet can be consumed.
          ESP_LOGD(TAG, "TTS stream flush; discarding interrupted playback");
          this->speaker_->stop();
          this->speaker_buffer_size_ = 0;
          this->speaker_buffer_index_ = 0;
          this->speaker_bytes_received_ = 0;
          this->stream_ended_ = false;
          this->cancel_timeout("speaker-timeout");
          this->cancel_timeout("playing");
          // A flush cancels playback, not microphone capture. The user may
          // still be in the middle of the interruption utterance.
          // A flush means the provider accepted an interruption and may need
          // time to generate replacement audio. Do not let the full-duplex
          // inactivity watchdog expire from the old playback timestamp while
          // that replacement is being prepared.
          this->reset_audio_activity();
          if (this->barge_in_ && this->state_ == State::RESPONSE_FINISHED)
            this->set_state_(State::STREAMING_RESPONSE, State::STREAMING_RESPONSE);
        }
        this->wait_for_stream_end_ = true;
        if (this->barge_in_) {
          this->speaker_stream_started_ = true;
          this->awaiting_provider_response_ = true;
          this->stream_ended_ = false;
          this->set_state_(State::STREAMING_RESPONSE, State::STREAMING_RESPONSE);
        }
        ESP_LOGD(TAG, flush ? "TTS stream restart after flush" : "TTS stream start");
        this->defer([this] { this->tts_stream_start_trigger_.trigger(); });
      }
#endif
      break;
    }
    case api::enums::VOICE_ASSISTANT_TTS_STREAM_END: {
#ifdef USE_SPEAKER
      if (this->speaker_ != nullptr) {
        this->stream_ended_ = true;
        ESP_LOGD(TAG, "TTS stream end");
      }
#endif
      break;
    }
    case api::enums::VOICE_ASSISTANT_STT_VAD_START:
      ESP_LOGD(TAG, "Starting STT by VAD");
      this->defer([this]() { this->stt_vad_start_trigger_.trigger(); });
      break;
    case api::enums::VOICE_ASSISTANT_STT_VAD_END:
      ESP_LOGD(TAG, "STT by VAD end");
      if (!this->barge_in_)
        this->set_state_(State::STOP_MICROPHONE, State::AWAITING_RESPONSE);
      this->defer([this]() { this->stt_vad_end_trigger_.trigger(); });
      break;
    default:
      ESP_LOGD(TAG, "Unhandled event type: %" PRId32, msg.event_type);
      break;
  }
}

void VoiceAssistant::on_audio(const api::VoiceAssistantAudio &msg) {
#ifdef USE_SPEAKER  // We should never get to this function if there is no speaker anyway
  if (this->barge_in_ && this->state_ != State::STREAMING_RESPONSE && this->state_ != State::RESPONSE_FINISHED) {
    ESP_LOGD(TAG, "Ignoring response audio outside playback state");
    return;
  }
  if ((this->speaker_ != nullptr) && (this->speaker_buffer_ != nullptr)) {
    if (this->barge_in_) {
      for (size_t offset = 0; offset + sizeof(int16_t) <= msg.data_len; offset += sizeof(int16_t)) {
        int16_t pcm;
        memcpy(&pcm, msg.data + offset, sizeof(pcm));
        const int32_t sample = pcm;
        const uint32_t magnitude = sample < 0 ? -sample : sample;
        if (magnitude >= 128) {
          this->last_playback_activity_ms_ = millis();
          break;
        }
      }
    }
    if (this->speaker_buffer_index_ + msg.data_len <= this->speaker_buffer_capacity_) {
      memcpy(this->speaker_buffer_ + this->speaker_buffer_index_, msg.data, msg.data_len);
      this->speaker_buffer_index_ += msg.data_len;
      this->speaker_buffer_size_ += msg.data_len;
      this->speaker_bytes_received_ += msg.data_len;
      this->write_speaker_();
      ESP_LOGV(TAG, "Received audio: %u bytes from API", msg.data_len);
    } else {
      ESP_LOGE(TAG, "Cannot receive audio, buffer is full");
    }
  }
#endif
}

void VoiceAssistant::on_timer_event(const api::VoiceAssistantTimerEventResponse &msg) {
  // Find existing timer or add a new one
  auto it = this->timers_.begin();
  for (; it != this->timers_.end(); ++it) {
    if (it->id == msg.timer_id)
      break;
  }
  if (it == this->timers_.end()) {
    this->timers_.push_back({});
    it = this->timers_.end() - 1;
  }
  it->id = msg.timer_id;
  it->name = msg.name;
  it->total_seconds = msg.total_seconds;
  it->seconds_left = msg.seconds_left;
  it->is_active = msg.is_active;

  char timer_buf[Timer::TO_STR_BUFFER_SIZE];
  ESP_LOGD(TAG,
           "Timer Event\n"
           "  Type: %" PRId32 "\n"
           "  %s",
           msg.event_type, it->to_str(timer_buf));

  switch (msg.event_type) {
    case api::enums::VOICE_ASSISTANT_TIMER_STARTED:
      this->timer_started_trigger_.trigger(*it);
      break;
    case api::enums::VOICE_ASSISTANT_TIMER_UPDATED:
      this->timer_updated_trigger_.trigger(*it);
      break;
    case api::enums::VOICE_ASSISTANT_TIMER_CANCELLED:
      this->timer_cancelled_trigger_.trigger(*it);
      this->timers_.erase(it);
      break;
    case api::enums::VOICE_ASSISTANT_TIMER_FINISHED:
      this->timer_finished_trigger_.trigger(*it);
      this->timers_.erase(it);
      break;
  }

  if (this->timers_.empty()) {
    this->cancel_interval("timer-event");
    this->timer_tick_running_ = false;
  } else if (!this->timer_tick_running_) {
    this->set_interval("timer-event", 1000, [this]() { this->timer_tick_(); });
    this->timer_tick_running_ = true;
  }
}

void VoiceAssistant::timer_tick_() {
  for (auto &timer : this->timers_) {
    if (timer.is_active && timer.seconds_left > 0) {
      timer.seconds_left--;
    }
  }
  this->timer_tick_trigger_.trigger(this->timers_);
}

void VoiceAssistant::on_announce(const api::VoiceAssistantAnnounceRequest &msg) {
#ifdef USE_MEDIA_PLAYER
  if (this->media_player_ != nullptr) {
    this->tts_start_trigger_.trigger(msg.text);

    this->media_player_response_state_ = MediaPlayerResponseState::URL_SENT;

    if (!msg.preannounce_media_id.empty()) {
      this->media_player_->make_call().set_media_url(msg.preannounce_media_id).set_announcement(true).perform();
    }
    // Enqueueing a URL with an empty playlist will still play the file immediately
    this->media_player_->make_call()
        .set_command(media_player::MEDIA_PLAYER_COMMAND_ENQUEUE)
        .set_media_url(msg.media_id)
        .set_announcement(true)
        .perform();
    this->continue_conversation_ = msg.start_conversation;

    this->start_playback_timeout_();

    if (this->continuous_) {
      this->set_state_(State::STOP_MICROPHONE, State::STREAMING_RESPONSE);
    } else {
      this->set_state_(State::STREAMING_RESPONSE, State::STREAMING_RESPONSE);
    }

    this->tts_end_trigger_.trigger(msg.media_id);
    this->end_trigger_.trigger();
  }
#endif
}

void VoiceAssistant::on_set_configuration(const std::vector<std::string> &active_wake_words) {
#ifdef USE_MICRO_WAKE_WORD
  if (this->micro_wake_word_) {
    // Disable all wake words first
    for (auto &model : this->micro_wake_word_->get_wake_words()) {
      model->disable();
    }

    // Enable only active wake words
    for (const auto &ww_id : active_wake_words) {
      for (auto &model : this->micro_wake_word_->get_wake_words()) {
        if (model->get_id() == ww_id) {
          model->enable();
          ESP_LOGD(TAG, "Enabled wake word: %s (id=%s)", model->get_wake_word().c_str(), model->get_id().c_str());
        }
      }
    }
  }
#endif
};

const Configuration &VoiceAssistant::get_configuration() {
  this->config_.available_wake_words.clear();
  this->config_.active_wake_words.clear();

#ifdef USE_MICRO_WAKE_WORD
  if (this->micro_wake_word_) {
    this->config_.max_active_wake_words = 1;

    for (auto &model : this->micro_wake_word_->get_wake_words()) {
      if (model->is_enabled()) {
        this->config_.active_wake_words.push_back(model->get_id());
      }

      WakeWord wake_word;
      wake_word.id = model->get_id();
      wake_word.wake_word = model->get_wake_word();
      for (const auto &lang : model->get_trained_languages()) {
        wake_word.trained_languages.push_back(lang);
      }
      this->config_.available_wake_words.push_back(std::move(wake_word));
    }
  } else {
#endif
    // No microWakeWord
    this->config_.max_active_wake_words = 0;
#ifdef USE_MICRO_WAKE_WORD
  }
#endif

  return this->config_;
};

VoiceAssistant *global_voice_assistant = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace esphome::voice_assistant

#endif  // USE_VOICE_ASSISTANT
