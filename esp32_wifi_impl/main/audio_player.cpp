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
#include <algorithm>
#include <arpa/inet.h>
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

    // Cloud uses bounded 32768-byte PCM WebSocket frames. Allocate the
    // matching receive buffer on the heap (the playback task stack is only
    // 8 KiB) so each transport frame is drained in one read where possible.
    constexpr size_t kPcmReadBufferBytes = 32768;
    uint8_t* input = static_cast<uint8_t*>(heap_caps_calloc(
        1, kPcmReadBufferBytes, MALLOC_CAP_8BIT));
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
        const int samples = read_result / 2;
        const int64_t write_started_us = esp_timer_get_time();
        const int written = AudioCodec_OutputData(codec_, reinterpret_cast<int16_t*>(input), samples);
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
    return stop_requested_ ? ESP_ERR_INVALID_STATE : (read_result < 0 ? ESP_FAIL : ESP_OK);
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
    uint8_t input[1024] = {};
    int read = 0;
    const int scale = volume_percent;
    AudioCodec_EnableOutput(codec_, true);
    const esp_err_t pa_err = gpio_set_level(AUDIO_CODEC_PA_PIN, 1);
    ESP_LOGI(TAG, "WAV playback output enabled PA GPIO17=%d set_err=%s volume=%d",
             gpio_get_level(AUDIO_CODEC_PA_PIN), esp_err_to_name(pa_err), scale);
    AudioCodec_SetOutputVolume(codec_, scale);
    size_t total_bytes = 0;
    while (!stop_requested_ && (read = esp_http_client_read(client, reinterpret_cast<char*>(input), sizeof(input))) > 0) {
        total_bytes += static_cast<size_t>(read);
        int samples = read / 2;
        auto* pcm = reinterpret_cast<int16_t*>(input);
        for (int i = 0; i < samples; ++i) pcm[i] = static_cast<int16_t>((pcm[i] * scale) / 100);
        const int written = AudioCodec_OutputData(codec_, pcm, samples);
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
    return stop_requested_ ? ESP_ERR_INVALID_STATE : (err == ESP_OK ? ESP_OK : err);
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

    uint8_t* pcm_buffer = static_cast<uint8_t*>(heap_caps_malloc(
        audio_remaining, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pcm_buffer) {
        pcm_buffer = static_cast<uint8_t*>(heap_caps_malloc(audio_remaining, MALLOC_CAP_8BIT));
    }
    if (!pcm_buffer) {
        ESP_LOGE(TAG, "WAV PCM buffer allocation failed, bytes=%u", static_cast<unsigned>(audio_remaining));
        close(sock);
        return ESP_ERR_NO_MEM;
    }

    const int64_t download_started_us = esp_timer_get_time();
    size_t downloaded = 0;
    int read = 0;
    while (!stop_requested_ && downloaded < audio_remaining &&
           (read = read_some(reinterpret_cast<char*>(pcm_buffer + downloaded),
                             std::min(static_cast<size_t>(4096), audio_remaining - downloaded))) > 0) {
        downloaded += static_cast<size_t>(read);
        esp_task_wdt_reset();
        if ((downloaded & 0x3fff) < static_cast<size_t>(read)) {
            ESP_LOGI(TAG, "WAV raw download progress=%u/%u",
                     static_cast<unsigned>(downloaded), static_cast<unsigned>(audio_remaining));
        }
    }
    close(sock);
    if (stop_requested_) {
        heap_caps_free(pcm_buffer);
        return ESP_ERR_INVALID_STATE;
    }
    if (downloaded != audio_remaining) {
        ESP_LOGW(TAG, "WAV download incomplete bytes=%u/%u read_result=%d",
                 static_cast<unsigned>(downloaded), static_cast<unsigned>(audio_remaining), read);
        heap_caps_free(pcm_buffer);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WAV raw download_done bytes=%u download_ms=%lld",
             static_cast<unsigned>(downloaded),
             (esp_timer_get_time() - download_started_us) / 1000);

    const int scale = volume_percent;
    AudioCodec_EnableOutput(codec_, true);
    AudioCodec_SetOutputVolume(codec_, scale);
    const esp_err_t pa_err = gpio_set_level(AUDIO_CODEC_PA_PIN, 1);
    ESP_LOGI(TAG, "WAV raw playback output enabled PA GPIO17=%d set_err=%s volume=%d",
             gpio_get_level(AUDIO_CODEC_PA_PIN), esp_err_to_name(pa_err), scale);
    size_t played = 0;
    while (!stop_requested_ && played < downloaded) {
        const size_t chunk = std::min(static_cast<size_t>(1024), (downloaded - played) / 2);
        auto* pcm = reinterpret_cast<int16_t*>(pcm_buffer + played);
        for (size_t i = 0; i < chunk; ++i) pcm[i] = static_cast<int16_t>((pcm[i] * scale) / 100);
        const int written = AudioCodec_OutputData(codec_, pcm, chunk);
        if (written != static_cast<int>(chunk)) {
            ESP_LOGW(TAG, "I2S/codec short write requested=%u written=%d",
                     static_cast<unsigned>(chunk), written);
        }
        esp_task_wdt_reset();
        played += chunk * 2;
        vTaskDelay(1);
    }
    AudioCodec_EnableOutput(codec_, false);
    heap_caps_free(pcm_buffer);
    ESP_LOGI(TAG, "WAV raw stream bytes=%u read_result=%d final_PA_GPIO17=%d elapsed_ms=%lld",
             static_cast<unsigned>(played), read, gpio_get_level(AUDIO_CODEC_PA_PIN),
             (esp_timer_get_time() - started_us) / 1000);
    return stop_requested_ ? ESP_ERR_INVALID_STATE : (played == downloaded ? ESP_OK : ESP_FAIL);
}
