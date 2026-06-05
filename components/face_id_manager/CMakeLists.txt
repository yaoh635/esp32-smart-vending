idf_component_register(
    SRCS "face_id_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES fatfs vfs esp_driver_sdmmc sdmmc esp_timer esp_driver_gpio esp_hw_support
)
