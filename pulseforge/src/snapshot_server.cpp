#include "snapshot_server.hpp"

#include "common.hpp"
#include "snapshot_protocol.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

bool recv_full(int fd, void *buf, size_t len) {
    auto *p = static_cast<unsigned char *>(buf);
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n <= 0) {
            return false; /* error or peer closed early */
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

bool send_full(int fd, const void *buf, size_t len) {
    const auto *p = static_cast<const unsigned char *>(buf);
    size_t sent = 0;
    while (sent < len) {
        /* MSG_NOSIGNAL: a receiver that disconnects mid-response must not
         * SIGPIPE-kill the publisher (see ws_server.cpp send_full) */
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

} // namespace

SnapshotServer::SnapshotServer()
    : ring_(kSnapshotRingCapacity), slot_seq_(kSnapshotRingCapacity, UINT64_MAX) {}

SnapshotServer::~SnapshotServer() { stop(); }

void SnapshotServer::start(int port) {
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
          "bind(snapshot)");
    CHECK(listen(listen_sock_->get(), 64), "listen(snapshot)");

    thread_ = std::thread(&SnapshotServer::accept_loop, this, port);
}

void SnapshotServer::stop() {
    if (!stop_.exchange(true)) {
        if (thread_.joinable()) {
            thread_.join();
        }
    }
}

void SnapshotServer::record(const TickMessage &msg) {
    std::lock_guard<std::mutex> lock(ring_mu_);
    size_t slot = static_cast<size_t>(msg.sequence % kSnapshotRingCapacity);
    ring_[slot]     = msg;
    slot_seq_[slot] = msg.sequence;
}

std::vector<TickMessage> SnapshotServer::serve_request(uint64_t from_seq,
                                                        uint64_t count) {
    std::vector<TickMessage> out;
    std::lock_guard<std::mutex> lock(ring_mu_);
    for (uint64_t s = from_seq; s < from_seq + count; s++) {
        size_t slot = static_cast<size_t>(s % kSnapshotRingCapacity);
        if (slot_seq_[slot] != s) {
            break; /* evicted, or never generated: stop at first hole */
        }
        out.push_back(ring_[slot]);
    }
    return out;
}

void SnapshotServer::handle_client(int client_fd) {
    SnapshotRequest req{};
    if (!recv_full(client_fd, &req, sizeof(req))) {
        ::close(client_fd);
        return;
    }
    uint64_t count = req.count;
    if (count > kMaxSnapshotRequestLen) {
        count = kMaxSnapshotRequestLen;
    }

    std::vector<TickMessage> msgs = serve_request(req.from_seq, count);

    SnapshotResponseHeader header{};
    header.returned_count = msgs.size();
    if (send_full(client_fd, &header, sizeof(header)) && !msgs.empty()) {
        send_full(client_fd, msgs.data(), msgs.size() * sizeof(TickMessage));
    }
    ::close(client_fd);
}

void SnapshotServer::accept_loop(int port) {
    (void)port;
    while (!stop_.load(std::memory_order_relaxed)) {
        pollfd pfd{};
        pfd.fd     = listen_sock_->get();
        pfd.events = POLLIN;
        /* Short timeout so stop_ is re-checked promptly instead of
         * blocking in accept() forever (mirrors why receiver.cpp omits
         * SA_RESTART on recvfrom: a responsive shutdown check matters
         * more here than shaving one syscall). */
        int rc = poll(&pfd, 1, 200 /* ms */);
        if (rc <= 0) {
            continue; /* timeout, or EINTR - just recheck stop_ */
        }
        int client_fd = accept(listen_sock_->get(), nullptr, nullptr);
        if (client_fd < 0) {
            continue;
        }
        handle_client(client_fd);
    }
}
