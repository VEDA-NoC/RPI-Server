#include "rtsps/rtsp_rewriter.h"

#include "rtsps/rtsp_message_parser.h"

#include <cctype>
#include <sstream>
#include <utility>

namespace rtsps {

int parse_channel_from_request_target(const std::string& target, int max_channels) {
    const std::string key = "/ch";
    const auto pos = lower_copy(target).find(key);
    if (pos == std::string::npos) {
        return -1;
    }
    const auto digit_pos = pos + key.size();
    if (digit_pos >= target.size() || !std::isdigit(static_cast<unsigned char>(target[digit_pos]))) {
        return -1;
    }
    const int channel = target[digit_pos] - '0';
    if (channel < 0 || channel >= max_channels) {
        return -1;
    }
    return channel;
}

std::string extract_public_base_from_target(const std::string& target, int channel) {
    const std::string key = "/ch" + std::to_string(channel);
    const auto pos = lower_copy(target).find(lower_copy(key));
    if (pos == std::string::npos) {
        return key;
    }
    return target.substr(0, pos + key.size());
}

RtspRewriter::RtspRewriter(const AppConfig& config, int channel, std::string public_base)
    : config_(config), channel_(channel), public_base_(std::move(public_base)) {
    std::ostringstream oss;
    oss << "rtsp://" << config_.camera_host << ":" << config_.camera_port << make_camera_path(channel_);
    upstream_base_ = oss.str();
}

const std::string& RtspRewriter::upstream_base() const {
    return upstream_base_;
}

std::string RtspRewriter::make_camera_path(int channel) const {
    return replace_all(config_.camera_path_template, "{channel}", std::to_string(channel));
}

std::string RtspRewriter::rewrite_client_request(const std::string& message) const {
    const std::size_t first_eol = message.find("\r\n");
    if (first_eol == std::string::npos) {
        return message;
    }

    std::string first = message.substr(0, first_eol);
    std::string rest = message.substr(first_eol + 2);

    std::istringstream iss(first);
    std::string method;
    std::string target;
    std::string version;
    iss >> method >> target >> version;
    if (!method.empty() && !target.empty() && !version.empty()) {
        std::string suffix;
        const auto ch_pos = lower_copy(target).find("/ch");
        if (ch_pos != std::string::npos) {
            const std::size_t after_ch_digit = ch_pos + 4;
            if (after_ch_digit < target.size()) {
                suffix = target.substr(after_ch_digit);
            }
        }
        first = method + " " + upstream_base_ + suffix + " " + version;
    }

    std::string out = first + "\r\n";
    auto lines = split_lines_keep_empty(rest);
    bool had_transport = false;
    for (const auto& line : lines) {
        if (line.empty()) {
            out += "\r\n";
            continue;
        }
        if (starts_with_ci(line, "Transport:")) {
            had_transport = true;
            out += "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n";
            continue;
        }
        out += replace_all(line, public_base_, upstream_base_) + "\r\n";
    }

    if (!had_transport && lower_copy(method) == "setup") {
        const std::size_t header_end = out.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            out.insert(header_end + 2, "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n");
        }
    }
    return out;
}

std::string RtspRewriter::rewrite_camera_response(const std::string& message) const {
    if (!message.empty() && static_cast<unsigned char>(message[0]) == '$') {
        return message;
    }

    const std::size_t header_end = message.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return replace_all(message, upstream_base_, public_base_);
    }

    std::string header = message.substr(0, header_end + 4);
    std::string body = message.substr(header_end + 4);
    header = replace_all(header, upstream_base_, public_base_);
    body = replace_all(body, upstream_base_, public_base_);

    const std::size_t content_length_pos = lower_copy(header).find("content-length:");
    if (content_length_pos != std::string::npos) {
        const std::size_t line_end = header.find("\r\n", content_length_pos);
        if (line_end != std::string::npos) {
            header.replace(content_length_pos, line_end - content_length_pos,
                           "Content-Length: " + std::to_string(body.size()));
        }
    }
    return header + body;
}

}  // namespace rtsps
