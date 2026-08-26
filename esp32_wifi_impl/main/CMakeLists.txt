idf_component_register(
    SRCS "app_main.cpp" "device_hub_client.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif esp_http_client json nvs_flash driver
)
