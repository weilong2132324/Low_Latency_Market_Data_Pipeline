#include "ws_server.hpp"

#include "common.hpp"
#include "sha1.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr const char *kWsMagicGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string base64_encode(const uint8_t *data, size_t len) {
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(kTable[(n >> 18) & 0x3f]);
        out.push_back(kTable[(n >> 12) & 0x3f]);
        out.push_back(kTable[(n >> 6) & 0x3f]);
        out.push_back(kTable[n & 0x3f]);
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t n = data[i] << 16;
        out.push_back(kTable[(n >> 18) & 0x3f]);
        out.push_back(kTable[(n >> 12) & 0x3f]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        uint32_t n = (data[i] << 16) | (data[i + 1] << 8);
        out.push_back(kTable[(n >> 18) & 0x3f]);
        out.push_back(kTable[(n >> 12) & 0x3f]);
        out.push_back(kTable[(n >> 6) & 0x3f]);
        out.push_back('=');
    }
    return out;
}

/* Reads one HTTP request (headers only, up to the blank line) off a
 * blocking socket, byte at a time - simple and correct; the handshake
 * is a one-time, tiny exchange, not a hot path. Returns empty on
 * error/EOF. */
std::string read_http_headers(int fd) {
    std::string req;
    char        c;
    while (req.size() < 8192) {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) {
            return std::string();
        }
        req.push_back(c);
        if (req.size() >= 4 &&
            req.compare(req.size() - 4, 4, "\r\n\r\n") == 0) {
            return req;
        }
    }
    return std::string();
}

std::string extract_header(const std::string &headers, const std::string &name) {
    size_t pos = headers.find(name + ":");
    if (pos == std::string::npos) {
        return std::string();
    }
    pos += name.size() + 1;
    size_t end = headers.find("\r\n", pos);
    std::string value = headers.substr(pos, end - pos);
    /* trim leading/trailing whitespace */
    size_t start = value.find_first_not_of(" \t");
    size_t stop  = value.find_last_not_of(" \t");
    if (start == std::string::npos) {
        return std::string();
    }
    return value.substr(start, stop - start + 1);
}

bool send_full(int fd, const void *buf, size_t len) {
    const auto *p = static_cast<const unsigned char *>(buf);
    size_t sent = 0;
    while (sent < len) {
        /* MSG_NOSIGNAL: writing to a socket whose peer has gone away
         * (a browser tab closed or reloaded) raises SIGPIPE, and SIGPIPE's
         * default action TERMINATES THE PROCESS. Without this flag every
         * page reload silently killed telemetry_bridge. With it, send()
         * just fails with EPIPE and the dead client is dropped. */
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

/* Builds one unmasked, FIN'd text frame (RFC 6455 Section 5.2). */
std::vector<uint8_t> build_text_frame(const std::string &payload) {
    std::vector<uint8_t> frame;
    frame.push_back(0x81); /* FIN=1, opcode=0x1 (text) */
    size_t len = payload.size();
    if (len <= 125) {
        frame.push_back(static_cast<uint8_t>(len));
    } else if (len <= 0xffff) {
        frame.push_back(126);
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xff));
        frame.push_back(static_cast<uint8_t>(len & 0xff));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; i--) {
            frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xff));
        }
    }
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

} // namespace

WebSocketServer::WebSocketServer() = default;
WebSocketServer::~WebSocketServer() { stop(); }

void WebSocketServer::start(int port) {
    listen_sock_ = std::make_unique<Socket>(AF_INET, SOCK_STREAM, 0);
    CHECK(listen_sock_->get(), "socket");

    int one = 1;
    CHECK(setsockopt(listen_sock_->get(), SOL_SOCKET, SO_REUSEADDR, &one,
                     sizeof(one)),
          "setsockopt(SO_REUSEADDR)");

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    CHECK(bind(listen_sock_->get(), reinterpret_cast<const sockaddr *>(&addr),
               static_cast<socklen_t>(sizeof(addr))),
          "bind(ws)");
    CHECK(listen(listen_sock_->get(), 16), "listen(ws)");

    thread_ = std::thread(&WebSocketServer::accept_loop, this, port);
}

void WebSocketServer::stop() {
    if (stop_.exchange(true)) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    std::lock_guard<std::mutex> lock(clients_mu_);
    for (int fd : clients_) {
        close(fd);
    }
    clients_.clear();
}

bool WebSocketServer::perform_handshake(int client_fd) {
    std::string headers = read_http_headers(client_fd);
    if (headers.empty()) {
        return false;
    }
    std::string key = extract_header(headers, "Sec-WebSocket-Key");
    if (key.empty()) {
        return false; /* not a WebSocket upgrade request */
    }

    auto        digest = sha1(key + kWsMagicGuid);
    std::string accept  = base64_encode(digest.data(), digest.size());

    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    return send_full(client_fd, response.data(), response.size());
}

void WebSocketServer::accept_loop(int port) {
    (void)port;
    while (!stop_.load(std::memory_order_relaxed)) {
        pollfd pfd{};
        pfd.fd     = listen_sock_->get();
        pfd.events = POLLIN;
        int rc = poll(&pfd, 1, 200 /* ms */);
        if (rc <= 0) {
            continue;
        }
        int client_fd = accept(listen_sock_->get(), nullptr, nullptr);
        if (client_fd < 0) {
            continue;
        }
        if (!perform_handshake(client_fd)) {
            close(client_fd);
            continue;
        }
        std::lock_guard<std::mutex> lock(clients_mu_);
        clients_.push_back(client_fd);
    }
}

void WebSocketServer::broadcast_text(const std::string &payload) {
    std::vector<uint8_t> frame = build_text_frame(payload);

    std::lock_guard<std::mutex> lock(clients_mu_);
    std::vector<int> alive;
    alive.reserve(clients_.size());
    for (int fd : clients_) {
        if (send_full(fd, frame.data(), frame.size())) {
            alive.push_back(fd);
        } else {
            close(fd);
        }
    }
    clients_ = std::move(alive);
}

size_t WebSocketServer::client_count() const {
    std::lock_guard<std::mutex> lock(clients_mu_);
    return clients_.size();
}
