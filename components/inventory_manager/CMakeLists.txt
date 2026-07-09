idf_component_register(
    SRCS "inventory_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES fatfs vfs esp_timer freertos
)
