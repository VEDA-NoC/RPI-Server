#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct RtspUrl {
    std::string host;
    int port = 8554;
    std::string path;
    std::string base;
};

struct Response {
    int status = 0;
    std::string header;
    std::string body;
};

std::string lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool starts_with_ci(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) {
        return false;
    }
    return lower_copy(s.substr(0, prefix.size())) == lower_copy(prefix);
}

std::string first_line(const std::string& message) {
    const size_t end = message.find("\r\n");
    if (end == std::string::npos) {
        return message;
    }
    return message.substr(0, end);
}

RtspUrl parse_rtsps_url(const std::string& url) {
    const std::string scheme = "rtsps://";
    if (url.rfind(scheme, 0) != 0) {
        throw std::runtime_error("URL must start with rtsps://");
    }

    const size_t host_start = scheme.size();
    const size_t path_start = url.find('/', host_start);
    if (path_start == std::string::npos) {
        throw std::runtime_error("URL must include path, for example /ch1");
    }

    std::string host_port = url.substr(host_start, path_start - host_start);
    RtspUrl out;
    out.path = url.substr(path_start);
    out.base = url;

    const size_t colon = host_port.rfind(':');
    if (colon == std::string::npos) {
        out.host = host_port;
    } else {
        out.host = host_port.substr(0, colon);
        out.port = std::stoi(host_port.substr(colon + 1));
    }
    if (out.host.empty() || out.path.empty()) {
        throw std::runtime_error("invalid rtsps URL");
    }
    return out;
}

int connect_tcp(const std::string& host, int port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    const std::string port_str = std::to_string(port);
    int rc = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
    if (rc != 0) {
        throw std::runtime_error("getaddrinfo failed: " + std::string(gai_strerror(rc)));
    }

    int fd = -1;
    for (addrinfo* p = result; p != nullptr; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    if (fd < 0) {
        throw std::runtime_error("connect failed");
    }
    return fd;
}

int content_length_from_header(const std::string& header) {
    size_t start = 0;
    while (start < header.size()) {
        size_t end = header.find("\r\n", start);
        if (end == std::string::npos) {
            break;
        }
        std::string line = header.substr(start, end - start);
        if (starts_with_ci(line, "Content-Length:")) {
            std::string value = line.substr(std::string("Content-Length:").size());
            return std::max(0, std::stoi(value));
        }
        start = end + 2;
    }
    return 0;
}

void ssl_write_all(SSL* ssl, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = SSL_write(ssl, data.data() + sent, static_cast<int>(data.size() - sent));
        if (n <= 0) {
            throw std::runtime_error("SSL_write failed");
        }
        sent += static_cast<size_t>(n);
    }
}

bool read_one_rtsp_response(SSL* ssl, std::string& buffer, Response& response) {
    while (true) {
        if (!buffer.empty() && static_cast<unsigned char>(buffer[0]) == '$') {
            std::cerr << "[test] unexpected RTP before RTSP response, buffered=" << buffer.size() << "\n";
            return false;
        }

        const size_t header_end = buffer.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            const std::string header = buffer.substr(0, header_end + 4);
            const int body_len = content_length_from_header(header);
            const size_t total = header_end + 4 + static_cast<size_t>(body_len);
            if (buffer.size() >= total) {
                response.header = header;
                response.body = buffer.substr(header_end + 4, static_cast<size_t>(body_len));
                buffer.erase(0, total);

                std::istringstream iss(header);
                std::string version;
                iss >> version >> response.status;
                std::cerr << "[test] response first line: " << first_line(header) << "\n";
                std::cerr << "[test] response header bytes=" << header.size() << " body bytes=" << body_len
                          << " remaining buffered=" << buffer.size() << "\n";
                return true;
            }
        }

        char temp[8192];
        int n = SSL_read(ssl, temp, sizeof(temp));
        if (n <= 0) {
            const int err = SSL_get_error(ssl, n);
            std::cerr << "[test] SSL_read failed err=" << err << " buffered=" << buffer.size() << "\n";
            ERR_print_errors_fp(stderr);
            throw std::runtime_error("SSL_read failed while waiting RTSP response");
        }
        std::cerr << "[test] read bytes=" << n << "\n";
        buffer.append(temp, static_cast<size_t>(n));
    }
}

std::string find_session(const std::string& header) {
    size_t start = 0;
    while (start < header.size()) {
        size_t end = header.find("\r\n", start);
        if (end == std::string::npos) {
            break;
        }
        std::string line = header.substr(start, end - start);
        if (starts_with_ci(line, "Session:")) {
            std::string value = line.substr(std::string("Session:").size());
            const size_t first = value.find_first_not_of(" \t");
            if (first == std::string::npos) {
                return {};
            }
            value = value.substr(first);
            const size_t semicolon = value.find(';');
            if (semicolon != std::string::npos) {
                value.resize(semicolon);
            }
            return value;
        }
        start = end + 2;
    }
    return {};
}

std::string find_first_media_control(const std::string& sdp) {
    std::istringstream iss(sdp);
    std::string line;
    bool in_video = false;
    std::string session_control;

    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("m=", 0) == 0) {
            in_video = line.rfind("m=video", 0) == 0;
            continue;
        }
        if (line.rfind("a=control:", 0) == 0) {
            std::string control = line.substr(std::string("a=control:").size());
            if (in_video && control != "*") {
                return control;
            }
            if (!in_video && session_control.empty() && control != "*") {
                session_control = control;
            }
        }
    }

    return session_control;
}

std::string make_control_url(const std::string& base, const std::string& control) {
    if (control.empty() || control == "*") {
        return base;
    }
    const std::string l = lower_copy(control);
    if (l.rfind("rtsp://", 0) == 0 || l.rfind("rtsps://", 0) == 0) {
        return control;
    }
    if (base.back() == '/' || control.front() == '/') {
        return base + control;
    }
    return base + "/" + control;
}

std::string make_request(const std::string& method, const std::string& url, int cseq,
                         const std::vector<std::string>& headers = {}) {
    std::ostringstream oss;
    oss << method << " " << url << " RTSP/1.0\r\n";
    oss << "CSeq: " << cseq << "\r\n";
    oss << "User-Agent: rpi-rtsps-test-client/1.0\r\n";
    for (const auto& h : headers) {
        oss << h << "\r\n";
    }
    oss << "\r\n";
    return oss.str();
}

Response send_request(SSL* ssl, std::string& buffer, const std::string& label, const std::string& request) {
    std::cerr << "[test] sending " << label << ": " << first_line(request) << "\n";
    ssl_write_all(ssl, request);
    Response res;
    if (!read_one_rtsp_response(ssl, buffer, res)) {
        throw std::runtime_error("unexpected RTP data while waiting for " + label);
    }
    std::cout << label << " " << res.status << "\n";
    if (res.status < 200 || res.status >= 300) {
        std::cerr << res.header << res.body << std::endl;
        throw std::runtime_error(label + " failed");
    }
    return res;
}

void count_rtp_packets(SSL* ssl, std::string& buffer, int seconds, const std::string& dump_path) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::seconds(seconds);
    const auto report_every = std::chrono::seconds(1);
    auto next_report = clock::now() + report_every;

    std::ofstream dump;
    if (!dump_path.empty()) {
        dump.open(dump_path, std::ios::binary);
        if (!dump) {
            throw std::runtime_error("failed to open dump file: " + dump_path);
        }
        std::cout << "Dump file: " << dump_path << "\n";
    }

    uint64_t packets = 0;
    uint64_t bytes = 0;
    uint64_t rtsp_messages = 0;

    while (clock::now() < deadline) {
        while (true) {
            if (buffer.size() >= 4 && static_cast<unsigned char>(buffer[0]) == '$') {
                const uint16_t len =
                    (static_cast<unsigned char>(buffer[2]) << 8) | static_cast<unsigned char>(buffer[3]);
                const size_t total = 4 + len;
                if (buffer.size() < total) {
                    break;
                }
                ++packets;
                bytes += len;
                if (dump) {
                    dump.write(buffer.data(), static_cast<std::streamsize>(total));
                }
                buffer.erase(0, total);
                continue;
            }

            const size_t header_end = buffer.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                const std::string header = buffer.substr(0, header_end + 4);
                const int body_len = content_length_from_header(header);
                const size_t total = header_end + 4 + static_cast<size_t>(body_len);
                if (buffer.size() < total) {
                    break;
                }
                ++rtsp_messages;
                buffer.erase(0, total);
                continue;
            }
            break;
        }

        if (clock::now() >= next_report) {
            std::cout << "RTP packets: " << packets << ", bytes: " << bytes
                      << ", extra RTSP messages: " << rtsp_messages << "\n";
            next_report += report_every;
        }

        const int fd = SSL_get_fd(ssl);
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        timeval tv{};
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ready = select(fd + 1, &readfds, nullptr, nullptr, &tv);
        if (ready < 0) {
            throw std::runtime_error("select failed while receiving RTP");
        }
        if (ready == 0) {
            continue;
        }

        char temp[8192];
        int n = SSL_read(ssl, temp, sizeof(temp));
        if (n <= 0) {
            const int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                continue;
            }
            std::cerr << "[test] SSL_read RTP failed err=" << err << "\n";
            ERR_print_errors_fp(stderr);
            throw std::runtime_error("SSL_read failed while receiving RTP");
        }
        std::cerr << "[test] RTP read bytes=" << n << " buffered_before_parse=" << buffer.size() << "\n";
        buffer.append(temp, static_cast<size_t>(n));
    }

    std::cout << "FINAL RTP packets: " << packets << ", bytes: " << bytes << ", extra RTSP messages: " << rtsp_messages
              << "\n";
    if (dump) {
        dump.flush();
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 4) {
        std::cerr << "Usage: " << argv[0] << " rtsps://host:port/ch1 [seconds] [interleaved_dump.bin]\n";
        return 1;
    }

    const std::string url_text = argv[1];
    const int seconds = (argc >= 3) ? std::stoi(argv[2]) : 10;
    const std::string dump_path = (argc == 4) ? argv[3] : "";

    try {
        std::cout.setf(std::ios::unitbuf);
        RtspUrl url = parse_rtsps_url(url_text);

        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_ssl_algorithms();

        SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) {
            throw std::runtime_error("SSL_CTX_new failed");
        }

        // Test client: accept self-signed certificates generated on the Raspberry Pi.
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

        int fd = connect_tcp(url.host, url.port);
        SSL* ssl = SSL_new(ctx);
        if (!ssl) {
            close(fd);
            SSL_CTX_free(ctx);
            throw std::runtime_error("SSL_new failed");
        }
        SSL_set_fd(ssl, fd);
        SSL_set_tlsext_host_name(ssl, url.host.c_str());

        while (true) {
            const int connect_result = SSL_connect(ssl);
            if (connect_result == 1) {
                break;
            }
            const int err = SSL_get_error(ssl, connect_result);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                ERR_print_errors_fp(stderr);
                throw std::runtime_error("SSL_connect failed");
            }

            fd_set rfds;
            fd_set wfds;
            FD_ZERO(&rfds);
            FD_ZERO(&wfds);
            if (err == SSL_ERROR_WANT_READ) {
                FD_SET(fd, &rfds);
            } else {
                FD_SET(fd, &wfds);
            }
            select(fd + 1, &rfds, &wfds, nullptr, nullptr);
        }
        std::cout << "TLS connected\n";

        std::string buffer;
        int cseq = 1;

        send_request(ssl, buffer, "OPTIONS", make_request("OPTIONS", url.base, cseq++));

        Response describe = send_request(ssl, buffer, "DESCRIBE",
                                         make_request("DESCRIBE", url.base, cseq++, {"Accept: application/sdp"}));

        std::string control = find_first_media_control(describe.body);
        std::string setup_url = make_control_url(url.base, control);
        std::cout << "SETUP URL: " << setup_url << "\n";

        Response setup =
            send_request(ssl, buffer, "SETUP",
                         make_request("SETUP", setup_url, cseq++, {"Transport: RTP/AVP/TCP;unicast;interleaved=0-1"}));

        std::string session = find_session(setup.header);
        if (session.empty()) {
            throw std::runtime_error("SETUP response has no Session header");
        }
        std::cout << "Session: " << session << "\n";

        send_request(ssl, buffer, "PLAY", make_request("PLAY", url.base, cseq++, {"Session: " + session}));

        count_rtp_packets(ssl, buffer, seconds, dump_path);

        ssl_write_all(ssl, make_request("TEARDOWN", url.base, cseq++, {"Session: " + session}));

        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(fd);
        SSL_CTX_free(ctx);
        EVP_cleanup();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
