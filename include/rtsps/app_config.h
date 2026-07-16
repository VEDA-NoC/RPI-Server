#pragma once

#include <cstddef>
#include <string>

namespace rtsps {

enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3, Trace = 4 };

LogLevel parse_log_level(const std::string& value);
std::string to_string(LogLevel level);

struct AppConfig {
    std::string listen_host = "0.0.0.0";
    int listen_port = 8554;
    int listen_backlog = 64;

    std::string cert_file = "certs/server.crt";
    std::string key_file = "certs/server.key";

    std::string camera_host;
    int camera_port = 554;
    std::string camera_user = "admin";
    std::string camera_password;
    std::string camera_path_template = "/{channel}/profile2/media.smp";
    int channels = 4;

    std::size_t io_buffer_bytes = 8192;
    std::size_t max_rtsp_header_bytes = 16 * 1024;
    std::size_t max_rtsp_body_bytes = 1024 * 1024;
    int select_timeout_ms = 1000;

    LogLevel log_level = LogLevel::Info;
};

AppConfig parse_args(int argc, char** argv);
void print_usage(const char* program_name);

}  // namespace rtsps
