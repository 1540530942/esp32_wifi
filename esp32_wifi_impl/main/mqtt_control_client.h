#pragma once

#include "device_hub_client.h"
#include "mqtt_client.h"
#include <functional>
#include <atomic>
#include <mutex>
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
    std::mutex seen_mutex_;
    std::set<std::string> seen_commands_;
};
