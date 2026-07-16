#include "rtsps/rtsp_proxy_session.h"

#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "rtsps/digest_authenticator.h"
#include "rtsps/rtsp_message_parser.h"
#include "rtsps/rtsp_rewriter.h"

namespace rtsps {
namespace {

struct SslDeleter {
    void operator()(SSL* ssl) const {
        if (ssl) {
            SSL_free(ssl);
        }
    }
};

}  // namespace

RtspProxySession::RtspProxySession(int client_fd, SSL_CTX* ssl_ctx, const AppConfig& config, Logger& logger,
                                   std::atomic<bool>& running)
    : client_fd_(client_fd), ssl_ctx_(ssl_ctx), config_(config), logger_(logger), running_(running) {}

int RtspProxySession::connect_camera() const {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    const std::string port_str = std::to_string(config_.camera_port);
    int rc = ::getaddrinfo(config_.camera_host.c_str(), port_str.c_str(), &hints, &result);
    if (rc != 0) {
        throw std::runtime_error("getaddrinfo() failed: " + std::string(gai_strerror(rc)));
    }

    int fd = -1;
    for (addrinfo* p = result; p != nullptr; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
            break;
        }
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(result);
    if (fd < 0) {
        throw std::runtime_error("connect() to camera failed");
    }
    return fd;
}

bool RtspProxySession::send_all(int fd, const char* data, std::size_t len) const {
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            logger_.warn("[tcp] send to camera failed errno=" + std::to_string(errno) + " " + std::strerror(errno));
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool RtspProxySession::ssl_write_all(SSL* ssl, const char* data, std::size_t len) const {
    std::size_t written = 0;
    while (written < len) {
        const int n = SSL_write(ssl, data + written, static_cast<int>(len - written));
        if (n <= 0) {
            logger_.warn("[tls] SSL_write failed err=" + std::to_string(SSL_get_error(ssl, n)));
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    return true;
}

void RtspProxySession::run() {
    logger_.info("[client] tcp accepted");

    std::unique_ptr<SSL, SslDeleter> ssl(SSL_new(ssl_ctx_));
    if (!ssl) {
        logger_.error("[client] SSL_new failed");
        ::close(client_fd_);
        return;
    }
    SSL_set_fd(ssl.get(), client_fd_);
    if (SSL_accept(ssl.get()) != 1) {
        logger_.warn("[client] TLS handshake failed");
        ::close(client_fd_);
        return;
    }
    logger_.info("[client] TLS connected");

    ParserLimits limits{config_.max_rtsp_header_bytes, config_.max_rtsp_body_bytes};
    DigestAuthenticator authenticator(config_);

    int camera_fd = -1;
    int channel = -1;
    std::string client_buf;
    std::string camera_buf;
    std::string last_upstream_request;
    std::unique_ptr<RtspRewriter> rewriter;
    std::vector<char> io_buffer(config_.io_buffer_bytes);

    while (running_.load()) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(client_fd_, &readfds);
        int maxfd = client_fd_;
        if (camera_fd >= 0) {
            FD_SET(camera_fd, &readfds);
            maxfd = std::max(maxfd, camera_fd);
        }

        timeval tv{};
        tv.tv_sec = config_.select_timeout_ms / 1000;
        tv.tv_usec = (config_.select_timeout_ms % 1000) * 1000;
        const int ready = ::select(maxfd + 1, &readfds, nullptr, nullptr, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0 && SSL_pending(ssl.get()) == 0) {
            continue;
        }

        if (FD_ISSET(client_fd_, &readfds) || SSL_pending(ssl.get()) > 0) {
            const int n = SSL_read(ssl.get(), io_buffer.data(), static_cast<int>(io_buffer.size()));
            if (n <= 0) {
                logger_.debug("[tls] client closed or SSL_read failed");
                break;
            }
            client_buf.append(io_buffer.data(), static_cast<std::size_t>(n));

            std::string message;
            while (extract_one_rtsp_message(client_buf, message, "client", limits)) {
                logger_.trace(rtsp_message_summary("[in ] client -> proxy", message));
                if (!message.empty() && static_cast<unsigned char>(message[0]) == '$') {
                    if (camera_fd >= 0 && !send_all(camera_fd, message.data(), message.size())) goto done;
                    continue;
                }

                if (channel < 0) {
                    const std::string request_target = extract_request_target(message);
                    channel = parse_channel_from_request_target(request_target, config_.channels);
                    if (channel < 0) {
                        logger_.warn("[client] invalid path. Use /ch0 .. /chN");
                        goto done;
                    }
                    rewriter = std::make_unique<RtspRewriter>(config_, channel,
                                                              extract_public_base_from_target(request_target, channel));
                    try {
                        camera_fd = connect_camera();
                    } catch (const std::exception& e) {
                        logger_.error("[camera] connect failed: " + std::string(e.what()));
                        goto done;
                    }
                    logger_.info("[client] channel " + std::to_string(channel) + " -> " + rewriter->upstream_base());
                }

                std::string out = rewriter->rewrite_client_request(message);
                if (authenticator.has_challenge()) {
                    out = authenticator.authorize(out);
                }
                last_upstream_request = out;
                logger_.debug("[rtsp] client -> camera " + rtsp_method_from_request(out));
                logger_.trace(rtsp_message_summary("[out] proxy -> camera", out));
                if (!send_all(camera_fd, out.data(), out.size())) goto done;
            }
        }

        if (camera_fd >= 0 && FD_ISSET(camera_fd, &readfds)) {
            const ssize_t n = ::recv(camera_fd, io_buffer.data(), io_buffer.size(), 0);
            if (n <= 0) {
                logger_.debug(n == 0 ? "[tcp] camera closed connection" : "[tcp] recv from camera failed");
                break;
            }
            camera_buf.append(io_buffer.data(), static_cast<std::size_t>(n));

            std::string message;
            while (extract_one_rtsp_message(camera_buf, message, "camera", limits)) {
                logger_.trace(rtsp_message_summary("[in ] camera -> proxy", message));
                std::string out = message;
                if (message.empty() || static_cast<unsigned char>(message[0]) != '$') {
                    const int status = rtsp_status_code(message);
                    logger_.debug("[rtsp] camera -> proxy " + std::to_string(status));
                    if (status == 401 && !last_upstream_request.empty() &&
                        authenticator.update_from_unauthorized(message)) {
                        std::string retry = authenticator.authorize(last_upstream_request);
                        logger_.info("[auth] camera requested Digest auth; retrying RTSP request");
                        logger_.trace(rtsp_message_summary("[out] proxy -> camera", retry));
                        if (!send_all(camera_fd, retry.data(), retry.size())) goto done;
                        last_upstream_request = retry;
                        continue;
                    }
                    out = rewriter->rewrite_camera_response(message);
                }
                logger_.trace(rtsp_message_summary("[out] proxy -> client", out));
                if (!ssl_write_all(ssl.get(), out.data(), out.size())) goto done;
            }
        }
    }

done:
    if (camera_fd >= 0) {
        ::close(camera_fd);
    }
    SSL_shutdown(ssl.get());
    ::close(client_fd_);
    logger_.info("[client] disconnected");
}

}  // namespace rtsps
