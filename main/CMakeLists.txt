idf_component_register(
    SRCS "peripherals.c" "periodic.c" "nvs.c" "http.c" "ota.c" "app_state.c" "comms.c" "sensors.c" "network.c" "main.c" "s3.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "cert.pem"
    PRIV_REQUIRES  esp_https_ota app_update
    REQUIRES esp_eth esp_event esp_netif esp_http_client nvs_flash esp_driver_gpio esp_driver_spi esp-tls w5500 esp_driver_gptimer esp_driver_i2c esp_adc
)