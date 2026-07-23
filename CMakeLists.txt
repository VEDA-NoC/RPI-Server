cmake_minimum_required(VERSION 3.16)
project(rpi_vms LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0)

add_library(rpi_vms_core STATIC
    src/config/app_config.cpp
    src/logging/logger.cpp
    src/db/recording_index.cpp
    src/media/channel_ingest.cpp
    src/media/channel_manager.cpp
    src/live/live_rtsp_server.cpp
    src/rtsp/rtsp_message_parser.cpp
    src/storage/storage_manager.cpp
)

target_include_directories(rpi_vms_core PUBLIC include ${GSTREAMER_INCLUDE_DIRS})
target_link_directories(rpi_vms_core PUBLIC ${GSTREAMER_LIBRARY_DIRS})
target_compile_options(rpi_vms_core PRIVATE ${GSTREAMER_CFLAGS_OTHER})
target_link_libraries(rpi_vms_core PUBLIC ${GSTREAMER_LIBRARIES} sqlite3 Threads::Threads OpenSSL::SSL OpenSSL::Crypto)

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

    add_executable(channel_manager_test tests/channel_manager_test.cpp)
    target_link_libraries(channel_manager_test PRIVATE rpi_vms_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(channel_manager_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME channel_manager COMMAND channel_manager_test)

    add_executable(live_rtsp_server_test tests/live_rtsp_server_test.cpp)
    target_link_libraries(live_rtsp_server_test PRIVATE rpi_vms_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(live_rtsp_server_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME live_rtsp_server COMMAND live_rtsp_server_test)

    find_program(BASH_EXECUTABLE bash)
    find_program(PYTHON3_EXECUTABLE python3)
    find_program(OPENSSL_EXECUTABLE openssl)
    if(BASH_EXECUTABLE AND PYTHON3_EXECUTABLE AND OPENSSL_EXECUTABLE)
        add_test(
            NAME m4_rtsps_routing
            COMMAND ${CMAKE_COMMAND} -E env M4_APP=$<TARGET_FILE:rpi_vms>
                    ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test-m4-rtsps-routing.sh
        )
        set_tests_properties(m4_rtsps_routing PROPERTIES TIMEOUT 30)
    endif()

    add_executable(rtsps_test_client tools/test_rtsps_client.cpp)
    target_link_libraries(rtsps_test_client PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(rtsps_test_client PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()
