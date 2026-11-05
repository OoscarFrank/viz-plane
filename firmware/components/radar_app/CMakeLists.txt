file(GLOB FONTS fonts/*.c)
idf_component_register(
    SRCS "ui.c" "adsb.c" "assets/map_bg.c" "assets/icons.c" "assets/airports.c" ${FONTS}
    INCLUDE_DIRS "include" "."
    REQUIRES lvgl__lvgl json)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
