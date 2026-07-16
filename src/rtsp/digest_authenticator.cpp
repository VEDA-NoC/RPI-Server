#include "rtsps/digest_authenticator.h"

#include "rtsps/rtsp_message_parser.h"

#include <iomanip>
#include <openssl/evp.h>
#include <sstream>
#include <stdexcept>

namespace rtsps {
namespace {

std::string md5_hex(const std::string& input) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        throw std::runtime_error("EVP_MD_CTX_new() failed");
    }
    if (EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, input.data(), input.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("MD5 digest failed");
    }
    EVP_MD_CTX_free(ctx);

    std::ostringstream oss;
    for (unsigned int i = 0; i < digest_len; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    }
    return oss.str();
}

std::string digest_param(const std::string& auth, const std::string& key) {
    const std::string needle = key + "=";
    std::size_t pos = lower_copy(auth).find(lower_copy(needle));
    if (pos == std::string::npos) {
        return {};
    }
    pos += needle.size();
    if (pos >= auth.size()) {
        return {};
    }
    if (auth[pos] == '"') {
        const std::size_t end = auth.find('"', pos + 1);
        if (end == std::string::npos) {
            return {};
        }
        return auth.substr(pos + 1, end - pos - 1);
    }
    const std::size_t end = auth.find(',', pos);
    if (end == std::string::npos) {
        return trim_copy(auth.substr(pos));
    }
    return trim_copy(auth.substr(pos, end - pos));
}

DigestChallenge parse_digest_challenge(const std::string& message) {
    DigestChallenge challenge;
    const std::string auth = header_value(message, "WWW-Authenticate");
    if (auth.empty() || lower_copy(auth).find("digest") == std::string::npos) {
        return challenge;
    }
    challenge.realm = digest_param(auth, "realm");
    challenge.nonce = digest_param(auth, "nonce");
    challenge.qop = digest_param(auth, "qop");
    if (!challenge.qop.empty()) {
        const std::size_t comma = challenge.qop.find(',');
        if (comma != std::string::npos) {
            challenge.qop = challenge.qop.substr(0, comma);
        }
        challenge.qop = trim_copy(challenge.qop);
    }
    challenge.valid = !challenge.realm.empty() && !challenge.nonce.empty();
    return challenge;
}

std::string make_nc(uint32_t value) {
    std::ostringstream oss;
    oss << std::hex << std::setw(8) << std::setfill('0') << value;
    return oss.str();
}

std::string add_authorization_header(const std::string& request, const std::string& auth_header) {
    const std::size_t header_end = request.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return request;
    }

    std::string out = request;
    const std::string lower = lower_copy(out);
    const std::size_t line_start = lower.find("\r\nauthorization:");
    if (line_start != std::string::npos && line_start < header_end) {
        const std::size_t line_end = out.find("\r\n", line_start + 2);
        out.erase(line_start + 2, line_end - line_start - 2);
        out.insert(line_start + 2, auth_header);
        return out;
    }
    out.insert(header_end + 2, auth_header + "\r\n");
    return out;
}

}  // namespace

DigestAuthenticator::DigestAuthenticator(const AppConfig& config) : config_(config) {}

bool DigestAuthenticator::has_challenge() const {
    return challenge_.valid;
}

bool DigestAuthenticator::update_from_unauthorized(const std::string& response) {
    DigestChallenge parsed = parse_digest_challenge(response);
    if (!parsed.valid) {
        return false;
    }
    challenge_ = parsed;
    return true;
}

std::string DigestAuthenticator::build_authorization(const std::string& method, const std::string& uri,
                                                     uint32_t nc_value) const {
    const std::string qop = challenge_.qop.empty() ? "" : "auth";
    const std::string nc = make_nc(nc_value);
    const std::string cnonce = "rpi-rtsps-proxy";
    const std::string ha1 = md5_hex(config_.camera_user + ":" + challenge_.realm + ":" + config_.camera_password);
    const std::string ha2 = md5_hex(method + ":" + uri);

    std::string response;
    if (qop.empty()) {
        response = md5_hex(ha1 + ":" + challenge_.nonce + ":" + ha2);
    } else {
        response = md5_hex(ha1 + ":" + challenge_.nonce + ":" + nc + ":" + cnonce + ":" + qop + ":" + ha2);
    }

    std::ostringstream oss;
    oss << "Authorization: Digest username=\"" << config_.camera_user << "\", realm=\"" << challenge_.realm
        << "\", nonce=\"" << challenge_.nonce << "\", uri=\"" << uri << "\", response=\"" << response << "\"";
    if (!qop.empty()) {
        oss << ", qop=" << qop << ", nc=" << nc << ", cnonce=\"" << cnonce << "\"";
    }
    return oss.str();
}

std::string DigestAuthenticator::authorize(const std::string& request) {
    if (!challenge_.valid) {
        return request;
    }
    std::istringstream iss(get_first_line(request));
    std::string method;
    std::string uri;
    std::string version;
    iss >> method >> uri >> version;
    if (method.empty() || uri.empty()) {
        return request;
    }
    return add_authorization_header(request, build_authorization(method, uri, nc_++));
}

}  // namespace rtsps
