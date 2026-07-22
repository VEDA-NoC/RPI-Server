#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "rtsps/logger.h"
#include "rtsps/recording_index.h"

namespace rtsps {

enum class VideoCodec { H264, H265 };

struct LiveEncodedFrameView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::int64_t pts_ns = -1;
    std::int64_t dts_ns = -1;
    std::int64_t duration_ns = -1;
    bool key_frame = false;
    int channel_id = 1;
    VideoCodec codec = VideoCodec::H264;
};

// The frame memory is valid only for the duration of on_frame(). A future M3
// subscriber must copy or retain what it needs. Slow callbacks affect only the
// leaky live branch; they cannot apply backpressure to the recording branch.
class LiveFrameSink {
public:
    virtual ~LiveFrameSink() = default;
    virtual void on_frame(const LiveEncodedFrameView& frame) = 0;
};

struct ChannelIngestConfig {
    std::string rtsp_location;
    std::string camera_user;
    std::string camera_password;
    std::string output_dir;
    int camera_channel = 0;
    int channel_id = 1;
    VideoCodec codec = VideoCodec::H264;
    int latency_ms = 200;
    std::int64_t segment_seconds = 60;
    int reconnect_delay_ms = 2000;
    unsigned int live_queue_max_buffers = 1;
};

struct ChannelIngestStats {
    std::uint64_t connection_attempts = 0;
    std::uint64_t reconnects = 0;
    std::uint64_t first_buffers = 0;
    std::uint64_t pipeline_errors = 0;
    std::uint64_t eos_events = 0;
    std::uint64_t live_frames = 0;
    std::uint64_t live_drops = 0;
};

VideoCodec parse_video_codec(const std::string& value);
std::string to_gst_encoding_name(VideoCodec codec);
std::string to_codec_name(VideoCodec codec);

// Exposed for topology unit tests. The source location and credentials are set
// as rtspsrc properties after parsing and are never embedded in this string.
std::string build_channel_ingest_pipeline_description(const ChannelIngestConfig& config,
                                                      const std::string& location_pattern);

class ChannelIngest {
public:
    ChannelIngest(ChannelIngestConfig config, RecordingIndex& index, Logger& logger, std::atomic<bool>& running,
                  LiveFrameSink* live_sink = nullptr);

    int run();
    ChannelIngestStats stats() const;

private:
    enum class AttemptResult { Stopped, PipelineError, EndOfStream };

    AttemptResult run_attempt(std::uint64_t attempt);
    std::string make_location_pattern(std::uint64_t attempt) const;
    bool wait_before_reconnect() const;

    ChannelIngestConfig config_;
    RecordingIndex& index_;
    Logger& logger_;
    std::atomic<bool>& running_;
    LiveFrameSink* live_sink_;
    std::atomic<bool> first_buffer_seen_{false};
    std::atomic<std::uint64_t> connection_attempts_{0};
    std::atomic<std::uint64_t> reconnects_{0};
    std::atomic<std::uint64_t> first_buffers_{0};
    std::atomic<std::uint64_t> pipeline_errors_{0};
    std::atomic<std::uint64_t> eos_events_{0};
    std::atomic<std::uint64_t> live_frames_{0};
    std::atomic<std::uint64_t> live_drops_{0};
};

}  // namespace rtsps
