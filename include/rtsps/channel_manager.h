#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rtsps/channel_ingest.h"
#include "rtsps/logger.h"
#include "rtsps/recording_index.h"

namespace rtsps {

struct ChannelMapping {
    int camera_channel = 0;
    int channel_id = 1;
};

std::vector<ChannelMapping> parse_channel_map(const std::string& value);
std::string channel_output_dir(const std::string& storage_root, int channel_id);

struct ManagedChannelStats {
    ChannelMapping mapping;
    ChannelIngestStats ingest;
    bool worker_failed = false;
    std::string error;
};

class ChannelManager {
public:
    ChannelManager(std::vector<ChannelIngestConfig> configs, RecordingIndex& index, Logger& logger,
                   std::atomic<bool>& running, LiveFrameSink* live_sink, int channel_start_delay_ms = 0);
    ~ChannelManager();

    ChannelManager(const ChannelManager&) = delete;
    ChannelManager& operator=(const ChannelManager&) = delete;

    void start();
    void stop();
    std::vector<ManagedChannelStats> stats() const;

private:
    struct ChannelWorker;

    Logger& logger_;
    std::atomic<bool>& running_;
    std::vector<std::unique_ptr<ChannelWorker>> workers_;
    int channel_start_delay_ms_ = 0;
    bool started_ = false;
};

}  // namespace rtsps
