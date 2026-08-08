idf_component_register(
    SRCS
    "grape_storage_sd.c"

    INCLUDE_DIRS
    "."

    REQUIRES
    fatfs
    sdmmc
    esp_driver_sdmmc
)