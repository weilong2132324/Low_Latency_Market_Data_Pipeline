/*
 * protocol_bench.cpp - binary (OUCH) vs text (FIX) order encoding.
 *
 * Same order, two wire formats, four timings: encode and decode each.
 * The binary format wins for structural reasons, not micro-tuning:
 *   - fixed offsets: no searching for delimiters
 *   - fixed widths: no digit counting, no ASCII<->int conversion
 *   - smaller: fewer bytes to copy, checksum and push through the NIC
 *   - no checksum pass: TCP already checksums the bytes (FIX's 10= is
 *     a legacy of unreliable transports)
 * Both codecs here are allocation-free; the gap is the format itself.
 *
 * Run: make bench-protocol
 */

#include "fix.hpp"
#include "ouch.hpp"
#include "soupbin.hpp"
#include "time_util.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace {

template <class T>
inline void do_not_optimize(const T &v) {
    __asm__ __volatile__("" : : "g"(&v) : "memory");
}

constexpr int kIters = 2000000;

double ns_per_op(uint64_t t0, uint64_t t1) {
    return static_cast<double>(t1 - t0) / kIters;
}

} // namespace

int main() {
    /* ---- OUCH encode: EnterOrder inside a SoupBinTCP 'U' frame ---- */
    uint8_t frame[64];
    size_t ouch_len = 0;
    uint64_t sink = 0;
    ouch::EnterOrder eo{};
    eo.side    = Side::Buy;
    eo.symbol  = make_symbol("PFRG");
    eo.tif     = TimeInForce::IOC;
    eo.clordid = ouch::make_clordid("PF123456");
    uint64_t t0 = now_ns();
    for (int i = 0; i < kIters; i++) {
        eo.user_ref = static_cast<uint32_t>(i);
        eo.qty      = static_cast<uint32_t>(100 + (i & 7));
        eo.price    = 1000000 + (i & 63) * 100;
        size_t pn = ouch::encode(eo, frame + 3, sizeof(frame) - 3);
        wire::put_u16(frame, static_cast<uint16_t>(pn + 1));
        frame[2] = 'U';
        ouch_len = pn + 3;
        do_not_optimize(frame);
        sink += frame[10];
    }
    const double ouch_enc = ns_per_op(t0, now_ns());

    /* ---- OUCH decode ---- */
    t0 = now_ns();
    for (int i = 0; i < kIters; i++) {
        /* vary the low byte of qty (frame offset 3 + type 1 + ref 4 + side 1 + 3)
         * so the compiler cannot hoist the decode out of the loop */
        frame[12] = static_cast<uint8_t>(i);
        auto m = ouch::decode_inbound(frame + 3, ouch_len - 3);
        const auto &o = std::get<ouch::EnterOrder>(*m);
        sink += o.qty + static_cast<uint64_t>(o.price);
    }
    const double ouch_dec = ns_per_op(t0, now_ns());

    /* ---- FIX encode ---- */
    char fbuf[256];
    std::string_view fixmsg;
    fix::NewOrderSingle nos;
    nos.sender_comp_id = "PFTRADER";
    nos.target_comp_id = "PFX";
    nos.sending_time   = "20261017-14:30:00.123"; /* pre-formatted, as real engines cache it */
    nos.clordid        = "PF123456";
    nos.symbol         = "PFRG";
    nos.side           = Side::Buy;
    nos.tif            = TimeInForce::IOC;
    t0 = now_ns();
    for (int i = 0; i < kIters; i++) {
        nos.msg_seq_num = static_cast<uint64_t>(i);
        nos.qty         = static_cast<Qty>(100 + (i & 7));
        nos.price       = 1000000 + (i & 63) * 100;
        fixmsg = fix::encode_new_order_single(nos, fbuf, sizeof(fbuf));
        do_not_optimize(fbuf);
        sink += static_cast<unsigned char>(fixmsg[20]);
    }
    const double fix_enc = ns_per_op(t0, now_ns());

    /* ---- FIX decode: parse + validate + extract the order ---- */
    std::string_view one = fix::encode_new_order_single(nos, fbuf, sizeof(fbuf));
    char copy[256];
    std::memcpy(copy, one.data(), one.size());
    fix::Message msg;
    fix::ParsedOrder po;
    t0 = now_ns();
    for (int i = 0; i < kIters; i++) {
        do_not_optimize(copy);
        if (msg.parse(copy, one.size()) == fix::Message::Error::None && fix::to_order(msg, po)) {
            sink += po.qty + static_cast<uint64_t>(po.price);
        }
    }
    const double fix_dec = ns_per_op(t0, now_ns());
    do_not_optimize(sink);

    std::printf("PulseForge protocol_bench: one limit order, %d iterations each\n\n", kIters);
    std::printf("  %-26s %8s %10s %10s\n", "format", "bytes", "encode ns", "decode ns");
    std::printf("  %-26s %8zu %10.1f %10.1f\n", "OUCH-style binary (+frame)", ouch_len, ouch_enc,
                ouch_dec);
    std::printf("  %-26s %8zu %10.1f %10.1f\n", "FIX 4.4 tag=value", one.size(), fix_enc, fix_dec);
    std::printf("\n  FIX/OUCH: %.1fx bytes, %.1fx encode, %.1fx decode\n",
                static_cast<double>(one.size()) / static_cast<double>(ouch_len), fix_enc / ouch_enc,
                fix_dec / ouch_dec);
    std::printf("\n  sample FIX message ('|' = SOH):\n  ");
    for (char c : one) {
        std::putchar(c == fix::kSOH ? '|' : c);
    }
    std::printf("\n");
    return 0;
}
