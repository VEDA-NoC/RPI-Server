#include "rtsps/app_config.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace rtsps {
namespace {

int parse_int_option(const std::string& name, const std::string& value, int min_value, int max_value) {
    int parsed = 0;
    try {
        std::size_t consumed = 0;
        parsed = std::stoi(value, &consumed);
        if (consumed != value.size()) {
            throw std::invalid_argument("trailing characters");
        }
    } catch (const std::exception&) {
        throw std::runtime_error(name + " requires a valid number, got '" + value + "'");
    }
    if (parsed < min_value || parsed > max_value) {
        throw std::runtime_error(name + " out of range (" + std::to_string(min_value) + "-" +
                                 std::to_string(max_value) + "): " + std::to_string(parsed));
    }
    return parsed;
}

std::size_t parse_size_option(const std::string& name, const std::string& value, std::size_t min_value,
                              std::size_t max_value) {
    std::size_t parsed = 0;
    try {
        std::size_t consumed = 0;
        parsed = static_cast<std::size_t>(std::stoull(value, &consumed));
        if (consumed != value.size()) {
            throw std::invalid_argument("trailing characters");
        }
    } catch (const std::exception&) {
        throw std::runtime_error(name + " requires a valid number, got '" + value + "'");
    }
    if (parsed < min_value || parsed > max_value) {
        throw std::runtime_error(name + " out of range");
    }
    return parsed;
}

}  // namespace

LogLevel parse_log_level(const std::string& value) {
    if (value == "error") return LogLevel::Error;
    if (value == "warn" || value == "warning") return LogLevel::Warn;
    if (value == "info") return LogLevel::Info;
    if (value == "debug") return LogLevel::Debug;
    if (value == "trace") return LogLevel::Trace;
    throw std::runtime_error("unknown log level: " + value);
}

std::string to_string(LogLevel level) {
    switch (level) {
        case LogLevel::Error:
            return "error";
        case LogLevel::Warn:
            return "warn";
        case LogLevel::Info:
            return "info";
        case LogLevel::Debug:
            return "debug";
        case LogLevel::Trace:
            return "trace";
    }
    return "unknown";
}

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]\n"
              << "  --listen-host 0.0.0.0\n"
              << "  --listen-port 8554\n"
              << "  --listen-backlog 64\n"
              << "  --cert certs/server.crt\n"
              << "  --key certs/server.key\n"
              << "  --camera-host CAMERA_IP\n"
              << "  --camera-port 554\n"
              << "  --camera-user admin\n"
              << "  --camera-password 'password'\n"
              << "  --camera-path-template '/{channel}/profile2/media.smp'\n"
              << "  --channels 4\n"
              << "  --io-buffer-bytes 8192\n"
              << "  --max-rtsp-header-bytes 16384\n"
              << "  --max-rtsp-body-bytes 1048576\n"
              << "  --select-timeout-ms 1000\n"
              << "  --log-level error|warn|info|debug|trace\n";
}

AppConfig parse_args(int argc, char** argv) {
    AppConfig cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need_value = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + name);
            return argv[++i];
        };

        if (arg == "--listen-host")
            cfg.listen_host = need_value(arg);
        else if (arg == "--listen-port")
            cfg.listen_port = parse_int_option(arg, need_value(arg), 1, 65535);
        else if (arg == "--listen-backlog")
            cfg.listen_backlog = parse_int_option(arg, need_value(arg), 1, 4096);
        else if (arg == "--cert")
            cfg.cert_file = need_value(arg);
        else if (arg == "--key")
            cfg.key_file = need_value(arg);
        else if (arg == "--camera-host")
            cfg.camera_host = need_value(arg);
        else if (arg == "--camera-port")
            cfg.camera_port = parse_int_option(arg, need_value(arg), 1, 65535);
        else if (arg == "--camera-user")
            cfg.camera_user = need_value(arg);
        else if (arg == "--camera-password")
            cfg.camera_password = need_value(arg);
        else if (arg == "--camera-path-template")
            cfg.camera_path_template = need_value(arg);
        else if (arg == "--channels")
            cfg.channels = parse_int_option(arg, need_value(arg), 1, 64);
        else if (arg == "--io-buffer-bytes")
            cfg.io_buffer_bytes = parse_size_option(arg, need_value(arg), 1024, 1024 * 1024);
        else if (arg == "--max-rtsp-header-bytes")
            cfg.max_rtsp_header_bytes = parse_size_option(arg, need_value(arg), 1024, 1024 * 1024);
        else if (arg == "--max-rtsp-body-bytes")
            cfg.max_rtsp_body_bytes = parse_size_option(arg, need_value(arg), 0, 64 * 1024 * 1024);
        else if (arg == "--select-timeout-ms")
            cfg.select_timeout_ms = parse_int_option(arg, need_value(arg), 100, 60000);
        else if (arg == "--log-level")
            cfg.log_level = parse_log_level(need_value(arg));
        else if (arg == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else
            throw std::runtime_error("unknown option: " + arg);
    }
    return cfg;
}

}  // namespace rtsps
