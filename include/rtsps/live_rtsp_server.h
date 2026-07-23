#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "rtsps/channel_ingest.h"
#include "rtsps/logger.h"

namespace rtsps {

struct OwnedLiveFrame {
    std::vector<std::uint8_t> data;
    std::int64_t pts_ns = -1;
    std::int64_t duration_ns = -1;
    bool key_frame = false;
    int channel_id = 1;
    VideoCodec codec = VideoCodec::H264;
};

class BoundedFrameQueue {
public:
    BoundedFrameQueue(std::size_t max_frames, std::size_t max_bytes);

    bool push(std::shared_ptr<const OwnedLiveFrame> frame);
    std::shared_ptr<const OwnedLiveFrame> try_pop();
    void close();
    std::size_t size() const;
    std::size_t bytes() const;
    std::uint64_t drops() const;

private:
    const std::size_t max_frames_;
    const std::size_t max_bytes_;
    mutable std::mutex mutex_;
    std::deque<std::shared_ptr<const OwnedLiveFrame>> frames_;
    std::size_t bytes_ = 0;
    std::uint64_t drops_ = 0;
    bool closed_ = false;
};

struct RtpPacket {
    std::vector<std::uint8_t> bytes;
    bool marker = false;
};

std::vector<std::vector<std::uint8_t>> split_access_unit(const std::vector<std::uint8_t>& access_unit);
std::vector<RtpPacket> packetize_access_unit(VideoCodec codec, const std::vector<std::vector<std::uint8_t>>& nal_units,
                                             std::uint16_t& sequence, std::uint32_t timestamp, std::uint32_t ssrc,
                                             std::size_t mtu);
int parse_public_channel_id(const std::string& request_target);

enum class RtspControlPhase { Initial, Ready, Playing, Closed };

struct RtspControlState {
    RtspControlPhase phase = RtspControlPhase::Initial;
    std::string session_id;
    int channel_id = -1;
    int rtp_channel = 0;
    int rtcp_channel = 1;
};

struct RtspControlResult {
    std::string response;
    bool start_play = false;
    bool close_session = false;
};

RtspControlResult process_rtsp_request(const std::string& request, int registered_channel_id, VideoCodec codec,
                                       const std::string& codec_fmtp, RtspControlState& state);

struct LiveRtspServerConfig {
    std::string listen_host = "0.0.0.0";
    int rtsp_port = 8554;
    int rtsps_port = 0;
    std::string tls_cert_file;
    std::string tls_key_file;
    int listen_backlog = 32;
    std::size_t max_header_bytes = 16 * 1024;
    std::size_t max_body_bytes = 1024 * 1024;
    std::size_t client_queue_frames = 30;
    std::size_t client_queue_bytes = 8 * 1024 * 1024;
    std::size_t rtp_mtu = 1200;
    int io_timeout_ms = 1000;
    struct Channel {
        int channel_id = 1;
        VideoCodec codec = VideoCodec::H264;
    };
    std::vector<Channel> channels;
    // Backward-compatible single-channel registration used by M3 callers.
    int channel_id = 1;
    VideoCodec codec = VideoCodec::H264;
};

struct LiveRtspServerStats {
    std::uint64_t sessions_created = 0;
    std::uint64_t sessions_closed = 0;
    std::uint64_t active_clients = 0;
    std::uint64_t first_rtp_transmissions = 0;
    std::uint64_t queue_drops = 0;
    std::uint64_t write_failures = 0;
};

bool find_live_channel(const LiveRtspServerConfig& config, int channel_id, VideoCodec& codec);

class LiveRtspServer final : public LiveFrameSink {
public:
    LiveRtspServer(LiveRtspServerConfig config, Logger& logger, std::atomic<bool>& running);
    ~LiveRtspServer() override;

    LiveRtspServer(const LiveRtspServer&) = delete;
    LiveRtspServer& operator=(const LiveRtspServer&) = delete;

    void start();
    void stop();
    void on_frame(const LiveEncodedFrameView& frame) override;
    LiveRtspServerStats stats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rtsps
