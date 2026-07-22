cmake_minimum_required(VERSION 3.16)
project(rpi_vms LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0)

add_library(rpi_vms_core STATIC
    src/config/app_config.cpp
    src/logging/logger.cpp
    src/db/recording_index.cpp
    src/media/channel_ingest.cpp
    src/storage/storage_manager.cpp
)

target_include_directories(rpi_vms_core PUBLIC include ${GSTREAMER_INCLUDE_DIRS})
target_link_directories(rpi_vms_core PUBLIC ${GSTREAMER_LIBRARY_DIRS})
target_compile_options(rpi_vms_core PRIVATE ${GSTREAMER_CFLAGS_OTHER})
target_link_libraries(rpi_vms_core PUBLIC ${GSTREAMER_LIBRARIES} sqlite3 Threads::Threads)

add_executable(rpi_vms src/main.cpp)
target_link_libraries(rpi_vms PRIVATE rpi_vms_core)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rpi_vms_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(rpi_vms PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(channel_ingest_test tests/channel_ingest_test.cpp)
    target_link_libraries(channel_ingest_test PRIVATE rpi_vms_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(channel_ingest_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME channel_ingest COMMAND channel_ingest_test)
endif()
