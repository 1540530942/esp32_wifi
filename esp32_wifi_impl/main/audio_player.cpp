#include "audio_player.h"

#include "audio/codecs/box_audio_codec.h"
#include "audio_board_config.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "driver/gpio.h"
#include "cJSON.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static const char* TAG = "audio_player";

AudioPlayer::AudioPlayer() {
    i2c_master_bus_config_t i2c_cfg = {};
    i2c_cfg.i2c_port = I2C_NUM_0;
    i2c_cfg.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
    i2c_cfg.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
    i2c_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    i2c_cfg.glitch_ignore_cnt = 7;
    i2c_cfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus_));
    codec_ = BoxAudioCodec_Create(i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
        AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
        AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR,
        AUDIO_CODEC_ES7210_ADDR, AUDIO_INPUT_REFERENCE);
    if (!codec_) {
        ESP_LOGE(TAG, "ES7210/ES8311 codec initialization failed");
        return;
    }
    AudioCodec_SetOutputVolume(codec_, 30);
    AudioCodec_Start(codec_);
    AudioCodec_EnableOutput(codec_, false);
    ESP_LOGI(TAG, "ES7210/ES8311 audio path ready, output volume=30%%, PA GPIO17=%d",
             gpio_get_level(AUDIO_CODEC_PA_PIN));
}

AudioPlayer::~AudioPlayer() {
    stop();
    if (codec_) {
        if (codec_->destroy_impl) codec_->destroy_impl(codec_);
        codec_ = nullptr;
    }
    if (i2c_bus_) i2c_del_master_bus(i2c_bus_);
}

esp_err_t AudioPlayer::play_wav_url(const std::string& url, uint8_t volume_percent) {
    if (!codec_) return ESP_ERR_INVALID_STATE;
    if (task_) return ESP_ERR_INVALID_STATE;
    const bool https = url.rfind("https://", 0) == 0;
    const bool cloud_http = url.rfind("http://110.40.154.41/devices/api/", 0) == 0;
    if (!https && !cloud_http) return ESP_ERR_INVALID_ARG;
    pending_url_ = url;
    pending_volume_ = std::min<uint8_t>(volume_percent, 100);
    pending_pcm_ = false;
    pending_pa_level_ = 1;
    stop_requested_ = false;
    last_result_ = ESP_ERR_INVALID_STATE;
    return xTaskCreate(&AudioPlayer::task_entry, "audio_play", 8192, this, 5, &task_) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t AudioPlayer::play_pcm_url(const std::string& url, uint8_t volume_percent, int pa_level) {
    if (!codec_) return ESP_ERR_INVALID_STATE;
    if (task_) return ESP_ERR_INVALID_STATE;
    if (url.rfind("wss://", 0) != 0 && url.rfind("ws://", 0) != 0) return ESP_ERR_INVALID_ARG;
    pending_url_ = url;
    pending_volume_ = std::min<uint8_t>(volume_percent, 100);
    pending_pcm_ = true;
    pending_pa_level_ = pa_level ? 1 : 0;
    stop_requested_ = false;
    last_result_ = ESP_ERR_INVALID_STATE;
    return xTaskCreate(&AudioPlayer::task_entry, "audio_pcm", 8192, this, 5, &task_) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t AudioPlayer::stop() {
    if (task_) stop_requested_ = true;
    return ESP_OK;
}

void AudioPlayer::task_entry(void* arg) {
    static_cast<AudioPlayer*>(arg)->play_task();
    vTaskDelete(nullptr);
}

void AudioPlayer::play_task() {
    const std::string url = pending_url_;
    const uint8_t volume = pending_volume_;
    const bool pcm = pending_pcm_;
    const int pa_level = pending_pa_level_;
    const esp_err_t wdt_add_err = pcm ? ESP_ERR_INVALID_STATE : esp_task_wdt_add(nullptr);
    if (wdt_add_err != ESP_OK && wdt_add_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "audio task watchdog registration failed: %s",
                 esp_err_to_name(wdt_add_err));
    }
    ESP_LOGI(TAG, "starting %s playback at %u%%", pcm ? "PCM WebSocket" : "WAV", volume);
    esp_err_t err = pcm ? play_pcm_stream_websocket(url, volume, pa_level) : play_wav_stream(url, volume);
    last_result_ = err;
    last_error_ = err == ESP_OK ? std::string() : std::string(esp_err_to_name(err));
    ESP_LOGI(TAG, "%s playback %s", pcm ? "PCM WebSocket" : "WAV", err == ESP_OK ? "finished" :
             (stop_requested_ ? "stopped" : "failed"));
    if (wdt_add_err == ESP_OK) esp_task_wdt_delete(nullptr);
    task_ = nullptr;
    stop_requested_ = false;
}

esp_err_t AudioPlayer::play_pcm_stream_websocket(const std::string& url,
                                                  uint8_t volume_percent, int pa_level) {
    const bool secure = url.rfind("wss://", 0) == 0;
    const size_t authority_start = secure ? 6 : 5;
    const size_t path_start = url.find('/', authority_start);
    if (path_start == std::string::npos) return ESP_ERR_INVALID_ARG;
    std::string authority = url.substr(authority_start, path_start - authority_start);
    std::string host = authority;
    int port = secure ? 443 : 80;
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        host = authority.substr(0, colon);
        port = std::atoi(authority.substr(colon + 1).c_str());
    }
    esp_transport_handle_t parent = secure ? esp_transport_ssl_init() : esp_transport_tcp_init();
    if (!parent) return ESP_ERR_NO_MEM;
    if (secure) esp_transport_ssl_crt_bundle_attach(parent, esp_crt_bundle_attach);
    esp_transport_handle_t ws = esp_transport_ws_init(parent);
    if (!ws) { esp_transport_destroy(parent); return ESP_ERR_NO_MEM; }
    const std::string path = url.substr(path_start);
    esp_transport_ws_set_path(ws, path.c_str());
    esp_transport_ws_set_subprotocol(ws, "binary");
    const int64_t started_us = esp_timer_get_time();
    if (esp_transport_connect(ws, host.c_str(), port, 10000) < 0) {
        ESP_LOGW(TAG, "PCM WebSocket connect failed host=%s port=%d status=%d",
                 host.c_str(), port, esp_transport_ws_get_upgrade_request_status(ws));
        esp_transport_destroy(ws); esp_transport_destroy(parent); return ESP_ERR_HTTP_CONNECT;
    }
    ESP_LOGI(TAG, "PCM WebSocket connected host=%s path=%s handshake_ms=%lld status=%d",
             host.c_str(), path.c_str(),
             (esp_timer_get_time() - started_us) / 1000,
             esp_transport_ws_get_upgrade_request_status(ws));

    // TLS/WebSocket reads may end on an arbitrary byte boundary, even when
    // every cloud frame contains an even number of PCM bytes. Keep one extra
    // byte so a split 16-bit sample is joined with the next read instead of
    // being dropped (which would byte-shift all following PCM into noise).
    constexpr size_t kPcmReadBufferBytes = 32768;
    uint8_t* input = static_cast<uint8_t*>(heap_caps_calloc(
        1, kPcmReadBufferBytes + 1, MALLOC_CAP_8BIT));
    if (!input) {
        esp_transport_close(ws);
        esp_transport_destroy(ws);
        esp_transport_destroy(parent);
        return ESP_ERR_NO_MEM;
    }
    size_t total_bytes = 0;
    uint32_t binary_frames = 0;
    int64_t first_binary_us = 0;
    int64_t receive_wait_us = 0;
    int64_t output_write_us = 0;
    bool output_enabled = false;
    int observed_pa_level = -1;
    int read_result = 0;
    bool has_partial_sample = false;
    uint8_t partial_sample = 0;
    while (!stop_requested_) {
        const int64_t read_started_us = esp_timer_get_time();
        // The cloud keeps the socket open after stream_end, so a short read
        // timeout is safe and avoids adding a full second of latency for each
        // TLS/WS fragment while still allowing the next fragment to arrive.
        read_result = esp_transport_read(ws, reinterpret_cast<char*>(input),
                                         kPcmReadBufferBytes, 200);
        receive_wait_us += esp_timer_get_time() - read_started_us;
        if (read_result <= 0) {
            if (read_result == 0) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            break;
        }
        const auto opcode = esp_transport_ws_get_read_opcode(ws);
        if (opcode == WS_TRANSPORT_OPCODES_TEXT) {
            ESP_LOGI(TAG, "PCM WebSocket metadata bytes=%d", read_result);
            const std::string control(reinterpret_cast<const char*>(input), read_result);
            if (control.find("\"stream_end\"") != std::string::npos) {
                ESP_LOGI(TAG, "PCM WebSocket stream_end received");
                read_result = 0;
                break;
            }
            continue;
        }
        if (opcode != WS_TRANSPORT_OPCODES_BINARY && opcode != WS_TRANSPORT_OPCODES_CONT) continue;
        const int64_t binary_now_us = esp_timer_get_time();
        if (first_binary_us == 0) first_binary_us = binary_now_us;
        ++binary_frames;
        if (!output_enabled) {
            AudioCodec_EnableOutput(codec_, true);
            const int requested_pa_level = pa_level ? 1 : 0;
            const esp_err_t pa_err = gpio_set_level(AUDIO_CODEC_PA_PIN, requested_pa_level);
            AudioCodec_SetOutputVolume(codec_, volume_percent);
            output_enabled = true;
            observed_pa_level = gpio_get_level(AUDIO_CODEC_PA_PIN);
            ESP_LOGI(TAG, "PCM output enabled PA GPIO17=%d requested=%d set_err=%s volume=%u",
                     observed_pa_level, requested_pa_level, esp_err_to_name(pa_err), volume_percent);
        }
        size_t pcm_bytes = static_cast<size_t>(read_result);
        if (has_partial_sample) {
            std::memmove(input + 1, input, pcm_bytes);
            input[0] = partial_sample;
            ++pcm_bytes;
            has_partial_sample = false;
        }
        if ((pcm_bytes & 1U) != 0) {
            partial_sample = input[pcm_bytes - 1];
            --pcm_bytes;
            has_partial_sample = true;
        }
        const int samples = static_cast<int>(pcm_bytes / 2);
        const int64_t write_started_us = esp_timer_get_time();
        const int written = samples > 0
            ? AudioCodec_OutputData(codec_, reinterpret_cast<int16_t*>(input), samples)
            : 0;
        output_write_us += esp_timer_get_time() - write_started_us;
        if (written != samples) ESP_LOGW(TAG, "PCM short write requested=%d written=%d", samples, written);
        total_bytes += static_cast<size_t>(read_result);
    }
    if (output_enabled) AudioCodec_EnableOutput(codec_, false);
    // esp_transport_destroy() only frees the transport object; close the
    // WebSocket first so the TLS parent releases its live connection before
    // its mbedTLS context is destroyed.
    esp_transport_close(ws);
    esp_transport_destroy(ws);
    esp_transport_destroy(parent);
    heap_caps_free(input);
    last_pcm_bytes_.store(total_bytes);
    last_pcm_elapsed_ms_.store((esp_timer_get_time() - started_us) / 1000);
    last_pcm_pa_level_.store(observed_pa_level);
    ESP_LOGI(TAG, "PCM WebSocket ended bytes=%u read_result=%d elapsed_ms=%lld",
             (unsigned)total_bytes, read_result, (esp_timer_get_time() - started_us) / 1000);
    ESP_LOGI(TAG, "PCM timing frames=%u first_binary_ms=%lld receive_wait_ms=%lld i2s_write_ms=%lld",
             (unsigned)binary_frames,
             first_binary_us == 0 ? -1LL : (first_binary_us - started_us) / 1000,
             receive_wait_us / 1000, output_write_us / 1000);
    if (has_partial_sample) {
        ESP_LOGW(TAG, "PCM stream ended with an incomplete 16-bit sample");
    }
    return stop_requested_ ? ESP_ERR_INVALID_STATE
                           : (read_result < 0 || has_partial_sample ? ESP_FAIL : ESP_OK);
}

esp_err_t AudioPlayer::play_wav_stream(const std::string& url, uint8_t volume_percent) {
    if (url.rfind("http://110.40.154.41/devices/api/", 0) == 0) {
        return play_wav_stream_raw_http(url, volume_percent);
    }
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = 15000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;
    const int64_t started_us = esp_timer_get_time();
    esp_err_t err = esp_http_client_open(client, 0);
    ESP_LOGI(TAG, "WAV https connect_ms=%lld err=%s",
             (esp_timer_get_time() - started_us) / 1000, esp_err_to_name(err));
    if (err != ESP_OK) { esp_http_client_cleanup(client); return err; }
    const int64_t headers_us = esp_timer_get_time();
    int length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "WAV HTTP response status=%d content_length=%d header_ms=%lld",
             status, length, (esp_timer_get_time() - headers_us) / 1000);
    if (status < 200 || status >= 300 || length < 44) {
        esp_http_client_close(client); esp_http_client_cleanup(client); return ESP_FAIL;
    }
    uint8_t header[44] = {};
    int got = esp_http_client_read(client, reinterpret_cast<char*>(header), sizeof(header));
    bool pcm16mono = got >= 44 && std::memcmp(header, "RIFF", 4) == 0 &&
                     std::memcmp(header + 8, "WAVE", 4) == 0 && header[20] == 1 &&
                     header[22] == 1 && header[34] == 16;
    if (!pcm16mono) {
        ESP_LOGW(TAG, "unsupported WAV; expected PCM 16-bit mono");
        esp_http_client_close(client); esp_http_client_cleanup(client); return ESP_ERR_NOT_SUPPORTED;
    }
    uint8_t input[1025] = {};
    int read = 0;
    const int scale = volume_percent;
    AudioCodec_EnableOutput(codec_, true);
    const esp_err_t pa_err = gpio_set_level(AUDIO_CODEC_PA_PIN, 1);
    ESP_LOGI(TAG, "WAV playback output enabled PA GPIO17=%d set_err=%s volume=%d",
             gpio_get_level(AUDIO_CODEC_PA_PIN), esp_err_to_name(pa_err), scale);
    AudioCodec_SetOutputVolume(codec_, scale);
    size_t total_bytes = 0;
    bool has_partial_sample = false;
    uint8_t partial_sample = 0;
    while (!stop_requested_ &&
           (read = esp_http_client_read(client, reinterpret_cast<char*>(input), 1024)) > 0) {
        total_bytes += static_cast<size_t>(read);
        size_t pcm_bytes = static_cast<size_t>(read);
        if (has_partial_sample) {
            std::memmove(input + 1, input, pcm_bytes);
            input[0] = partial_sample;
            ++pcm_bytes;
            has_partial_sample = false;
        }
        if ((pcm_bytes & 1U) != 0) {
            partial_sample = input[pcm_bytes - 1];
            --pcm_bytes;
            has_partial_sample = true;
        }
        const int samples = static_cast<int>(pcm_bytes / 2);
        const int written = samples > 0
            ? AudioCodec_OutputData(codec_, reinterpret_cast<int16_t*>(input), samples)
            : 0;
        if (written != samples) {
            ESP_LOGW(TAG, "I2S/codec short write requested=%d written=%d", samples, written);
        }
        vTaskDelay(1);
    }
    AudioCodec_EnableOutput(codec_, false);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "WAV stream bytes=%u read_result=%d final_PA_GPIO17=%d elapsed_ms=%lld",
             (unsigned)total_bytes, read, gpio_get_level(AUDIO_CODEC_PA_PIN),
             (esp_timer_get_time() - started_us) / 1000);
    return stop_requested_ ? ESP_ERR_INVALID_STATE
                           : (read < 0 || has_partial_sample ? ESP_FAIL : ESP_OK);
}

esp_err_t AudioPlayer::play_wav_stream_raw_http(const std::string& url,
                                                uint8_t volume_percent) {
    constexpr const char* prefix = "http://";
    const size_t authority_start = std::strlen(prefix);
    const size_t path_start = url.find('/', authority_start);
    if (path_start == std::string::npos) return ESP_ERR_INVALID_ARG;
    std::string authority = url.substr(authority_start, path_start - authority_start);
    const int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) return ESP_FAIL;
    timeval timeout = {.tv_sec = 10, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    in_addr addr = {};
    if (inet_pton(AF_INET, authority.c_str(), &addr) != 1) {
        close(sock);
        return ESP_ERR_INVALID_ARG;
    }
    sockaddr_in peer = {};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(80);
    peer.sin_addr = addr;
    const int64_t started_us = esp_timer_get_time();
    if (connect(sock, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) != 0) {
        ESP_LOGW(TAG, "WAV raw connect failed errno=%d", errno);
        close(sock);
        return ESP_ERR_HTTP_CONNECT;
    }
    ESP_LOGI(TAG, "WAV raw connected connect_ms=%lld", (esp_timer_get_time() - started_us) / 1000);
    const std::string request = "GET " + url.substr(path_start) +
        " HTTP/1.0\r\nHost: " + authority +
        "\r\nUser-Agent: esp32-wangyutang/1.0\r\nConnection: close\r\n\r\n";
    size_t sent = 0;
    while (sent < request.size()) {
        const int n = send(sock, request.data() + sent, request.size() - sent, 0);
        if (n <= 0) {
            ESP_LOGW(TAG, "WAV raw send failed errno=%d", errno);
            close(sock);
            return ESP_FAIL;
        }
        sent += static_cast<size_t>(n);
    }

    const int64_t header_started_us = esp_timer_get_time();
    std::string received;
    char network_buffer[2048];
    size_t header_end = std::string::npos;
    while (received.size() < 8192 && header_end == std::string::npos) {
        const int n = recv(sock, network_buffer, sizeof(network_buffer), 0);
        if (n <= 0) {
            ESP_LOGW(TAG, "WAV raw header read failed n=%d errno=%d", n, errno);
            close(sock);
            return ESP_ERR_HTTP_FETCH_HEADER;
        }
        received.append(network_buffer, n);
        header_end = received.find("\r\n\r\n");
    }
    if (header_end == std::string::npos || received.size() < 12 ||
        received.compare(0, 7, "HTTP/1.") != 0) {
        close(sock);
        return ESP_FAIL;
    }
    const int status = std::atoi(received.c_str() + 9);
    const size_t body_offset = header_end + 4;
    ESP_LOGI(TAG, "WAV raw response status=%d buffered_body=%u header_ms=%lld", status,
             static_cast<unsigned>(received.size() - body_offset),
             (esp_timer_get_time() - header_started_us) / 1000);
    if (status < 200 || status >= 300) {
        close(sock);
        return ESP_FAIL;
    }

    size_t buffered_pos = body_offset;
    auto read_bytes = [&](char* destination, size_t wanted) -> int {
        size_t copied = 0;
        if (buffered_pos < received.size()) {
            const size_t available = received.size() - buffered_pos;
            const size_t take = std::min(available, wanted);
            std::memcpy(destination, received.data() + buffered_pos, take);
            buffered_pos += take;
            copied += take;
        }
        while (copied < wanted) {
            const int n = recv(sock, destination + copied, wanted - copied, 0);
            if (n <= 0) return copied > 0 ? static_cast<int>(copied) : n;
            copied += static_cast<size_t>(n);
        }
        return static_cast<int>(copied);
    };

    uint8_t header[44] = {};
    const int got = read_bytes(reinterpret_cast<char*>(header), sizeof(header));
    ESP_LOGI(TAG, "WAV raw first read=%d", got);
    const bool pcm16mono = got >= 44 && std::memcmp(header, "RIFF", 4) == 0 &&
        std::memcmp(header + 8, "WAVE", 4) == 0 && header[20] == 1 &&
        header[22] == 1 && header[34] == 16;
    if (!pcm16mono) {
        ESP_LOGW(TAG, "unsupported raw WAV; expected PCM 16-bit mono");
        close(sock);
        return ESP_ERR_NOT_SUPPORTED;
    }
    const uint32_t sample_rate = header[24] | (header[25] << 8) |
                                 (header[26] << 16) | (header[27] << 24);
    ESP_LOGI(TAG, "WAV raw format sample_rate=%u", static_cast<unsigned>(sample_rate));
    size_t audio_remaining = static_cast<size_t>(header[40]) |
                             (static_cast<size_t>(header[41]) << 8) |
                              (static_cast<size_t>(header[42]) << 16) |
                             (static_cast<size_t>(header[43]) << 24);
    ESP_LOGI(TAG, "WAV PCM bytes remaining=%u", static_cast<unsigned>(audio_remaining));

    // For the body, consume whatever is currently available instead of
    // forcing every call to fill 4096 bytes.  On this WAN/Wi-Fi path a short
    // TCP read is normal; waiting for the remainder inside one call can hit
    // the socket timeout and starve the task watchdog.
    auto read_some = [&](char* destination, size_t wanted) -> int {
        if (buffered_pos < received.size()) {
            const size_t available = received.size() - buffered_pos;
            const size_t take = std::min(available, wanted);
            std::memcpy(destination, received.data() + buffered_pos, take);
            buffered_pos += take;
            return static_cast<int>(take);
        }
        return recv(sock, destination, wanted, 0);
    };

    // Stream through a bounded buffer. The GitHub Actions image intentionally
    // does not require PSRAM; allocating the complete WAV made normal TTS
    // clips fail with ESP_ERR_NO_MEM while MQTT/TLS were resident.
    constexpr size_t kWavBufferBytes = 8192;
    uint8_t* pcm_buffer = static_cast<uint8_t*>(heap_caps_malloc(
        kWavBufferBytes + 1, MALLOC_CAP_8BIT));
    if (!pcm_buffer) {
        ESP_LOGE(TAG, "WAV stream buffer allocation failed, bytes=%u",
                 static_cast<unsigned>(kWavBufferBytes + 1));
        close(sock);
        return ESP_ERR_NO_MEM;
    }

    const int scale = volume_percent;
    AudioCodec_EnableOutput(codec_, true);
    AudioCodec_SetOutputVolume(codec_, scale);
    const esp_err_t pa_err = gpio_set_level(AUDIO_CODEC_PA_PIN, 1);
    ESP_LOGI(TAG, "WAV raw playback output enabled PA GPIO17=%d set_err=%s volume=%d",
             gpio_get_level(AUDIO_CODEC_PA_PIN), esp_err_to_name(pa_err), scale);

    size_t downloaded = 0;
    size_t played = 0;
    int read = 0;
    bool has_partial_sample = false;
    uint8_t partial_sample = 0;
    while (!stop_requested_ && downloaded < audio_remaining &&
           (read = read_some(reinterpret_cast<char*>(pcm_buffer),
                             std::min(kWavBufferBytes, audio_remaining - downloaded))) > 0) {
        downloaded += static_cast<size_t>(read);
        size_t pcm_bytes = static_cast<size_t>(read);
        if (has_partial_sample) {
            std::memmove(pcm_buffer + 1, pcm_buffer, pcm_bytes);
            pcm_buffer[0] = partial_sample;
            ++pcm_bytes;
            has_partial_sample = false;
        }
        if ((pcm_bytes & 1U) != 0) {
            partial_sample = pcm_buffer[pcm_bytes - 1];
            --pcm_bytes;
            has_partial_sample = true;
        }
        const size_t samples = pcm_bytes / 2;
        const int written = samples > 0
            ? AudioCodec_OutputData(codec_, reinterpret_cast<int16_t*>(pcm_buffer), samples)
            : 0;
        if (written != static_cast<int>(samples)) {
            ESP_LOGW(TAG, "I2S/codec short write requested=%u written=%d",
                     static_cast<unsigned>(samples), written);
        }
        played += static_cast<size_t>(std::max(written, 0)) * 2;
        esp_task_wdt_reset();
        if ((downloaded & 0x3fff) < static_cast<size_t>(read)) {
            ESP_LOGI(TAG, "WAV raw stream progress=%u/%u",
                     static_cast<unsigned>(downloaded),
                     static_cast<unsigned>(audio_remaining));
        }
    }
    AudioCodec_EnableOutput(codec_, false);
    close(sock);
    if (stop_requested_) {
        heap_caps_free(pcm_buffer);
        return ESP_ERR_INVALID_STATE;
    }
    if (downloaded != audio_remaining || has_partial_sample) {
        ESP_LOGW(TAG, "WAV stream incomplete bytes=%u/%u partial_sample=%d read_result=%d",
                 static_cast<unsigned>(downloaded), static_cast<unsigned>(audio_remaining),
                 has_partial_sample ? 1 : 0, read);
        heap_caps_free(pcm_buffer);
        return ESP_FAIL;
    }
    heap_caps_free(pcm_buffer);
    ESP_LOGI(TAG, "WAV raw stream bytes=%u read_result=%d final_PA_GPIO17=%d elapsed_ms=%lld",
             static_cast<unsigned>(played), read, gpio_get_level(AUDIO_CODEC_PA_PIN),
             (esp_timer_get_time() - started_us) / 1000);
    return stop_requested_ ? ESP_ERR_INVALID_STATE : (played == downloaded ? ESP_OK : ESP_FAIL);
}

// ---------------------------------------------------------------------------
// Mic + ASR + broadcast validation loop.
//
// The ES7210 input path is already open (AudioCodec_Start() in the ctor
// enables it and nothing since has disabled it) -- this only drains it. See
// docs/logs/ for the discovery that made this low-risk: BoxAudioCodec opens
// the mic with channel_mask{0,1} when AUDIO_INPUT_REFERENCE is set, so a
// "frame" here is 2 interleaved int16 (mic, echo-reference); we keep channel
// 0 and drop the reference channel for the mono ASR upload.
// ---------------------------------------------------------------------------

namespace {

// Plain HTTP via the gateway IP: TLS to :443 is MITM-broken on this ISP path
// (esp-x509-crt-bundle reports a signature failure), which is the same reason
// device_hub, OTA and the /ws/audio uplink all avoid it. Verified: the gateway
// serves both of these on port 80.
constexpr const char* kAsrUrl = "http://110.40.154.41/common/api/asr/transcribe";
constexpr const char* kTtsUrl = "http://110.40.154.41/audio_interact/api/tts";
constexpr const char* kBoundary = "----ESP32MicAsrBoundary7MA4YWxk";

void write_wav_header(uint8_t* hdr, uint32_t data_bytes, uint32_t sample_rate) {
    const uint32_t byte_rate = sample_rate * 2;  // mono, 16-bit
    const uint32_t riff_size = 36 + data_bytes;
    std::memcpy(hdr, "RIFF", 4);
    hdr[4] = riff_size & 0xff; hdr[5] = (riff_size >> 8) & 0xff;
    hdr[6] = (riff_size >> 16) & 0xff; hdr[7] = (riff_size >> 24) & 0xff;
    std::memcpy(hdr + 8, "WAVE", 4);
    std::memcpy(hdr + 12, "fmt ", 4);
    hdr[16] = 16; hdr[17] = 0; hdr[18] = 0; hdr[19] = 0;   // fmt chunk size
    hdr[20] = 1; hdr[21] = 0;                              // PCM
    hdr[22] = 1; hdr[23] = 0;                              // mono
    hdr[24] = sample_rate & 0xff; hdr[25] = (sample_rate >> 8) & 0xff;
    hdr[26] = (sample_rate >> 16) & 0xff; hdr[27] = (sample_rate >> 24) & 0xff;
    hdr[28] = byte_rate & 0xff; hdr[29] = (byte_rate >> 8) & 0xff;
    hdr[30] = (byte_rate >> 16) & 0xff; hdr[31] = (byte_rate >> 24) & 0xff;
    hdr[32] = 2; hdr[33] = 0;                              // block align
    hdr[34] = 16; hdr[35] = 0;                             // bits per sample
    std::memcpy(hdr + 36, "data", 4);
    hdr[40] = data_bytes & 0xff; hdr[41] = (data_bytes >> 8) & 0xff;
    hdr[42] = (data_bytes >> 16) & 0xff; hdr[43] = (data_bytes >> 24) & 0xff;
}

// Reads the full HTTP response body (after fetch_headers) into a heap_caps
// buffer, growing capacity as needed. Caller frees with heap_caps_free().
// Used for both endpoints since neither response size is known up front on
// the ESP32 side (content_length is trusted when present, -1 otherwise).
esp_err_t read_full_response(esp_http_client_handle_t client, uint8_t** out_buf, size_t* out_len) {
    int64_t content_length = esp_http_client_fetch_headers(client);
    size_t cap = content_length > 0 ? static_cast<size_t>(content_length) : (64 * 1024);
    uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM));
    if (!buf) return ESP_ERR_NO_MEM;
    size_t total = 0;
    for (;;) {
        if (total == cap) {
            size_t new_cap = cap * 2;
            uint8_t* grown = static_cast<uint8_t*>(heap_caps_malloc(new_cap, MALLOC_CAP_SPIRAM));
            if (!grown) { heap_caps_free(buf); return ESP_ERR_NO_MEM; }
            std::memcpy(grown, buf, total);
            heap_caps_free(buf);
            buf = grown; cap = new_cap;
        }
        int r = esp_http_client_read(client, reinterpret_cast<char*>(buf + total), cap - total);
        if (r < 0) { heap_caps_free(buf); return ESP_FAIL; }
        if (r == 0) {
            // A zero read is not necessarily EOF: on a long transfer the socket
            // can simply have nothing buffered yet. Trust the declared length,
            // or the completion flag, instead -- otherwise large bodies (a
            // 600 KB TTS WAV, say) get silently truncated mid-stream.
            if (content_length > 0 && total < static_cast<size_t>(content_length)) continue;
            if (!esp_http_client_is_complete_data_received(client)) continue;
            break;
        }
        total += static_cast<size_t>(r);
    }
    *out_buf = buf; *out_len = total;
    return ESP_OK;
}

// POST a WAV file as multipart/form-data field "file" to the cloud ASR
// endpoint (see common_api_manager/modules/asr_transcribe/router.py) and
// extract the recognized "text" field from the JSON response.
esp_err_t asr_transcribe(const uint8_t* wav, size_t wav_len, std::string* out_text) {
    std::string prefix = std::string("--") + kBoundary +
        "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"rec.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    std::string suffix = std::string("\r\n--") + kBoundary + "--\r\n";
    const size_t total_len = prefix.size() + wav_len + suffix.size();

    esp_http_client_config_t config = {};
    config.url = kAsrUrl;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 20000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;
    std::string content_type = std::string("multipart/form-data; boundary=") + kBoundary;
    esp_http_client_set_header(client, "Content-Type", content_type.c_str());

    esp_err_t err = esp_http_client_open(client, static_cast<int>(total_len));
    if (err != ESP_OK) { esp_http_client_cleanup(client); return err; }
    if (esp_http_client_write(client, prefix.data(), prefix.size()) < 0 ||
        esp_http_client_write(client, reinterpret_cast<const char*>(wav), wav_len) < 0 ||
        esp_http_client_write(client, suffix.data(), suffix.size()) < 0) {
        esp_http_client_close(client); esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    uint8_t* resp = nullptr; size_t resp_len = 0;
    err = read_full_response(client, &resp, &resp_len);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client); esp_http_client_cleanup(client);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "ASR http status=%d resp_len=%u", status, static_cast<unsigned>(resp_len));
    if (status < 200 || status >= 300) { heap_caps_free(resp); return ESP_FAIL; }

    cJSON* root = cJSON_ParseWithLength(reinterpret_cast<const char*>(resp), resp_len);
    heap_caps_free(resp);
    if (!root) return ESP_FAIL;
    cJSON* text_item = cJSON_GetObjectItem(root, "text");
    *out_text = cJSON_IsString(text_item) ? text_item->valuestring : "";
    cJSON_Delete(root);
    return ESP_OK;
}

// POST {"text": ...} to audio_interact's /api/tts and return the raw WAV
// response body (heap_caps-allocated; caller frees). Same endpoint the
// browser console's "TTS 播报" test button already uses.
esp_err_t tts_synthesize(const std::string& text, uint8_t** out_wav, size_t* out_len) {
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "text", text.c_str());
    char* body_s = cJSON_PrintUnformatted(body);
    std::string body_str = body_s ? body_s : "{}";
    cJSON_free(body_s);
    cJSON_Delete(body);

    esp_http_client_config_t config = {};
    config.url = kTtsUrl;
    config.method = HTTP_METHOD_POST;
    // Synthesis is slow and scales with the text: ~13 s for 30 characters,
    // ~25 s for 60. The old 20 s budget silently failed anything but a short
    // phrase, so allow a full sentence to come back.
    config.timeout_ms = 90000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(client, static_cast<int>(body_str.size()));
    if (err != ESP_OK) { esp_http_client_cleanup(client); return err; }
    if (esp_http_client_write(client, body_str.data(), body_str.size()) < 0) {
        esp_http_client_close(client); esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    uint8_t* resp = nullptr; size_t resp_len = 0;
    err = read_full_response(client, &resp, &resp_len);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client); esp_http_client_cleanup(client);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "TTS http status=%d resp_len=%u", status, static_cast<unsigned>(resp_len));
    if (status < 200 || status >= 300 || resp_len < 44) { heap_caps_free(resp); return ESP_FAIL; }
    *out_wav = resp; *out_len = resp_len;
    return ESP_OK;
}

}  // namespace

esp_err_t AudioPlayer::fetch_tts(const std::string& text, uint8_t** wav, size_t* len) {
    return tts_synthesize(text, wav, len);
}

esp_err_t AudioPlayer::run_mic_asr_test(int seconds, uint8_t volume_percent) {
    if (!codec_) return ESP_ERR_INVALID_STATE;
    const int sample_rate = AudioCodec_GetInputSampleRate(codec_);
    const int channels = AudioCodec_GetInputChannels(codec_);  // 2: [mic, echo-ref]
    const size_t frames = static_cast<size_t>(sample_rate) * seconds;
    const size_t raw_samples = frames * channels;

    int16_t* raw = static_cast<int16_t*>(heap_caps_malloc(raw_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (!raw) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "mic_asr_test: recording %ds (rate=%d ch=%d frames=%u)",
             seconds, sample_rate, channels, static_cast<unsigned>(frames));
    const int64_t rec_started_us = esp_timer_get_time();
    bool ok = AudioCodec_InputData(codec_, raw, raw_samples);
    ESP_LOGI(TAG, "mic_asr_test: record done ok=%d elapsed_ms=%lld",
             ok, (esp_timer_get_time() - rec_started_us) / 1000);
    if (!ok) { heap_caps_free(raw); return ESP_FAIL; }

    // Downmix: keep channel 0 (mic), drop channel 1 (echo reference).
    const size_t wav_data_bytes = frames * sizeof(int16_t);
    uint8_t* wav = static_cast<uint8_t*>(heap_caps_malloc(44 + wav_data_bytes, MALLOC_CAP_SPIRAM));
    if (!wav) { heap_caps_free(raw); return ESP_ERR_NO_MEM; }
    write_wav_header(wav, static_cast<uint32_t>(wav_data_bytes), static_cast<uint32_t>(sample_rate));
    int16_t* mono = reinterpret_cast<int16_t*>(wav + 44);
    for (size_t i = 0; i < frames; ++i) mono[i] = raw[i * channels + 0];
    heap_caps_free(raw);

    std::string asr_text;
    esp_err_t err = asr_transcribe(wav, 44 + wav_data_bytes, &asr_text);
    heap_caps_free(wav);
    if (err != ESP_OK) {
        last_error_ = std::string("asr_failed:") + esp_err_to_name(err);
        return err;
    }
    last_asr_text_ = asr_text;
    ESP_LOGI(TAG, "mic_asr_test: ASR text=\"%s\"", asr_text.c_str());
    if (asr_text.empty()) asr_text = "没有识别到内容";

    uint8_t* tts_wav = nullptr; size_t tts_len = 0;
    err = tts_synthesize(std::string("你说的是：") + asr_text, &tts_wav, &tts_len);
    if (err != ESP_OK) {
        last_error_ = std::string("tts_failed:") + esp_err_to_name(err);
        return err;
    }

    AudioCodec_EnableOutput(codec_, true);
    gpio_set_level(AUDIO_CODEC_PA_PIN, 1);
    AudioCodec_SetOutputVolume(codec_, volume_percent);
    const int16_t* pcm = reinterpret_cast<const int16_t*>(tts_wav + 44);
    const size_t pcm_samples = (tts_len - 44) / sizeof(int16_t);
    const size_t chunk = 1024;
    for (size_t off = 0; off < pcm_samples; off += chunk) {
        const size_t n = std::min(chunk, pcm_samples - off);
        AudioCodec_OutputData(codec_, pcm + off, n);
        vTaskDelay(1);
    }
    AudioCodec_EnableOutput(codec_, false);
    heap_caps_free(tts_wav);
    ESP_LOGI(TAG, "mic_asr_test: done, spoke reply back");
    return ESP_OK;
}

namespace {
double channel_rms(const int16_t* interleaved, size_t frames, int channels, int ch) {
    double sum_sq = 0.0;
    for (size_t i = 0; i < frames; ++i) {
        const double s = interleaved[i * channels + ch];
        sum_sq += s * s;
    }
    return frames ? std::sqrt(sum_sq / frames) : 0.0;
}
}  // namespace

esp_err_t AudioPlayer::run_aec_reference_probe() {
    if (!codec_) return ESP_ERR_INVALID_STATE;
    const int sample_rate = AudioCodec_GetInputSampleRate(codec_);
    const int channels = AudioCodec_GetInputChannels(codec_);
    ESP_LOGI(TAG, "aec_probe: input_channels=%d input_reference=%d",
             channels, AudioCodec_GetInputReference(codec_));
    if (channels < 2) {
        ESP_LOGW(TAG, "aec_probe: only %d input channel(s), no reference channel to test", channels);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const size_t chunk_frames = 1024;
    int16_t* buf = static_cast<int16_t*>(heap_caps_malloc(chunk_frames * channels * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (!buf) return ESP_ERR_NO_MEM;

    // --- Phase 1: silence baseline (nothing playing) ---
    AudioCodec_EnableOutput(codec_, false);
    double silent_sum0 = 0, silent_sum1 = 0;
    const int silent_chunks = (sample_rate / 2) / chunk_frames + 1;  // ~0.5s
    for (int i = 0; i < silent_chunks; ++i) {
        AudioCodec_InputData(codec_, buf, chunk_frames * channels);
        silent_sum0 += channel_rms(buf, chunk_frames, channels, 0);
        silent_sum1 += channel_rms(buf, chunk_frames, channels, 1);
    }
    const double silent_rms0 = silent_sum0 / silent_chunks;
    const double silent_rms1 = silent_sum1 / silent_chunks;

    // --- Phase 2: loud 1kHz tone playing, read input concurrently ---
    AudioCodec_EnableOutput(codec_, true);
    gpio_set_level(AUDIO_CODEC_PA_PIN, 1);
    AudioCodec_SetOutputVolume(codec_, 80);
    int16_t* tone = static_cast<int16_t*>(heap_caps_malloc(chunk_frames * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    double play_sum0 = 0, play_sum1 = 0;
    const int play_chunks = (sample_rate * 3 / 2) / chunk_frames + 1;  // ~1.5s
    static double phase = 0.0;
    const double phase_inc = 2.0 * M_PI * 1000.0 / sample_rate;  // 1kHz tone
    for (int i = 0; i < play_chunks; ++i) {
        for (size_t n = 0; n < chunk_frames; ++n) {
            tone[n] = static_cast<int16_t>(20000.0 * std::sin(phase));
            phase += phase_inc;
        }
        AudioCodec_OutputData(codec_, tone, chunk_frames);
        AudioCodec_InputData(codec_, buf, chunk_frames * channels);
        // Skip the first couple of chunks: I2S DMA needs a moment for the
        // tone to actually reach the speaker/loop back before reads reflect it.
        if (i >= 2) {
            play_sum0 += channel_rms(buf, chunk_frames, channels, 0);
            play_sum1 += channel_rms(buf, chunk_frames, channels, 1);
        }
    }
    const int counted = play_chunks - 2;
    const double play_rms0 = play_sum0 / counted;
    const double play_rms1 = play_sum1 / counted;
    AudioCodec_EnableOutput(codec_, false);
    heap_caps_free(tone);
    heap_caps_free(buf);

    last_probe_result_ = {
        static_cast<float>(silent_rms0), static_cast<float>(silent_rms1),
        static_cast<float>(play_rms0), static_cast<float>(play_rms1),
        (play_rms1 > silent_rms1 * 3.0 && play_rms1 > 50.0),
    };

    ESP_LOGI(TAG, "aec_probe: SILENCE  ch0(mic)=%.1f ch1(ref)=%.1f", silent_rms0, silent_rms1);
    ESP_LOGI(TAG, "aec_probe: PLAYING  ch0(mic)=%.1f ch1(ref)=%.1f", play_rms0, play_rms1);
    ESP_LOGI(TAG, "aec_probe: ch1 delta=%.1f -> %s",
             play_rms1 - silent_rms1,
             (play_rms1 > silent_rms1 * 3.0 && play_rms1 > 50.0)
                 ? "REFERENCE CHANNEL IS REAL (hardware AEC wired)"
                 : "no correlation -- reference channel looks unconnected/floating");
    return ESP_OK;
}
