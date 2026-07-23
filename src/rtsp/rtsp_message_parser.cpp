#include "rtsps/rtsp_message_parser.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>

namespace rtsps {

std::string lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool starts_with_ci(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) {
        return false;
    }
    return lower_copy(s.substr(0, prefix.size())) == lower_copy(prefix);
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) {
        return s;
    }
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string trim_copy(const std::string& s) {
    const std::size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::vector<std::string> split_lines_keep_empty(const std::string& header) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < header.size()) {
        std::size_t end = header.find("\r\n", start);
        if (end == std::string::npos) {
            lines.push_back(header.substr(start));
            break;
        }
        lines.push_back(header.substr(start, end - start));
        start = end + 2;
    }
    return lines;
}

std::string get_first_line(const std::string& message) {
    const std::size_t first_eol = message.find("\r\n");
    if (first_eol == std::string::npos) {
        return message;
    }
    return message.substr(0, first_eol);
}

std::string message_header_part(const std::string& message) {
    const std::size_t header_end = message.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return message;
    }
    return message.substr(0, header_end + 4);
}

std::string header_value(const std::string& header, const std::string& name) {
    const std::string prefix = lower_copy(name) + ":";
    std::size_t start = 0;
    while (start < header.size()) {
        std::size_t end = header.find("\r\n", start);
        if (end == std::string::npos) {
            break;
        }
        std::string line = header.substr(start, end - start);
        if (starts_with_ci(line, prefix)) {
            const std::size_t value_start = name.size() + 1;
            return trim_copy(line.substr(value_start));
        }
        start = end + 2;
    }
    return {};
}

int content_length_from_header(const std::string& header) {
    for (const auto& line : split_lines_keep_empty(header)) {
        if (!starts_with_ci(line, "Content-Length:")) {
            continue;
        }
        std::string value = trim_copy(line.substr(std::string("Content-Length:").size()));
        if (value.empty() ||
            !std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); })) {
            return -1;
        }
        try {
            return std::stoi(value);
        } catch (const std::exception&) {
            return -1;
        }
    }
    return 0;
}

int rtsp_status_code(const std::string& message) {
    std::istringstream iss(get_first_line(message));
    std::string version;
    int status = 0;
    iss >> version >> status;
    return status;
}

std::string rtsp_method_from_request(const std::string& request) {
    std::istringstream iss(get_first_line(request));
    std::string method;
    iss >> method;
    return method;
}

std::string extract_request_target(const std::string& message) {
    std::istringstream iss(get_first_line(message));
    std::string method;
    std::string target;
    std::string version;
    iss >> method >> target >> version;
    return target;
}

namespace {

bool starts_with_at(const std::string& s, std::size_t pos, const std::string& prefix) {
    return pos <= s.size() && s.size() - pos >= prefix.size() && s.compare(pos, prefix.size(), prefix) == 0;
}

bool is_rtsp_message_start_at(const std::string& s, std::size_t pos) {
    static const std::vector<std::string> prefixes = {"RTSP/",          "OPTIONS ",  "DESCRIBE ", "SETUP ",
                                                      "PLAY ",          "PAUSE ",    "TEARDOWN ", "GET_PARAMETER ",
                                                      "SET_PARAMETER ", "ANNOUNCE ", "RECORD "};
    for (const auto& prefix : prefixes) {
        if (starts_with_at(s, pos, prefix)) {
            return true;
        }
    }
    return false;
}

std::size_t find_next_message_start(const std::string& buffer, std::size_t start) {
    for (std::size_t i = start; i < buffer.size(); ++i) {
        if (static_cast<unsigned char>(buffer[i]) == '$' || is_rtsp_message_start_at(buffer, i)) {
            return i;
        }
    }
    return std::string::npos;
}

bool resync_message_buffer(std::string& buffer, const ParserLimits& limits) {
    const std::size_t next = find_next_message_start(buffer, 1);
    if (next != std::string::npos) {
        buffer.erase(0, next);
        return true;
    }
    if (buffer.size() > limits.max_header_bytes) {
        buffer.clear();
    }
    return false;
}

}  // namespace

bool extract_one_rtsp_message(std::string& buffer, std::string& message, const std::string& source,
                              const ParserLimits& limits) {
    (void)source;
    message.clear();
    while (!buffer.empty()) {
        if (static_cast<unsigned char>(buffer[0]) == '$') {
            if (buffer.size() < 4) {
                return false;
            }
            const uint16_t len = (static_cast<unsigned char>(buffer[2]) << 8) | static_cast<unsigned char>(buffer[3]);
            const std::size_t total = 4 + len;
            if (buffer.size() < total) {
                return false;
            }
            message = buffer.substr(0, total);
            buffer.erase(0, total);
            return true;
        }

        if (!is_rtsp_message_start_at(buffer, 0)) {
            if (resync_message_buffer(buffer, limits)) {
                continue;
            }
            return false;
        }

        const std::size_t header_end = buffer.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            if (buffer.size() > limits.max_header_bytes) {
                resync_message_buffer(buffer, limits);
            }
            return false;
        }

        const std::size_t header_bytes = header_end + 4;
        if (header_bytes > limits.max_header_bytes) {
            resync_message_buffer(buffer, limits);
            continue;
        }

        const int body_len = content_length_from_header(buffer.substr(0, header_bytes));
        if (body_len < 0 || static_cast<std::size_t>(body_len) > limits.max_body_bytes) {
            resync_message_buffer(buffer, limits);
            continue;
        }

        const std::size_t total = header_bytes + static_cast<std::size_t>(body_len);
        if (buffer.size() < total) {
            return false;
        }
        message = buffer.substr(0, total);
        buffer.erase(0, total);
        return true;
    }
    return false;
}

std::string cseq_from_message(const std::string& message) { return header_value(message_header_part(message), "CSeq"); }

std::string method_or_status(const std::string& message) {
    const std::string first = get_first_line(message);
    if (first.rfind("RTSP/", 0) == 0) {
        std::istringstream iss(first);
        std::string version;
        std::string status;
        iss >> version >> status;
        return "status=" + status;
    }
    return "method=" + rtsp_method_from_request(message);
}

namespace {

std::string redact_uri_userinfo(std::string text) {
    for (const std::string& scheme : {std::string("rtsp://"), std::string("rtsps://")}) {
        std::size_t search_from = 0;
        while ((search_from = lower_copy(text).find(scheme, search_from)) != std::string::npos) {
            const std::size_t start = search_from + scheme.size();
            const std::size_t authority_end = text.find_first_of("/ \t\r\n", start);
            const std::size_t at = text.find('@', start);
            if (at != std::string::npos && (authority_end == std::string::npos || at < authority_end)) {
                text.replace(start, at - start, "<redacted>");
                search_from = start + std::string("<redacted>@").size();
            } else {
                search_from = start;
            }
        }
    }
    return text;
}

}  // namespace

std::string sanitized_rtsp_for_log(const std::string& message) {
    const std::string header = message_header_part(message);
    std::ostringstream out;
    std::size_t start = 0;
    while (start < header.size()) {
        std::size_t end = header.find("\r\n", start);
        if (end == std::string::npos) {
            break;
        }
        std::string line = header.substr(start, end - start);
        if (starts_with_ci(line, "Authorization:")) {
            out << "Authorization: <redacted>\n";
        } else if (starts_with_ci(line, "Proxy-Authorization:")) {
            out << "Proxy-Authorization: <redacted>\n";
        } else {
            out << redact_uri_userinfo(line) << "\n";
        }
        start = end + 2;
    }
    return out.str();
}

std::string rtsp_message_summary(const std::string& direction, const std::string& message) {
    std::ostringstream out;
    if (!message.empty() && static_cast<unsigned char>(message[0]) == '$') {
        if (message.size() >= 4) {
            const int channel = static_cast<unsigned char>(message[1]);
            const uint16_t len = (static_cast<unsigned char>(message[2]) << 8) | static_cast<unsigned char>(message[3]);
            out << direction << " RTP interleaved channel=" << channel << " payload_len=" << len;
        } else {
            out << direction << " RTP interleaved short message size=" << message.size();
        }
        return out.str();
    }

    const int body_len = content_length_from_header(message_header_part(message));
    out << direction << " RTSP " << method_or_status(message) << " cseq=" << cseq_from_message(message)
        << " header_bytes=" << message_header_part(message).size() << " body_bytes=" << body_len << "\n"
        << sanitized_rtsp_for_log(message);
    return out.str();
}

}  // namespace rtsps
