#include "rtsps/channel_manager.h"

#include <array>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace rtsps {
namespace {

int parse_mapping_number(const std::string& field, const std::string& value, int min_value, int max_value) {
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(value, &consumed);
        if (consumed != value.size() || parsed < min_value || parsed > max_value) {
            throw std::runtime_error("range");
        }
        return parsed;
    } catch (const std::exception&) {
        throw std::runtime_error("invalid " + field + " in --channel-map: " + value);
    }
}

}  // namespace

std::vector<ChannelMapping> parse_channel_map(const std::string& value) {
    if (value.empty()) {
        throw std::runtime_error("--channel-map must not be empty");
    }

    std::array<bool, 4> camera_seen{};
    std::array<bool, 5> channel_seen{};
    std::vector<ChannelMapping> mappings;
    std::size_t begin = 0;
    while (begin < value.size()) {
        const std::size_t comma = value.find(',', begin);
        const std::string item = value.substr(begin, comma == std::string::npos ? std::string::npos : comma - begin);
        const std::size_t separator = item.find(':');
        if (separator == std::string::npos || item.find(':', separator + 1) != std::string::npos) {
            throw std::runtime_error("--channel-map entries must use CAMERA:VMS");
        }
        ChannelMapping mapping;
        mapping.camera_channel = parse_mapping_number("camera channel", item.substr(0, separator), 0, 3);
        mapping.channel_id = parse_mapping_number("VMS channel", item.substr(separator + 1), 1, 4);
        if (camera_seen[static_cast<std::size_t>(mapping.camera_channel)]) {
            throw std::runtime_error("--channel-map contains a duplicate camera channel");
        }
        if (channel_seen[static_cast<std::size_t>(mapping.channel_id)]) {
            throw std::runtime_error("--channel-map contains a duplicate VMS channel");
        }
        camera_seen[static_cast<std::size_t>(mapping.camera_channel)] = true;
        channel_seen[static_cast<std::size_t>(mapping.channel_id)] = true;
        mappings.push_back(mapping);
        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1;
        if (begin == value.size()) {
            throw std::runtime_error("--channel-map must not end with a comma");
        }
    }
    return mappings;
}

std::string channel_output_dir(const std::string& storage_root, int channel_id) {
    if (channel_id < 1 || channel_id > 4) {
        throw std::runtime_error("channel_id out of range (1-4)");
    }
    return (std::filesystem::path(storage_root) / "recordings" / ("ch" + std::to_string(channel_id))).string();
}

struct ChannelManager::ChannelWorker {
    ChannelWorker(ChannelIngestConfig config, RecordingIndex& index, Logger& logger, std::atomic<bool>& running,
                  LiveFrameSink* live_sink)
        : mapping{config.camera_channel, config.channel_id},
          ingest(std::move(config), index, logger, running, live_sink) {}

    ChannelMapping mapping;
    ChannelIngest ingest;
    std::thread thread;
    mutable std::mutex state_mutex;
    bool failed = false;
    std::string error;
};

ChannelManager::ChannelManager(std::vector<ChannelIngestConfig> configs, RecordingIndex& index, Logger& logger,
                               std::atomic<bool>& running, LiveFrameSink* live_sink)
    : logger_(logger), running_(running) {
    if (configs.empty() || configs.size() > 4) {
        throw std::runtime_error("ChannelManager requires 1 to 4 channels");
    }
    std::array<bool, 4> camera_seen{};
    std::array<bool, 5> channel_seen{};
    for (auto& config : configs) {
        if (config.camera_channel < 0 || config.camera_channel > 3 || config.channel_id < 1 || config.channel_id > 4) {
            throw std::runtime_error("ChannelManager mapping out of range");
        }
        if (camera_seen[static_cast<std::size_t>(config.camera_channel)] ||
            channel_seen[static_cast<std::size_t>(config.channel_id)]) {
            throw std::runtime_error("ChannelManager channel mappings must be unique");
        }
        camera_seen[static_cast<std::size_t>(config.camera_channel)] = true;
        channel_seen[static_cast<std::size_t>(config.channel_id)] = true;
        workers_.push_back(std::make_unique<ChannelWorker>(std::move(config), index, logger_, running_, live_sink));
    }
}

ChannelManager::~ChannelManager() { stop(); }

void ChannelManager::start() {
    if (started_) {
        return;
    }
    started_ = true;
    for (auto& worker : workers_) {
        ChannelWorker* const current = worker.get();
        current->thread = std::thread([this, current]() {
            try {
                current->ingest.run();
            } catch (const std::exception& ex) {
                {
                    std::lock_guard<std::mutex> lock(current->state_mutex);
                    current->failed = true;
                    current->error = ex.what();
                }
                logger_.error("[channel] state=worker_failed channel_id=" +
                              std::to_string(current->mapping.channel_id) + " message=" + ex.what());
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lock(current->state_mutex);
                    current->failed = true;
                    current->error = "unknown";
                }
                logger_.error("[channel] state=worker_failed channel_id=" +
                              std::to_string(current->mapping.channel_id) + " message=unknown");
            }
        });
    }
}

void ChannelManager::stop() {
    if (!started_) {
        return;
    }
    running_.store(false);
    for (auto& worker : workers_) {
        if (worker->thread.joinable()) {
            worker->thread.join();
        }
    }
    started_ = false;
}

std::vector<ManagedChannelStats> ChannelManager::stats() const {
    std::vector<ManagedChannelStats> result;
    result.reserve(workers_.size());
    for (const auto& worker : workers_) {
        ManagedChannelStats item;
        item.mapping = worker->mapping;
        item.ingest = worker->ingest.stats();
        {
            std::lock_guard<std::mutex> lock(worker->state_mutex);
            item.worker_failed = worker->failed;
            item.error = worker->error;
        }
        result.push_back(std::move(item));
    }
    return result;
}

}  // namespace rtsps
