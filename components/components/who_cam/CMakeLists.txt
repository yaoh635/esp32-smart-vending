set(include_dirs    .)
set(src_dirs        .)

set(requires esp_timer esp_lcd esp-dl)

if (IDF_TARGET STREQUAL "esp32s3")
    list(APPEND src_dirs who_s3_cam)
    list(APPEND include_dirs who_s3_cam)
elseif (IDF_TARGET STREQUAL "esp32p4")
    list(APPEND src_dirs who_ov5647_cam)
    list(APPEND include_dirs who_ov5647_cam)
endif()

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})
idf_component_optional_requires(PUBLIC esp-dl cam_mipi_ov56457)
