#pragma once

#include <openssl/ssl.h>

#include <atomic>
#include <cstddef>

#include "rtsps/app_config.h"
#include "rtsps/logger.h"

namespace rtsps {

class RtspProxySession {
public:
    RtspProxySession(int client_fd, SSL_CTX* ssl_ctx, const AppConfig& config, Logger& logger,
                     std::atomic<bool>& running);

    void run();

private:
    int connect_camera() const;
    bool send_all(int fd, const char* data, std::size_t len) const;
    bool ssl_write_all(SSL* ssl, const char* data, std::size_t len) const;

    int client_fd_;
    SSL_CTX* ssl_ctx_;
    AppConfig config_;
    Logger& logger_;
    std::atomic<bool>& running_;
};

}  // namespace rtsps
