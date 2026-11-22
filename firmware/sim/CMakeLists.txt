# Mac simulator: the same app as the ESP32, in an 800x480 SDL window.
cmake_minimum_required(VERSION 3.16)
project(radar_sim C)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

set(APP ${CMAKE_CURRENT_LIST_DIR}/../components/radar_app)
set(LVGL ${CMAKE_CURRENT_LIST_DIR}/../components/lvgl__lvgl)
file(GLOB_RECURSE LVGL_SRC ${LVGL}/src/*.c)
file(GLOB FONTS ${APP}/fonts/*.c)

add_library(lvgl STATIC ${LVGL_SRC})
target_include_directories(lvgl PUBLIC ${LVGL} ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_compile_options(lvgl PRIVATE -w)

add_executable(radar_sim main_sim.c plat_sim.c cjson/cJSON.c
  ${APP}/ui.c ${APP}/adsb.c ${APP}/assets/zones.c ${APP}/assets/icons.c ${APP}/assets/airports.c ${FONTS})
target_include_directories(radar_sim PRIVATE ${APP} ${APP}/include cjson ${SDL2_INCLUDE_DIRS})
target_link_directories(radar_sim PRIVATE ${SDL2_LIBRARY_DIRS})
target_link_libraries(radar_sim PRIVATE lvgl ${SDL2_LIBRARIES} CURL::libcurl ZLIB::ZLIB Threads::Threads m)
target_compile_options(radar_sim PRIVATE -Wall -Wextra -Wno-unused-parameter)
