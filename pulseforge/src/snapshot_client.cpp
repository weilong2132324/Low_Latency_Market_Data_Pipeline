#include "snapshot_client.hpp"

#include "snapshot_protocol.hpp"

#include <arpa/inet.h>
#include <cstdio>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

bool recv_full(int fd, void *buf, size_t len) {
    auto *p = static_cast<unsigned char *>(buf);
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n <= 0) {
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

bool send_full(int fd, const void *buf, size_t len) {
    const auto *p = static_cast<const unsigned char *>(buf);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

} // namespace

std::vector<TickMessage> request_snapshot_recovery(std::string host, int port,
                                                    uint64_t from_seq,
                                                    uint64_t count) {
    std::vector<TickMessage> result;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::fprintf(stderr, "snapshot recovery: socket() failed\n");
        return result;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "snapshot recovery: bad host '%s'\n", host.c_str());
        close(fd);
        return result;
    }

    if (connect(fd, reinterpret_cast<const sockaddr *>(&addr),
                static_cast<socklen_t>(sizeof(addr))) != 0) {
        std::fprintf(stderr,
                "snapshot recovery: connect to %s:%d failed - repair for "
                "seq [%llu, %llu) will stay a gap\n",
                host.c_str(), port, static_cast<unsigned long long>(from_seq),
                static_cast<unsigned long long>(from_seq + count));
        close(fd);
        return result;
    }

    SnapshotRequest req{};
    req.from_seq = from_seq;
    req.count    = count;
    if (!send_full(fd, &req, sizeof(req))) {
        close(fd);
        return result;
    }

    SnapshotResponseHeader header{};
    if (!recv_full(fd, &header, sizeof(header))) {
        close(fd);
        return result;
    }

    if (header.returned_count > 0) {
        result.resize(static_cast<size_t>(header.returned_count));
        if (!recv_full(fd, result.data(),
                       result.size() * sizeof(TickMessage))) {
            result.clear(); /* partial read is not a safe partial result */
        }
    }

    close(fd);
    return result;
}
