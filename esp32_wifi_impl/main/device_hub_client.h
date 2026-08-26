#pragma once

#include "esp_err.h"
#include "esp_http_client.h"
#include <functional>
#include <string>

struct HubCommand {
    std::string id;
    std::string action;
    std::string payload_json;
};

class DeviceHubClient {
public:
    using StateProvider = std::function<std::string()>;
    using CommandHandler = std::function<bool(const HubCommand&)>;

    DeviceHubClient(std::string base_url, std::string token, std::string device_id,
                    std::string firmware, StateProvider state_provider,
                    CommandHandler command_handler);

    esp_err_t register_device();
    esp_err_t heartbeat();
    esp_err_t acknowledge(const HubCommand& command, bool success, const char* error = nullptr);

private:
    esp_err_t post_json(const std::string& path, const std::string& body, std::string* response);
    esp_err_t process_commands(const std::string& response);

    std::string base_url_;
    std::string token_;
    std::string device_id_;
    std::string firmware_;
    StateProvider state_provider_;
    CommandHandler command_handler_;
};
