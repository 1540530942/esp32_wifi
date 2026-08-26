#include "device_hub_client.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include <cstring>
#include <utility>

static const char* TAG = "device_hub";

DeviceHubClient::DeviceHubClient(std::string base_url, std::string token, std::string device_id,
                                 std::string firmware, StateProvider state_provider,
                                 CommandHandler command_handler)
    : base_url_(std::move(base_url)), token_(std::move(token)), device_id_(std::move(device_id)),
      firmware_(std::move(firmware)), state_provider_(std::move(state_provider)),
      command_handler_(std::move(command_handler)) {}

esp_err_t DeviceHubClient::post_json(const std::string& path, const std::string& body,
                                     std::string* response) {
    std::string url = base_url_ + path;
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 12000;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (!token_.empty()) {
        std::string auth = "Bearer " + token_;
        esp_http_client_set_header(client, "Authorization", auth.c_str());
    }
    esp_http_client_set_post_field(client, body.c_str(), body.size());

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        int length = esp_http_client_get_content_length(client);
        if (status < 200 || status >= 300) {
            ESP_LOGW(TAG, "POST %s returned HTTP %d", path.c_str(), status);
            err = ESP_FAIL;
        } else if (response && length > 0 && length < 16384) {
            response->resize(length);
            int read = esp_http_client_read_response(client, response->data(), length);
            if (read >= 0) response->resize(read);
        }
    } else {
        ESP_LOGW(TAG, "POST %s failed: %s", path.c_str(), esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t DeviceHubClient::register_device() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", device_id_.c_str());
    cJSON_AddStringToObject(root, "device_type", "esp32_xiaozhi");
    cJSON_AddStringToObject(root, "firmware", firmware_.c_str());
    cJSON_AddStringToObject(root, "capabilities", "audio_playback,microphone,display");
    char* text = cJSON_PrintUnformatted(root);
    std::string body = text ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(root);
    std::string response;
    return post_json("/register", body, &response);
}

esp_err_t DeviceHubClient::heartbeat() {
    std::string state = state_provider_ ? state_provider_() : "{}";
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", device_id_.c_str());
    cJSON* state_json = cJSON_Parse(state.c_str());
    if (state_json) cJSON_AddItemToObject(root, "state", state_json);
    else cJSON_AddStringToObject(root, "state", state.c_str());
    char* text = cJSON_PrintUnformatted(root);
    std::string body = text ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(root);
    std::string response;
    esp_err_t err = post_json("/heartbeat", body, &response);
    if (err == ESP_OK) err = process_commands(response);
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
            cJSON* id = cJSON_GetObjectItem(item, "command_id");
            cJSON* action = cJSON_GetObjectItem(item, "action");
            if (!cJSON_IsString(id) || !cJSON_IsString(action)) continue;
            char* payload = cJSON_PrintUnformatted(cJSON_GetObjectItem(item, "payload"));
            HubCommand command{ id->valuestring, action->valuestring, payload ? payload : "{}" };
            cJSON_free(payload);
            bool ok = command_handler_ ? command_handler_(command) : false;
            acknowledge(command, ok, ok ? nullptr : "unsupported_or_failed");
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t DeviceHubClient::acknowledge(const HubCommand& command, bool success, const char* error) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", device_id_.c_str());
    cJSON_AddStringToObject(root, "command_id", command.id.c_str());
    cJSON_AddStringToObject(root, "status", success ? "done" : "failed");
    if (error) cJSON_AddStringToObject(root, "error", error);
    char* text = cJSON_PrintUnformatted(root);
    std::string body = text ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(root);
    return post_json("/ack", body, nullptr);
}
