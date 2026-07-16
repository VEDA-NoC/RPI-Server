#include "rtsps/app_config.h"
#include "rtsps/gstreamer_recorder.h"
#include "rtsps/logger.h"
#include "rtsps/recording_index.h"
#include "rtsps/storage_manager.h"

#include <atomic>
#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

std::atomic<bool> g_running{true};

void handle_shutdown_signal(int) {
    g_running.store(false);
}

struct VmsConfig {
    std::string camera_host = "CAMERA_IP";
    int camera_port = 554;
    std::string camera_user = "admin";
    std::string camera_password;
    std::string camera_path_template = "/{camera_channel}/profile2/media.smp";
    int camera_channel = 0;
    std::string storage_root = "/mnt/vms-storage";
    std::string media_db;
    int channel_id = 1;
    rtsps::VideoCodec codec = rtsps::VideoCodec::H264;
    int latency_ms = 200;
    int segment_seconds = 60;
    std::uintmax_t min_free_bytes = 1024ULL * 1024ULL * 1024ULL;
    bool require_storage_mount = false;
    rtsps::LogLevel log_level = rtsps::LogLevel::Info;
};

int parse_int(const std::string& name, const std::string& value, int min_value, int max_value) {
    int parsed = 0;
    try {
        std::size_t consumed = 0;
        parsed = std::stoi(value, &consumed);
        if (consumed != value.size()) {
            throw std::invalid_argument("trailing characters");
        }
    } catch (const std::exception&) {
        throw std::runtime_error(name + " requires a valid number");
    }
    if (parsed < min_value || parsed > max_value) {
        throw std::runtime_error(name + " out of range");
    }
    return parsed;
}

std::uintmax_t parse_size(const std::string& name, const std::string& value) {
    try {
        std::size_t consumed = 0;
        const auto parsed = static_cast<std::uintmax_t>(std::stoull(value, &consumed));
        if (consumed != value.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return parsed;
    } catch (const std::exception&) {
        throw std::runtime_error(name + " requires a valid number");
    }
}

void print_usage(const char* program) {
    std::cout
        << "Usage: " << program << " --camera-host HOST --camera-password PASSWORD [options]\n"
        << "  --camera-host CAMERA_IP\n"
        << "  --camera-port 554\n"
        << "  --camera-user admin\n"
        << "  --camera-password password\n"
        << "  --camera-path-template '/{camera_channel}/profile2/media.smp'\n"
        << "  --camera-channel 0\n"
        << "  --storage-root /mnt/vms-storage\n"
        << "  --media-db /mnt/vms-storage/index/media.db\n"
        << "  --channel-id 1\n"
        << "  --codec h264|h265\n"
        << "  --latency-ms 200\n"
        << "  --segment-seconds 60\n"
        << "  --min-free-bytes 1073741824\n"
        << "  --require-storage-mount\n"
        << "  --log-level error|warn|info|debug|trace\n";
}

VmsConfig parse_vms_args(int argc, char** argv) {
    VmsConfig config;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need_value = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for " + name);
            }
            return argv[++i];
        };

        if (arg == "--camera-host") config.camera_host = need_value(arg);
        else if (arg == "--camera-port") config.camera_port = parse_int(arg, need_value(arg), 1, 65535);
        else if (arg == "--camera-user") config.camera_user = need_value(arg);
        else if (arg == "--camera-password") config.camera_password = need_value(arg);
        else if (arg == "--camera-path-template") config.camera_path_template = need_value(arg);
        else if (arg == "--camera-channel") config.camera_channel = parse_int(arg, need_value(arg), 0, 3);
        else if (arg == "--rtsp-url") throw std::runtime_error("--rtsp-url is not used; pass camera fields instead");
        else if (arg == "--storage-root") config.storage_root = need_value(arg);
        else if (arg == "--media-db") config.media_db = need_value(arg);
        else if (arg == "--channel-id") config.channel_id = parse_int(arg, need_value(arg), 1, 4);
        else if (arg == "--codec") config.codec = rtsps::parse_video_codec(need_value(arg));
        else if (arg == "--latency-ms") config.latency_ms = parse_int(arg, need_value(arg), 0, 10000);
        else if (arg == "--segment-seconds") config.segment_seconds = parse_int(arg, need_value(arg), 1, 3600);
        else if (arg == "--min-free-bytes") config.min_free_bytes = parse_size(arg, need_value(arg));
        else if (arg == "--require-storage-mount") config.require_storage_mount = true;
        else if (arg == "--log-level") config.log_level = rtsps::parse_log_level(need_value(arg));
        else if (arg == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("unknown option: " + arg);
        }
    }

    if (config.camera_host.empty()) {
        throw std::runtime_error("--camera-host is required");
    }
    if (config.camera_user.empty()) {
        throw std::runtime_error("--camera-user is required");
    }
    if (config.camera_password.empty()) {
        throw std::runtime_error("--camera-password is required");
    }
    if (config.media_db.empty()) {
        config.media_db = (std::filesystem::path(config.storage_root) / "index" / "media.db").string();
    }
    return config;
}

std::string replace_all(std::string value, const std::string& from, const std::string& to) {
    std::size_t pos = 0;
    while ((pos = value.find(from, pos)) != std::string::npos) {
        value.replace(pos, from.size(), to);
        pos += to.size();
    }
    return value;
}

std::string url_escape_userinfo(const std::string& value) {
    std::string out;
    constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char ch : value) {
        const bool safe = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                          ch == '-' || ch == '.' || ch == '_' || ch == '~';
        if (safe) {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(hex[ch >> 4]);
            out.push_back(hex[ch & 0x0F]);
        }
    }
    return out;
}

std::string build_camera_rtsp_url(const VmsConfig& config) {
    const std::string camera_channel = std::to_string(config.camera_channel);
    std::string path = replace_all(config.camera_path_template, "{camera_channel}", camera_channel);
    path = replace_all(path, "{channel}", camera_channel);
    if (path.empty() || path.front() != '/') {
        path.insert(path.begin(), '/');
    }

    return "rtsp://" + url_escape_userinfo(config.camera_user) + ":" +
           url_escape_userinfo(config.camera_password) + "@" + config.camera_host + ":" +
           std::to_string(config.camera_port) + path;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, handle_shutdown_signal);
    std::signal(SIGTERM, handle_shutdown_signal);
    std::signal(SIGPIPE, SIG_IGN);

    try {
        VmsConfig config = parse_vms_args(argc, argv);
        rtsps::Logger logger(config.log_level);

        rtsps::StorageManager storage(config.storage_root, config.min_free_bytes, config.require_storage_mount, logger);
        const rtsps::StorageStatus status = storage.check();
        logger.info("[storage] state=" + rtsps::to_string(status.state) +
                    " mount_point=" + std::string(status.is_mount_point ? "yes" : "no") +
                    " message=" + status.message);
        if (status.state != rtsps::StorageState::Ready) {
            return 2;
        }

        rtsps::RecordingIndex index(config.media_db, logger);
        index.open();
        index.initialize_schema();

        const std::string output_dir =
            (std::filesystem::path(config.storage_root) / "recordings" /
             ("ch" + std::to_string(config.channel_id)))
                .string();
        logger.info("[channel] channel_id=" + std::to_string(config.channel_id) +
                    " camera_channel=" + std::to_string(config.camera_channel) +
                    " storage=" + output_dir);

        rtsps::GStreamerRecorderConfig recorder_config;
        recorder_config.rtsp_url = build_camera_rtsp_url(config);
        recorder_config.output_dir = output_dir;
        recorder_config.channel_id = config.channel_id;
        recorder_config.codec = config.codec;
        recorder_config.latency_ms = config.latency_ms;
        recorder_config.segment_seconds = config.segment_seconds;

        rtsps::GStreamerRecorder recorder(recorder_config, index, logger, g_running);
        return recorder.run();
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
}
