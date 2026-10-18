/*
 * itch.hpp - ITCH-style market data + MoldUDP64 packet framing.
 *
 * Responsibility: encode/decode the exchange's public market-data feed.
 * The message layouts below are byte-for-byte NASDAQ TotalView-ITCH 5.0
 * (the subset needed for an order-by-order book), carried inside
 * MoldUDP64 packets over UDP multicast - the same stack a real NASDAQ
 * feed handler parses.
 *
 * ITCH in one paragraph: it is an ORDER-BY-ORDER ("Level 3", "market by
 * order") feed. The exchange does not tell you "best bid is 100.01 x 500";
 * it tells you every individual event - order 7 added (buy 200 @ 100.01),
 * order 7 executed 50, order 7 cancelled 50, order 7 deleted - and YOU
 * rebuild the book by replaying them in sequence. That is why sequence
 * numbers and gap detection matter so much: miss one message and your
 * book is silently wrong forever (see feed_handler.hpp).
 *
 *   'S' System Event     12 bytes   start/end of day markers
 *   'R' Stock Directory  39 bytes   symbol <-> locate code mapping
 *   'A' Add Order        36 bytes   new resting order (visible liquidity)
 *   'E' Order Executed   31 bytes   resting order traded (partially or fully)
 *   'X' Order Cancel     23 bytes   resting order reduced by N shares
 *   'D' Order Delete     19 bytes   resting order removed entirely
 *   'G' End Of Snapshot  21 bytes   (GLIMPSE) snapshot done, resume at seq N
 *
 * Every ITCH message starts with: type(1) locate(2) tracking(2) timestamp(6).
 *   - stock locate: a small integer standing in for the symbol, so the
 *     hot path indexes an array instead of comparing 8-byte strings.
 *   - timestamp: ns since midnight, 48 bits.
 *
 * MoldUDP64 (the transport): one UDP datagram carries
 *     session(10) | sequence(8) | count(2) | {len(2) | message}...
 * "sequence" is the sequence number of the FIRST message in the packet;
 * the k-th message in the packet has sequence + k. Packing many small
 * messages into one datagram amortises the per-packet cost (syscall,
 * interrupt, UDP/IP header). A packet with count == 0 is a heartbeat: it
 * carries the NEXT expected sequence, so a receiver that missed the last
 * real packet during a quiet period can still detect the gap.
 *
 * Thread-safety: pure functions + a single-threaded PacketBuilder.
 */

#ifndef PULSEFORGE_ITCH_HPP
#define PULSEFORGE_ITCH_HPP

#include "trading_types.hpp"
#include "wire.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <variant>

namespace itch {

namespace len {
constexpr size_t SystemEvent    = 12;
constexpr size_t StockDirectory = 39;
constexpr size_t AddOrder       = 36;
constexpr size_t OrderExecuted  = 31;
constexpr size_t OrderCancel    = 23;
constexpr size_t OrderDelete    = 19;
constexpr size_t EndOfSnapshot  = 21;
} // namespace len

constexpr size_t kMaxMessageLen = 39;

/* System event codes (subset). */
constexpr char kStartOfMessages    = 'O';
constexpr char kStartOfMarketHours = 'Q';
constexpr char kEndOfMarketHours   = 'M';
constexpr char kEndOfMessages      = 'C';

struct SystemEvent {
    uint16_t locate;
    uint64_t timestamp;
    char     event_code;
};

struct StockDirectory {
    uint16_t locate;
    uint64_t timestamp;
    Symbol   stock;
    uint32_t round_lot_size;
};

struct AddOrder {
    uint16_t locate;
    uint64_t timestamp;
    uint64_t order_ref;
    Side     side;
    uint32_t shares;
    Symbol   stock;
    uint32_t price; /* 4 implied decimals, same scale as our Price */
};

struct OrderExecuted {
    uint16_t locate;
    uint64_t timestamp;
    uint64_t order_ref;
    uint32_t executed_shares;
    uint64_t match_number;
};

struct OrderCancel {
    uint16_t locate;
    uint64_t timestamp;
    uint64_t order_ref;
    uint32_t cancelled_shares;
};

struct OrderDelete {
    uint16_t locate;
    uint64_t timestamp;
    uint64_t order_ref;
};

struct EndOfSnapshot {
    uint64_t next_sequence; /* first incremental sequence NOT in the snapshot */
};

/* A decoded message is exactly one of these. std::variant is a
 * type-safe tagged union: it stores the largest alternative inline (no
 * heap), plus an index saying which one is live. See feed_handler.hpp
 * for the std::visit side. */
using Message = std::variant<SystemEvent, StockDirectory, AddOrder,
                             OrderExecuted, OrderCancel, OrderDelete,
                             EndOfSnapshot>;

/* ---- encoders: return bytes written, 0 if `cap` is too small ---------- */

namespace detail {
inline void header(wire::Writer &w, char type, uint16_t locate, uint64_t ts) {
    w.ch(type);
    w.u16(locate);
    w.u16(0); /* tracking number: NASDAQ-internal, unused here */
    w.u48(ts);
}
inline size_t done(const wire::Writer &w) { return w.ok() ? w.size() : 0; }
} // namespace detail

inline size_t encode(const SystemEvent &m, uint8_t *out, size_t cap) {
    wire::Writer w(out, cap);
    detail::header(w, 'S', m.locate, m.timestamp);
    w.ch(m.event_code);
    return detail::done(w);
}

inline size_t encode(const StockDirectory &m, uint8_t *out, size_t cap) {
    wire::Writer w(out, cap);
    detail::header(w, 'R', m.locate, m.timestamp);
    w.bytes(m.stock.data(), m.stock.size());
    w.ch('Q');      /* market category: NASDAQ Global Select       */
    w.ch('N');      /* financial status: normal                    */
    w.u32(m.round_lot_size);
    w.ch('N');      /* round lots only                             */
    w.ch('C');      /* issue classification: common stock          */
    w.alpha("Z", 2);/* issue sub-type                               */
    w.ch('P');      /* authenticity: production                    */
    w.ch(' ');      /* short sale threshold indicator: n/a         */
    w.ch(' ');      /* IPO flag: n/a                                */
    w.ch('1');      /* LULD reference price tier                   */
    w.ch('N');      /* ETP flag                                     */
    w.u32(0);       /* ETP leverage factor                         */
    w.ch('N');      /* inverse indicator                           */
    return detail::done(w);
}

inline size_t encode(const AddOrder &m, uint8_t *out, size_t cap) {
    wire::Writer w(out, cap);
    detail::header(w, 'A', m.locate, m.timestamp);
    w.u64(m.order_ref);
    w.ch(static_cast<char>(m.side));
    w.u32(m.shares);
    w.bytes(m.stock.data(), m.stock.size());
    w.u32(m.price);
    return detail::done(w);
}

inline size_t encode(const OrderExecuted &m, uint8_t *out, size_t cap) {
    wire::Writer w(out, cap);
    detail::header(w, 'E', m.locate, m.timestamp);
    w.u64(m.order_ref);
    w.u32(m.executed_shares);
    w.u64(m.match_number);
    return detail::done(w);
}

inline size_t encode(const OrderCancel &m, uint8_t *out, size_t cap) {
    wire::Writer w(out, cap);
    detail::header(w, 'X', m.locate, m.timestamp);
    w.u64(m.order_ref);
    w.u32(m.cancelled_shares);
    return detail::done(w);
}

inline size_t encode(const OrderDelete &m, uint8_t *out, size_t cap) {
    wire::Writer w(out, cap);
    detail::header(w, 'D', m.locate, m.timestamp);
    w.u64(m.order_ref);
    return detail::done(w);
}

/* GLIMPSE carries this sequence as 20 ASCII digits, right-justified
 * (left-padded with spaces), like SoupBinTCP sequence numbers. */
inline size_t encode(const EndOfSnapshot &m, uint8_t *out, size_t cap) {
    if (cap < len::EndOfSnapshot) {
        return 0;
    }
    out[0] = 'G';
    char digits[20];
    auto res = std::to_chars(digits, digits + sizeof(digits), m.next_sequence);
    size_t n = static_cast<size_t>(res.ptr - digits);
    std::memset(out + 1, ' ', 20 - n);
    std::memcpy(out + 1 + (20 - n), digits, n);
    return len::EndOfSnapshot;
}

/* ---- decoder ----------------------------------------------------------
 * Validates type AND exact length (a truncated or corrupt message must
 * never be half-applied to the book). Returns nullopt on anything
 * malformed or of a type we do not handle. */
inline std::optional<Message> decode(const uint8_t *p, size_t n) {
    if (n == 0) {
        return std::nullopt;
    }
    wire::Reader r(p, n);
    const char type = r.ch();

    auto common = [&r](uint16_t &locate, uint64_t &ts) {
        locate = r.u16();
        r.skip(2); /* tracking number */
        ts = r.u48();
    };

    switch (type) {
    case 'S': {
        if (n != len::SystemEvent) return std::nullopt;
        SystemEvent m{};
        common(m.locate, m.timestamp);
        m.event_code = r.ch();
        return r.ok() ? std::optional<Message>(m) : std::nullopt;
    }
    case 'R': {
        if (n != len::StockDirectory) return std::nullopt;
        StockDirectory m{};
        common(m.locate, m.timestamp);
        r.bytes(m.stock.data(), m.stock.size());
        r.skip(2); /* market category, financial status */
        m.round_lot_size = r.u32();
        return r.ok() ? std::optional<Message>(m) : std::nullopt;
    }
    case 'A': {
        if (n != len::AddOrder) return std::nullopt;
        AddOrder m{};
        common(m.locate, m.timestamp);
        m.order_ref = r.u64();
        char side   = r.ch();
        if (!is_valid_side(side)) return std::nullopt;
        m.side   = static_cast<Side>(side);
        m.shares = r.u32();
        r.bytes(m.stock.data(), m.stock.size());
        m.price = r.u32();
        return r.ok() ? std::optional<Message>(m) : std::nullopt;
    }
    case 'E': {
        if (n != len::OrderExecuted) return std::nullopt;
        OrderExecuted m{};
        common(m.locate, m.timestamp);
        m.order_ref       = r.u64();
        m.executed_shares = r.u32();
        m.match_number    = r.u64();
        return r.ok() ? std::optional<Message>(m) : std::nullopt;
    }
    case 'X': {
        if (n != len::OrderCancel) return std::nullopt;
        OrderCancel m{};
        common(m.locate, m.timestamp);
        m.order_ref        = r.u64();
        m.cancelled_shares = r.u32();
        return r.ok() ? std::optional<Message>(m) : std::nullopt;
    }
    case 'D': {
        if (n != len::OrderDelete) return std::nullopt;
        OrderDelete m{};
        common(m.locate, m.timestamp);
        m.order_ref = r.u64();
        return r.ok() ? std::optional<Message>(m) : std::nullopt;
    }
    case 'G': {
        if (n != len::EndOfSnapshot) return std::nullopt;
        const char *s = reinterpret_cast<const char *>(p + 1);
        const char *e = s + 20;
        while (s < e && *s == ' ') ++s;
        EndOfSnapshot m{};
        auto res = std::from_chars(s, e, m.next_sequence);
        if (res.ec != std::errc() || res.ptr != e) return std::nullopt;
        return std::optional<Message>(m);
    }
    default:
        return std::nullopt;
    }
}

} // namespace itch

namespace mold {

constexpr size_t   kSessionLen   = 10;
constexpr size_t   kHeaderLen    = 20;
/* Stay under a 1500-byte Ethernet MTU minus IP(20) + UDP(8) headers so a
 * packet is never IP-fragmented: losing one fragment loses the whole
 * datagram, which multiplies the effective loss rate. */
constexpr size_t   kMaxPacketLen = 1400;
constexpr uint16_t kEndOfSession = 0xFFFF;

struct Header {
    std::array<char, kSessionLen> session;
    uint64_t                      sequence; /* seq of the first message */
    uint16_t                      count;    /* 0 = heartbeat            */
};

inline bool parse_header(const uint8_t *p, size_t n, Header &h) {
    if (n < kHeaderLen) {
        return false;
    }
    std::memcpy(h.session.data(), p, kSessionLen);
    h.sequence = wire::get_u64(p + 10);
    h.count    = wire::get_u16(p + 18);
    return true;
}

/*
 * Calls f(sequence, msg_ptr, msg_len) for each message block in the
 * packet. Returns false if the packet is malformed (a length field runs
 * past the end) - the caller must then treat the packet as lost.
 */
template <typename F>
bool for_each_message(const uint8_t *pkt, size_t n, const Header &h, F &&f) {
    if (h.count == kEndOfSession) {
        return true;
    }
    size_t pos = kHeaderLen;
    for (uint16_t i = 0; i < h.count; i++) {
        if (n - pos < 2) {
            return false;
        }
        uint16_t mlen = wire::get_u16(pkt + pos);
        pos += 2;
        if (n - pos < mlen) {
            return false;
        }
        f(h.sequence + i, pkt + pos, static_cast<size_t>(mlen));
        pos += mlen;
    }
    return true;
}

/*
 * PacketBuilder - accumulates message blocks into one MoldUDP64 packet.
 *   begin(seq); append(msg)...; finish(); send(data(), size());
 * Owns a fixed in-object buffer: building a packet never allocates.
 */
class PacketBuilder {
public:
    explicit PacketBuilder(std::string_view session) {
        wire::put_alpha(buf_, kSessionLen, session);
        begin(1);
    }

    void begin(uint64_t first_seq) {
        first_seq_ = first_seq;
        count_     = 0;
        len_       = kHeaderLen;
        wire::put_u64(buf_ + 10, first_seq);
    }

    /* false = does not fit; flush this packet and retry in a new one. */
    bool append(const uint8_t *msg, size_t n) {
        if (n > 0xFFFF || kMaxPacketLen - len_ < n + 2) {
            return false;
        }
        wire::put_u16(buf_ + len_, static_cast<uint16_t>(n));
        std::memcpy(buf_ + len_ + 2, msg, n);
        len_ += n + 2;
        ++count_;
        return true;
    }

    /* Writes the message count into the header; call before sending. */
    void finish() { wire::put_u16(buf_ + 18, count_); }

    const uint8_t *data() const { return buf_; }
    size_t size() const { return len_; }
    uint16_t count() const { return count_; }
    uint64_t first_seq() const { return first_seq_; }

private:
    uint8_t  buf_[kMaxPacketLen];
    size_t   len_       = kHeaderLen;
    uint16_t count_     = 0;
    uint64_t first_seq_ = 1;
};

} // namespace mold

#endif /* PULSEFORGE_ITCH_HPP */
