idf_component_register(
    SRCS "app_main.cpp" "device_hub_client.cpp" "audio_player.cpp"
         "mqtt_control_client.cpp"
         "audio/audio_codec.c" "audio/codecs/box_audio_codec.c"
    INCLUDE_DIRS "." "audio" "audio/codecs"
    REQUIRES esp_wifi esp_event esp_netif esp_http_client tcp_transport json nvs_flash driver mqtt
             mbedtls esp_timer esp_codec_dev
)
