idf_component_register(
    SRCS "app_main.cpp" "device_hub_client.cpp" "audio_player.cpp"
         "audio/audio_codec.c" "audio/codecs/box_audio_codec.c"
    INCLUDE_DIRS "." "audio" "audio/codecs"
    REQUIRES esp_wifi esp_event esp_netif esp_http_client json nvs_flash driver
             mbedtls esp_timer esp_codec_dev
)
