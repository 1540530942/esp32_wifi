#include "device_hub_client.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include <cstring>
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

esp_err_t DeviceHubClient::post_json(const std::string& path, const std::string& body,
                                     std::string* response) {
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
            HubCommand cmd{ id->valuestring, action->valuestring,
                            args_s ? args_s : "{}" };
            cJSON_free(args_s);
            ESP_LOGI(TAG, "command: %s id=%s", cmd.action.c_str(), cmd.id.c_str());
            std::string status = command_handler_ ? command_handler_(cmd) : "unsupported";
            acknowledge(cmd, status, nullptr);
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
