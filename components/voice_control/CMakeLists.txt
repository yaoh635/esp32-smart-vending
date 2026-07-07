idf_component_register(
    SRCS "voice_control.c" "esp_board_init.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp-sr/src/include"
    REQUIRES esp-sr esp_codec_dev driver
)

component_compile_options(-w)
