#pragma once

#include <cstdint>
#include <string>

#include "rtsps/app_config.h"

namespace rtsps {

struct DigestChallenge {
    bool valid = false;
    std::string realm;
    std::string nonce;
    std::string qop;
};

class DigestAuthenticator {
public:
    explicit DigestAuthenticator(const AppConfig& config);

    bool has_challenge() const;
    bool update_from_unauthorized(const std::string& response);
    std::string authorize(const std::string& request);

private:
    std::string build_authorization(const std::string& method, const std::string& uri, uint32_t nc_value) const;

    const AppConfig& config_;
    DigestChallenge challenge_;
    uint32_t nc_ = 1;
};

}  // namespace rtsps
