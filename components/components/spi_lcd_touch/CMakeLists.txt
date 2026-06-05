idf_component_register(
    SRCS "spi_lcd_touch.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl__lvgl
    PRIV_REQUIRES driver esp_lcd esp_timer
)
