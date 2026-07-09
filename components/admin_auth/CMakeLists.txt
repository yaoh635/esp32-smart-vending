idf_component_register(
    SRCS "admin_auth.c"
    INCLUDE_DIRS "include"
    REQUIRES fatfs vfs mbedtls
)
