#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace rtsps {

struct ParserLimits {
    std::size_t max_header_bytes = 16 * 1024;
    std::size_t max_body_bytes = 1024 * 1024;
};

std::string lower_copy(std::string s);
bool starts_with_ci(const std::string& s, const std::string& prefix);
std::string replace_all(std::string s, const std::string& from, const std::string& to);
std::string trim_copy(std::string s);
std::vector<std::string> split_lines_keep_empty(const std::string& header);

std::string get_first_line(const std::string& message);
std::string message_header_part(const std::string& message);
std::string header_value(const std::string& header, const std::string& name);
int content_length_from_header(const std::string& header);
int rtsp_status_code(const std::string& message);
std::string rtsp_method_from_request(const std::string& request);
std::string extract_request_target(const std::string& message);

bool extract_one_rtsp_message(std::string& buffer, std::string& message, const std::string& source,
                              const ParserLimits& limits);

std::string cseq_from_message(const std::string& message);
std::string method_or_status(const std::string& message);
std::string sanitized_rtsp_for_log(const std::string& message);
std::string rtsp_message_summary(const std::string& direction, const std::string& message);

}  // namespace rtsps
