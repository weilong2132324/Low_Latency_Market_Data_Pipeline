/*
 * fix.hpp - a small FIX 4.4 encoder and zero-copy parser.
 *
 * Responsibility: FIX (Financial Information eXchange) is the lingua
 * franca of order entry between firms, brokers and venues. Almost every
 * HFT interview touches it, and almost every firm has a FIX engine
 * somewhere (drop copies, broker connections, slower venues) even if the
 * latency-critical path uses a binary protocol like OUCH (ouch.hpp).
 *
 * Wire format: a flat list of tag=value fields, each ended by SOH (0x01,
 * shown as '|' in logs). A NewOrderSingle looks like:
 *   8=FIX.4.4|9=129|35=D|49=PFTRADER|56=PFX|34=7|52=20261017-14:30:00.123|
 *   11=PF7|55=PFRG|54=1|38=100|40=2|44=100.25|59=3|10=123|
 * Three fields are structural and always in this order:
 *   8  BeginString  protocol version, always first
 *   9  BodyLength   byte count from the field after 9= up to (and
 *                   including) the SOH before 10=
 *   10 CheckSum     sum of every byte before "10=", mod 256, as 3 digits
 * Everything between 9 and 10 is the body: 35 MsgType ('D' = new order),
 * 49/56 sender/target, 34 MsgSeqNum, 52 SendingTime, then the order:
 * 11 ClOrdID, 55 Symbol, 54 Side (1 buy, 2 sell), 38 OrderQty,
 * 40 OrdType (2 = limit), 44 Price, 59 TimeInForce (0 day, 3 IOC).
 *
 * Performance lessons encoded here:
 *   - The encoder writes the BODY first (into reserved space) and then
 *     writes the "8=..|9=<len>|" header BACKWARDS in front of it. The
 *     body length is only known after the body is written; building
 *     backwards avoids a second formatting pass or a memmove.
 *   - Integers go through std::to_chars / std::from_chars: no locale, no
 *     allocation, no exceptions - unlike sprintf/stoi/stringstream.
 *   - SendingTime is passed in pre-formatted. Formatting a timestamp
 *     (gmtime + digits) per message is expensive; real engines cache the
 *     "YYYYMMDD-HH:MM:SS" prefix and only rewrite the milliseconds.
 *   - The parser is zero-copy: each field is a string_view into the
 *     caller's buffer. Nothing is allocated while parsing.
 * Even so, FIX is text: you must scan for delimiters and convert ASCII
 * digits. Compare with OUCH where every field sits at a fixed offset.
 *
 * Thread-safety: pure functions; a fix::Message is single-threaded.
 */

#ifndef PULSEFORGE_FIX_HPP
#define PULSEFORGE_FIX_HPP

#include "trading_types.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace fix {

constexpr char kSOH = '\x01';

namespace tag {
constexpr int BeginString  = 8;
constexpr int BodyLength   = 9;
constexpr int CheckSum     = 10;
constexpr int ClOrdID      = 11;
constexpr int MsgSeqNum    = 34;
constexpr int MsgType      = 35;
constexpr int OrderQty     = 38;
constexpr int OrdType      = 40;
constexpr int Price        = 44;
constexpr int SenderCompID = 49;
constexpr int SendingTime  = 52;
constexpr int Side         = 54;
constexpr int Symbol       = 55;
constexpr int TargetCompID = 56;
constexpr int TimeInForce  = 59;
} // namespace tag

struct NewOrderSingle {
    std::string_view sender_comp_id;
    std::string_view target_comp_id;
    uint64_t         msg_seq_num = 0;
    std::string_view sending_time; /* "YYYYMMDD-HH:MM:SS.sss", pre-formatted */
    std::string_view clordid;
    std::string_view symbol;
    Side             side  = Side::Buy;
    Qty              qty   = 0;
    ::Price          price = 0;
    TimeInForce      tif   = TimeInForce::Day;
};

namespace detail {

/* Bounds-checked output cursor (same "latch an error flag" idea as
 * wire::Writer, for text). */
struct Out {
    char *p;
    char *end;
    bool  ok = true;

    void put(char c) {
        if (p < end) {
            *p++ = c;
        } else {
            ok = false;
        }
    }
    void put(std::string_view s) {
        if (static_cast<size_t>(end - p) >= s.size()) {
            std::memcpy(p, s.data(), s.size());
            p += s.size();
        } else {
            ok = false;
        }
    }
    void put_uint(uint64_t v) {
        auto r = std::to_chars(p, end, v);
        if (r.ec == std::errc()) {
            p = r.ptr;
        } else {
            ok = false;
        }
    }
    /* 1002500 -> "100.25"; trailing zeros trimmed (FIX prices are
     * decimals, "100.25" and "100.2500" mean the same). */
    void put_price(::Price v) {
        if (v < 0) {
            put('-');
            v = -v;
        }
        put_uint(static_cast<uint64_t>(v / kPriceScale));
        int64_t frac = v % kPriceScale;
        if (frac != 0) {
            char digits[4];
            for (int i = 3; i >= 0; i--) {
                digits[i] = static_cast<char>('0' + frac % 10);
                frac /= 10;
            }
            size_t n = 4;
            while (n > 0 && digits[n - 1] == '0') --n;
            put('.');
            put(std::string_view(digits, n));
        }
    }
    void field(int t, std::string_view v) {
        put_uint(static_cast<uint64_t>(t));
        put('=');
        put(v);
        put(kSOH);
    }
    void field_uint(int t, uint64_t v) {
        put_uint(static_cast<uint64_t>(t));
        put('=');
        put_uint(v);
        put(kSOH);
    }
};

inline unsigned checksum(const char *p, size_t n) {
    unsigned sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += static_cast<unsigned char>(p[i]);
    }
    return sum % 256;
}

} // namespace detail

/* Room reserved in front of the body for "8=FIX.4.4|9=NNNNNN|". */
constexpr size_t kHeaderReserve = 24;

/*
 * Encodes a NewOrderSingle into buf. Returns a view of the finished
 * message (which starts somewhere inside buf, NOT necessarily at buf[0] -
 * the header was written backwards), or an empty view if cap is too small.
 */
inline std::string_view encode_new_order_single(const NewOrderSingle &m,
                                                char *buf, size_t cap) {
    if (cap <= kHeaderReserve + 8) {
        return {};
    }
    char *body_start = buf + kHeaderReserve;
    detail::Out o{body_start, buf + cap};
    o.field(tag::MsgType, "D");
    o.field(tag::SenderCompID, m.sender_comp_id);
    o.field(tag::TargetCompID, m.target_comp_id);
    o.field_uint(tag::MsgSeqNum, m.msg_seq_num);
    o.field(tag::SendingTime, m.sending_time);
    o.field(tag::ClOrdID, m.clordid);
    o.field(tag::Symbol, m.symbol);
    o.field(tag::Side, m.side == Side::Buy ? "1" : "2");
    o.field_uint(tag::OrderQty, m.qty);
    o.field(tag::OrdType, "2"); /* limit */
    o.put_uint(tag::Price);
    o.put('=');
    o.put_price(m.price);
    o.put(kSOH);
    o.field(tag::TimeInForce, m.tif == TimeInForce::IOC ? "3" : "0");
    if (!o.ok) {
        return {};
    }
    const size_t body_len = static_cast<size_t>(o.p - body_start);

    /* Header, written right-to-left so it ends exactly at body_start. */
    char len_digits[8];
    auto lr = std::to_chars(len_digits, len_digits + sizeof(len_digits), body_len);
    if (lr.ec != std::errc()) {
        return {};
    }
    const size_t nd = static_cast<size_t>(lr.ptr - len_digits);
    /* Two literals on purpose: "\x019=" would parse as ONE hex escape
     * (\x019), a classic C/C++ string-literal trap. */
    constexpr std::string_view kBegin = "8=FIX.4.4\x01" "9=";
    const size_t header_len = kBegin.size() + nd + 1;
    static_assert(kBegin.size() + 8 + 1 <= kHeaderReserve, "header reserve too small");
    char *start = body_start - header_len;
    std::memcpy(start, kBegin.data(), kBegin.size());
    std::memcpy(start + kBegin.size(), len_digits, nd);
    start[header_len - 1] = kSOH;

    /* Trailer: 10=CCC| over everything before it. */
    unsigned cs = detail::checksum(start, static_cast<size_t>(o.p - start));
    o.put("10=");
    o.put(static_cast<char>('0' + cs / 100));
    o.put(static_cast<char>('0' + (cs / 10) % 10));
    o.put(static_cast<char>('0' + cs % 10));
    o.put(kSOH);
    if (!o.ok) {
        return {};
    }
    return std::string_view(start, static_cast<size_t>(o.p - start));
}

/* Decimal string -> fixed-point Price ("100.25" -> 1002500). Rejects
 * more than 4 decimals rather than silently rounding. */
inline bool parse_price(std::string_view s, ::Price &out) {
    bool neg = false;
    size_t i = 0;
    if (!s.empty() && s[0] == '-') {
        neg = true;
        i   = 1;
    }
    const char *b = s.data() + i;
    const char *e = s.data() + s.size();
    uint64_t whole = 0;
    auto r = std::from_chars(b, e, whole);
    if (r.ec != std::errc()) {
        return false;
    }
    int64_t frac = 0;
    if (r.ptr != e) {
        if (*r.ptr != '.') {
            return false;
        }
        const char *f = r.ptr + 1;
        size_t nd = static_cast<size_t>(e - f);
        if (nd == 0 || nd > 4) {
            return false;
        }
        for (const char *c = f; c < e; c++) {
            if (*c < '0' || *c > '9') {
                return false;
            }
            frac = frac * 10 + (*c - '0');
        }
        for (size_t k = nd; k < 4; k++) {
            frac *= 10;
        }
    }
    ::Price v = static_cast<::Price>(whole) * kPriceScale + frac;
    out = neg ? -v : v;
    return true;
}

/*
 * Message - parsed view of one FIX message. parse() validates the
 * structure (8 first, 9 second, 10 last), the BodyLength and the
 * CheckSum, and records every field as (tag, string_view into buf).
 * The buffer must outlive the Message.
 */
class Message {
public:
    enum class Error {
        None,
        Malformed,       /* missing '=' or SOH, non-numeric tag      */
        BadBeginString,  /* first field is not 8=                    */
        BadBodyLength,   /* 9= missing or does not match the bytes   */
        BadChecksum,     /* 10= missing or does not match the bytes  */
        TooManyFields,
    };

    static constexpr size_t kMaxFields = 64;

    Error parse(const char *buf, size_t len) {
        n_ = 0;
        size_t pos = 0;
        size_t body_start = 0, checksum_at = 0;
        uint64_t declared_len = 0;
        bool have_len = false, have_checksum = false;

        while (pos < len) {
            const char *eq = static_cast<const char *>(
                std::memchr(buf + pos, '=', len - pos));
            if (eq == nullptr) {
                return Error::Malformed;
            }
            int t = 0;
            auto tr = std::from_chars(buf + pos, eq, t);
            if (tr.ec != std::errc() || tr.ptr != eq || t <= 0) {
                return Error::Malformed;
            }
            const size_t vstart = static_cast<size_t>(eq - buf) + 1;
            const char *soh = static_cast<const char *>(
                std::memchr(buf + vstart, kSOH, len - vstart));
            if (soh == nullptr) {
                return Error::Malformed;
            }
            if (n_ == kMaxFields) {
                return Error::TooManyFields;
            }
            std::string_view value(buf + vstart, static_cast<size_t>(soh - (buf + vstart)));

            if (n_ == 0 && t != tag::BeginString) {
                return Error::BadBeginString;
            }
            if (n_ == 1) {
                if (t != tag::BodyLength) {
                    return Error::BadBodyLength;
                }
                auto lr = std::from_chars(value.data(), value.data() + value.size(), declared_len);
                if (lr.ec != std::errc() || lr.ptr != value.data() + value.size()) {
                    return Error::BadBodyLength;
                }
                have_len   = true;
                body_start = static_cast<size_t>(soh - buf) + 1;
            }
            if (t == tag::CheckSum) {
                checksum_at   = pos;
                have_checksum = true;
            }
            fields_[n_++] = Field{t, value};
            pos = static_cast<size_t>(soh - buf) + 1;
            if (have_checksum) {
                break; /* 10= is always last */
            }
        }

        if (!have_len) {
            return Error::BadBodyLength;
        }
        if (!have_checksum) {
            return Error::BadChecksum;
        }
        if (checksum_at - body_start != declared_len) {
            return Error::BadBodyLength;
        }
        std::string_view cs = fields_[n_ - 1].value;
        unsigned declared_cs = 0;
        auto cr = std::from_chars(cs.data(), cs.data() + cs.size(), declared_cs);
        if (cs.size() != 3 || cr.ec != std::errc() ||
            declared_cs != detail::checksum(buf, checksum_at)) {
            return Error::BadChecksum;
        }
        return Error::None;
    }

    /* Linear scan: a NewOrderSingle has ~15 fields, so this beats any
     * map - fewer instructions and all fields sit in one or two cache lines. */
    const std::string_view *find(int t) const {
        for (size_t i = 0; i < n_; i++) {
            if (fields_[i].tag == t) {
                return &fields_[i].value;
            }
        }
        return nullptr;
    }

    bool get_uint(int t, uint64_t &out) const {
        const std::string_view *v = find(t);
        if (v == nullptr) {
            return false;
        }
        auto r = std::from_chars(v->data(), v->data() + v->size(), out);
        return r.ec == std::errc() && r.ptr == v->data() + v->size();
    }

    bool get_price(int t, ::Price &out) const {
        const std::string_view *v = find(t);
        return v != nullptr && parse_price(*v, out);
    }

    size_t field_count() const { return n_; }

private:
    struct Field {
        int              tag;
        std::string_view value;
    };
    std::array<Field, kMaxFields> fields_{};
    size_t                        n_ = 0;
};

inline const char *to_string(Message::Error e) {
    switch (e) {
    case Message::Error::None:           return "ok";
    case Message::Error::Malformed:      return "malformed";
    case Message::Error::BadBeginString: return "bad BeginString(8)";
    case Message::Error::BadBodyLength:  return "bad BodyLength(9)";
    case Message::Error::BadChecksum:    return "bad CheckSum(10)";
    case Message::Error::TooManyFields:  return "too many fields";
    }
    return "?";
}

/* The order fields of a parsed NewOrderSingle (what a gateway needs). */
struct ParsedOrder {
    std::string_view clordid;
    std::string_view symbol;
    Side             side  = Side::Buy;
    Qty              qty   = 0;
    ::Price          price = 0;
    TimeInForce      tif   = TimeInForce::Day;
};

inline bool to_order(const Message &m, ParsedOrder &o) {
    const std::string_view *type = m.find(tag::MsgType);
    const std::string_view *cl   = m.find(tag::ClOrdID);
    const std::string_view *sym  = m.find(tag::Symbol);
    const std::string_view *side = m.find(tag::Side);
    const std::string_view *tif  = m.find(tag::TimeInForce);
    uint64_t qty = 0;
    if (type == nullptr || *type != "D" || cl == nullptr || sym == nullptr ||
        side == nullptr || !m.get_uint(tag::OrderQty, qty) ||
        !m.get_price(tag::Price, o.price)) {
        return false;
    }
    if (*side == "1") {
        o.side = Side::Buy;
    } else if (*side == "2") {
        o.side = Side::Sell;
    } else {
        return false;
    }
    o.tif     = (tif != nullptr && *tif == "3") ? TimeInForce::IOC : TimeInForce::Day;
    o.clordid = *cl;
    o.symbol  = *sym;
    o.qty     = static_cast<Qty>(qty);
    return true;
}

} // namespace fix

#endif /* PULSEFORGE_FIX_HPP */
