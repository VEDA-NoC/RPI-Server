#include "rtsps/live_rtsp_server.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "rtsps/rtsp_message_parser.h"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::string request(const std::string& method, const std::string& target, int cseq,
                    const std::vector<std::string>& headers = {}) {
    std::string output = method + " " + target + " RTSP/1.0\r\nCSeq: " + std::to_string(cseq) + "\r\n";
    for (const auto& header : headers) output += header + "\r\n";
    return output + "\r\n";
}

void test_channel_routes_and_control_state() {
    require(rtsps::parse_public_channel_id("rtsp://127.0.0.1:8554/ch1") == 1, "/ch1 route failed");
    require(rtsps::parse_public_channel_id("/ch4/trackID=0") == 4, "/ch4 track route failed");
    require(rtsps::parse_public_channel_id("/ch0") < 0, "/ch0 must not be public");
    require(rtsps::parse_public_channel_id("/ch5") < 0, "out-of-range channel must fail");

    rtsps::RtspControlState state;
    auto result = rtsps::process_rtsp_request(request("OPTIONS", "/ch1", 1), 1, rtsps::VideoCodec::H264, {}, state);
    require(rtsps::rtsp_status_code(result.response) == 200, "OPTIONS failed");
    result = rtsps::process_rtsp_request(request("DESCRIBE", "/ch1", 2), 1, rtsps::VideoCodec::H264,
                                         "packetization-mode=1", state);
    require(rtsps::rtsp_status_code(result.response) == 200, "DESCRIBE failed");
    require(result.response.find("H264/90000") != std::string::npos, "H.264 SDP clock is missing");
    result = rtsps::process_rtsp_request(
        request("SETUP", "/ch1/trackID=0", 3, {"Transport: RTP/AVP/TCP;unicast;interleaved=4-5"}), 1,
        rtsps::VideoCodec::H264, {}, state);
    require(rtsps::rtsp_status_code(result.response) == 200 && state.phase == rtsps::RtspControlPhase::Ready,
            "SETUP state transition failed");
    const std::string session = state.session_id;
    result = rtsps::process_rtsp_request(request("PLAY", "/ch1", 4, {"Session: " + session}), 1,
                                         rtsps::VideoCodec::H264, {}, state);
    require(rtsps::rtsp_status_code(result.response) == 200 && result.start_play &&
                state.phase == rtsps::RtspControlPhase::Playing,
            "PLAY state transition failed");
    result = rtsps::process_rtsp_request(request("TEARDOWN", "/ch1", 5, {"Session: " + session}), 1,
                                         rtsps::VideoCodec::H264, {}, state);
    require(rtsps::rtsp_status_code(result.response) == 200 && result.close_session,
            "TEARDOWN state transition failed");

    rtsps::RtspControlState missing;
    result = rtsps::process_rtsp_request(request("DESCRIBE", "/ch2", 1), 1, rtsps::VideoCodec::H264, {}, missing);
    require(rtsps::rtsp_status_code(result.response) == 404, "unregistered /ch2 did not return 404");
    result = rtsps::process_rtsp_request(request("SETUP", "/ch1", 2, {"Transport: RTP/AVP;unicast"}), 1,
                                         rtsps::VideoCodec::H264, {}, missing);
    require(rtsps::rtsp_status_code(result.response) == 461, "UDP transport was not rejected");

    rtsps::RtspControlState h265;
    result = rtsps::process_rtsp_request(request("DESCRIBE", "/ch1", 1), 1, rtsps::VideoCodec::H265, {}, h265);
    require(result.response.find("H265/90000") != std::string::npos, "H.265 SDP clock is missing");
}

void test_rtp_packetization() {
    std::uint16_t sequence = 100;
    const std::uint32_t timestamp = 90000;
    const std::uint32_t ssrc = 0x12345678;
    const std::vector<std::vector<std::uint8_t>> h264_nals{{0x67, 1, 2}, {0x65, 3, 4}};
    const auto packets =
        rtsps::packetize_access_unit(rtsps::VideoCodec::H264, h264_nals, sequence, timestamp, ssrc, 1200);
    require(packets.size() == 2 && !packets[0].marker && packets[1].marker, "H.264 AU marker placement failed");
    require(packets[0].bytes[2] == 0 && packets[0].bytes[3] == 100, "RTP sequence encoding failed");
    require(packets[0].bytes[4] == 0 && packets[0].bytes[5] == 1 && packets[0].bytes[6] == 0x5f &&
                packets[0].bytes[7] == 0x90,
            "RTP timestamp encoding failed");
    require(packets[0].bytes[8] == 0x12 && packets[0].bytes[11] == 0x78, "RTP SSRC encoding failed");

    std::vector<std::uint8_t> large_h264(3000, 0xaa);
    large_h264[0] = 0x65;
    sequence = 10;
    const auto h264_fragments =
        rtsps::packetize_access_unit(rtsps::VideoCodec::H264, {large_h264}, sequence, 1, 2, 200);
    require(h264_fragments.size() > 1 && (h264_fragments.front().bytes[13] & 0x80U) != 0 &&
                (h264_fragments.back().bytes[13] & 0x40U) != 0 && h264_fragments.back().marker,
            "H.264 FU-A fragmentation failed");

    std::vector<std::uint8_t> large_h265(3000, 0xbb);
    large_h265[0] = static_cast<std::uint8_t>(19U << 1U);
    large_h265[1] = 1;
    sequence = 20;
    const auto h265_fragments =
        rtsps::packetize_access_unit(rtsps::VideoCodec::H265, {large_h265}, sequence, 2, 3, 200);
    require(h265_fragments.size() > 1 && ((h265_fragments.front().bytes[12] >> 1U) & 0x3fU) == 49 &&
                (h265_fragments.front().bytes[14] & 0x80U) != 0 && (h265_fragments.back().bytes[14] & 0x40U) != 0 &&
                h265_fragments.back().marker,
            "H.265 FU fragmentation failed");

    const std::vector<std::uint8_t> annex_b{0, 0, 0, 1, 0x67, 1, 0, 0, 1, 0x68, 2};
    require(rtsps::split_access_unit(annex_b).size() == 2, "Annex-B splitting failed");
    const std::vector<std::uint8_t> length_prefixed{0, 0, 0, 2, 0x67, 1, 0, 0, 0, 2, 0x68, 2};
    require(rtsps::split_access_unit(length_prefixed).size() == 2, "length-prefixed splitting failed");
}

void test_bounded_independent_queues() {
    rtsps::BoundedFrameQueue slow(2, 64);
    rtsps::BoundedFrameQueue fast(2, 64);
    for (int i = 0; i < 10; ++i) {
        auto frame = std::make_shared<rtsps::OwnedLiveFrame>();
        frame->data.assign(16, static_cast<std::uint8_t>(i));
        slow.push(frame);
        fast.push(frame);
        require(fast.try_pop() != nullptr, "fast client stopped receiving while slow client queued");
    }
    require(slow.size() == 2 && slow.bytes() == 32 && slow.drops() == 8, "slow queue is not bounded/drop-oldest");
    require(fast.drops() == 0, "slow queue drops leaked into fast client state");
}

void test_parser_limits_and_redaction() {
    rtsps::ParserLimits limits{64, 8};
    std::string oversized = "OPTIONS /ch1 RTSP/1.0\r\nX:" + std::string(80, 'a') + "\r\n\r\n";
    std::string message;
    require(!rtsps::extract_one_rtsp_message(oversized, message, "test", limits), "oversized header was accepted");
    const std::string sensitive =
        "OPTIONS rtsp://user:secret@camera/ch1 RTSP/1.0\r\nCSeq: 1\r\nAuthorization: Digest secret\r\n\r\n";
    const std::string logged = rtsps::sanitized_rtsp_for_log(sensitive);
    require(logged.find("Digest secret") == std::string::npos, "Authorization leaked into log representation");
    require(logged.find("user:secret") == std::string::npos, "URI userinfo leaked into log representation");
}

}  // namespace

int main() {
    test_channel_routes_and_control_state();
    test_rtp_packetization();
    test_bounded_independent_queues();
    test_parser_limits_and_redaction();
    std::cout << "live_rtsp_server_test: PASS\n";
    return 0;
}
