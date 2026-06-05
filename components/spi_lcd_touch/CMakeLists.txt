idf_component_register(
    SRCS "spi_lcd_touch.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES driver esp_lcd esp_timer
)
