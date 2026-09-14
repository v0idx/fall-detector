idf_component_register(SRCS "driver_mpu6500_basic.c" "driver_mpu6500_interface_template.c" "driver_mpu6500.c"
                        REQUIRES "esp_driver_i2c"
                        INCLUDE_DIRS "include")
