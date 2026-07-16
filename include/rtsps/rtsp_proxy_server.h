#pragma once

#include "rtsps/app_config.h"
#include "rtsps/logger.h"
#include "rtsps/tls_context.h"

#include <atomic>
#include <thread>
#include <vector>

namespace rtsps {

class RtspProxyServer {
public:
    RtspProxyServer(AppConfig config, TlsContext& tls_context, Logger& logger, std::atomic<bool>& running);

    int run();

private:
    int open_listen_socket() const;
    void join_client_threads();

    AppConfig config_;
    TlsContext& tls_context_;
    Logger& logger_;
    std::atomic<bool>& running_;
    std::vector<std::thread> client_threads_;
};

}  // namespace rtsps
