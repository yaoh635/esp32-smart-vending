idf_component_register(
    SRCS "order_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES fatfs vfs esp_timer freertos
)
