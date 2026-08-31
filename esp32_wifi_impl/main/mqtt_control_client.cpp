#include "mqtt_control_client.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <utility>

static const char* TAG = "mqtt_control";

struct MqttCommandJob {
    MqttControlClient* owner;
    HubCommand command;
};

MqttControlClient::MqttControlClient(std::string broker_uri, std::string device_id,
                                     CommandHandler command_handler)
    : broker_uri_(std::move(broker_uri)), device_id_(std::move(device_id)),
      command_topic_("devices/" + device_id_ + "/command/#"),
      ack_topic_("devices/" + device_id_ + "/ack"),
      state_topic_("devices/" + device_id_ + "/state"),
      log_topic_("devices/" + device_id_ + "/log"),
      command_handler_(std::move(command_handler)) {}

MqttControlClient::~MqttControlClient() {
    stop();
}

esp_err_t MqttControlClient::start() {
    if (client_) return ESP_OK;
    esp_mqtt_client_config_t config = {};
    config.broker.address.uri = broker_uri_.c_str();
    config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    config.session.keepalive = 30;
    config.session.last_will.topic = state_topic_.c_str();
    config.session.last_will.msg = "{\"status\":\"offline\"}";
    config.session.last_will.qos = 1;
    config.session.last_will.retain = 1;
    config.network.reconnect_timeout_ms = 3000;
    config.network.timeout_ms = 8000;
    config.buffer.size = 4096;
    config.buffer.out_size = 4096;

    client_ = esp_mqtt_client_init(&config);
    if (!client_) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_mqtt_client_register_event(client_, MQTT_EVENT_ANY,
                                                    &MqttControlClient::event_handler,
                                                    this);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
        return err;
    }
    err = esp_mqtt_client_start(client_);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
    }
    return err;
}

esp_err_t MqttControlClient::stop() {
    if (!client_) return ESP_OK;
    esp_err_t err = esp_mqtt_client_stop(client_);
    esp_mqtt_client_destroy(client_);
    client_ = nullptr;
    return err;
}

void MqttControlClient::publish_progress(const std::string& command_id,
                                         const std::string& action,
                                         const std::string& message) {
    publish_status(command_id, "progress", message, action);
}

void MqttControlClient::event_handler(void* handler_args, esp_event_base_t,
                                      int32_t event_id, void* event_data) {
    auto* self = static_cast<MqttControlClient*>(handler_args);
    if (self) self->on_event(static_cast<esp_mqtt_event_handle_t>(event_data));
    (void)event_id;
}

void MqttControlClient::on_event(esp_mqtt_event_handle_t event) {
    if (!event) return;
    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
        connected_.store(true);
        ESP_LOGI(TAG, "connected broker=%s", broker_uri_.c_str());
        esp_mqtt_client_subscribe(client_, command_topic_.c_str(), 1);
        esp_mqtt_client_publish(client_, state_topic_.c_str(),
                                "{\"status\":\"online\"}", 0, 1, 1);
        break;
    case MQTT_EVENT_DATA: {
        const bool first = event->current_data_offset == 0;
        if (first) incoming_payload_.clear();
        if (event->data && event->data_len > 0) {
            incoming_payload_.append(event->data, event->data_len);
        }
        if (event->current_data_offset + event->data_len >= event->total_data_len) {
            on_command(incoming_payload_);
            incoming_payload_.clear();
        }
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        connected_.store(false);
        ESP_LOGW(TAG, "disconnected; MQTT client will reconnect");
        break;
    case MQTT_EVENT_ERROR:
        if (event->error_handle) {
            const auto* error = event->error_handle;
            ESP_LOGW(TAG, "MQTT error type=%d esp_err=0x%x tls_err=0x%x tls_flags=0x%x sock_errno=%d",
                     error->error_type, error->esp_tls_last_esp_err,
                     error->esp_tls_stack_err, error->esp_tls_cert_verify_flags,
                     error->esp_transport_sock_errno);
        } else {
            ESP_LOGW(TAG, "MQTT error (no error detail)");
        }
        break;
    default:
        break;
    }
}

void MqttControlClient::on_command(const std::string& payload) {
    if (payload.empty() || payload.find_first_not_of(" \t\r\n") == std::string::npos) {
        // MQTT retained-topic cleanup is represented by an empty retained
        // payload; it is not a device command.
        return;
    }
    cJSON* root = cJSON_Parse(payload.c_str());
    if (!root) {
        ESP_LOGW(TAG, "invalid command JSON");
        return;
    }
    cJSON* id = cJSON_GetObjectItem(root, "command_id");
    if (!cJSON_IsString(id)) id = cJSON_GetObjectItem(root, "id");
    cJSON* action = cJSON_GetObjectItem(root, "action");
    cJSON* args = cJSON_GetObjectItem(root, "args");
    if (!args) args = cJSON_GetObjectItem(root, "payload");
    cJSON* text = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(id) || !cJSON_IsString(action)) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "command missing command_id/id or action");
        return;
    }
    const std::string command_id = id->valuestring;
    if (!mark_command_seen(command_id)) {
        ESP_LOGW(TAG, "duplicate command ignored id=%s", command_id.c_str());
        cJSON_Delete(root);
        return;
    }
    char* args_json = args ? cJSON_PrintUnformatted(args) : nullptr;
    MqttCommandJob* job = new MqttCommandJob{
        this,
        HubCommand{id->valuestring, action->valuestring,
                   args_json ? args_json : "{}",
                   cJSON_IsString(text) ? text->valuestring : ""}
    };
    cJSON_free(args_json);
    cJSON_Delete(root);
    if (xTaskCreate(&MqttControlClient::command_task, "mqtt_cmd", 8192, job, 5,
                    nullptr) != pdPASS) {
        publish_status(job->command.id, "failed", "command task allocation failed");
        delete job;
    }
}

bool MqttControlClient::mark_command_seen(const std::string& command_id) {
    std::lock_guard<std::mutex> lock(seen_mutex_);
    if (seen_commands_.find(command_id) != seen_commands_.end()) return false;
    if (seen_commands_.size() >= 64) seen_commands_.erase(seen_commands_.begin());
    seen_commands_.insert(command_id);
    return true;
}

void MqttControlClient::command_task(void* arg) {
    auto* job = static_cast<MqttCommandJob*>(arg);
    if (!job || !job->owner) {
        vTaskDelete(nullptr);
        return;
    }
    MqttControlClient* owner = job->owner;
    owner->publish_status(job->command.id, "accepted", {}, job->command.action);
    const std::string result = owner->command_handler_
        ? owner->command_handler_(job->command) : "unsupported";
    const size_t separator = result.find('|');
    const std::string status = separator == std::string::npos ? result : result.substr(0, separator);
    const std::string message = separator == std::string::npos ? std::string() : result.substr(separator + 1);
    std::string stream_id;
    cJSON* args = cJSON_Parse(job->command.args_json.c_str());
    if (args) {
        cJSON* item = cJSON_GetObjectItem(args, "stream_id");
        if (cJSON_IsString(item)) stream_id = item->valuestring;
        cJSON_Delete(args);
    }
    owner->publish_status(job->command.id, status, message, job->command.action, stream_id);
    delete job;
    vTaskDelete(nullptr);
}

void MqttControlClient::publish_status(const std::string& command_id,
                                       const std::string& status,
                                       const std::string& message,
                                       const std::string& action,
                                       const std::string& stream_id) {
    if (!client_) return;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "command_id", command_id.c_str());
    cJSON_AddStringToObject(root, "status", status.c_str());
    if (!message.empty()) cJSON_AddStringToObject(root, "message", message.c_str());
    if (!action.empty()) cJSON_AddStringToObject(root, "action", action.c_str());
    if (!stream_id.empty()) cJSON_AddStringToObject(root, "stream_id", stream_id.c_str());
    if (status == "done" && action == "stream_prepare") {
        cJSON_AddStringToObject(root, "event", "playback_done");
    }
    char* json = cJSON_PrintUnformatted(root);
    if (json) {
        esp_mqtt_client_publish(client_, ack_topic_.c_str(), json, 0, 1, 0);
        cJSON_free(json);
    }
    cJSON_Delete(root);
}
