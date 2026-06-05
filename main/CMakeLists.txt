set(srcs "main.cpp")

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       REQUIRES
                           fatfs
                           sd_card
                           spi_lcd_touch
                           vending_machine_ui
                           face_id_manager
                           who_cam
                           who_frame_cap
                           who_detect
                           cam_mipi_ov56457
                           esp-dl
                           human_face_detect
                           human_face_recognition
                           esp_driver_gpio
                           esp_timer
                       WHOLE_ARCHIVE)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-attributes)
