idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES 
        microlink 
        nvs_flash 
        esp_wifi 
        esp_netif 
        esp_event 
        esp_http_client 
        esp-tls
        json 
        esp_http_server
        driver
        esp_adc
        esp_driver_rmt
)