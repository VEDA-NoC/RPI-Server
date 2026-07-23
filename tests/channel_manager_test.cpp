#include "rtsps/channel_manager.h"

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::filesystem::path temporary_root(const std::string& name) {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() / (name + '-' + std::to_string(unique));
    std::filesystem::create_directories(root);
    return root;
}

int scalar_int(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK, "query prepare failed");
    require(sqlite3_step(statement) == SQLITE_ROW, "query did not return a row");
    const int value = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    return value;
}

void test_explicit_mapping_and_paths() {
    const auto mappings = rtsps::parse_channel_map("0:1,1:2,2:3,3:4");
    require(mappings.size() == 4, "four-channel mapping size is wrong");
    for (std::size_t index = 0; index < mappings.size(); ++index) {
        require(mappings[index].camera_channel == static_cast<int>(index), "camera mapping is not 0..3");
        require(mappings[index].channel_id == static_cast<int>(index + 1), "VMS mapping is not 1..4");
        const std::string path = rtsps::channel_output_dir("/mnt/vms-storage", mappings[index].channel_id);
        require(path.find("recordings/ch" + std::to_string(index + 1)) != std::string::npos,
                "recording path does not use VMS channel");
        require(path.find("ch0") == std::string::npos, "forbidden ch0 path was generated");
    }

    bool duplicate_rejected = false;
    try {
        (void)rtsps::parse_channel_map("0:1,1:1");
    } catch (const std::exception&) {
        duplicate_rejected = true;
    }
    require(duplicate_rejected, "duplicate VMS mapping was accepted");
}

void test_shared_writer_serialization_and_channel_ids() {
    const auto root = temporary_root("rpi-vms-channel-manager-db");
    rtsps::Logger logger(rtsps::LogLevel::Error);
    rtsps::RecordingIndex index((root / "media.db").string(), logger, 500);
    index.open();
    index.initialize_schema();

    std::vector<std::thread> writers;
    for (int channel_id = 1; channel_id <= 4; ++channel_id) {
        index.upsert_channel_mapping(channel_id, channel_id - 1);
        writers.emplace_back([&, channel_id]() {
            const auto path = root / ("ch" + std::to_string(channel_id) + ".mp4");
            {
                std::ofstream output(path);
                output << "segment";
            }
            index.mark_segment_opened(channel_id, path.string(), "mp4", "h264");
            index.mark_segment_closed(path.string());
        });
    }
    for (auto& writer : writers) writer.join();

    sqlite3* db = nullptr;
    require(sqlite3_open((root / "media.db").string().c_str(), &db) == SQLITE_OK, "verification DB open failed");
    require(scalar_int(db, "SELECT COUNT(*) FROM recording_segments WHERE channel_id BETWEEN 1 AND 4") == 4,
            "shared DB does not contain all VMS channel IDs");
    require(scalar_int(db, "SELECT COUNT(*) FROM recording_segments WHERE complete = 1") == 4,
            "closed segments were not marked complete");
    require(scalar_int(db, "SELECT COUNT(*) FROM recording_segments WHERE channel_id = 0") == 0,
            "DB contains forbidden channel_id=0");
    require(scalar_int(db,
                       "SELECT COUNT(*) FROM vms_channels "
                       "WHERE channel_id BETWEEN 1 AND 4 AND camera_channel = channel_id - 1") == 4,
            "DB does not contain the explicit camera-to-VMS mapping");
    require(scalar_int(db, "SELECT COUNT(*) FROM schema_migrations WHERE version = 2") == 1,
            "channel mapping migration was not recorded");
    sqlite3_close(db);
    std::filesystem::remove_all(root);
}

void test_busy_timeout_waits_for_external_writer() {
    const auto root = temporary_root("rpi-vms-channel-manager-busy");
    const std::string db_path = (root / "media.db").string();
    rtsps::Logger logger(rtsps::LogLevel::Error);
    rtsps::RecordingIndex index(db_path, logger, 1000);
    index.open();
    index.initialize_schema();

    sqlite3* locker = nullptr;
    require(sqlite3_open(db_path.c_str(), &locker) == SQLITE_OK, "locker DB open failed");
    require(sqlite3_exec(locker, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK,
            "failed to acquire external writer lock");

    std::atomic<bool> completed{false};
    const auto start = std::chrono::steady_clock::now();
    std::thread writer([&]() {
        index.mark_segment_opened(1, (root / "busy.mp4").string(), "mp4", "h264");
        completed.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    require(!completed.load(), "writer ignored the external SQLite lock");
    require(sqlite3_exec(locker, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK, "locker commit failed");
    writer.join();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    require(completed.load() && elapsed.count() >= 100 && elapsed.count() < 1000,
            "busy_timeout did not wait and recover within policy");
    require(index.busy_timeout_ms() == 1000, "configured busy timeout is not observable");

    sqlite3_close(locker);
    std::filesystem::remove_all(root);
}

void test_channel_failures_reconnect_independently() {
    const auto root = temporary_root("rpi-vms-channel-manager-workers");
    rtsps::Logger logger(rtsps::LogLevel::Error);
    rtsps::RecordingIndex index((root / "media.db").string(), logger, 100);
    index.open();
    index.initialize_schema();

    std::vector<rtsps::ChannelIngestConfig> configs;
    for (int channel_id = 1; channel_id <= 4; ++channel_id) {
        rtsps::ChannelIngestConfig config;
        config.rtsp_location = "rtsp://127.0.0.1:" + std::to_string(channel_id) + "/unreachable";
        config.camera_user = "unit-user";
        config.camera_password = "unit-password";
        config.output_dir = (root / ("ch" + std::to_string(channel_id))).string();
        config.camera_channel = channel_id - 1;
        config.channel_id = channel_id;
        config.reconnect_delay_ms = 10;
        configs.push_back(std::move(config));
    }

    std::atomic<bool> running{true};
    rtsps::ChannelManager manager(std::move(configs), index, logger, running, nullptr);
    manager.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    manager.stop();
    const auto stats = manager.stats();
    require(stats.size() == 4, "manager did not retain four workers");
    for (const auto& channel : stats) {
        require(channel.ingest.pipeline_errors >= 1, "one channel did not observe its own connection failure");
        require(channel.ingest.reconnects >= 1, "one channel did not independently reconnect");
        require(channel.ingest.connection_attempts >= 2, "one channel failure stopped retry progress");
        require(!channel.worker_failed, "recoverable connection failure escaped the channel worker");
    }
    std::filesystem::remove_all(root);
}

void test_channel_workers_start_with_configured_spacing() {
    const auto root = temporary_root("rpi-vms-channel-manager-start-delay");
    rtsps::Logger logger(rtsps::LogLevel::Error);
    rtsps::RecordingIndex index((root / "media.db").string(), logger, 100);
    index.open();
    index.initialize_schema();

    std::vector<rtsps::ChannelIngestConfig> configs;
    for (int channel_id = 1; channel_id <= 3; ++channel_id) {
        rtsps::ChannelIngestConfig config;
        config.rtsp_location = "rtsp://127.0.0.1:" + std::to_string(channel_id) + "/unreachable";
        config.camera_user = "unit-user";
        config.camera_password = "unit-password";
        config.output_dir = (root / ("ch" + std::to_string(channel_id))).string();
        config.camera_channel = channel_id - 1;
        config.channel_id = channel_id;
        config.reconnect_delay_ms = 10;
        configs.push_back(std::move(config));
    }

    std::atomic<bool> running{true};
    rtsps::ChannelManager manager(std::move(configs), index, logger, running, nullptr, 150);
    const auto start = std::chrono::steady_clock::now();
    manager.start();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    require(elapsed.count() >= 250 && elapsed.count() < 1000,
            "manager did not apply spacing between initial channel starts");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    manager.stop();

    const auto stats = manager.stats();
    require(stats.size() == 3, "start-delay manager did not retain three workers");
    for (const auto& channel : stats) {
        require(channel.ingest.connection_attempts >= 1, "start-delay manager did not start every worker");
    }
    std::filesystem::remove_all(root);
}

}  // namespace

int main() {
    test_explicit_mapping_and_paths();
    test_shared_writer_serialization_and_channel_ids();
    test_busy_timeout_waits_for_external_writer();
    test_channel_failures_reconnect_independently();
    test_channel_workers_start_with_configured_spacing();
    std::cout << "channel_manager_test: PASS\n";
    return 0;
}
