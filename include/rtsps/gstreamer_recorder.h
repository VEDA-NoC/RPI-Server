#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "rtsps/logger.h"
#include "rtsps/recording_index.h"

namespace rtsps {

enum class VideoCodec { H264, H265 };

struct GStreamerRecorderConfig {
    std::string rtsp_url;
    std::string output_dir;
    int channel_id = 1;
    VideoCodec codec = VideoCodec::H264;
    int latency_ms = 200;
    std::int64_t segment_seconds = 60;
};

VideoCodec parse_video_codec(const std::string& value);
std::string to_gst_encoding_name(VideoCodec codec);
std::string to_codec_name(VideoCodec codec);

class GStreamerRecorder {
public:
    GStreamerRecorder(GStreamerRecorderConfig config, RecordingIndex& index, Logger& logger,
                      std::atomic<bool>& running);

    int run();

private:
    std::string build_pipeline() const;
    std::string make_location_pattern() const;

    GStreamerRecorderConfig config_;
    RecordingIndex& index_;
    Logger& logger_;
    std::atomic<bool>& running_;
};

}  // namespace rtsps
