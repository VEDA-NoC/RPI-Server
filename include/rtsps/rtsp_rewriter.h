#pragma once

#include "rtsps/app_config.h"

#include <string>

namespace rtsps {

class RtspRewriter {
public:
    RtspRewriter(const AppConfig& config, int channel, std::string public_base);

    const std::string& upstream_base() const;
    std::string rewrite_client_request(const std::string& message) const;
    std::string rewrite_camera_response(const std::string& message) const;

private:
    std::string make_camera_path(int channel) const;

    const AppConfig& config_;
    int channel_;
    std::string upstream_base_;
    std::string public_base_;
};

int parse_channel_from_request_target(const std::string& target, int max_channels);
std::string extract_public_base_from_target(const std::string& target, int channel);

}  // namespace rtsps
