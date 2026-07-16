cmake_minimum_required(VERSION 3.16)
project(rpi_vms LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0)

add_executable(rpi_vms
    src/main.cpp
    src/config/app_config.cpp
    src/logging/logger.cpp
    src/db/recording_index.cpp
    src/media/gstreamer_recorder.cpp
    src/storage/storage_manager.cpp
)

target_include_directories(rpi_vms PRIVATE include ${GSTREAMER_INCLUDE_DIRS})
target_link_directories(rpi_vms PRIVATE ${GSTREAMER_LIBRARY_DIRS})
target_compile_options(rpi_vms PRIVATE ${GSTREAMER_CFLAGS_OTHER})
target_link_libraries(rpi_vms PRIVATE ${GSTREAMER_LIBRARIES} sqlite3 Threads::Threads)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rpi_vms PRIVATE -Wall -Wextra -Wpedantic)
endif()

