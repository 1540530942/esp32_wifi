#pragma once

#include "esp_err.h"
#include "esp_http_client.h"
#include <functional>
#include <string>

struct HubCommand {
    std::string id;       // platform field: "id" (NOT "command_id")
    std::string action;
    std::string args_json;
};

class DeviceHubClient {
public:
    // Return value: "done" | "failed" | "unsupported"
    using CommandHandler = std::function<std::string(const HubCommand&)>;
    using StateProvider  = std::function<std::string()>;

    DeviceHubClient(std::string base_url,
                    std::string device_id,
                    std::string device_name,
                    std::string firmware,
                    StateProvider   state_provider,
                    CommandHandler  command_handler);

    esp_err_t register_device();
    esp_err_t heartbeat();

private:
    esp_err_t post_json(const std::string& path, const std::string& body,
                        std::string* response);
    esp_err_t process_commands(const std::string& response);
    esp_err_t acknowledge(const HubCommand& cmd, const std::string& status,
                          const char* message = nullptr);

    std::string base_url_;
    std::string device_id_;
    std::string device_name_;
    std::string firmware_;
    StateProvider  state_provider_;
    CommandHandler command_handler_;
};
