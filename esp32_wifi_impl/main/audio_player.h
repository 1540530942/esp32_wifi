#pragma once

#include "audio/audio_codec.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdint>
#include <atomic>
#include <string>

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();
    esp_err_t play_wav_url(const std::string& url, uint8_t volume_percent = 30);
    esp_err_t play_pcm_url(const std::string& url, uint8_t volume_percent = 30,
                           int pa_level = 1);
    esp_err_t stop();
    AudioCodec* codec() const { return codec_; }
    bool is_playing() const { return task_ != nullptr; }
    esp_err_t last_result() const { return last_result_; }
    const std::string& last_error() const { return last_error_; }
    size_t last_pcm_bytes() const { return last_pcm_bytes_.load(); }
    int64_t last_pcm_elapsed_ms() const { return last_pcm_elapsed_ms_.load(); }
    int last_pcm_pa_level() const { return last_pcm_pa_level_.load(); }

    // Mic-capture -> cloud ASR -> TTS-playback validation loop (see
    // docs/logs/ for the write-up). Records `seconds` of mono PCM from the
    // already-enabled ES7210 input path, POSTs it to the cloud ASR endpoint,
    // then synthesizes and speaks a confirmation. Blocks the caller for the
    // whole round trip (record + two HTTPS calls); call from a dedicated task,
    // not from the button ISR / heartbeat loop directly.
    esp_err_t run_mic_asr_test(int seconds, uint8_t volume_percent = 40);
    const std::string& last_asr_text() const { return last_asr_text_; }

    // One-shot hardware diagnostic: is the ES7210 reference channel (channel
    // 1, only present when AUDIO_INPUT_REFERENCE) actually wired to the
    // speaker output (real hardware AEC reference), or just an unconnected
    // ADC input the board config optimistically flags as present? Plays a
    // loud tone while concurrently reading both input channels, and logs
    // RMS(ch0)/RMS(ch1) for silence vs. playback so the two can be compared
    // by eye in the serial log. Does not touch run_mic_asr_test's path.
    esp_err_t run_aec_reference_probe();

    // Synthesize `text` through the cloud TTS and hand back the raw WAV
    // (heap_caps buffer, caller frees with heap_caps_free). Same call
    // run_mic_asr_test uses to speak its reply, so it is a known-good path --
    // unlike play_wav_url, which does not produce audio on this device.
    esp_err_t fetch_tts(const std::string& text, uint8_t** wav, size_t* len);

    // Populated by run_aec_reference_probe() on ESP_OK, so a remote caller
    // (device-hub command) can read the measurement instead of the UART log.
    struct AecProbeResult {
        float silent_rms0 = 0, silent_rms1 = 0, play_rms0 = 0, play_rms1 = 0;
        bool reference_is_real = false;
    };
    const AecProbeResult& last_probe_result() const { return last_probe_result_; }

private:
    static void task_entry(void* arg);
    void play_task();
    esp_err_t play_wav_stream(const std::string& url, uint8_t volume_percent);
    esp_err_t play_wav_stream_raw_http(const std::string& url, uint8_t volume_percent);
    esp_err_t play_pcm_stream_websocket(const std::string& url, uint8_t volume_percent,
                                        int pa_level);
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    AudioCodec* codec_ = nullptr;
    TaskHandle_t task_ = nullptr;
    volatile bool stop_requested_ = false;
    volatile esp_err_t last_result_ = ESP_OK;
    std::string pending_url_;
    uint8_t pending_volume_ = 30;
    bool pending_pcm_ = false;
    int pending_pa_level_ = 1;
    std::atomic<size_t> last_pcm_bytes_{0};
    std::atomic<int64_t> last_pcm_elapsed_ms_{0};
    std::atomic<int> last_pcm_pa_level_{-1};
    std::string last_error_;
    std::string last_asr_text_;
    AecProbeResult last_probe_result_{};
};
