/*
 * ouch.hpp - OUCH-style binary order entry messages.
 *
 * Responsibility: the messages a trading firm and an exchange exchange
 * over the order-entry TCP session. Modelled on NASDAQ OUCH 5.0 (fixed
 * width, big-endian, carried inside SoupBinTCP frames - see soupbin.hpp)
 * with the field set trimmed to what this lab uses. Display/capacity/
 * ISO/cross-type fields and "appendages" are omitted; the shape - and
 * every lesson about it - is the real one.
 *
 * Inbound (client -> exchange, in SoupBinTCP 'U' frames):
 *   'O' Enter Order   41 bytes
 *   'X' Cancel Order   9 bytes
 * Outbound (exchange -> client, in SoupBinTCP 'S' frames):
 *   'A' Order Accepted 58 bytes   carries the exchange's OrderReference -
 *                                 the SAME id that appears in the public
 *                                 ITCH feed for this order, so a client
 *                                 can recognise its own orders in market data
 *   'E' Order Executed 34 bytes   a fill
 *   'C' Order Canceled 18 bytes   shares removed (user request, IOC remainder,
 *                                 self-match prevention, ...)
 *   'J' Order Rejected 28 bytes
 *
 * UserRefNum (OUCH 5.0): instead of OUCH 4's 14-character order token,
 * the client numbers its orders 1, 2, 3, ... and must keep them strictly
 * increasing within a session. Why the change? An integer key is cheap
 * to validate ("> last one?" catches duplicates in O(1), no lookup) and
 * cheap to index on both sides. ClOrdID is still there as a free-form
 * client label, but nothing is keyed on it.
 *
 * Binary vs FIX: compare with fix.hpp. Here a field is at a FIXED OFFSET,
 * so decoding is a few loads + byte swaps, and the message size is known
 * from its type. FIX is "tag=value|" text: you must scan for delimiters
 * and parse ASCII digits. bench/protocol_bench measures the difference.
 *
 * Thread-safety: pure functions.
 */

#ifndef PULSEFORGE_OUCH_HPP
#define PULSEFORGE_OUCH_HPP

#include "trading_types.hpp"
#include "wire.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

namespace ouch {

namespace len {
constexpr size_t EnterOrder    = 41;
constexpr size_t CancelOrder   = 9;
constexpr size_t OrderAccepted = 58;
constexpr size_t OrderExecuted = 34;
constexpr size_t OrderCanceled = 18;
constexpr size_t OrderRejected = 28;
} // namespace len

constexpr size_t kMaxMessageLen = 58;

using ClOrdId = std::array<char, 14>;

inline ClOrdId make_clordid(std::string_view s) {
    ClOrdId id;
    id.fill(' ');
    for (size_t i = 0; i < s.size() && i < id.size(); i++) {
        id[i] = s[i];
    }
    return id;
}

/* Liquidity flags on executions. Maker/taker pricing: exchanges usually
 * PAY a rebate to the side that added liquidity and CHARGE the side that
 * removed it - so this one byte is real money. */
constexpr char kAddedLiquidity   = 'A';
constexpr char kRemovedLiquidity = 'R';

/* Cancel reasons (subset of OUCH's). */
constexpr char kCancelUserRequested = 'U';
constexpr char kCancelImmediate     = 'I'; /* IOC remainder          */
constexpr char kCancelSelfMatch     = 'Q'; /* self-match prevention  */
constexpr char kCancelSupervisory   = 'S'; /* exchange/ops action   */

/* Reject reasons (this lab's set, OUCH-like single chars). */
constexpr char kRejectInvalidPrice   = 'X';
constexpr char kRejectInvalidQty     = 'Z';
constexpr char kRejectInvalidSymbol  = 'S';
constexpr char kRejectInvalidUserRef = 'N';
constexpr char kRejectOther          = 'O';

struct EnterOrder {
    uint32_t    user_ref;
    Side        side;
    uint32_t    qty;
    Symbol      symbol;
    Price       price;
    TimeInForce tif;
    ClOrdId     clordid;
};

struct CancelOrder {
    uint32_t user_ref;
    uint32_t qty; /* the order's NEW intended size: 0 = cancel it entirely */
};

struct OrderAccepted {
    uint64_t    timestamp; /* ns since midnight, when the exchange accepted it */
    uint32_t    user_ref;
    Side        side;
    uint32_t    qty;
    Symbol      symbol;
    Price       price;
    TimeInForce tif;
    uint64_t    order_ref; /* exchange order id (== ITCH order reference) */
    ClOrdId     clordid;
    char        state; /* 'L' live */
};

struct OrderExecuted {
    uint64_t timestamp;
    uint32_t user_ref;
    uint32_t qty;
    Price    price;
    char     liquidity;
    uint64_t match_number;
};

struct OrderCanceled {
    uint64_t timestamp;
    uint32_t user_ref;
    uint32_t qty; /* shares removed by this cancel (a decrement) */
    char     reason;
};

struct OrderRejected {
    uint64_t timestamp;
    uint32_t user_ref;
    char     reason;
    ClOrdId  clordid;
};

using Inbound  = std::variant<EnterOrder, CancelOrder>;
using Outbound = std::variant<OrderAccepted, OrderExecuted, OrderCanceled, OrderRejected>;

/* ---- encoders: bytes written, or 0 if cap is too small ---------------- */

inline size_t encode(const EnterOrder &m, uint8_t *out, size_t cap) {
    wire::Writer w(out, cap);
    w.ch('O');
    w.u32(m.user_ref);
    w.ch(static_cast<char>(m.side));
    w.u32(m.qty);
    w.bytes(m.symbol.data(), m.symbol.size());
    w.i64(m.price);
    w.ch(static_cast<char>(m.tif));
    w.bytes(m.clordid.data(), m.clordid.size());
    return w.ok() ? w.size() : 0;
}

inline size_t encode(const CancelOrder &m, uint8_t *out, size_t cap) {
    wire::Writer w(out, cap);
    w.ch('X');
    w.u32(m.user_ref);
    w.u32(m.qty);
    return w.ok() ? w.size() : 0;
}

inline size_t encode(const OrderAccepted &m, uint8_t *out, size_t cap) {
    wire::Writer w(out, cap);
    w.ch('A');
    w.u64(m.timestamp);
    w.u32(m.user_ref);
    w.ch(static_cast<char>(m.side));
    w.u32(m.qty);
    w.bytes(m.symbol.data(), m.symbol.size());
    w.i64(m.price);
    w.ch(static_cast<char>(m.tif));
    w.u64(m.order_ref);
    w.bytes(m.clordid.data(), m.clordid.size());
    w.ch(m.state);
    return w.ok() ? w.size() : 0;
}

inline size_t encode(const OrderExecuted &m, uint8_t *out, size_t cap) {
    wire::Writer w(out, cap);
    w.ch('E');
    w.u64(m.timestamp);
    w.u32(m.user_ref);
    w.u32(m.qty);
    w.i64(m.price);
    w.ch(m.liquidity);
    w.u64(m.match_number);
    return w.ok() ? w.size() : 0;
}

inline size_t encode(const OrderCanceled &m, uint8_t *out, size_t cap) {
    wire::Writer w(out, cap);
    w.ch('C');
    w.u64(m.timestamp);
    w.u32(m.user_ref);
    w.u32(m.qty);
    w.ch(m.reason);
    return w.ok() ? w.size() : 0;
}

inline size_t encode(const OrderRejected &m, uint8_t *out, size_t cap) {
    wire::Writer w(out, cap);
    w.ch('J');
    w.u64(m.timestamp);
    w.u32(m.user_ref);
    w.ch(m.reason);
    w.bytes(m.clordid.data(), m.clordid.size());
    return w.ok() ? w.size() : 0;
}

/* ---- decoders: exact length + field validation, nullopt if bad ------- */

inline std::optional<Inbound> decode_inbound(const uint8_t *p, size_t n) {
    if (n == 0) {
        return std::nullopt;
    }
    wire::Reader r(p, n);
    switch (r.ch()) {
    case 'O': {
        if (n != len::EnterOrder) return std::nullopt;
        EnterOrder m{};
        m.user_ref = r.u32();
        char side  = r.ch();
        m.qty      = r.u32();
        r.bytes(m.symbol.data(), m.symbol.size());
        m.price  = r.i64();
        char tif = r.ch();
        r.bytes(m.clordid.data(), m.clordid.size());
        if (!r.ok() || !is_valid_side(side) || !is_valid_tif(tif)) return std::nullopt;
        m.side = static_cast<Side>(side);
        m.tif  = static_cast<TimeInForce>(tif);
        return std::optional<Inbound>(m);
    }
    case 'X': {
        if (n != len::CancelOrder) return std::nullopt;
        CancelOrder m{};
        m.user_ref = r.u32();
        m.qty      = r.u32();
        return r.ok() ? std::optional<Inbound>(m) : std::nullopt;
    }
    default:
        return std::nullopt;
    }
}

inline std::optional<Outbound> decode_outbound(const uint8_t *p, size_t n) {
    if (n == 0) {
        return std::nullopt;
    }
    wire::Reader r(p, n);
    switch (r.ch()) {
    case 'A': {
        if (n != len::OrderAccepted) return std::nullopt;
        OrderAccepted m{};
        m.timestamp = r.u64();
        m.user_ref  = r.u32();
        char side   = r.ch();
        m.qty       = r.u32();
        r.bytes(m.symbol.data(), m.symbol.size());
        m.price     = r.i64();
        char tif    = r.ch();
        m.order_ref = r.u64();
        r.bytes(m.clordid.data(), m.clordid.size());
        m.state = r.ch();
        if (!r.ok() || !is_valid_side(side) || !is_valid_tif(tif)) return std::nullopt;
        m.side = static_cast<Side>(side);
        m.tif  = static_cast<TimeInForce>(tif);
        return std::optional<Outbound>(m);
    }
    case 'E': {
        if (n != len::OrderExecuted) return std::nullopt;
        OrderExecuted m{};
        m.timestamp    = r.u64();
        m.user_ref     = r.u32();
        m.qty          = r.u32();
        m.price        = r.i64();
        m.liquidity    = r.ch();
        m.match_number = r.u64();
        return r.ok() ? std::optional<Outbound>(m) : std::nullopt;
    }
    case 'C': {
        if (n != len::OrderCanceled) return std::nullopt;
        OrderCanceled m{};
        m.timestamp = r.u64();
        m.user_ref  = r.u32();
        m.qty       = r.u32();
        m.reason    = r.ch();
        return r.ok() ? std::optional<Outbound>(m) : std::nullopt;
    }
    case 'J': {
        if (n != len::OrderRejected) return std::nullopt;
        OrderRejected m{};
        m.timestamp = r.u64();
        m.user_ref  = r.u32();
        m.reason    = r.ch();
        r.bytes(m.clordid.data(), m.clordid.size());
        return r.ok() ? std::optional<Outbound>(m) : std::nullopt;
    }
    default:
        return std::nullopt;
    }
}

} // namespace ouch

#endif /* PULSEFORGE_OUCH_HPP */
