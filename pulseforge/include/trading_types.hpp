/*
 * trading_types.hpp - the vocabulary of the trading loop.
 *
 * Responsibility: one place for the small types every trading module
 * agrees on (order book, matching engine, ITCH/OUCH/FIX codecs, exchange,
 * trader), so a "price" or a "side" means exactly the same thing in all
 * of them.
 *
 * Why prices are integers (fixed-point), never double:
 *   - 0.1 + 0.2 != 0.3 in binary floating point. A matching engine that
 *     compares 100.1 against 100.1 computed two different ways can fail
 *     to match orders that should cross. Integers compare exactly.
 *   - Integer math is deterministic across compilers/CPUs/flags; FP is
 *     not guaranteed to be (fused multiply-add, x87 vs SSE, -ffast-math).
 *   - The wire protocols do it this way: NASDAQ ITCH/OUCH carry prices as
 *     integers with 4 implied decimal places. So we use the same scale
 *     internally and never convert on the hot path.
 *   Price 1'002'500 == $100.2500. kPriceScale = 10'000 per dollar.
 *
 * Thread-safety: everything here is a value type or a pure function.
 */

#ifndef PULSEFORGE_TRADING_TYPES_HPP
#define PULSEFORGE_TRADING_TYPES_HPP

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

using OrderId = uint64_t; /* exchange-assigned "order reference number" */
using Price   = int64_t;  /* fixed point, 4 implied decimals            */
using Qty     = uint32_t; /* shares                                     */

constexpr Price kPriceScale = 10000;            /* $1.00            */
constexpr Price kCent       = kPriceScale / 100; /* $0.01 == 100     */

/* The char values are the ones the wire protocols use ('B'/'S' in ITCH
 * and OUCH), so encoding a Side is a cast, not a lookup. */
enum class Side : char { Buy = 'B', Sell = 'S' };

constexpr Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }
constexpr bool is_valid_side(char c) { return c == 'B' || c == 'S'; }

/* Time in force: how long an order may rest. Values follow FIX tag 59 /
 * OUCH 5.0 ('0' = Day, '3' = Immediate-Or-Cancel). An IOC order trades
 * whatever it can against the book right now and the remainder is
 * cancelled - it never rests, so it never shows up in market data. */
enum class TimeInForce : char { Day = '0', IOC = '3' };

constexpr bool is_valid_tif(char c) { return c == '0' || c == '3'; }

/* ---- time ------------------------------------------------------------
 * ITCH and OUCH timestamps are "nanoseconds since midnight" (UTC here).
 * Everything in the trading loop reads CLOCK_REALTIME (now_realtime_ns)
 * so that exchange stamps, kernel receive stamps and the trader's own
 * stamps are on one clock and can be subtracted from each other. */
constexpr uint64_t kNanosPerDay = 86400ULL * 1000000000ULL;

constexpr uint64_t ns_since_midnight(uint64_t realtime_ns) {
    return realtime_ns % kNanosPerDay;
}

/* later - earlier for two ns-since-midnight stamps, tolerant of the
 * midnight wrap, as a signed value (negative = "earlier" was actually
 * later, e.g. clock skew between two hosts). */
constexpr int64_t mid_diff(uint64_t later, uint64_t earlier) {
    int64_t d = static_cast<int64_t>(later) - static_cast<int64_t>(earlier);
    const int64_t day = static_cast<int64_t>(kNanosPerDay);
    if (d > day / 2) {
        d -= day;
    } else if (d < -day / 2) {
        d += day;
    }
    return d;
}

/* ---- symbols ---------------------------------------------------------
 * ITCH/OUCH symbols are 8-byte, left-justified, space-padded ASCII. We
 * keep them in that exact form so they can be memcpy'd to/from the wire
 * and compared as 8 bytes - no std::string, no allocation. */
using Symbol = std::array<char, 8>;

inline Symbol make_symbol(std::string_view s) {
    Symbol out;
    out.fill(' ');
    for (size_t i = 0; i < s.size() && i < out.size(); i++) {
        out[i] = s[i];
    }
    return out;
}

inline std::string symbol_str(const Symbol &s) {
    size_t n = s.size();
    while (n > 0 && s[n - 1] == ' ') {
        --n;
    }
    return std::string(s.data(), n);
}

/* Cold path only (logging/reports): 1002500 -> "100.2500". */
inline std::string format_price(Price p) {
    char buf[32];
    const char *sign = p < 0 ? "-" : "";
    uint64_t a = static_cast<uint64_t>(p < 0 ? -p : p);
    std::snprintf(buf, sizeof(buf), "%s%llu.%04llu", sign,
                  static_cast<unsigned long long>(a / kPriceScale),
                  static_cast<unsigned long long>(a % kPriceScale));
    return buf;
}

/* ---- std::visit helper -----------------------------------------------
 * The "overloaded" idiom: build one callable out of several lambdas so
 * std::visit can dispatch on every alternative of a std::variant.
 *   std::visit(overloaded{ [](const A &a) {...}, [](const B &b) {...} }, v);
 * Inherit from every lambda type, pull all their operator()s into one
 * overload set; the deduction guide lets you skip the template args. */
template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

#endif /* PULSEFORGE_TRADING_TYPES_HPP */
