#include "afe_audio_engine.h"

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <sstream>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_vadn_models.h>

#include "audio_service.h"
#include "wake_words/custom_wake_word.h"

#define TAG "AfeAudioEngine"

#if CONFIG_USE_AUDIO_PROCESSOR
static constexpr bool kUseAfeForVoiceProcessing = true;
#else
static constexpr bool kUseAfeForVoiceProcessing = false;
#endif

AfeAudioEngine::AfeAudioEngine() {
    event_group_ = xEventGroupCreate();
}

AfeAudioEngine::~AfeAudioEngine() {
    custom_wake_word_.reset();
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }
    if (wake_word_encode_task_stack_ != nullptr) {
        heap_caps_free(wake_word_encode_task_stack_);
    }
    if (wake_word_encode_task_buffer_ != nullptr) {
        heap_caps_free(wake_word_encode_task_buffer_);
    }
    if (owns_models_ && models_ != nullptr) {
        esp_srmodel_deinit(models_);
    }
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
}

bool AfeAudioEngine::Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    if (afe_data_ != nullptr || (codec_ != nullptr && !kUseAfeForVoiceProcessing && wake_detector_ == WakeDetector::kNone)) {
        return true;
    }
    if (codec == nullptr) {
        ESP_LOGE(TAG, "Codec is null");
        return false;
    }

    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;
    output_buffer_.reserve(frame_samples_);

    if (models_list == nullptr) {
        models_ = esp_srmodel_init("model");
        owns_models_ = models_ != nullptr;
    } else {
        models_ = models_list;
    }

    char* wakenet_model_name = nullptr;
    char* multinet_model_name = nullptr;
    if (models_ != nullptr && models_->num > 0) {
        wakenet_model_name = esp_srmodel_filter(models_, ESP_WN_PREFIX, nullptr);
        multinet_model_name = esp_srmodel_filter(models_, ESP_MN_PREFIX, nullptr);
        for (int i = 0; i < models_->num; ++i) {
            ESP_LOGI(TAG, "Model %d: %s", i, models_->model_name[i]);
        }
    }

    if (multinet_model_name != nullptr) {
        wake_detector_ = WakeDetector::kMultiNet;
        custom_wake_word_ = std::make_unique<CustomWakeWord>();
        custom_wake_word_->OnWakeWordDetected([this](const std::string& wake_word) {
            last_detected_wake_word_ = wake_word;
            xEventGroupClearBits(event_group_, kWakeWordEnabled);
            UpdateActiveState();
            if (wake_word_detected_callback_) {
                wake_word_detected_callback_(wake_word);
            }
        });
        if (!custom_wake_word_->Initialize(codec_, models_)) {
            ESP_LOGE(TAG, "Failed to initialize MultiNet wake-word detector");
            custom_wake_word_.reset();
            wake_detector_ = WakeDetector::kNone;
            return false;
        }
    } else if (wakenet_model_name != nullptr) {
        wake_detector_ = WakeDetector::kWakeNet;
        auto words = esp_srmodel_get_wake_words(models_, wakenet_model_name);
        if (words != nullptr) {
            std::stringstream stream(words);
            std::string word;
            while (std::getline(stream, word, ';')) {
                wake_words_.push_back(word);
            }
        }
#if CONFIG_SEND_WAKE_WORD_DATA
        if (!wake_word_audio_cache_.Initialize(16000 * 2)) {
            ESP_LOGW(TAG, "Wake-word audio upload disabled: PSRAM cache allocation failed");
        }
#endif
    }

    const bool needs_afe = kUseAfeForVoiceProcessing || wake_detector_ != WakeDetector::kNone;
    if (!needs_afe) {
        ESP_LOGI(TAG, "Initialized as raw engine because AFE features are disabled");
        return true;
    }

    int ref_num = codec_->input_reference() ? 1 : 0;
    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - ref_num; ++i) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; ++i) {
        input_format.push_back('R');
    }

    char* vad_model_name = models_ == nullptr
        ? nullptr
        : esp_srmodel_filter(models_, ESP_VADN_PREFIX, nullptr);
    afe_config_t* afe_config = afe_config_init(
        input_format.c_str(), models_, AFE_TYPE_FD, AFE_MODE_LOW_COST);
    if (afe_config == nullptr) {
        ESP_LOGE(TAG, "Failed to create AFE configuration");
        return false;
    }

    afe_config->aec_init = codec_->input_reference();
    afe_config->aec_mode = AEC_MODE_FD_HIGH_PERF;
    afe_config->aec_nlp_level = AEC_NLP_LEVEL_VERYAGGR;
    afe_config->ns_init = false;
    afe_config->vad_init = kUseAfeForVoiceProcessing;
    afe_config->vad_mode = VAD_MODE_0;
    afe_config->vad_min_noise_ms = 100;
    if (vad_model_name != nullptr) {
        afe_config->vad_model_name = vad_model_name;
    }
    afe_config->wakenet_init = wake_detector_ == WakeDetector::kWakeNet;
    afe_config->wakenet_model_name = wake_detector_ == WakeDetector::kWakeNet
        ? wakenet_model_name
        : nullptr;
    afe_config->agc_init = false;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    if (afe_iface_ != nullptr) {
        afe_data_ = afe_iface_->create_from_config(afe_config);
    }
    afe_config_free(afe_config);

    if (afe_iface_ == nullptr || afe_data_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create FD AFE instance");
        afe_iface_ = nullptr;
        afe_data_ = nullptr;
        return false;
    }

    if (wake_detector_ == WakeDetector::kWakeNet) {
        afe_iface_->disable_wakenet(afe_data_);
    }
    if (codec_->input_reference()) {
        afe_iface_->disable_aec(afe_data_);
    }
    afe_iface_->print_pipeline(afe_data_);

    BaseType_t task_created = xTaskCreate([](void* arg) {
        auto* engine = static_cast<AfeAudioEngine*>(arg);
        engine->ProcessingTask();
        vTaskDelete(nullptr);
    }, "audio_afe", 4096, this, 3, &processing_task_);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create AFE processing task");
        afe_iface_->destroy(afe_data_);
        afe_data_ = nullptr;
        afe_iface_ = nullptr;
        return false;
    }

    const char* detector = wake_detector_ == WakeDetector::kWakeNet
        ? "WakeNet"
        : (wake_detector_ == WakeDetector::kMultiNet ? "MultiNet" : "none");
    ESP_LOGI(TAG, "Initialized FD AFE, detector: %s, NS: off, feed: %d, fetch: %d",
        detector, afe_iface_->get_feed_chunksize(afe_data_), afe_iface_->get_fetch_chunksize(afe_data_));
    return true;
}

void AfeAudioEngine::Feed(std::vector<int16_t>&& data) {
    EventBits_t bits = xEventGroupGetBits(event_group_);
    if ((bits & kVoiceProcessingEnabled) && !kUseAfeForVoiceProcessing) {
        OutputRawAudio(data);
    }
    if (afe_data_ == nullptr || (bits & kAfeActive) == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    if ((xEventGroupGetBits(event_group_) & kAfeActive) == 0) {
        return;
    }
    input_buffer_.insert(input_buffer_.end(), data.begin(), data.end());
    size_t chunk_size = afe_iface_->get_feed_chunksize(afe_data_) * codec_->input_channels();
    while (input_buffer_.size() >= chunk_size) {
        afe_iface_->feed(afe_data_, input_buffer_.data());
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + chunk_size);
    }
}

void AfeAudioEngine::EnableWakeWordDetection(bool enable) {
    if (!HasWakeWord()) {
        return;
    }

    // WakeNet enable/disable on the AFE instance is applied by ProcessingTask
    // (see ApplyAfeControls), driven by the kWakeWordEnabled bit.
    if (enable) {
        if (wake_detector_ == WakeDetector::kMultiNet) {
            custom_wake_word_->Start();
        }
        xEventGroupSetBits(event_group_, kWakeWordEnabled);
    } else {
        xEventGroupClearBits(event_group_, kWakeWordEnabled);
        if (wake_detector_ == WakeDetector::kMultiNet) {
            custom_wake_word_->Stop();
        }
    }
    UpdateActiveState();
}

void AfeAudioEngine::EnableVoiceProcessing(bool enable) {
    if (enable) {
        xEventGroupSetBits(event_group_, kVoiceProcessingEnabled);
    } else {
        xEventGroupClearBits(event_group_, kVoiceProcessingEnabled);
        is_speaking_ = false;
    }
    UpdateActiveState();
}

void AfeAudioEngine::EnableDeviceAec(bool enable) {
    device_aec_enabled_.store(enable);
    if (enable && (codec_ == nullptr || !codec_->input_reference())) {
        ESP_LOGW(TAG, "Device AEC requires a playback reference channel");
    }
    UpdateAecState();
}

void AfeAudioEngine::EnableBargeInDetection(bool enable) {
    const bool changed = barge_in_enabled_.exchange(enable) != enable;
    barge_in_reset_pending_.store(true);
    if (changed) {
        ESP_LOGI("BargeIn", "%s near-end speech detection", enable ? "Enabled" : "Disabled");
    }
}

bool AfeAudioEngine::HasWakeWord() const {
    return wake_detector_ != WakeDetector::kNone;
}

bool AfeAudioEngine::IsWakeWordDetectionEnabled() const {
    return event_group_ != nullptr && (xEventGroupGetBits(event_group_) & kWakeWordEnabled) != 0;
}

bool AfeAudioEngine::IsVoiceProcessingEnabled() const {
    return event_group_ != nullptr && (xEventGroupGetBits(event_group_) & kVoiceProcessingEnabled) != 0;
}

size_t AfeAudioEngine::GetFeedSize() const {
    return afe_data_ == nullptr ? frame_samples_ : afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeAudioEngine::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = std::move(callback);
}

void AfeAudioEngine::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = std::move(callback);
}

void AfeAudioEngine::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = std::move(callback);
}

void AfeAudioEngine::OnBargeInDetected(std::function<void()> callback) {
    barge_in_detected_callback_ = std::move(callback);
}

void AfeAudioEngine::UpdateActiveState() {
    EventBits_t bits = xEventGroupGetBits(event_group_);
    const bool afe_active = afe_data_ != nullptr &&
        ((bits & kWakeWordEnabled) ||
         (kUseAfeForVoiceProcessing && (bits & kVoiceProcessingEnabled)));
    if (afe_active) {
        xEventGroupSetBits(event_group_, kAfeActive);
    } else {
        xEventGroupClearBits(event_group_, kAfeActive);
        control_generation_.fetch_add(1);
        std::lock_guard<std::mutex> lock(input_buffer_mutex_);
        input_buffer_.clear();
        if (afe_data_ != nullptr) {
            // Don't call reset_buffer() here: this runs in the main task while
            // ProcessingTask may be inside fetch_with_delay() on the same AFE
            // instance, and a concurrent reset corrupts the ring buffer state.
            // Defer the reset to ProcessingTask, which owns the fetch side.
            reset_pending_ = true;
        }
    }
    if ((bits & kVoiceProcessingEnabled) == 0) {
        // output_buffer_ is owned by the task that produces output frames,
        // so let that task clear it instead of racing with it here.
        output_reset_pending_ = true;
    }
    afe_control_dirty_ = true;
}

void AfeAudioEngine::UpdateAecState() {
    if (afe_data_ == nullptr || codec_ == nullptr || !codec_->input_reference()) {
        return;
    }
    afe_control_dirty_ = true;
}

void AfeAudioEngine::ApplyAfeControls() {
    EventBits_t bits = xEventGroupGetBits(event_group_);
    if (wake_detector_ == WakeDetector::kWakeNet) {
        if (bits & kWakeWordEnabled) {
            afe_iface_->enable_wakenet(afe_data_);
        } else {
            afe_iface_->disable_wakenet(afe_data_);
        }
    }
    if (codec_->input_reference()) {
        const bool enable_aec = (bits & kWakeWordEnabled) ||
            (device_aec_enabled_.load() && (bits & kVoiceProcessingEnabled));
        if (enable_aec) {
            afe_iface_->enable_aec(afe_data_);
        } else {
            afe_iface_->disable_aec(afe_data_);
        }
    }
}

void AfeAudioEngine::ApplyPendingReset() {
    if (!reset_pending_.exchange(false)) {
        return;
    }
    // Discard audio recorded before (re)activation. Holding input_buffer_mutex_
    // serializes the reset against Feed(); fetch/reset both run in this task.
    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    input_buffer_.clear();
    afe_iface_->reset_buffer(afe_data_);
}

void AfeAudioEngine::ProcessingTask() {
    while (true) {
        xEventGroupWaitBits(event_group_, kAfeActive, pdFALSE, pdTRUE, portMAX_DELAY);
        ApplyPendingReset();
        if ((xEventGroupGetBits(event_group_) & kAfeActive) == 0) {
            continue;
        }
        if (afe_control_dirty_.exchange(false)) {
            // WakeNet/AEC toggles are not safe against a concurrent fetch,
            // so they are applied here, in the task that owns the fetch side.
            ApplyAfeControls();
        }
        const uint32_t generation = control_generation_.load();
        auto* result = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
        if (generation != control_generation_.load() ||
            (xEventGroupGetBits(event_group_) & kAfeActive) == 0) {
            // A disable/re-enable may make an old blocked fetch return after the
            // AFE is active again. Reset immediately and never process that frame.
            ApplyPendingReset();
            continue;
        }
        if (result == nullptr || result->ret_value == ESP_FAIL) {
            if (result != nullptr) {
                ESP_LOGW(TAG, "AFE fetch failed: %d", result->ret_value);
            }
            continue;
        }

        EventBits_t bits = xEventGroupGetBits(event_group_);
        if (bits & kWakeWordEnabled) {
            HandleWakeWordResult(result);
        }
        if (kUseAfeForVoiceProcessing && (bits & kVoiceProcessingEnabled)) {
            HandleVoiceResult(result);
        }
    }
}

void AfeAudioEngine::HandleWakeWordResult(const afe_fetch_result_t* result) {
    if (wake_detector_ == WakeDetector::kMultiNet) {
        custom_wake_word_->FeedMono(
            result->data, result->data_size / sizeof(int16_t));
        return;
    }

#if CONFIG_SEND_WAKE_WORD_DATA
    wake_word_audio_cache_.Store(result->data, result->data_size / sizeof(int16_t));
#endif
    if (result->wakeup_state != WAKENET_DETECTED) {
        return;
    }

    int model_index = result->wakenet_model_index - 1;
    if (model_index < 0 || model_index >= static_cast<int>(wake_words_.size())) {
        ESP_LOGE(TAG, "Invalid WakeNet model index: %d", result->wakenet_model_index);
        return;
    }

    last_detected_wake_word_ = wake_words_[model_index];
    xEventGroupClearBits(event_group_, kWakeWordEnabled);
    // UpdateActiveState marks the AFE controls dirty; the next loop iteration
    // of ProcessingTask disables WakeNet via ApplyAfeControls.
    UpdateActiveState();
    if (wake_word_detected_callback_) {
        wake_word_detected_callback_(last_detected_wake_word_);
    }
}

void AfeAudioEngine::HandleVoiceResult(const afe_fetch_result_t* result) {
    if (output_reset_pending_.exchange(false)) {
        output_buffer_.clear();
#if defined(CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_LCD_1_85C) && defined(CONFIG_VERSION_2_0)
        ResetAfeDiagnostics();
#endif
    }
    HandleBargeInResult(result);
    if (vad_state_change_callback_) {
        if (result->vad_state == VAD_SPEECH && !is_speaking_) {
            is_speaking_ = true;
            vad_state_change_callback_(true);
        } else if (result->vad_state == VAD_SILENCE && is_speaking_) {
            is_speaking_ = false;
            vad_state_change_callback_(false);
        }
    }
    if (!output_callback_) {
        return;
    }

    size_t samples = result->data_size / sizeof(int16_t);
#if defined(CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_LCD_1_85C) && defined(CONFIG_VERSION_2_0)
    AccumulateAfeDiagnostics(result);
#endif
    output_buffer_.insert(output_buffer_.end(), result->data, result->data + samples);
    while (output_buffer_.size() >= static_cast<size_t>(frame_samples_)) {
        if (output_buffer_.size() == static_cast<size_t>(frame_samples_)) {
            output_callback_(std::move(output_buffer_));
            output_buffer_.clear();
            output_buffer_.reserve(frame_samples_);
        } else {
            output_callback_(std::vector<int16_t>(
                output_buffer_.begin(), output_buffer_.begin() + frame_samples_));
            output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
        }
    }
}

void AfeAudioEngine::HandleBargeInResult(const afe_fetch_result_t* result) {
    if (barge_in_reset_pending_.exchange(false)) {
        barge_in_warmup_samples_ = 0;
        barge_in_candidate_samples_ = 0;
        barge_in_correlation_sum_ = 0.0;
        barge_in_echo_correlation_ = 0.8;
        barge_in_noise_rms_ = 8.0;
        barge_in_triggered_ = false;
        barge_in_last_diagnostic_us_ = 0;
    }
    if (!barge_in_enabled_.load() || barge_in_triggered_ || result->data == nullptr || result->data_size <= 0) {
        return;
    }

    const uint32_t samples = result->data_size / sizeof(int16_t);
    uint64_t post_sum_squares = 0;
    uint32_t post_peak = 0;
    for (uint32_t i = 0; i < samples; ++i) {
        const int32_t sample = result->data[i];
        post_sum_squares += static_cast<int64_t>(sample) * sample;
        post_peak = std::max(post_peak, static_cast<uint32_t>(std::abs(sample)));
    }
    const double post_rms = std::sqrt(static_cast<double>(post_sum_squares) / samples);

    bool has_reference = result->raw_data != nullptr && result->raw_data_channels >= 2;
    double reference_rms = 0.0;
    double correlation = 0.0;
    if (has_reference) {
        const int channels = result->raw_data_channels;
        int64_t mic_sum = 0;
        int64_t reference_sum = 0;
        uint64_t mic_sum_squares = 0;
        uint64_t reference_sum_squares = 0;
        int64_t products = 0;
        for (uint32_t i = 0; i < samples; ++i) {
            const int32_t mic = result->raw_data[i * channels];
            const int32_t reference = result->raw_data[i * channels + channels - 1];
            mic_sum += mic;
            reference_sum += reference;
            mic_sum_squares += static_cast<int64_t>(mic) * mic;
            reference_sum_squares += static_cast<int64_t>(reference) * reference;
            products += static_cast<int64_t>(mic) * reference;
        }
        const long double count = samples;
        const long double covariance = products - static_cast<long double>(mic_sum) * reference_sum / count;
        const long double mic_variance = mic_sum_squares - static_cast<long double>(mic_sum) * mic_sum / count;
        const long double reference_variance = reference_sum_squares -
            static_cast<long double>(reference_sum) * reference_sum / count;
        if (mic_variance > 0 && reference_variance > 0) {
            correlation = static_cast<double>(covariance / std::sqrt(mic_variance * reference_variance));
        }
        reference_rms = std::sqrt(static_cast<double>(reference_sum_squares) / samples);
        has_reference = reference_rms >= 256.0;
    }

    if (result->vad_state == VAD_SILENCE) {
        barge_in_noise_rms_ = barge_in_noise_rms_ * 0.96 + post_rms * 0.04;
    }

    if (has_reference && barge_in_warmup_samples_ < kBargeInWarmupSamples) {
        barge_in_correlation_sum_ += std::abs(correlation) * samples;
        barge_in_warmup_samples_ += samples;
        if (barge_in_warmup_samples_ >= kBargeInWarmupSamples) {
            barge_in_echo_correlation_ = barge_in_correlation_sum_ / barge_in_warmup_samples_;
            ESP_LOGI("BargeIn", "Echo baseline ready: corr=%.3f noise_rms=%.0f",
                barge_in_echo_correlation_, barge_in_noise_rms_);
        }
        return;
    }

    const double minimum_rms = std::max(20.0, barge_in_noise_rms_ * 2.5);
    const double correlation_limit = std::clamp(barge_in_echo_correlation_ - 0.15, 0.45, 0.68);
    const bool near_end_frame = result->vad_state == VAD_SPEECH &&
        post_rms >= minimum_rms && post_peak >= 150 &&
        (!has_reference || std::abs(correlation) <= correlation_limit);

    const int64_t now = esp_timer_get_time();
    if (now - barge_in_last_diagnostic_us_ >= 1000000) {
        barge_in_last_diagnostic_us_ = now;
        ESP_LOGI("BargeIn",
            "candidate: vad=%s post_rms=%.0f peak=%" PRIu32
            " noise=%.0f ref=%s corr=%.3f limit=%.3f warmup=%" PRIu32 "ms accepted=%s",
            result->vad_state == VAD_SPEECH ? "speech" : "silence", post_rms, post_peak,
            barge_in_noise_rms_, has_reference ? "yes" : "no", correlation, correlation_limit,
            barge_in_warmup_samples_ * 1000 / 16000, near_end_frame ? "yes" : "no");
    }

    if (near_end_frame) {
        barge_in_candidate_samples_ += samples;
    } else if (result->vad_state == VAD_SPEECH) {
        const uint32_t decay = samples / 2;
        barge_in_candidate_samples_ = barge_in_candidate_samples_ > decay
            ? barge_in_candidate_samples_ - decay : 0;
    } else {
        barge_in_candidate_samples_ = 0;
    }

    if (barge_in_candidate_samples_ >= kBargeInMinSpeechSamples) {
        barge_in_triggered_ = true;
        ESP_LOGI("BargeIn",
            "Near-end speech confirmed: post_rms=%.0f peak=%" PRIu32
            " corr=%.3f limit=%.3f ref_rms=%.0f duration_ms=%" PRIu32,
            post_rms, post_peak, correlation, correlation_limit, reference_rms,
            barge_in_candidate_samples_ * 1000 / 16000);
        if (barge_in_detected_callback_) {
            barge_in_detected_callback_();
        }
    }
}

#if defined(CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_LCD_1_85C) && defined(CONFIG_VERSION_2_0)
void AfeAudioEngine::ResetAfeDiagnostics() {
    afe_diagnostic_sum_squares_ = 0;
    afe_diagnostic_samples_ = 0;
    afe_diagnostic_peak_ = 0;
    afe_diagnostic_clipped_ = 0;
    afe_diagnostic_speech_samples_ = 0;
}

void AfeAudioEngine::AccumulateAfeDiagnostics(const afe_fetch_result_t* result) {
    const uint32_t samples = result->data_size / sizeof(int16_t);
    for (uint32_t i = 0; i < samples; ++i) {
        const int32_t sample = result->data[i];
        const uint32_t magnitude = static_cast<uint32_t>(std::abs(sample));
        afe_diagnostic_sum_squares_ += static_cast<int64_t>(sample) * sample;
        afe_diagnostic_peak_ = std::max(afe_diagnostic_peak_, magnitude);
        if (sample <= -32760 || sample >= 32760) {
            ++afe_diagnostic_clipped_;
        }
    }
    afe_diagnostic_samples_ += samples;
    if (result->vad_state == VAD_SPEECH) {
        afe_diagnostic_speech_samples_ += samples;
    }

    if (afe_diagnostic_samples_ < kAfeDiagnosticSamples) {
        return;
    }

    const double rms = std::sqrt(
        static_cast<double>(afe_diagnostic_sum_squares_) / afe_diagnostic_samples_);
    const double clipped_percent =
        100.0 * afe_diagnostic_clipped_ / afe_diagnostic_samples_;
    const double speech_percent =
        100.0 * afe_diagnostic_speech_samples_ / afe_diagnostic_samples_;
    ESP_LOGI("AfeDiag",
        "post_aec_rms=%.0f peak=%" PRIu32 " clip_pct=%.2f "
        "vad_speech_pct=%.1f vad_state=%s samples=%" PRIu32,
        rms, afe_diagnostic_peak_, clipped_percent, speech_percent,
        result->vad_state == VAD_SPEECH ? "speech" : "silence", afe_diagnostic_samples_);
    ResetAfeDiagnostics();
}
#endif

void AfeAudioEngine::OutputRawAudio(const std::vector<int16_t>& data) {
    if (!output_callback_ || codec_ == nullptr) {
        return;
    }
    if (output_reset_pending_.exchange(false)) {
        output_buffer_.clear();
    }
    const size_t channels = codec_->input_channels();
    if (channels <= 1) {
        output_buffer_.insert(output_buffer_.end(), data.begin(), data.end());
    } else {
        for (size_t i = 0; i < data.size(); i += channels) {
            output_buffer_.push_back(data[i]);
        }
    }
    while (output_buffer_.size() >= static_cast<size_t>(frame_samples_)) {
        output_callback_(std::vector<int16_t>(
            output_buffer_.begin(), output_buffer_.begin() + frame_samples_));
        output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
    }
}

void AfeAudioEngine::EncodeWakeWordData() {
    if (wake_detector_ == WakeDetector::kMultiNet) {
        custom_wake_word_->EncodeWakeWordData();
        return;
    }
    if (wake_detector_ != WakeDetector::kWakeNet) {
        return;
    }

    const size_t stack_size = 4096 * 6;
    wake_word_opus_.clear();
    if (wake_word_encode_task_stack_ == nullptr) {
        wake_word_encode_task_stack_ = static_cast<StackType_t*>(
            heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM));
        assert(wake_word_encode_task_stack_ != nullptr);
    }
    if (wake_word_encode_task_buffer_ == nullptr) {
        wake_word_encode_task_buffer_ = static_cast<StaticTask_t*>(
            heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
        assert(wake_word_encode_task_buffer_ != nullptr);
    }

    wake_word_encode_task_ = xTaskCreateStatic([](void* arg) {
        auto* engine = static_cast<AfeAudioEngine*>(arg);
        auto start_time = esp_timer_get_time();
        esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
        void* encoder_handle = nullptr;
        auto ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &encoder_handle);
        if (encoder_handle == nullptr) {
            ESP_LOGE(TAG, "Failed to create wake-word encoder: %d", ret);
            engine->wake_word_audio_cache_.Clear();
            {
                std::lock_guard<std::mutex> lock(engine->wake_word_mutex_);
                engine->wake_word_opus_.emplace_back();
                engine->wake_word_cv_.notify_all();
            }
            vTaskDelete(nullptr);
            return;
        }

        int frame_size = 0;
        int outbuf_size = 0;
        esp_opus_enc_get_frame_size(encoder_handle, &frame_size, &outbuf_size);
        frame_size /= sizeof(int16_t);
        int packets = 0;
        std::vector<int16_t> input(frame_size);
        esp_audio_enc_in_frame_t in = {};
        esp_audio_enc_out_frame_t out = {};

        const size_t cached_samples = engine->wake_word_audio_cache_.Size();
        for (size_t offset = 0;
             offset + static_cast<size_t>(frame_size) <= cached_samples;
             offset += frame_size) {
            if (engine->wake_word_audio_cache_.Read(
                    offset, input.data(), frame_size) != static_cast<size_t>(frame_size)) {
                break;
            }
            std::vector<uint8_t> opus_buf(outbuf_size);
            in.buffer = reinterpret_cast<uint8_t*>(input.data());
            in.len = frame_size * sizeof(int16_t);
            out.buffer = opus_buf.data();
            out.len = outbuf_size;
            out.encoded_bytes = 0;
            ret = esp_opus_enc_process(encoder_handle, &in, &out);
            if (ret == ESP_AUDIO_ERR_OK) {
                std::lock_guard<std::mutex> lock(engine->wake_word_mutex_);
                engine->wake_word_opus_.emplace_back(
                    opus_buf.data(), opus_buf.data() + out.encoded_bytes);
                engine->wake_word_cv_.notify_all();
                ++packets;
            } else {
                ESP_LOGE(TAG, "Failed to encode wake-word audio: %d", ret);
            }
        }

        engine->wake_word_audio_cache_.Clear();
        esp_opus_enc_close(encoder_handle);
        ESP_LOGI(TAG, "Encoded wake word into %d packets in %ld ms", packets,
            static_cast<long>((esp_timer_get_time() - start_time) / 1000));
        {
            std::lock_guard<std::mutex> lock(engine->wake_word_mutex_);
            engine->wake_word_opus_.emplace_back();
            engine->wake_word_cv_.notify_all();
        }
        vTaskDelete(nullptr);
    }, "encode_wake_word", stack_size, this, 2,
        wake_word_encode_task_stack_, wake_word_encode_task_buffer_);
}

bool AfeAudioEngine::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    if (wake_detector_ == WakeDetector::kMultiNet) {
        return custom_wake_word_->GetWakeWordOpus(opus);
    }
    if (wake_detector_ != WakeDetector::kWakeNet) {
        return false;
    }
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this]() { return !wake_word_opus_.empty(); });
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    return !opus.empty();
}
