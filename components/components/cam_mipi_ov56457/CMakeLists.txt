idf_component_register(SRCS "camera.c"
                       INCLUDE_DIRS "." "include"
                       REQUIRES esp_driver_cam esp_driver_isp esp_driver_i2c
                                esp_cam_sensor esp_sccb_intf esp_hw_support heap freertos)
