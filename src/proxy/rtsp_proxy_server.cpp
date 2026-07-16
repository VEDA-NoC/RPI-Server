#include "rtsps/rtsp_proxy_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

#include "rtsps/rtsp_proxy_session.h"

namespace rtsps {

RtspProxyServer::RtspProxyServer(AppConfig config, TlsContext& tls_context, Logger& logger, std::atomic<bool>& running)
    : config_(std::move(config)), tls_context_(tls_context), logger_(logger), running_(running) {}

int RtspProxyServer::run() {
    int server_fd = open_listen_socket();
    if (server_fd < 0) {
        return 1;
    }

    logger_.info("[server] listening on " + config_.listen_host + ":" + std::to_string(config_.listen_port));
    logger_.info("[server] URL example: rtsps://<raspberry-pi-ip>:" + std::to_string(config_.listen_port) + "/ch0");

    while (running_.load()) {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = ::accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (running_.load()) {
                logger_.error(std::string("[server] accept failed: ") + std::strerror(errno));
            }
            break;
        }

        client_threads_.emplace_back([this, client_fd]() {
            RtspProxySession session(client_fd, tls_context_.get(), config_, logger_, running_);
            session.run();
        });
    }

    ::close(server_fd);
    join_client_threads();
    logger_.info("[server] stopped");
    return 0;
}

void RtspProxyServer::join_client_threads() {
    for (std::thread& client_thread : client_threads_) {
        if (client_thread.joinable()) {
            client_thread.join();
        }
    }
}

int RtspProxyServer::open_listen_socket() const {
    int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        logger_.error(std::string("[server] socket failed: ") + std::strerror(errno));
        return -1;
    }

    int yes = 1;
    ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(config_.listen_port));
    if (::inet_pton(AF_INET, config_.listen_host.c_str(), &addr.sin_addr) != 1) {
        logger_.error("[server] invalid listen host: " + config_.listen_host);
        ::close(server_fd);
        return -1;
    }

    if (::bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        logger_.error(std::string("[server] bind failed: ") + std::strerror(errno));
        ::close(server_fd);
        return -1;
    }

    if (::listen(server_fd, config_.listen_backlog) < 0) {
        logger_.error(std::string("[server] listen failed: ") + std::strerror(errno));
        ::close(server_fd);
        return -1;
    }

    return server_fd;
}

}  // namespace rtsps
