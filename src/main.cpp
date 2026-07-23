#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "rtsps/app_config.h"
#include "rtsps/channel_ingest.h"
#include "rtsps/channel_manager.h"
#include "rtsps/live_rtsp_server.h"
#include "rtsps/logger.h"
#include "rtsps/recording_index.h"
#include "rtsps/storage_manager.h"

namespace {

std::atomic<bool> g_running{true};

void handle_shutdown_signal(int) { g_running.store(false); }

struct VmsConfig {
    std::string camera_host;
    int camera_port = 554;
    std::string camera_user = "admin";
    std::string camera_password;
    bool camera_password_stdin = false;
    std::string camera_path_template = "/{camera_channel}/profile2/media.smp";
    int camera_channel = 0;
    bool legacy_mapping_set = false;
    std::string channel_map;
    std::string storage_root = "/mnt/vms-storage";
    std::string media_db;
    int channel_id = 1;
    int db_busy_timeout_ms = 5000;
    rtsps::VideoCodec codec = rtsps::VideoCodec::H264;
    int latency_ms = 200;
    int segment_seconds = 60;
    int reconnect_delay_ms = 2000;
    int ingest_startup_timeout_ms = 15000;
    std::string live_listen_host = "0.0.0.0";
    int rtsp_port = 8554;
    int rtsps_port = 0;
    std::string tls_cert_file;
    std::string tls_key_file;
    std::size_t live_client_queue_frames = 30;
    std::size_t live_client_queue_bytes = 8 * 1024 * 1024;
    std::size_t rtp_mtu = 1200;
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
    std::cout << "Usage: " << program << " --camera-host HOST --camera-password-stdin [options]\n"
              << "  --camera-host CAMERA_IP\n"
              << "  --camera-port 554\n"
              << "  --camera-user admin\n"
              << "  --camera-password PASSWORD (legacy; visible in process arguments)\n"
              << "  --camera-password-stdin\n"
              << "  --camera-path-template '/{camera_channel}/profile2/media.smp'\n"
              << "  --channel-map '0:1,1:2,2:3,3:4'\n"
              << "  --camera-channel 0 (legacy single-channel mapping)\n"
              << "  --storage-root /mnt/vms-storage\n"
              << "  --media-db /mnt/vms-storage/index/media.db\n"
              << "  --channel-id 1 (legacy single-channel mapping)\n"
              << "  --db-busy-timeout-ms 5000\n"
              << "  --codec h264|h265\n"
              << "  --latency-ms 200\n"
              << "  --segment-seconds 60\n"
              << "  --reconnect-delay-ms 2000\n"
              << "  --ingest-startup-timeout-ms 15000\n"
              << "  --live-listen-host 0.0.0.0\n"
              << "  --rtsp-port 8554 (0 disables plain RTSP)\n"
              << "  --rtsps-port 0 (0 disables RTSPS)\n"
              << "  --tls-cert certs/server.crt\n"
              << "  --tls-key certs/server.key\n"
              << "  --live-client-queue-frames 30\n"
              << "  --live-client-queue-bytes 8388608\n"
              << "  --rtp-mtu 1200\n"
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

        if (arg == "--camera-host")
            config.camera_host = need_value(arg);
        else if (arg == "--camera-port")
            config.camera_port = parse_int(arg, need_value(arg), 1, 65535);
        else if (arg == "--camera-user")
            config.camera_user = need_value(arg);
        else if (arg == "--camera-password")
            config.camera_password = need_value(arg);
        else if (arg == "--camera-password-stdin")
            config.camera_password_stdin = true;
        else if (arg == "--camera-path-template")
            config.camera_path_template = need_value(arg);
        else if (arg == "--camera-channel") {
            config.camera_channel = parse_int(arg, need_value(arg), 0, 3);
            config.legacy_mapping_set = true;
        } else if (arg == "--channel-map")
            config.channel_map = need_value(arg);
        else if (arg == "--rtsp-url")
            throw std::runtime_error("--rtsp-url is not used; pass camera fields instead");
        else if (arg == "--storage-root")
            config.storage_root = need_value(arg);
        else if (arg == "--media-db")
            config.media_db = need_value(arg);
        else if (arg == "--channel-id") {
            config.channel_id = parse_int(arg, need_value(arg), 1, 4);
            config.legacy_mapping_set = true;
        } else if (arg == "--db-busy-timeout-ms")
            config.db_busy_timeout_ms = parse_int(arg, need_value(arg), 0, 60000);
        else if (arg == "--codec")
            config.codec = rtsps::parse_video_codec(need_value(arg));
        else if (arg == "--latency-ms")
            config.latency_ms = parse_int(arg, need_value(arg), 0, 10000);
        else if (arg == "--segment-seconds")
            config.segment_seconds = parse_int(arg, need_value(arg), 1, 3600);
        else if (arg == "--reconnect-delay-ms")
            config.reconnect_delay_ms = parse_int(arg, need_value(arg), 0, 60000);
        else if (arg == "--ingest-startup-timeout-ms")
            config.ingest_startup_timeout_ms = parse_int(arg, need_value(arg), 1000, 300000);
        else if (arg == "--live-listen-host")
            config.live_listen_host = need_value(arg);
        else if (arg == "--rtsp-port")
            config.rtsp_port = parse_int(arg, need_value(arg), 0, 65535);
        else if (arg == "--rtsps-port")
            config.rtsps_port = parse_int(arg, need_value(arg), 0, 65535);
        else if (arg == "--tls-cert")
            config.tls_cert_file = need_value(arg);
        else if (arg == "--tls-key")
            config.tls_key_file = need_value(arg);
        else if (arg == "--live-client-queue-frames")
            config.live_client_queue_frames = parse_size(arg, need_value(arg));
        else if (arg == "--live-client-queue-bytes")
            config.live_client_queue_bytes = parse_size(arg, need_value(arg));
        else if (arg == "--rtp-mtu")
            config.rtp_mtu = parse_size(arg, need_value(arg));
        else if (arg == "--min-free-bytes")
            config.min_free_bytes = parse_size(arg, need_value(arg));
        else if (arg == "--require-storage-mount")
            config.require_storage_mount = true;
        else if (arg == "--log-level")
            config.log_level = rtsps::parse_log_level(need_value(arg));
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
    if (config.camera_host.find_first_of("/@ \t\r\n") != std::string::npos ||
        config.camera_host.find("://") != std::string::npos) {
        throw std::runtime_error("--camera-host must be a host without scheme, path, or userinfo");
    }
    if (config.camera_user.empty()) {
        throw std::runtime_error("--camera-user is required");
    }
    if (config.camera_password_stdin && !config.camera_password.empty()) {
        throw std::runtime_error("--camera-password and --camera-password-stdin are mutually exclusive");
    }
    if (config.camera_password_stdin) {
        if (!std::getline(std::cin, config.camera_password)) {
            throw std::runtime_error("failed to read camera password from stdin");
        }
        if (!config.camera_password.empty() && config.camera_password.back() == '\r') {
            config.camera_password.pop_back();
        }
    }
    if (config.camera_password.empty()) {
        throw std::runtime_error("--camera-password or --camera-password-stdin is required");
    }
    if (config.legacy_mapping_set && !config.channel_map.empty()) {
        throw std::runtime_error("--channel-map cannot be combined with --camera-channel or --channel-id");
    }
    if (config.channel_map.empty()) {
        config.channel_map = config.legacy_mapping_set
                                 ? std::to_string(config.camera_channel) + ":" + std::to_string(config.channel_id)
                                 : "0:1,1:2,2:3,3:4";
    }
    if (config.rtsp_port == 0 && config.rtsps_port == 0) {
        throw std::runtime_error("at least one of --rtsp-port or --rtsps-port must be enabled");
    }
    if (config.rtsps_port > 0 && (config.tls_cert_file.empty() || config.tls_key_file.empty())) {
        throw std::runtime_error("--rtsps-port requires --tls-cert and --tls-key");
    }
    if (config.live_client_queue_frames == 0 || config.live_client_queue_frames > 10000) {
        throw std::runtime_error("--live-client-queue-frames out of range");
    }
    if (config.live_client_queue_bytes == 0 || config.rtp_mtu < 256 || config.rtp_mtu > 65535) {
        throw std::runtime_error("live queue byte limit or RTP MTU out of range");
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

std::string build_camera_rtsp_location(const VmsConfig& config, int camera_channel_id) {
    const std::string camera_channel = std::to_string(camera_channel_id);
    std::string path = replace_all(config.camera_path_template, "{camera_channel}", camera_channel);
    path = replace_all(path, "{channel}", camera_channel);
    if (path.empty() || path.front() != '/') {
        path.insert(path.begin(), '/');
    }

    return "rtsp://" + config.camera_host + ":" + std::to_string(config.camera_port) + path;
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
                    " mount_point=" + std::string(status.is_mount_point ? "yes" : "no") + " message=" + status.message);
        if (status.state != rtsps::StorageState::Ready) {
            return 2;
        }

        rtsps::RecordingIndex index(config.media_db, logger, config.db_busy_timeout_ms);
        index.open();
        index.initialize_schema();

        const std::vector<rtsps::ChannelMapping> mappings = rtsps::parse_channel_map(config.channel_map);
        std::vector<rtsps::ChannelIngestConfig> ingest_configs;
        ingest_configs.reserve(mappings.size());

        rtsps::LiveRtspServerConfig live_config;
        live_config.listen_host = config.live_listen_host;
        live_config.rtsp_port = config.rtsp_port;
        live_config.rtsps_port = config.rtsps_port;
        live_config.tls_cert_file = config.tls_cert_file;
        live_config.tls_key_file = config.tls_key_file;
        live_config.client_queue_frames = config.live_client_queue_frames;
        live_config.client_queue_bytes = config.live_client_queue_bytes;
        live_config.rtp_mtu = config.rtp_mtu;
        for (const auto& mapping : mappings) {
            index.upsert_channel_mapping(mapping.channel_id, mapping.camera_channel);
            rtsps::ChannelIngestConfig ingest_config;
            ingest_config.rtsp_location = build_camera_rtsp_location(config, mapping.camera_channel);
            ingest_config.camera_user = config.camera_user;
            ingest_config.camera_password = config.camera_password;
            ingest_config.output_dir = rtsps::channel_output_dir(config.storage_root, mapping.channel_id);
            ingest_config.camera_channel = mapping.camera_channel;
            ingest_config.channel_id = mapping.channel_id;
            ingest_config.codec = config.codec;
            ingest_config.latency_ms = config.latency_ms;
            ingest_config.segment_seconds = config.segment_seconds;
            ingest_config.reconnect_delay_ms = config.reconnect_delay_ms;
            ingest_config.startup_timeout_ms = config.ingest_startup_timeout_ms;
            std::filesystem::create_directories(ingest_config.output_dir);
            logger.info("[channel] channel_id=" + std::to_string(mapping.channel_id) + " camera_channel=" +
                        std::to_string(mapping.camera_channel) + " storage=" + ingest_config.output_dir);
            ingest_configs.push_back(std::move(ingest_config));
            live_config.channels.push_back({mapping.channel_id, config.codec});
        }

        rtsps::LiveRtspServer live_server(live_config, logger, g_running);
        live_server.start();

        rtsps::ChannelManager channel_manager(std::move(ingest_configs), index, logger, g_running, &live_server);
        channel_manager.start();
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        channel_manager.stop();
        live_server.stop();
        for (const auto& channel : channel_manager.stats()) {
            logger.info("[channel] state=stopped channel_id=" + std::to_string(channel.mapping.channel_id) +
                        " camera_channel=" + std::to_string(channel.mapping.camera_channel) +
                        " attempts=" + std::to_string(channel.ingest.connection_attempts) +
                        " reconnects=" + std::to_string(channel.ingest.reconnects) +
                        " worker_failed=" + (channel.worker_failed ? "yes" : "no"));
        }
        const rtsps::LiveRtspServerStats live_stats = live_server.stats();
        logger.info("[live-server] sessions_created=" + std::to_string(live_stats.sessions_created) +
                    " sessions_closed=" + std::to_string(live_stats.sessions_closed) +
                    " first_rtp=" + std::to_string(live_stats.first_rtp_transmissions) +
                    " queue_drops=" + std::to_string(live_stats.queue_drops));
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
}
