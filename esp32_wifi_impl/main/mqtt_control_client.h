#pragma once

#include "device_hub_client.h"
#include "mqtt_client.h"
#include <functional>
#include <atomic>
#include <mutex>
#include <map>
#include <set>
#include <string>

class MqttControlClient {
public:
    using CommandHandler = std::function<std::string(const HubCommand&)>;

    MqttControlClient(std::string broker_uri, std::string device_id,
                      CommandHandler command_handler);
    ~MqttControlClient();

    esp_err_t start();
    esp_err_t stop();
    bool is_connected() const { return connected_.load(); }
    uint32_t connect_attempts() const { return connect_attempts_.load(); }
    uint32_t disconnect_count() const { return disconnect_count_.load(); }
    int last_error_type() const { return last_error_type_.load(); }
    int last_esp_error() const { return last_esp_error_.load(); }
    int last_socket_errno() const { return last_socket_errno_.load(); }
    void publish_progress(const std::string& command_id, const std::string& action,
                          const std::string& message);

private:
    static void event_handler(void* handler_args, esp_event_base_t base,
                              int32_t event_id, void* event_data);
    static void command_task(void* arg);
    void on_event(esp_mqtt_event_handle_t event);
    void on_command(const std::string& payload);
    bool mark_command_seen(const std::string& command_id);
    void publish_status(const std::string& command_id, const std::string& status,
                        const std::string& message = {},
                        const std::string& action = {},
                        const std::string& stream_id = {});
    void flush_pending_status();

    std::string broker_uri_;
    std::string device_id_;
    std::string command_topic_;
    std::string ack_topic_;
    std::string state_topic_;
    std::string log_topic_;
    CommandHandler command_handler_;
    esp_mqtt_client_handle_t client_ = nullptr;
    std::string incoming_payload_;
    std::atomic<bool> connected_{false};
    std::atomic<uint32_t> connect_attempts_{0};
    std::atomic<uint32_t> disconnect_count_{0};
    std::atomic<int> last_error_type_{0};
    std::atomic<int> last_esp_error_{0};
    std::atomic<int> last_socket_errno_{0};
    std::mutex seen_mutex_;
    std::set<std::string> seen_commands_;
    std::mutex pending_status_mutex_;
    std::map<std::string, std::string> pending_status_;
};
