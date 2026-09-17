#pragma once

#include "audio/audio_codec.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
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
    // For a hardware-fault scan (see app_main's i2c_scan command): both codecs
    // stopped answering their known addresses, and probing every 7-bit address
    // on the same bus tells whether NOTHING is present (chips unpowered/dead)
    // or something answers at an unexpected address (misconfigured address
    // pins, or a part swapped for the wrong one).
    i2c_master_bus_handle_t i2c_bus() const { return i2c_bus_; }
    bool is_playing() const { return busy_.load(); }
    esp_err_t last_result() const { return last_result_; }
    const std::string& last_error() const { return last_error_; }
    size_t last_pcm_bytes() const { return last_pcm_bytes_.load(); }
    int64_t last_pcm_elapsed_ms() const { return last_pcm_elapsed_ms_.load(); }
    int last_pcm_pa_level() const { return last_pcm_pa_level_.load(); }

    // Audio handed to the codec but not yet heard, in ms. The I2S write blocks
    // once the DMA ring is full, so "written minus elapsed" is how far ahead of
    // the speaker the writer has got -- which is exactly the audio a barge-in
    // throws away. Needed to truncate conversation history correctly after an
    // interruption: if TTS is cut at the 20th character and the full 80 go into
    // the history, the model believes it finished and carries on from there.
    int spk_buffer_ms() const;
    // Audio actually played in the current utterance, in ms.
    int spk_played_ms() const;

    // T3.2 asks for the stop to take effect immediately rather than after the
    // buffer drains. This is how long the last stop() actually took to reach
    // the point where the output is torn down -- i.e. how much extra audio the
    // user heard after the barge-in decision. -1 before the first stop.
    //
    // It matters because E4's t_duck marks the moment the decision is made, not
    // the moment sound stops, so the latency figure can look healthy while the
    // robot is still audibly talking.
    int last_stop_latency_ms() const { return last_stop_latency_ms_.load(); }

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
    // Called right after every successful AudioCodec_OutputData(). The clock
    // starts at the FIRST write, not at play_task() entry: the HTTP connect
    // and WAV-header parse in between would otherwise be counted as elapsed
    // playback and make the buffer estimate read low for the first second.
    void note_samples_written(int samples);
    // Hands `samples` to the codec in DMA-descriptor-sized pieces, checking
    // stop_requested_ between them, and returns how many were actually written.
    //
    // A single AudioCodec_OutputData() of a whole 8 KB read buffer is 4096
    // samples = 256 ms of audio, and it blocks until all of it has been queued
    // into a ring that only holds 90 ms -- so the stop flag was only tested
    // about every 256 ms and the speaker kept playing until then. Measured at
    // 143 ms and 159 ms on v54. T3.2 asks for the stop to be immediate, and
    // E4's t_duck cannot see this delay because it marks the decision, not the
    // silence.
    int write_interruptible(const int16_t* data, int samples);
    // Let the I2S DMA ring play out before the output is closed (closing
    // discards it). Only on a normal finish -- a barge-in wants it discarded.
    void drain_output() const;
    esp_err_t play_wav_stream(const std::string& url, uint8_t volume_percent);
    esp_err_t play_wav_stream_raw_http(const std::string& url, uint8_t volume_percent);
    esp_err_t play_pcm_stream_websocket(const std::string& url, uint8_t volume_percent,
                                        int pa_level);
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    AudioCodec* codec_ = nullptr;
    // Created once in the constructor and never torn down: play_wav_url() /
    // play_pcm_url() used to xTaskCreate a fresh 8 KB-stack task per call,
    // which failed with ESP_ERR_NO_MEM whenever internal RAM happened to be
    // fragmented below ~8 KB contiguous at that instant (this device runs
    // AFE full-duplex continuously, so that baseline is chronically tight --
    // see docs/aec/SUMMARY.md). A persistent task sidesteps the failure mode
    // entirely: the 8 KB stack is claimed once, at boot, when fragmentation
    // is lowest, and every later call just wakes it via a semaphore instead
    // of allocating anything.
    TaskHandle_t task_ = nullptr;
    SemaphoreHandle_t request_sem_ = nullptr;
    std::atomic<bool> busy_{false};
    volatile bool stop_requested_ = false;
    volatile esp_err_t last_result_ = ESP_OK;
    std::string pending_url_;
    uint8_t pending_volume_ = 30;
    bool pending_pcm_ = false;
    int pending_pa_level_ = 1;
    std::atomic<size_t> last_pcm_bytes_{0};
    std::atomic<int64_t> last_pcm_elapsed_ms_{0};
    std::atomic<int> last_pcm_pa_level_{-1};
    std::atomic<uint64_t> samples_written_{0};
    std::atomic<int64_t> first_write_us_{0};  // 0 = nothing written yet this utterance
    std::atomic<int> last_played_ms_{0};      // frozen at the end of each utterance
    std::atomic<int64_t> stop_requested_us_{0};
    std::atomic<int> last_stop_latency_ms_{-1};
    std::string last_error_;
    std::string last_asr_text_;
    AecProbeResult last_probe_result_{};
};
