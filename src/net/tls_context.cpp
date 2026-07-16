#include "rtsps/tls_context.h"

#include <stdexcept>

namespace rtsps {

TlsContext::TlsContext(const AppConfig& config) {
    ctx_ = SSL_CTX_new(TLS_server_method());
    if (!ctx_) {
        throw std::runtime_error("SSL_CTX_new() failed");
    }
    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
    if (SSL_CTX_use_certificate_file(ctx_, config.cert_file.c_str(), SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
        throw std::runtime_error("failed to load certificate: " + config.cert_file);
    }
    if (SSL_CTX_use_PrivateKey_file(ctx_, config.key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
        throw std::runtime_error("failed to load private key: " + config.key_file);
    }
    if (SSL_CTX_check_private_key(ctx_) != 1) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
        throw std::runtime_error("certificate and private key do not match");
    }
}

TlsContext::~TlsContext() {
    if (ctx_) {
        SSL_CTX_free(ctx_);
    }
}

SSL_CTX* TlsContext::get() const { return ctx_; }

}  // namespace rtsps
