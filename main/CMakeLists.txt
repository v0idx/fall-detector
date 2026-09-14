idf_component_register(SRCS "main.c"
                    REQUIRES mpu6500_driver esp_driver_gpio mqtt esp_netif esp_wifi nvs_flash
                    INCLUDE_DIRS ".")
