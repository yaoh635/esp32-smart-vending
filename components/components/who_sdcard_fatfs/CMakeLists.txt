idf_component_register(
    SRCS "who_sdcard_fatfs.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_sdmmc sdmmc vfs fatfs
)
