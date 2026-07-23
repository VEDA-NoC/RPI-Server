#include "rtsps/live_rtsp_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#include "rtsps/rtsp_message_parser.h"

namespace rtsps {
namespace {

constexpr std::uint8_t kPayloadType = 96;
constexpr std::chrono::seconds kRtcpInterval{5};

std::string random_session_id() {
    std::random_device random;
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(8) << random() << std::setw(8) << random();
    return out.str();
}

std::uint32_t random_u32() {
    std::random_device random;
    return (static_cast<std::uint32_t>(random()) << 16U) ^ static_cast<std::uint32_t>(random());
}

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 24U));
    output.push_back(static_cast<std::uint8_t>(value >> 16U));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value));
}

std::vector<std::uint8_t> make_rtp_packet(const std::uint8_t* payload, std::size_t payload_size, bool marker,
                                          std::uint16_t sequence, std::uint32_t timestamp, std::uint32_t ssrc) {
    std::vector<std::uint8_t> packet;
    packet.reserve(12 + payload_size);
    packet.push_back(0x80);
    packet.push_back(static_cast<std::uint8_t>(kPayloadType | (marker ? 0x80 : 0)));
    append_u16(packet, sequence);
    append_u32(packet, timestamp);
    append_u32(packet, ssrc);
    packet.insert(packet.end(), payload, payload + payload_size);
    return packet;
}

std::vector<std::uint8_t> interleaved(int channel, const std::vector<std::uint8_t>& packet) {
    if (packet.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error("interleaved RTP packet is too large");
    }
    std::vector<std::uint8_t> output;
    output.reserve(packet.size() + 4);
    output.push_back('$');
    output.push_back(static_cast<std::uint8_t>(channel));
    append_u16(output, static_cast<std::uint16_t>(packet.size()));
    output.insert(output.end(), packet.begin(), packet.end());
    return output;
}

std::string reason_phrase(int status) {
    switch (status) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 454:
            return "Session Not Found";
        case 455:
            return "Method Not Valid in This State";
        case 461:
            return "Unsupported Transport";
        default:
            return "Error";
    }
}

std::string make_response(int status, const std::string& cseq, const std::vector<std::string>& headers = {},
                          const std::string& body = {}) {
    std::ostringstream out;
    out << "RTSP/1.0 " << status << ' ' << reason_phrase(status) << "\r\n";
    if (!cseq.empty()) out << "CSeq: " << cseq << "\r\n";
    out << "Server: rpi-vms-m3\r\n";
    for (const auto& header : headers) out << header << "\r\n";
    if (!body.empty()) out << "Content-Length: " << body.size() << "\r\n";
    out << "\r\n" << body;
    return out.str();
}

bool valid_session_header(const std::string& header, const std::string& expected) {
    return !expected.empty() && (header == expected || starts_with_ci(header, expected + ";"));
}

bool parse_interleaved_channels(const std::string& transport, int& rtp_channel, int& rtcp_channel) {
    if (lower_copy(transport).find("rtp/avp/tcp") == std::string::npos) {
        return false;
    }
    const std::string lower = lower_copy(transport);
    const std::size_t pos = lower.find("interleaved=");
    if (pos == std::string::npos) {
        rtp_channel = 0;
        rtcp_channel = 1;
        return true;
    }
    std::istringstream values(lower.substr(pos + std::string("interleaved=").size()));
    char separator = 0;
    if (!(values >> rtp_channel >> separator >> rtcp_channel) || separator != '-' || rtp_channel < 0 ||
        rtcp_channel < 0 || rtp_channel > 255 || rtcp_channel > 255 || rtp_channel == rtcp_channel) {
        return false;
    }
    return true;
}

std::string build_sdp(int channel_id, VideoCodec codec, const std::string& codec_fmtp) {
    std::ostringstream sdp;
    sdp << "v=0\r\n"
        << "o=- 0 0 IN IP4 0.0.0.0\r\n"
        << "s=RPi VMS channel " << channel_id << "\r\n"
        << "t=0 0\r\n"
        << "a=control:*\r\n"
        << "m=video 0 RTP/AVP " << static_cast<int>(kPayloadType) << "\r\n"
        << "a=rtpmap:" << static_cast<int>(kPayloadType) << ' ' << (codec == VideoCodec::H265 ? "H265" : "H264")
        << "/90000\r\n";
    if (!codec_fmtp.empty()) sdp << "a=fmtp:" << static_cast<int>(kPayloadType) << ' ' << codec_fmtp << "\r\n";
    sdp << "a=control:trackID=0\r\n";
    return sdp.str();
}

std::size_t start_code_size(const std::vector<std::uint8_t>& data, std::size_t pos) {
    if (pos + 3 <= data.size() && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1) return 3;
    if (pos + 4 <= data.size() && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 0 && data[pos + 3] == 1)
        return 4;
    return 0;
}

std::uint32_t read_u32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24U) | (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) | static_cast<std::uint32_t>(data[3]);
}

std::string base64_encode(const std::vector<std::uint8_t>& input) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve((input.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < input.size();) {
        const std::size_t remaining = input.size() - i;
        const std::uint32_t a = input[i];
        const std::uint32_t b = remaining > 1 ? input[i + 1] : 0;
        const std::uint32_t c = remaining > 2 ? input[i + 2] : 0;
        const std::uint32_t value = (a << 16U) | (b << 8U) | c;
        output.push_back(alphabet[(value >> 18U) & 0x3fU]);
        output.push_back(alphabet[(value >> 12U) & 0x3fU]);
        output.push_back(remaining > 1 ? alphabet[(value >> 6U) & 0x3fU] : '=');
        output.push_back(remaining > 2 ? alphabet[value & 0x3fU] : '=');
        i += std::min<std::size_t>(3, remaining);
    }
    return output;
}

class ClientConnection {
public:
    ClientConnection(int fd, SSL_CTX* tls_context) : fd_(fd) {
        if (tls_context) {
            ssl_ = SSL_new(tls_context);
            if (!ssl_) throw std::runtime_error("SSL_new failed");
            SSL_set_fd(ssl_, fd_);
            if (SSL_accept(ssl_) != 1) {
                SSL_free(ssl_);
                ssl_ = nullptr;
                throw std::runtime_error("TLS handshake failed");
            }
        }
    }

    ~ClientConnection() {
        if (ssl_) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
        }
        if (fd_ >= 0) ::close(fd_);
    }

    int fd() const { return fd_; }
    bool tls() const { return ssl_ != nullptr; }
    int pending() const { return ssl_ ? SSL_pending(ssl_) : 0; }

    int read(char* data, std::size_t size) {
        if (ssl_) return SSL_read(ssl_, data, static_cast<int>(size));
        return static_cast<int>(::recv(fd_, data, size, 0));
    }

    bool write(const std::uint8_t* data, std::size_t size) {
        std::lock_guard<std::mutex> lock(write_mutex_);
        std::size_t offset = 0;
        while (offset < size) {
            int written = 0;
            if (ssl_) {
                written = SSL_write(ssl_, data + offset, static_cast<int>(size - offset));
            } else {
                written = static_cast<int>(::send(fd_, data + offset, size - offset, MSG_NOSIGNAL));
            }
            if (written <= 0) return false;
            offset += static_cast<std::size_t>(written);
        }
        return true;
    }

    bool write(const std::string& data) {
        return write(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    }

private:
    int fd_;
    SSL* ssl_ = nullptr;
    std::mutex write_mutex_;
};

struct ClientMediaState {
    explicit ClientMediaState(std::size_t max_frames, std::size_t max_bytes)
        : queue(max_frames, max_bytes),
          sequence(static_cast<std::uint16_t>(random_u32())),
          ssrc(random_u32()),
          timestamp_origin(random_u32()) {}

    BoundedFrameQueue queue;
    std::atomic<bool> playing{false};
    std::atomic<bool> connected{true};
    std::atomic<int> channel_id{-1};
    std::uint16_t sequence;
    std::uint32_t ssrc;
    std::uint32_t timestamp_origin;
    std::int64_t first_pts_ns = -1;
    std::uint32_t last_timestamp = 0;
    std::uint64_t packet_count = 0;
    std::uint64_t octet_count = 0;
    bool first_rtp_sent = false;
    bool waiting_for_keyframe = true;
    std::chrono::steady_clock::time_point last_rtcp{};
};

std::vector<std::uint8_t> make_rtcp_sender_report(ClientMediaState& media, std::uint32_t rtp_timestamp) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds_since_1970 = std::chrono::duration_cast<std::chrono::seconds>(now);
    const auto fraction = now - seconds_since_1970;
    constexpr std::uint64_t kNtpEpochOffset = 2208988800ULL;
    const std::uint32_t ntp_seconds = static_cast<std::uint32_t>(seconds_since_1970.count() + kNtpEpochOffset);
    const auto fraction_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(fraction).count();
    const std::uint32_t ntp_fraction =
        static_cast<std::uint32_t>((static_cast<std::uint64_t>(fraction_ns) << 32U) / 1000000000ULL);

    std::vector<std::uint8_t> packet;
    packet.reserve(28);
    packet.push_back(0x80);
    packet.push_back(200);
    append_u16(packet, 6);
    append_u32(packet, media.ssrc);
    append_u32(packet, ntp_seconds);
    append_u32(packet, ntp_fraction);
    append_u32(packet, rtp_timestamp);
    append_u32(packet, static_cast<std::uint32_t>(media.packet_count));
    append_u32(packet, static_cast<std::uint32_t>(media.octet_count));
    return packet;
}

}  // namespace

BoundedFrameQueue::BoundedFrameQueue(std::size_t max_frames, std::size_t max_bytes)
    : max_frames_(max_frames), max_bytes_(max_bytes) {
    if (max_frames_ == 0 || max_bytes_ == 0) throw std::runtime_error("live client queue limits must be non-zero");
}

bool BoundedFrameQueue::push(std::shared_ptr<const OwnedLiveFrame> frame) {
    if (!frame) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return false;
    if (frame->data.size() > max_bytes_) {
        ++drops_;
        return false;
    }
    bool dropped = false;
    while (!frames_.empty() && (frames_.size() >= max_frames_ || bytes_ + frame->data.size() > max_bytes_)) {
        bytes_ -= frames_.front()->data.size();
        frames_.pop_front();
        ++drops_;
        dropped = true;
    }
    bytes_ += frame->data.size();
    frames_.push_back(std::move(frame));
    return !dropped;
}

std::shared_ptr<const OwnedLiveFrame> BoundedFrameQueue::try_pop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frames_.empty()) return {};
    auto frame = std::move(frames_.front());
    frames_.pop_front();
    bytes_ -= frame->data.size();
    return frame;
}

void BoundedFrameQueue::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    frames_.clear();
    bytes_ = 0;
}

std::size_t BoundedFrameQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_.size();
}

std::size_t BoundedFrameQueue::bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_;
}

std::uint64_t BoundedFrameQueue::drops() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return drops_;
}

std::vector<std::vector<std::uint8_t>> split_access_unit(const std::vector<std::uint8_t>& access_unit) {
    std::vector<std::vector<std::uint8_t>> nal_units;
    std::size_t first_start = std::string::npos;
    for (std::size_t i = 0; i < access_unit.size(); ++i) {
        if (start_code_size(access_unit, i) != 0) {
            first_start = i;
            break;
        }
    }
    if (first_start != std::string::npos) {
        std::size_t pos = first_start;
        while (pos < access_unit.size()) {
            const std::size_t prefix = start_code_size(access_unit, pos);
            if (prefix == 0) {
                ++pos;
                continue;
            }
            const std::size_t nal_start = pos + prefix;
            std::size_t next = nal_start;
            while (next < access_unit.size() && start_code_size(access_unit, next) == 0) ++next;
            if (next > nal_start)
                nal_units.emplace_back(access_unit.begin() + static_cast<std::ptrdiff_t>(nal_start),
                                       access_unit.begin() + static_cast<std::ptrdiff_t>(next));
            pos = next;
        }
        return nal_units;
    }

    std::size_t pos = 0;
    while (pos + 4 <= access_unit.size()) {
        const std::uint32_t length = read_u32(access_unit.data() + pos);
        if (length == 0 || length > access_unit.size() - pos - 4) {
            nal_units.clear();
            break;
        }
        pos += 4;
        nal_units.emplace_back(access_unit.begin() + static_cast<std::ptrdiff_t>(pos),
                               access_unit.begin() + static_cast<std::ptrdiff_t>(pos + length));
        pos += length;
    }
    if (!nal_units.empty() && pos == access_unit.size()) return nal_units;
    if (!access_unit.empty()) nal_units.push_back(access_unit);
    return nal_units;
}

std::vector<RtpPacket> packetize_access_unit(VideoCodec codec, const std::vector<std::vector<std::uint8_t>>& nal_units,
                                             std::uint16_t& sequence, std::uint32_t timestamp, std::uint32_t ssrc,
                                             std::size_t mtu) {
    if (mtu <= 15) throw std::runtime_error("RTP MTU is too small");
    std::vector<RtpPacket> packets;
    for (std::size_t nal_index = 0; nal_index < nal_units.size(); ++nal_index) {
        const auto& nal = nal_units[nal_index];
        if (nal.empty()) continue;
        const bool last_nal = nal_index + 1 == nal_units.size();
        const std::size_t max_payload = mtu - 12;
        if (nal.size() <= max_payload) {
            packets.push_back(
                {make_rtp_packet(nal.data(), nal.size(), last_nal, sequence++, timestamp, ssrc), last_nal});
            continue;
        }

        if (codec == VideoCodec::H264) {
            const std::uint8_t header = nal[0];
            const std::uint8_t fu_indicator = static_cast<std::uint8_t>((header & 0xe0U) | 28U);
            const std::uint8_t nal_type = static_cast<std::uint8_t>(header & 0x1fU);
            const std::size_t chunk_limit = max_payload - 2;
            std::size_t offset = 1;
            bool first = true;
            while (offset < nal.size()) {
                const std::size_t chunk = std::min(chunk_limit, nal.size() - offset);
                const bool end = offset + chunk == nal.size();
                std::vector<std::uint8_t> payload{
                    fu_indicator, static_cast<std::uint8_t>(nal_type | (first ? 0x80U : 0U) | (end ? 0x40U : 0U))};
                payload.insert(payload.end(), nal.begin() + static_cast<std::ptrdiff_t>(offset),
                               nal.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
                const bool marker = end && last_nal;
                packets.push_back(
                    {make_rtp_packet(payload.data(), payload.size(), marker, sequence++, timestamp, ssrc), marker});
                first = false;
                offset += chunk;
            }
        } else {
            if (nal.size() < 3) continue;
            const std::uint8_t original_type = static_cast<std::uint8_t>((nal[0] >> 1U) & 0x3fU);
            const std::uint8_t fu_header0 = static_cast<std::uint8_t>((nal[0] & 0x81U) | (49U << 1U));
            const std::uint8_t fu_header1 = nal[1];
            const std::size_t chunk_limit = max_payload - 3;
            std::size_t offset = 2;
            bool first = true;
            while (offset < nal.size()) {
                const std::size_t chunk = std::min(chunk_limit, nal.size() - offset);
                const bool end = offset + chunk == nal.size();
                std::vector<std::uint8_t> payload{
                    fu_header0, fu_header1,
                    static_cast<std::uint8_t>(original_type | (first ? 0x80U : 0U) | (end ? 0x40U : 0U))};
                payload.insert(payload.end(), nal.begin() + static_cast<std::ptrdiff_t>(offset),
                               nal.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
                const bool marker = end && last_nal;
                packets.push_back(
                    {make_rtp_packet(payload.data(), payload.size(), marker, sequence++, timestamp, ssrc), marker});
                first = false;
                offset += chunk;
            }
        }
    }
    return packets;
}

int parse_public_channel_id(const std::string& request_target) {
    std::string path = request_target;
    const std::size_t scheme = path.find("://");
    if (scheme != std::string::npos) {
        const std::size_t slash = path.find('/', scheme + 3);
        path = slash == std::string::npos ? "/" : path.substr(slash);
    }
    const std::size_t query = path.find_first_of("?#");
    if (query != std::string::npos) path.resize(query);
    if (path.size() < 4 || path.compare(0, 3, "/ch") != 0) return -1;
    std::size_t end = 3;
    while (end < path.size() && path[end] >= '0' && path[end] <= '9') ++end;
    if (end == 3 || (end < path.size() && path[end] != '/')) return -1;
    try {
        const int channel = std::stoi(path.substr(3, end - 3));
        return channel >= 1 && channel <= 4 ? channel : -1;
    } catch (const std::exception&) {
        return -1;
    }
}

RtspControlResult process_rtsp_request(const std::string& request, int registered_channel_id, VideoCodec codec,
                                       const std::string& codec_fmtp, RtspControlState& state) {
    const std::string cseq = cseq_from_message(request);
    if (cseq.empty()) return {make_response(400, {}), false, true};
    const std::string method = rtsp_method_from_request(request);
    const std::string target = extract_request_target(request);
    const int channel = parse_public_channel_id(target);

    if (method == "OPTIONS") {
        return {make_response(200, cseq, {"Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER"}), false,
                false};
    }
    if (channel < 0 || channel != registered_channel_id) return {make_response(404, cseq), false, false};

    if (method == "DESCRIBE") {
        const std::string sdp = build_sdp(channel, codec, codec_fmtp);
        return {
            make_response(200, cseq,
                          {"Content-Type: application/sdp", "Content-Base: /ch" + std::to_string(channel) + "/"}, sdp),
            false, false};
    }
    if (method == "SETUP") {
        if (state.phase != RtspControlPhase::Initial) return {make_response(455, cseq), false, false};
        int rtp_channel = 0;
        int rtcp_channel = 1;
        const std::string transport = header_value(message_header_part(request), "Transport");
        if (!parse_interleaved_channels(transport, rtp_channel, rtcp_channel))
            return {make_response(461, cseq), false, false};
        state.session_id = random_session_id();
        state.channel_id = channel;
        state.rtp_channel = rtp_channel;
        state.rtcp_channel = rtcp_channel;
        state.phase = RtspControlPhase::Ready;
        return {make_response(200, cseq,
                              {"Transport: RTP/AVP/TCP;unicast;interleaved=" + std::to_string(rtp_channel) + "-" +
                                   std::to_string(rtcp_channel),
                               "Session: " + state.session_id + ";timeout=60"}),
                false, false};
    }
    if (method == "PLAY") {
        if (state.phase != RtspControlPhase::Ready || state.channel_id != channel)
            return {make_response(455, cseq), false, false};
        if (!valid_session_header(header_value(message_header_part(request), "Session"), state.session_id))
            return {make_response(454, cseq), false, false};
        state.phase = RtspControlPhase::Playing;
        return {make_response(
                    200, cseq,
                    {"Session: " + state.session_id, "RTP-Info: url=/ch" + std::to_string(channel) + "/trackID=0"}),
                true, false};
    }
    if (method == "GET_PARAMETER") {
        if (!valid_session_header(header_value(message_header_part(request), "Session"), state.session_id))
            return {make_response(454, cseq), false, false};
        return {make_response(200, cseq, {"Session: " + state.session_id}), false, false};
    }
    if (method == "TEARDOWN") {
        if (!valid_session_header(header_value(message_header_part(request), "Session"), state.session_id))
            return {make_response(454, cseq), false, false};
        state.phase = RtspControlPhase::Closed;
        return {make_response(200, cseq, {"Session: " + state.session_id}), false, true};
    }
    return {make_response(405, cseq, {"Allow: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER"}), false, false};
}

bool find_live_channel(const LiveRtspServerConfig& config, int channel_id, VideoCodec& codec) {
    if (config.channels.empty()) {
        if (config.channel_id != channel_id) return false;
        codec = config.codec;
        return true;
    }
    const auto found = std::find_if(config.channels.begin(), config.channels.end(),
                                    [channel_id](const auto& channel) { return channel.channel_id == channel_id; });
    if (found == config.channels.end()) return false;
    codec = found->codec;
    return true;
}

class LiveRtspServer::Impl {
public:
    Impl(LiveRtspServerConfig config, Logger& logger, std::atomic<bool>& running)
        : config_(std::move(config)), logger_(logger), running_(running) {
        if (config_.channels.empty()) {
            config_.channels.push_back({config_.channel_id, config_.codec});
        }
        std::array<bool, 5> channel_seen{};
        for (const auto& channel : config_.channels) {
            if (channel.channel_id < 1 || channel.channel_id > 4)
                throw std::runtime_error("live channel_id out of range");
            if (channel_seen[static_cast<std::size_t>(channel.channel_id)])
                throw std::runtime_error("duplicate live channel registration");
            channel_seen[static_cast<std::size_t>(channel.channel_id)] = true;
        }
        if (config_.rtsp_port <= 0 && config_.rtsps_port <= 0)
            throw std::runtime_error("at least one live RTSP listener must be enabled");
        if (config_.rtsps_port > 0 && (config_.tls_cert_file.empty() || config_.tls_key_file.empty()))
            throw std::runtime_error("RTSPS requires --tls-cert and --tls-key");
    }

    ~Impl() { stop(); }

    void start() {
        if (started_.exchange(true)) return;
        if (config_.rtsps_port > 0) initialize_tls();
        try {
            if (config_.rtsp_port > 0) plain_listener_fd_ = open_listener(config_.rtsp_port);
            if (config_.rtsps_port > 0) tls_listener_fd_ = open_listener(config_.rtsps_port);
        } catch (...) {
            close_listeners();
            started_.store(false);
            throw;
        }
        if (plain_listener_fd_ >= 0)
            listener_threads_.emplace_back([this]() { accept_loop(plain_listener_fd_, nullptr, "rtsp"); });
        if (tls_listener_fd_ >= 0)
            listener_threads_.emplace_back([this]() { accept_loop(tls_listener_fd_, tls_context_, "rtsps"); });
    }

    void stop() {
        if (!started_.exchange(false)) return;
        close_listeners();
        for (auto& thread : listener_threads_)
            if (thread.joinable()) thread.join();
        listener_threads_.clear();
        std::vector<std::thread> sessions;
        {
            std::lock_guard<std::mutex> lock(session_threads_mutex_);
            sessions.swap(session_threads_);
        }
        for (auto& thread : sessions)
            if (thread.joinable()) thread.join();
        if (tls_context_) {
            SSL_CTX_free(tls_context_);
            tls_context_ = nullptr;
        }
        logger_.info("[live-server] state=stopped");
    }

    void on_frame(const LiveEncodedFrameView& view) {
        const auto* channel = registered_channel(view.channel_id);
        if (!view.data || view.size == 0 || !channel || channel->codec != view.codec) return;
        auto frame = std::make_shared<OwnedLiveFrame>();
        frame->data.assign(view.data, view.data + view.size);
        frame->pts_ns = view.pts_ns;
        frame->duration_ns = view.duration_ns;
        frame->key_frame = view.key_frame;
        frame->channel_id = view.channel_id;
        frame->codec = view.codec;

        update_parameter_sets(*frame);
        std::vector<std::shared_ptr<ClientMediaState>> clients;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            clients_.erase(
                std::remove_if(clients_.begin(), clients_.end(), [](const auto& weak) { return weak.expired(); }),
                clients_.end());
            for (const auto& weak : clients_) {
                if (auto client = weak.lock(); client && client->connected.load() && client->playing.load() &&
                                               client->channel_id.load() == view.channel_id)
                    clients.push_back(std::move(client));
            }
        }
        for (const auto& client : clients) {
            const std::uint64_t before = client->queue.drops();
            client->queue.push(frame);
            const std::uint64_t after = client->queue.drops();
            if (after > before) {
                queue_drops_.fetch_add(after - before);
                if (after == 1 || after % 100 == 0)
                    logger_.warn("[live-client] state=queue_drop policy=drop_oldest drops=" + std::to_string(after));
            }
        }
    }

    LiveRtspServerStats stats() const {
        return {sessions_created_.load(),        sessions_closed_.load(), active_clients_.load(),
                first_rtp_transmissions_.load(), queue_drops_.load(),     write_failures_.load()};
    }

private:
    void initialize_tls() {
        tls_context_ = SSL_CTX_new(TLS_server_method());
        if (!tls_context_) throw std::runtime_error("SSL_CTX_new failed");
        SSL_CTX_set_min_proto_version(tls_context_, TLS1_2_VERSION);
        if (SSL_CTX_use_certificate_file(tls_context_, config_.tls_cert_file.c_str(), SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_use_PrivateKey_file(tls_context_, config_.tls_key_file.c_str(), SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(tls_context_) != 1) {
            SSL_CTX_free(tls_context_);
            tls_context_ = nullptr;
            throw std::runtime_error("failed to load matching TLS certificate and private key");
        }
    }

    int open_listener(int port) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) throw std::runtime_error("live listener socket failed");
        int yes = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        if (::inet_pton(AF_INET, config_.listen_host.c_str(), &address.sin_addr) != 1 ||
            ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
            ::listen(fd, config_.listen_backlog) < 0) {
            const std::string message = std::strerror(errno);
            ::close(fd);
            throw std::runtime_error("failed to listen on " + config_.listen_host + ":" + std::to_string(port) + ": " +
                                     message);
        }
        logger_.info(
            "[live-server] state=listening transport=" + std::string(port == config_.rtsps_port ? "rtsps" : "rtsp") +
            " address=" + config_.listen_host + ":" + std::to_string(port) + " channels=" + registered_channel_list());
        return fd;
    }

    void close_listeners() {
        if (plain_listener_fd_ >= 0) {
            ::shutdown(plain_listener_fd_, SHUT_RDWR);
            ::close(plain_listener_fd_);
            plain_listener_fd_ = -1;
        }
        if (tls_listener_fd_ >= 0) {
            ::shutdown(tls_listener_fd_, SHUT_RDWR);
            ::close(tls_listener_fd_);
            tls_listener_fd_ = -1;
        }
    }

    void accept_loop(int listener_fd, SSL_CTX* tls_context, const std::string& transport) {
        while (running_.load() && started_.load()) {
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(listener_fd, &read_fds);
            timeval timeout{0, 200000};
            const int ready = ::select(listener_fd + 1, &read_fds, nullptr, nullptr, &timeout);
            if (ready <= 0) continue;
            const int client_fd = ::accept(listener_fd, nullptr, nullptr);
            if (client_fd < 0) continue;
            timeval io_timeout{config_.io_timeout_ms / 1000, (config_.io_timeout_ms % 1000) * 1000};
            ::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout));
            ::setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout));
            std::lock_guard<std::mutex> lock(session_threads_mutex_);
            session_threads_.emplace_back(
                [this, client_fd, tls_context, transport]() { run_session(client_fd, tls_context, transport); });
        }
    }

    const LiveRtspServerConfig::Channel* registered_channel(int channel_id) const {
        const auto found = std::find_if(config_.channels.begin(), config_.channels.end(),
                                        [channel_id](const auto& channel) { return channel.channel_id == channel_id; });
        return found == config_.channels.end() ? nullptr : &*found;
    }

    std::string registered_channel_list() const {
        std::string result;
        for (const auto& channel : config_.channels) {
            if (!result.empty()) result += ',';
            result += "/ch" + std::to_string(channel.channel_id);
        }
        return result;
    }

    struct ParameterSets {
        std::vector<std::uint8_t> h264_sps;
        std::vector<std::uint8_t> h264_pps;
        std::vector<std::uint8_t> h265_vps;
        std::vector<std::uint8_t> h265_sps;
        std::vector<std::uint8_t> h265_pps;
    };

    std::string codec_fmtp(int channel_id, VideoCodec codec) const {
        std::lock_guard<std::mutex> lock(parameter_mutex_);
        const auto& sets = parameter_sets_by_channel_[static_cast<std::size_t>(channel_id)];
        if (codec == VideoCodec::H264 && sets.h264_sps.size() && sets.h264_pps.size())
            return "packetization-mode=1;sprop-parameter-sets=" + base64_encode(sets.h264_sps) + "," +
                   base64_encode(sets.h264_pps);
        if (codec == VideoCodec::H265 && sets.h265_vps.size() && sets.h265_sps.size() && sets.h265_pps.size())
            return "sprop-vps=" + base64_encode(sets.h265_vps) + ";sprop-sps=" + base64_encode(sets.h265_sps) +
                   ";sprop-pps=" + base64_encode(sets.h265_pps);
        return {};
    }

    std::vector<std::vector<std::uint8_t>> parameter_sets(int channel_id, VideoCodec codec) const {
        std::lock_guard<std::mutex> lock(parameter_mutex_);
        const auto& sets = parameter_sets_by_channel_[static_cast<std::size_t>(channel_id)];
        if (codec == VideoCodec::H264) return {sets.h264_sps, sets.h264_pps};
        return {sets.h265_vps, sets.h265_sps, sets.h265_pps};
    }

    void update_parameter_sets(const OwnedLiveFrame& frame) {
        const auto nal_units = split_access_unit(frame.data);
        std::lock_guard<std::mutex> lock(parameter_mutex_);
        auto& sets = parameter_sets_by_channel_[static_cast<std::size_t>(frame.channel_id)];
        for (const auto& nal : nal_units) {
            if (nal.empty()) continue;
            if (frame.codec == VideoCodec::H264) {
                const int type = nal[0] & 0x1f;
                if (type == 7) sets.h264_sps = nal;
                if (type == 8) sets.h264_pps = nal;
            } else if (nal.size() >= 2) {
                const int type = (nal[0] >> 1) & 0x3f;
                if (type == 32) sets.h265_vps = nal;
                if (type == 33) sets.h265_sps = nal;
                if (type == 34) sets.h265_pps = nal;
            }
        }
    }

    std::uint32_t frame_timestamp(ClientMediaState& media, const OwnedLiveFrame& frame) const {
        if (frame.pts_ns >= 0) {
            if (media.first_pts_ns < 0) media.first_pts_ns = frame.pts_ns;
            const std::int64_t delta = std::max<std::int64_t>(0, frame.pts_ns - media.first_pts_ns);
            media.last_timestamp =
                media.timestamp_origin + static_cast<std::uint32_t>((delta * 90000LL) / 1000000000LL);
        } else {
            const std::int64_t duration = frame.duration_ns > 0 ? frame.duration_ns : 33333333;
            media.last_timestamp += static_cast<std::uint32_t>((duration * 90000LL) / 1000000000LL);
            if (media.last_timestamp == 0) media.last_timestamp = media.timestamp_origin;
        }
        return media.last_timestamp;
    }

    bool send_frame(ClientConnection& connection, ClientMediaState& media, const OwnedLiveFrame& frame,
                    const RtspControlState& control) {
        if (media.waiting_for_keyframe && !frame.key_frame) return true;
        std::vector<std::vector<std::uint8_t>> nal_units;
        if (media.waiting_for_keyframe) {
            nal_units = parameter_sets(frame.channel_id, frame.codec);
            nal_units.erase(
                std::remove_if(nal_units.begin(), nal_units.end(), [](const auto& nal) { return nal.empty(); }),
                nal_units.end());
            media.waiting_for_keyframe = false;
        }
        auto frame_nals = split_access_unit(frame.data);
        nal_units.insert(nal_units.end(), std::make_move_iterator(frame_nals.begin()),
                         std::make_move_iterator(frame_nals.end()));
        const std::uint32_t timestamp = frame_timestamp(media, frame);
        const auto packets =
            packetize_access_unit(frame.codec, nal_units, media.sequence, timestamp, media.ssrc, config_.rtp_mtu);
        for (const auto& packet : packets) {
            const auto wire = interleaved(control.rtp_channel, packet.bytes);
            if (!connection.write(wire.data(), wire.size())) return false;
            ++media.packet_count;
            media.octet_count += packet.bytes.size() - 12;
        }
        if (!packets.empty() && !media.first_rtp_sent) {
            media.first_rtp_sent = true;
            first_rtp_transmissions_.fetch_add(1);
            logger_.info("[live-client] state=first_rtp channel_id=" + std::to_string(frame.channel_id));
        }
        const auto now = std::chrono::steady_clock::now();
        if (media.last_rtcp.time_since_epoch().count() == 0 || now - media.last_rtcp >= kRtcpInterval) {
            const auto report = make_rtcp_sender_report(media, timestamp);
            const auto wire = interleaved(control.rtcp_channel, report);
            if (!connection.write(wire.data(), wire.size())) return false;
            media.last_rtcp = now;
        }
        return true;
    }

    void run_session(int client_fd, SSL_CTX* tls_context, const std::string& transport) {
        std::shared_ptr<ClientMediaState> media =
            std::make_shared<ClientMediaState>(config_.client_queue_frames, config_.client_queue_bytes);
        bool connection_established = false;
        bool counted_session = false;
        try {
            ClientConnection connection(client_fd, tls_context);
            connection_established = true;
            if (connection.tls()) logger_.info("[live-client] state=tls_handshake_success");
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                clients_.push_back(media);
            }
            const std::uint64_t active = active_clients_.fetch_add(1) + 1;
            sessions_created_.fetch_add(1);
            counted_session = true;
            logger_.info("[live-client] state=session_created transport=" + transport +
                         " active_clients=" + std::to_string(active));

            RtspControlState control;
            ParserLimits limits{config_.max_header_bytes, config_.max_body_bytes};
            std::string input;
            std::array<char, 8192> buffer{};
            bool connected = true;
            while (connected && running_.load() && started_.load()) {
                fd_set read_fds;
                FD_ZERO(&read_fds);
                FD_SET(connection.fd(), &read_fds);
                timeval timeout{0, 10000};
                const int ready = ::select(connection.fd() + 1, &read_fds, nullptr, nullptr, &timeout);
                if (ready > 0 || connection.pending() > 0) {
                    const int count = connection.read(buffer.data(), buffer.size());
                    if (count <= 0) break;
                    input.append(buffer.data(), static_cast<std::size_t>(count));
                    if (input.size() > config_.max_header_bytes + config_.max_body_bytes) {
                        connection.write(make_response(400, {}));
                        break;
                    }
                    std::string request;
                    while (extract_one_rtsp_message(input, request, "public-client", limits)) {
                        if (!request.empty() && static_cast<unsigned char>(request[0]) == '$') continue;
                        logger_.trace(rtsp_message_summary("[in] public-client", request));
                        const int requested_channel = parse_public_channel_id(extract_request_target(request));
                        const auto* channel = registered_channel(requested_channel);
                        if (!channel) logger_.warn("[live-client] state=channel_not_found");
                        const int registered_id = channel ? channel->channel_id : -1;
                        const VideoCodec codec = channel ? channel->codec : VideoCodec::H264;
                        RtspControlResult result = process_rtsp_request(
                            request, registered_id, codec, channel ? codec_fmtp(registered_id, codec) : "", control);
                        if (!connection.write(result.response)) {
                            connected = false;
                            break;
                        }
                        if (result.start_play) {
                            media->channel_id.store(control.channel_id);
                            media->playing.store(true);
                            logger_.info("[live-client] state=play channel_id=" + std::to_string(control.channel_id));
                        }
                        if (result.close_session) {
                            connected = false;
                            break;
                        }
                    }
                    if (!input.empty() && input.find("\r\n\r\n") != std::string::npos &&
                        !starts_with_ci(input, "OPTIONS ") && !starts_with_ci(input, "DESCRIBE ") &&
                        !starts_with_ci(input, "SETUP ") && !starts_with_ci(input, "PLAY ") &&
                        !starts_with_ci(input, "TEARDOWN ") && !starts_with_ci(input, "GET_PARAMETER ")) {
                        connection.write(make_response(400, {}));
                        break;
                    }
                }

                if (media->playing.load()) {
                    if (auto frame = media->queue.try_pop()) {
                        if (!send_frame(connection, *media, *frame, control)) {
                            write_failures_.fetch_add(1);
                            logger_.warn("[live-client] state=write_failure_disconnect");
                            break;
                        }
                    }
                }
            }
        } catch (const std::exception& error) {
            if (tls_context)
                logger_.warn(std::string("[live-client] state=tls_handshake_failure message=") + error.what());
            else
                logger_.warn(std::string("[live-client] state=session_error message=") + error.what());
            if (!connection_established && client_fd >= 0) ::close(client_fd);
        }
        media->connected.store(false);
        media->playing.store(false);
        media->queue.close();
        if (counted_session) {
            const std::uint64_t active = active_clients_.fetch_sub(1) - 1;
            sessions_closed_.fetch_add(1);
            logger_.info("[live-client] state=session_closed active_clients=" + std::to_string(active) +
                         " queue_drops=" + std::to_string(media->queue.drops()));
        }
    }

    LiveRtspServerConfig config_;
    Logger& logger_;
    std::atomic<bool>& running_;
    std::atomic<bool> started_{false};
    int plain_listener_fd_ = -1;
    int tls_listener_fd_ = -1;
    SSL_CTX* tls_context_ = nullptr;
    std::vector<std::thread> listener_threads_;
    std::mutex session_threads_mutex_;
    std::vector<std::thread> session_threads_;
    mutable std::mutex clients_mutex_;
    std::vector<std::weak_ptr<ClientMediaState>> clients_;
    mutable std::mutex parameter_mutex_;
    std::array<ParameterSets, 5> parameter_sets_by_channel_;
    std::atomic<std::uint64_t> sessions_created_{0};
    std::atomic<std::uint64_t> sessions_closed_{0};
    std::atomic<std::uint64_t> active_clients_{0};
    std::atomic<std::uint64_t> first_rtp_transmissions_{0};
    std::atomic<std::uint64_t> queue_drops_{0};
    std::atomic<std::uint64_t> write_failures_{0};
};

LiveRtspServer::LiveRtspServer(LiveRtspServerConfig config, Logger& logger, std::atomic<bool>& running)
    : impl_(std::make_unique<Impl>(std::move(config), logger, running)) {}

LiveRtspServer::~LiveRtspServer() = default;

void LiveRtspServer::start() { impl_->start(); }

void LiveRtspServer::stop() { impl_->stop(); }

void LiveRtspServer::on_frame(const LiveEncodedFrameView& frame) { impl_->on_frame(frame); }

LiveRtspServerStats LiveRtspServer::stats() const { return impl_->stats(); }

}  // namespace rtsps
