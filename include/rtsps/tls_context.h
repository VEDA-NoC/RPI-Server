#pragma once

#include <openssl/ssl.h>

#include "rtsps/app_config.h"

namespace rtsps {

class TlsContext {
public:
    explicit TlsContext(const AppConfig& config);
    ~TlsContext();

    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    SSL_CTX* get() const;

private:
    SSL_CTX* ctx_ = nullptr;
};

}  // namespace rtsps
