idf_component_register(SRCS "hello_world_main.c"
                       PRIV_REQUIRES spi_flash
                       INCLUDE_DIRS ""
                       REQUIRES driver esp_timer esp_adc esp_lcd dht esp_wifi esp_event nvs_flash esp_netif mqtt log 
                    )
