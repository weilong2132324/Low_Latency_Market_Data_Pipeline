/*
 * test_wire_protocols.cpp - byte-level tests for wire.hpp, ITCH,
 * MoldUDP64, SoupBinTCP, OUCH and FIX.
 *
 * The message-length checks are deliberate: a protocol layout is a
 * contract with the other side, and "the struct got one byte longer" is
 * the bug these tests exist to catch.
 */

#include "fix.hpp"
#include "itch.hpp"
#include "ouch.hpp"
#include "soupbin.hpp"
#include "wire.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

static void test_wire_byte_order() {
    uint8_t b[8];
    wire::put_u32(b, 0x01020304u);
    EXPECT(b[0] == 1 && b[1] == 2 && b[2] == 3 && b[3] == 4); /* big-endian on the wire */
    EXPECT(wire::get_u32(b) == 0x01020304u);

    wire::put_u16(b, 0xBEEF);
    EXPECT(b[0] == 0xBE && b[1] == 0xEF && wire::get_u16(b) == 0xBEEF);

    const uint64_t ts = 86399999999999ULL; /* last ns of a day fits in 48 bits */
    wire::put_u48(b, ts);
    EXPECT(wire::get_u48(b) == ts);

    wire::put_u64(b, 0x0102030405060708ULL);
    EXPECT(b[0] == 1 && b[7] == 8 && wire::get_u64(b) == 0x0102030405060708ULL);
}

static void test_writer_reader_bounds() {
    uint8_t b[3];
    wire::Writer w(b, sizeof(b));
    w.u16(7);
    EXPECT(w.ok() && w.size() == 2);
    w.u32(1); /* does not fit: latches the error, writes nothing */
    EXPECT(!w.ok() && w.size() == 2);
    w.u8(1); /* still a no-op after the error */
    EXPECT(w.size() == 2);

    wire::Reader r(b, 2);
    EXPECT(r.u16() == 7 && r.ok());
    EXPECT(r.u8() == 0 && !r.ok()); /* past the end */

    uint8_t a[6];
    wire::put_alpha(a, 6, "AB");
    EXPECT(std::memcmp(a, "AB    ", 6) == 0);
    EXPECT(wire::get_alpha(a, 6) == "AB");
}

static void test_itch_roundtrip() {
    uint8_t b[64];
    itch::AddOrder a{};
    a.locate    = 3;
    a.timestamp = 34200000000123ULL; /* 09:30:00.000000123 */
    a.order_ref = 0x1122334455667788ULL;
    a.side      = Side::Sell;
    a.shares    = 300;
    a.stock     = make_symbol("PFRG");
    a.price     = 1002500;
    size_t n = itch::encode(a, b, sizeof(b));
    EXPECT(n == itch::len::AddOrder && n == 36);
    EXPECT(b[0] == 'A');
    auto m = itch::decode(b, n);
    EXPECT(m.has_value());
    if (m) {
        const auto *d = std::get_if<itch::AddOrder>(&*m);
        EXPECT(d != nullptr);
        if (d) {
            EXPECT(d->locate == 3 && d->timestamp == a.timestamp && d->order_ref == a.order_ref);
            EXPECT(d->side == Side::Sell && d->shares == 300 && d->price == 1002500);
            EXPECT(d->stock == make_symbol("PFRG"));
        }
    }
    EXPECT(!itch::decode(b, n - 1).has_value()); /* truncated -> rejected */
    b[19] = 'Q';                                 /* corrupt the side byte */
    EXPECT(!itch::decode(b, n).has_value());

    itch::OrderExecuted e{2, 5, 77, 100, 9};
    EXPECT(itch::encode(e, b, sizeof(b)) == 31);
    auto me = itch::decode(b, 31);
    EXPECT(me && std::get<itch::OrderExecuted>(*me).executed_shares == 100 &&
           std::get<itch::OrderExecuted>(*me).match_number == 9);

    itch::OrderCancel x{1, 5, 77, 40};
    EXPECT(itch::encode(x, b, sizeof(b)) == 23);
    auto mx = itch::decode(b, 23);
    EXPECT(mx && std::get<itch::OrderCancel>(*mx).cancelled_shares == 40);

    itch::OrderDelete d{1, 5, 77};
    EXPECT(itch::encode(d, b, sizeof(b)) == 19);
    auto md = itch::decode(b, 19);
    EXPECT(md && std::get<itch::OrderDelete>(*md).order_ref == 77);

    itch::SystemEvent s{0, 5, itch::kStartOfMessages};
    EXPECT(itch::encode(s, b, sizeof(b)) == 12);
    auto ms = itch::decode(b, 12);
    EXPECT(ms && std::get<itch::SystemEvent>(*ms).event_code == 'O');

    itch::StockDirectory sd{4, 5, make_symbol("ACME"), 100};
    EXPECT(itch::encode(sd, b, sizeof(b)) == 39);
    auto msd = itch::decode(b, 39);
    EXPECT(msd && std::get<itch::StockDirectory>(*msd).locate == 4 &&
           std::get<itch::StockDirectory>(*msd).round_lot_size == 100);

    itch::EndOfSnapshot eos{123456789012ULL};
    EXPECT(itch::encode(eos, b, sizeof(b)) == 21);
    auto meos = itch::decode(b, 21);
    EXPECT(meos && std::get<itch::EndOfSnapshot>(*meos).next_sequence == 123456789012ULL);

    EXPECT(itch::encode(a, b, 10) == 0); /* buffer too small -> 0, no overflow */
}

static void test_moldudp64() {
    mold::PacketBuilder pb("PFX0000001");
    pb.begin(1000);
    uint8_t m1[3] = {1, 2, 3}, m2[1] = {9}, m3[5] = {5, 5, 5, 5, 5};
    EXPECT(pb.append(m1, 3) && pb.append(m2, 1) && pb.append(m3, 5));
    pb.finish();
    EXPECT(pb.size() == mold::kHeaderLen + (2 + 3) + (2 + 1) + (2 + 5));

    mold::Header h;
    EXPECT(mold::parse_header(pb.data(), pb.size(), h));
    EXPECT(h.sequence == 1000 && h.count == 3);
    EXPECT(std::memcmp(h.session.data(), "PFX0000001", 10) == 0);

    std::vector<uint64_t> seqs;
    std::vector<size_t> lens;
    bool ok = mold::for_each_message(pb.data(), pb.size(), h, [&](uint64_t s, const uint8_t *, size_t n) {
        seqs.push_back(s);
        lens.push_back(n);
    });
    EXPECT(ok && seqs.size() == 3 && seqs[0] == 1000 && seqs[2] == 1002);
    EXPECT(lens[0] == 3 && lens[1] == 1 && lens[2] == 5);

    /* a length field running past the end must be detected */
    EXPECT(!mold::for_each_message(pb.data(), pb.size() - 1, h, [](uint64_t, const uint8_t *, size_t) {}));

    /* the builder refuses to exceed the MTU-safe packet size */
    pb.begin(1);
    uint8_t big[100] = {};
    size_t appended = 0;
    while (pb.append(big, sizeof(big))) {
        ++appended;
    }
    EXPECT(pb.size() <= mold::kMaxPacketLen);
    EXPECT(appended == (mold::kMaxPacketLen - mold::kHeaderLen) / 102);
}

static void test_soupbin_framing() {
    /* two frames, delivered one byte at a time: the reader must only
     * ever hand out complete frames */
    uint8_t stream[64];
    const uint8_t p1[] = {'h', 'i'};
    size_t n1 = soup::write_frame(stream, sizeof(stream), 'U', p1, 2);
    size_t n2 = soup::write_frame(stream + n1, sizeof(stream) - n1, 'R', nullptr, 0);
    EXPECT(n1 == 5 && n2 == 3);
    EXPECT(stream[0] == 0 && stream[1] == 3); /* length counts type + payload */

    soup::FrameReader r;
    std::vector<char> types;
    for (size_t i = 0; i < n1 + n2; i++) {
        *r.write_ptr() = stream[i];
        r.commit(1);
        char t;
        const uint8_t *p;
        size_t len;
        while (r.next(t, p, len) == soup::FrameReader::Status::Frame) {
            types.push_back(t);
            if (t == 'U') {
                EXPECT(len == 2 && p[0] == 'h' && p[1] == 'i');
            } else {
                EXPECT(len == 0);
            }
        }
    }
    EXPECT(types.size() == 2 && types[0] == 'U' && types[1] == 'R');

    /* both frames glued into one recv() */
    soup::FrameReader r2;
    std::memcpy(r2.write_ptr(), stream, n1 + n2);
    r2.commit(n1 + n2);
    char t;
    const uint8_t *p;
    size_t len;
    EXPECT(r2.next(t, p, len) == soup::FrameReader::Status::Frame && t == 'U');
    EXPECT(r2.next(t, p, len) == soup::FrameReader::Status::Frame && t == 'R');
    EXPECT(r2.next(t, p, len) == soup::FrameReader::Status::NeedMore);

    /* hostile length fields */
    soup::FrameReader r3;
    uint8_t zero[2] = {0, 0};
    std::memcpy(r3.write_ptr(), zero, 2);
    r3.commit(2);
    EXPECT(r3.next(t, p, len) == soup::FrameReader::Status::Error);
    soup::FrameReader r4;
    uint8_t huge[2] = {0xFF, 0xFF};
    std::memcpy(r4.write_ptr(), huge, 2);
    r4.commit(2);
    EXPECT(r4.next(t, p, len) == soup::FrameReader::Status::Error);

    /* login request / accepted */
    soup::LoginRequest lr;
    lr.username = "TRADR1";
    lr.password = "pw";
    lr.requested_sequence = 42;
    uint8_t buf[64];
    EXPECT(soup::encode_login_request(lr, buf, sizeof(buf)) == 46);
    soup::LoginRequest back;
    EXPECT(soup::decode_login_request(buf, 46, back));
    EXPECT(back.username == "TRADR1" && back.password == "pw" && back.requested_sequence == 42);

    soup::LoginAccepted la;
    la.session  = "PFX0000001";
    la.sequence = 7;
    EXPECT(soup::encode_login_accepted(la, buf, sizeof(buf)) == 30);
    soup::LoginAccepted lb;
    EXPECT(soup::decode_login_accepted(buf, 30, lb) && lb.session == "PFX0000001" && lb.sequence == 7);
}

static void test_ouch_roundtrip() {
    uint8_t b[80];
    ouch::EnterOrder eo{};
    eo.user_ref = 17;
    eo.side     = Side::Buy;
    eo.qty      = 100;
    eo.symbol   = make_symbol("PFRG");
    eo.price    = 1002500;
    eo.tif      = TimeInForce::IOC;
    eo.clordid  = ouch::make_clordid("PF17");
    EXPECT(ouch::encode(eo, b, sizeof(b)) == ouch::len::EnterOrder);
    auto in = ouch::decode_inbound(b, ouch::len::EnterOrder);
    EXPECT(in.has_value());
    if (in) {
        const auto &d = std::get<ouch::EnterOrder>(*in);
        EXPECT(d.user_ref == 17 && d.side == Side::Buy && d.qty == 100 && d.price == 1002500);
        EXPECT(d.tif == TimeInForce::IOC && d.symbol == make_symbol("PFRG"));
        EXPECT(d.clordid == ouch::make_clordid("PF17"));
    }
    b[5] = 'Z'; /* bad side */
    EXPECT(!ouch::decode_inbound(b, ouch::len::EnterOrder).has_value());

    ouch::CancelOrder co{17, 0};
    EXPECT(ouch::encode(co, b, sizeof(b)) == ouch::len::CancelOrder);
    auto ci = ouch::decode_inbound(b, ouch::len::CancelOrder);
    EXPECT(ci && std::get<ouch::CancelOrder>(*ci).user_ref == 17);

    ouch::OrderAccepted oa{};
    oa.timestamp = 1;
    oa.user_ref  = 17;
    oa.side      = Side::Sell;
    oa.qty       = 5;
    oa.symbol    = make_symbol("ACME");
    oa.price     = -100; /* signed prices survive the u64 round trip */
    oa.tif       = TimeInForce::Day;
    oa.order_ref = 99;
    oa.clordid   = ouch::make_clordid("X");
    oa.state     = 'L';
    EXPECT(ouch::encode(oa, b, sizeof(b)) == ouch::len::OrderAccepted);
    auto oo = ouch::decode_outbound(b, ouch::len::OrderAccepted);
    EXPECT(oo && std::get<ouch::OrderAccepted>(*oo).price == -100 &&
           std::get<ouch::OrderAccepted>(*oo).order_ref == 99);

    ouch::OrderExecuted ex{2, 17, 50, 1002500, ouch::kRemovedLiquidity, 1234};
    EXPECT(ouch::encode(ex, b, sizeof(b)) == ouch::len::OrderExecuted);
    auto oe = ouch::decode_outbound(b, ouch::len::OrderExecuted);
    EXPECT(oe && std::get<ouch::OrderExecuted>(*oe).match_number == 1234 &&
           std::get<ouch::OrderExecuted>(*oe).liquidity == 'R');

    ouch::OrderCanceled cx{3, 17, 50, ouch::kCancelImmediate};
    EXPECT(ouch::encode(cx, b, sizeof(b)) == ouch::len::OrderCanceled);
    auto oc = ouch::decode_outbound(b, ouch::len::OrderCanceled);
    EXPECT(oc && std::get<ouch::OrderCanceled>(*oc).reason == 'I');

    ouch::OrderRejected rj{4, 17, ouch::kRejectInvalidPrice, ouch::make_clordid("R")};
    EXPECT(ouch::encode(rj, b, sizeof(b)) == ouch::len::OrderRejected);
    auto orj = ouch::decode_outbound(b, ouch::len::OrderRejected);
    EXPECT(orj && std::get<ouch::OrderRejected>(*orj).reason == 'X');
}

static unsigned independent_checksum(std::string_view s) {
    unsigned sum = 0;
    for (char c : s) sum += static_cast<unsigned char>(c);
    return sum % 256;
}

static void test_fix() {
    fix::NewOrderSingle nos;
    nos.sender_comp_id = "PFTRADER";
    nos.target_comp_id = "PFX";
    nos.msg_seq_num    = 7;
    nos.sending_time   = "20261017-14:30:00.123";
    nos.clordid        = "PF7";
    nos.symbol         = "PFRG";
    nos.side           = Side::Sell;
    nos.qty            = 100;
    nos.price          = 1002500;
    nos.tif            = TimeInForce::IOC;
    char buf[256];
    std::string_view msg = fix::encode_new_order_single(nos, buf, sizeof(buf));
    EXPECT(!msg.empty());
    EXPECT(msg.substr(0, 10) == std::string_view("8=FIX.4.4\x01", 10));

    /* verify BodyLength and CheckSum independently of the parser */
    size_t body_start = msg.find('\x01', 10) + 1; /* after 9=NNN| */
    size_t cs_pos     = msg.rfind("10=");
    std::string_view len_field = msg.substr(12, msg.find('\x01', 10) - 12);
    EXPECT(std::to_string(cs_pos - body_start) == std::string(len_field));
    char expect_cs[4];
    std::snprintf(expect_cs, sizeof(expect_cs), "%03u", independent_checksum(msg.substr(0, cs_pos)));
    EXPECT(msg.substr(cs_pos + 3, 3) == expect_cs);
    EXPECT(msg.find("44=100.25\x01") != std::string_view::npos); /* trailing zeros trimmed */

    fix::Message m;
    EXPECT(m.parse(msg.data(), msg.size()) == fix::Message::Error::None);
    fix::ParsedOrder po;
    EXPECT(fix::to_order(m, po));
    EXPECT(po.side == Side::Sell && po.qty == 100 && po.price == 1002500);
    EXPECT(po.tif == TimeInForce::IOC && po.symbol == "PFRG" && po.clordid == "PF7");
    uint64_t seq = 0;
    EXPECT(m.get_uint(fix::tag::MsgSeqNum, seq) && seq == 7);

    /* corrupt one byte of the body: the checksum must catch it */
    std::string bad(msg);
    bad[bad.find("38=") + 3] = '9';
    EXPECT(m.parse(bad.data(), bad.size()) == fix::Message::Error::BadChecksum);

    /* drop a byte from the body: BodyLength no longer matches */
    std::string shorter(msg);
    shorter.erase(shorter.find("55=") + 3, 1);
    EXPECT(m.parse(shorter.data(), shorter.size()) == fix::Message::Error::BadBodyLength);

    std::string nobegin = "9=5\x01" "35=D\x01" "10=000\x01";
    EXPECT(m.parse(nobegin.data(), nobegin.size()) == fix::Message::Error::BadBeginString);

    Price p = 0;
    EXPECT(fix::parse_price("100.25", p) && p == 1002500);
    EXPECT(fix::parse_price("100", p) && p == 1000000);
    EXPECT(fix::parse_price("0.0001", p) && p == 1);
    EXPECT(fix::parse_price("-3.5", p) && p == -35000);
    EXPECT(!fix::parse_price("1.23456", p)); /* > 4 decimals: refuse to round */
    EXPECT(!fix::parse_price("1.2x", p));
    EXPECT(!fix::parse_price("abc", p));

    EXPECT(fix::encode_new_order_single(nos, buf, 40).empty()); /* too small */
}

int main() {
    test_wire_byte_order();
    test_writer_reader_bounds();
    test_itch_roundtrip();
    test_moldudp64();
    test_soupbin_framing();
    test_ouch_roundtrip();
    test_fix();
    if (g_failures == 0) {
        std::printf("test_wire_protocols: ALL PASSED\n");
        return 0;
    }
    std::printf("test_wire_protocols: %d FAILURE(S)\n", g_failures);
    return 1;
}
