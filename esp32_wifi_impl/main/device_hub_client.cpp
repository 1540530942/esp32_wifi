#include "device_hub_client.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <utility>

static const char* TAG = "device_hub";

DeviceHubClient::DeviceHubClient(std::string base_url, std::string device_id,
                                 std::string device_name, std::string firmware,
                                 StateProvider state_provider,
                                 CommandHandler command_handler)
    : base_url_(std::move(base_url)), device_id_(std::move(device_id)),
      device_name_(std::move(device_name)), firmware_(std::move(firmware)),
      state_provider_(std::move(state_provider)),
      command_handler_(std::move(command_handler)) {}

static esp_err_t post_http_raw(const std::string& base_url, const std::string& path,
                               const std::string& body, std::string* response) {
    constexpr const char* prefix = "http://";
    if (base_url.rfind(prefix, 0) != 0) return ESP_ERR_INVALID_ARG;
    std::string authority = base_url.substr(std::strlen(prefix));
    std::string base_path;
    const size_t slash = authority.find('/');
    if (slash != std::string::npos) {
        base_path = authority.substr(slash);
        authority.resize(slash);
    }
    const size_t colon = authority.find(':');
    const int port = colon == std::string::npos ? 80 : std::atoi(authority.c_str() + colon + 1);
    if (colon != std::string::npos) authority.resize(colon);

    in_addr addr = {};
    if (inet_pton(AF_INET, authority.c_str(), &addr) != 1) return ESP_ERR_INVALID_ARG;
    const int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) return ESP_FAIL;
    timeval timeout = {.tv_sec = 8, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_in peer = {};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(static_cast<uint16_t>(port));
    peer.sin_addr = addr;
    esp_err_t result = ESP_FAIL;
    const int connect_result = connect(sock, reinterpret_cast<sockaddr*>(&peer), sizeof(peer));
    if (connect_result != 0) {
        ESP_LOGW(TAG, "raw connect %s:%d failed errno=%d", authority.c_str(), port, errno);
    } else {
        ESP_LOGI(TAG, "raw connected to %s:%d", authority.c_str(), port);
        std::string request = "POST " + base_path + path + " HTTP/1.0\r\nHost: " + authority +
                              "\r\nUser-Agent: esp32-wangyutang/1.0\r\n"
                              "Content-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\n\r\n" + body;
        size_t sent = 0;
        while (sent < request.size()) {
            const int n = send(sock, request.data() + sent, request.size() - sent, 0);
            if (n <= 0) {
                ESP_LOGW(TAG, "raw send failed errno=%d", errno);
                break;
            }
            sent += static_cast<size_t>(n);
        }
        if (sent == request.size()) {
            shutdown(sock, SHUT_WR);
            std::string raw;
            char buffer[1024];
            int n;
            size_t expected_body = 0;
            bool has_content_length = false;
            while ((n = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
                raw.append(buffer, n);
                const size_t header_end = raw.find("\r\n\r\n");
                if (!has_content_length && header_end != std::string::npos) {
                    const std::string headers = raw.substr(0, header_end);
                    const std::string marker = "Content-Length:";
                    const size_t pos = headers.find(marker);
                    if (pos != std::string::npos) {
                        expected_body = static_cast<size_t>(std::strtoul(
                            headers.c_str() + pos + marker.size(), nullptr, 10));
                        has_content_length = true;
                    }
                }
                if (has_content_length) {
                    const size_t body_start = raw.find("\r\n\r\n");
                    if (body_start != std::string::npos &&
                        raw.size() - body_start - 4 >= expected_body) break;
                }
            }
            ESP_LOGI(TAG, "raw recv ended n=%d errno=%d total=%u", n, errno,
                     static_cast<unsigned>(raw.size()));
            ESP_LOGI(TAG, "raw recv ended n=%d errno=%d total=%u", n, errno,
                     static_cast<unsigned>(raw.size()));
            ESP_LOGI(TAG, "raw response head: %.*s", (int)(raw.size() > 120 ? 120 : raw.size()), raw.c_str());
            const size_t header_end = raw.find("\r\n\r\n");
            if (header_end != std::string::npos && raw.size() >= 12 &&
                raw.compare(0, 7, "HTTP/1.") == 0) {
                const int status = std::atoi(raw.c_str() + 9);
                ESP_LOGI(TAG, "raw POST %s HTTP %d bytes=%u", path.c_str(), status,
                         static_cast<unsigned>(raw.size() - header_end - 4));
                if (status >= 200 && status < 300) {
                    if (response) *response = raw.substr(header_end + 4);
                    result = ESP_OK;
                }
            }
        }
    }
    close(sock);
    return result;
}

esp_err_t DeviceHubClient::post_json(const std::string& path, const std::string& body,
                                     std::string* response) {
    if (base_url_.rfind("http://", 0) == 0) {
        esp_err_t raw_err = post_http_raw(base_url_, path, body, response);
        if (raw_err != ESP_OK) ESP_LOGW(TAG, "raw POST %s failed: %s", path.c_str(),
                                        esp_err_to_name(raw_err));
        return raw_err;
    }
    std::string url = base_url_ + path;
    esp_http_client_config_t config = {};
    config.url              = url.c_str();
    config.method           = HTTP_METHOD_POST;
    // Cellular/consumer Wi-Fi paths can take several seconds to establish a
    // TLS connection. Keep the request bounded, but allow one slow handshake.
    config.timeout_ms       = 30000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.addr_type        = HTTP_ADDR_TYPE_INET;
    config.keep_alive_enable = false;
    config.user_agent       = "esp32-wangyutang/1.0";

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body.data(), (int)body.size());
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status < 200 || status >= 300) {
            ESP_LOGW(TAG, "POST %s returned HTTP %d", path.c_str(), status);
            err = ESP_FAIL;
        } else if (response) {
            int length = (int)esp_http_client_get_content_length(client);
            if (length > 0 && length < 16384) {
                response->resize(length);
                int read = esp_http_client_read_response(client, response->data(), length);
                if (read >= 0) response->resize((size_t)read);
            } else {
                response->clear();
            }
        }
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "POST %s failed: %s", path.c_str(), esp_err_to_name(err));
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t DeviceHubClient::register_device() {
    // Contract: {device_id, device_type, name, state:{firmware,...}}
    std::string state = state_provider_ ? state_provider_() : "{}";
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id",   device_id_.c_str());
    cJSON_AddStringToObject(root, "device_type", "esp32-s3");
    cJSON_AddStringToObject(root, "name",        device_name_.c_str());
    cJSON* state_json = cJSON_Parse(state.c_str());
    if (state_json) cJSON_AddItemToObject(root, "state", state_json);
    char* text = cJSON_PrintUnformatted(root);
    std::string body = text ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(root);
    std::string response;
    esp_err_t err = post_json("/register", body, &response);
    if (err == ESP_OK) ESP_LOGI(TAG, "registered OK");
    return err;
}

esp_err_t DeviceHubClient::log_event(const std::string& level,
                                      const std::string& message) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", device_id_.c_str());
    cJSON_AddStringToObject(root, "level", level.c_str());
    cJSON_AddStringToObject(root, "message", message.c_str());
    cJSON_AddNumberToObject(root, "ts", esp_timer_get_time() / 1000000.0);
    char* text = cJSON_PrintUnformatted(root);
    std::string body = text ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(root);
    return post_json("/log", body, nullptr);
}

esp_err_t DeviceHubClient::heartbeat() {
    std::string state = state_provider_ ? state_provider_() : "{}";
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", device_id_.c_str());
    cJSON* state_json = cJSON_Parse(state.c_str());
    if (state_json) cJSON_AddItemToObject(root, "state", state_json);
    char* text = cJSON_PrintUnformatted(root);
    std::string body = text ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(root);
    std::string response;
    esp_err_t err = post_json("/heartbeat", body, &response);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "heartbeat response bytes=%u body=%.*s",
                 static_cast<unsigned>(response.size()),
                 static_cast<int>(response.size() > 320 ? 320 : response.size()),
                 response.c_str());
        err = process_commands(response);
    }
    return err;
}

esp_err_t DeviceHubClient::process_commands(const std::string& response) {
    if (response.empty()) return ESP_OK;
    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) return ESP_OK;
    cJSON* commands = cJSON_GetObjectItem(root, "commands");
    if (cJSON_IsArray(commands)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, commands) {
            // Platform sends "id" (NOT "command_id") — critical field name
            cJSON* id     = cJSON_GetObjectItem(item, "id");
            if (!cJSON_IsString(id)) id = cJSON_GetObjectItem(item, "command_id");
            cJSON* action = cJSON_GetObjectItem(item, "action");
            if (!cJSON_IsString(id) || !cJSON_IsString(action)) {
                ESP_LOGW(TAG, "command missing id or action, skipping");
                continue;
            }
            cJSON* args    = cJSON_GetObjectItem(item, "args");
            if (!args) args = cJSON_GetObjectItem(item, "payload");
            char*  args_s  = args ? cJSON_PrintUnformatted(args) : nullptr;
            cJSON* text_item = cJSON_GetObjectItem(item, "text");
            HubCommand cmd{ id->valuestring, action->valuestring,
                            args_s ? args_s : "{}",
                            cJSON_IsString(text_item) ? text_item->valuestring : "" };
            cJSON_free(args_s);
            ESP_LOGI(TAG, "command: %s id=%s", cmd.action.c_str(), cmd.id.c_str());
            std::string status = command_handler_ ? command_handler_(cmd) : "unsupported";
            const esp_err_t ack_err = acknowledge(cmd, status, nullptr);
            std::string trace = "command_id=" + cmd.id + " action=" + cmd.action +
                                " status=" + status + " ack=" + esp_err_to_name(ack_err);
            if (!cmd.text.empty()) trace += " text=" + cmd.text;
            log_event(ack_err == ESP_OK ? "info" : "warn", trace);
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t DeviceHubClient::acknowledge(const HubCommand& cmd, const std::string& status,
                                       const char* message) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id",  device_id_.c_str());
    cJSON_AddStringToObject(root, "command_id", cmd.id.c_str());  // ack still uses command_id
    cJSON_AddStringToObject(root, "status",     status.c_str());
    if (message) cJSON_AddStringToObject(root, "message", message);
    char* text = cJSON_PrintUnformatted(root);
    std::string body = text ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(root);
    return post_json("/ack", body, nullptr);
}
