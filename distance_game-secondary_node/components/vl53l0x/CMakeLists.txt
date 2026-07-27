idf_component_register(
    SRCS
        "Api/core/src/vl53l0x_api.c"
        "Api/core/src/vl53l0x_api_core.c"
        "Api/core/src/vl53l0x_api_ranging.c"
        "Api/core/src/vl53l0x_api_calibration.c"
        "Api/core/src/vl53l0x_api_strings.c"
        "Api/platform/src/vl53l0x_i2c_platform.c"
        "Api/platform/src/vl53l0x_platform.c"
        "Api/platform/src/vl53l0x_platform_log.c"
    INCLUDE_DIRS
        "Api/core/inc"
        "Api/platform/inc"
    REQUIRES
        driver
        log
        freertos
)

# Keep vendor core sources untouched while allowing GCC's conservative
# maybe-uninitialized diagnostic in vl53l0x_api.c to remain a warning.
set_source_files_properties(
    "Api/core/src/vl53l0x_api.c"
    PROPERTIES COMPILE_OPTIONS "-Wno-error=maybe-uninitialized"
)
