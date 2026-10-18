/*
 * soupbin.hpp - SoupBinTCP-style session framing over TCP.
 *
 * Responsibility: TCP gives you a reliable, ordered BYTE STREAM - not
 * messages. One send() of 50 bytes can arrive as recv()s of 20 + 30
 * bytes, and two sends can arrive glued together in one recv(). Every
 * TCP protocol therefore needs framing: a way to find where one message
 * ends and the next begins. NASDAQ's SoupBinTCP (used under OUCH order
 * entry and GLIMPSE snapshots) does it with a length prefix:
 *
 *     packet_length (u16, big-endian, counts the type byte + payload)
 *     packet_type   (1 ASCII char)
 *     payload       (packet_length - 1 bytes)
 *
 * Packet types (subset):
 *   client -> server: 'L' login request, 'U' unsequenced data (orders),
 *                     'R' client heartbeat, 'O' logout request
 *   server -> client: 'A' login accepted, 'J' login rejected,
 *                     'S' sequenced data (execution reports), 'H' server
 *                     heartbeat, 'Z' end of session
 * "Sequenced" server messages are numbered so a client that reconnects
 * can ask to resume from message N (the replay is not implemented here;
 * login always starts at 1 - documented limitation).
 *
 * FrameReader is the receive side: recv() straight into its buffer,
 * then pull complete frames out of it. The classic bugs it avoids:
 *   - assuming one recv() == one message (works on loopback in testing,
 *     breaks under load or on a real network)
 *   - an attacker/bug sending length 65535 and making you wait forever
 *     or allocate huge buffers (we cap at kMaxFrame and call it an error)
 *
 * Thread-safety: single-threaded; one FrameReader per connection.
 */

#ifndef PULSEFORGE_SOUPBIN_HPP
#define PULSEFORGE_SOUPBIN_HPP

#include "wire.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace soup {

constexpr char kLoginRequest    = 'L';
constexpr char kLoginAccepted   = 'A';
constexpr char kLoginRejected   = 'J';
constexpr char kSequencedData   = 'S';
constexpr char kUnsequencedData = 'U';
constexpr char kServerHeartbeat = 'H';
constexpr char kClientHeartbeat = 'R';
constexpr char kEndOfSession    = 'Z';
constexpr char kLogoutRequest   = 'O';

/* Largest legal packet_length. Our biggest payload is ~60 bytes; 1 KiB
 * leaves headroom while still bounding what a bad peer can make us do. */
constexpr size_t kMaxFrame       = 1024;
constexpr size_t kFrameHeaderLen = 3; /* u16 length + type */

constexpr size_t kLoginRequestLen  = 46; /* user 6, pass 10, session 10, seq 20 */
constexpr size_t kLoginAcceptedLen = 30; /* session 10, seq 20                  */

/* Writes one frame into `out` (hot path: a stack buffer). Returns total
 * bytes, or 0 if it does not fit / is too large. */
inline size_t write_frame(uint8_t *out, size_t cap, char type,
                          const uint8_t *payload, size_t n) {
    if (n + 1 > kMaxFrame || cap < n + kFrameHeaderLen) {
        return 0;
    }
    wire::put_u16(out, static_cast<uint16_t>(n + 1));
    out[2] = static_cast<uint8_t>(type);
    if (n > 0) {
        std::memcpy(out + 3, payload, n);
    }
    return n + kFrameHeaderLen;
}

/* Appends one frame to a growable buffer (exchange-side output queue). */
inline void append_frame(std::vector<uint8_t> &out, char type,
                         const uint8_t *payload, size_t n) {
    size_t old = out.size();
    out.resize(old + n + kFrameHeaderLen);
    (void)write_frame(out.data() + old, n + kFrameHeaderLen, type, payload, n);
}

/* 20-byte numeric ASCII field, right-justified, space padded. */
inline void put_numeric20(uint8_t *p, uint64_t v) {
    char digits[20];
    auto res = std::to_chars(digits, digits + sizeof(digits), v);
    size_t n = static_cast<size_t>(res.ptr - digits);
    std::memset(p, ' ', 20 - n);
    std::memcpy(p + (20 - n), digits, n);
}

inline bool get_numeric20(const uint8_t *p, uint64_t &out) {
    const char *s = reinterpret_cast<const char *>(p);
    const char *e = s + 20;
    while (s < e && *s == ' ') ++s;
    if (s == e) {
        out = 0;
        return true;
    }
    auto res = std::from_chars(s, e, out);
    return res.ec == std::errc() && res.ptr == e;
}

struct LoginRequest {
    std::string username;          /* 6 chars max  */
    std::string password;          /* 10 chars max */
    std::string requested_session; /* blank = current session */
    uint64_t    requested_sequence = 1;
};

inline size_t encode_login_request(const LoginRequest &m, uint8_t *out, size_t cap) {
    if (cap < kLoginRequestLen) {
        return 0;
    }
    wire::put_alpha(out, 6, m.username);
    wire::put_alpha(out + 6, 10, m.password);
    wire::put_alpha(out + 16, 10, m.requested_session);
    put_numeric20(out + 26, m.requested_sequence);
    return kLoginRequestLen;
}

inline bool decode_login_request(const uint8_t *p, size_t n, LoginRequest &m) {
    if (n != kLoginRequestLen) {
        return false;
    }
    m.username          = std::string(wire::get_alpha(p, 6));
    m.password          = std::string(wire::get_alpha(p + 6, 10));
    m.requested_session = std::string(wire::get_alpha(p + 16, 10));
    return get_numeric20(p + 26, m.requested_sequence);
}

struct LoginAccepted {
    std::string session;
    uint64_t    sequence = 1; /* next sequenced message the server will send */
};

inline size_t encode_login_accepted(const LoginAccepted &m, uint8_t *out, size_t cap) {
    if (cap < kLoginAcceptedLen) {
        return 0;
    }
    wire::put_alpha(out, 10, m.session);
    put_numeric20(out + 10, m.sequence);
    return kLoginAcceptedLen;
}

inline bool decode_login_accepted(const uint8_t *p, size_t n, LoginAccepted &m) {
    if (n != kLoginAcceptedLen) {
        return false;
    }
    m.session = std::string(wire::get_alpha(p, 10));
    return get_numeric20(p + 10, m.sequence);
}

/*
 * FrameReader - reassembles frames from an arbitrary chunking of bytes.
 *
 *   uint8_t *dst = r.write_ptr(); size_t room = r.write_space();
 *   ssize_t n = recv(fd, dst, room, 0);   r.commit(n);
 *   while (r.next(type, payload, len) == Status::Frame) handle(...);
 *
 * The payload pointer points into the reader's buffer and is valid only
 * until the next write_ptr()/commit() (which may compact the buffer).
 * Fixed 64 KiB in-object buffer: no allocation per connection after
 * construction, and bounded memory per peer.
 */
class FrameReader {
public:
    enum class Status { Frame, NeedMore, Error };

    uint8_t *write_ptr() {
        /* Make room: slide the unparsed tail to the front once the free
         * space at the end can no longer hold a maximum-size frame. */
        if (kBufSize - wr_ < kMaxFrame + kFrameHeaderLen && rd_ > 0) {
            std::memmove(buf_.data(), buf_.data() + rd_, wr_ - rd_);
            wr_ -= rd_;
            rd_ = 0;
        }
        return buf_.data() + wr_;
    }

    size_t write_space() const { return kBufSize - wr_; }

    void commit(size_t n) { wr_ += n; }

    Status next(char &type, const uint8_t *&payload, size_t &len) {
        size_t avail = wr_ - rd_;
        if (avail < 2) {
            return Status::NeedMore;
        }
        uint16_t flen = wire::get_u16(buf_.data() + rd_);
        if (flen == 0 || flen > kMaxFrame) {
            return Status::Error; /* corrupt or hostile stream: drop the connection */
        }
        if (avail < 2u + flen) {
            return Status::NeedMore; /* partial frame - wait for more bytes */
        }
        type    = static_cast<char>(buf_[rd_ + 2]);
        payload = buf_.data() + rd_ + 3;
        len     = static_cast<size_t>(flen) - 1;
        rd_ += 2u + flen;
        if (rd_ == wr_) {
            rd_ = wr_ = 0; /* fully drained: rewind for free (no memmove) */
        }
        return Status::Frame;
    }

    size_t buffered() const { return wr_ - rd_; }

private:
    static constexpr size_t kBufSize = 64 * 1024;
    std::array<uint8_t, kBufSize> buf_{};
    size_t rd_ = 0;
    size_t wr_ = 0;
};

} // namespace soup

#endif /* PULSEFORGE_SOUPBIN_HPP */
