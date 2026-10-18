/*
 * trader.cpp - a complete (toy) trading client, and a tick-to-trade
 * latency microscope.
 *
 * Responsibility: close the loop against bin/exchange:
 *
 *   ITCH multicast --> feed handler --> replica OrderBook --> strategy
 *        ^ (SO_TIMESTAMPING                                     |
 *        |  kernel RX stamp)                                    v
 *   exchange <-- OUCH over TCP <-- order entry <-- pre-trade risk gate
 *            --> Accepted/Executed --> positions, PnL, latency stats
 *
 * Startup order matters (snapshot + incremental sync, feed_handler.hpp):
 *   1. join the multicast group  -> the kernel starts queueing live packets
 *   2. pull the snapshot over TCP -> book as of sequence S
 *   3. drain the socket: skip messages < S, apply >= S
 * On a sequence gap or a book inconsistency: mark the book stale, cancel
 * our open orders, re-snapshot, carry on. Never trade on a stale book.
 *
 * The latency report: every packet and every order is timestamped at
 * each stage, all on CLOCK_REALTIME so they can be subtracted:
 *
 *   exchange event (ITCH ts) -> kernel RX (SO_TIMESTAMPING) -> user space
 *   -> book updated -> strategy+risk+encode -> send() -> [kernel TX stamp]
 *   -> exchange accepted (OUCH ts) -> ack back in user space
 *
 * "Tick-to-trade" here = kernel RX stamp of the triggering packet ->
 * send() returned. In production you would measure NIC-to-NIC with
 * hardware timestamps (or a passive network tap); the stages are the same.
 *
 * Modes: default blocks in epoll_wait (sleeps until a packet arrives -
 * cheap on CPU, but each wakeup costs scheduler latency); --spin polls
 * with a zero timeout (burns a core, removes the wakeup). Compare the
 * "kernel rx -> user space" row between the two. Pin with --cpu N.
 *
 * Usage: bin/trader [--help]
 */

#include "common.hpp"
#include "cpu_util.hpp"
#include "event_loop.hpp"
#include "feed_handler.hpp"
#include "flat_id_map.hpp"
#include "itch.hpp"
#include "latency_sampler.hpp"
#include "multicast.hpp"
#include "net_timestamp.hpp"
#include "order_book.hpp"
#include "ouch.hpp"
#include "risk.hpp"
#include "soupbin.hpp"
#include "strategy.hpp"
#include "time_util.hpp"
#include "trading_types.hpp"

#include <arpa/inet.h>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <vector>

namespace {

constexpr uint64_t kTimerPeriodNs  = 100000000ULL;  /* 100 ms housekeeping */
constexpr uint64_t kHeartbeatNs    = 1000000000ULL;
constexpr size_t   kTxRing         = 256;
constexpr int      kMdRcvBufBytes  = 8 * 1024 * 1024;

struct TraderConfig {
    std::string    host          = "127.0.0.1";
    int            order_port    = 9100;
    int            snapshot_port = 9101;
    std::string    md_group      = "239.255.0.2";
    int            md_port       = 9200;
    std::string    symbol        = "PFRG";
    std::string    user          = "TRADR1";
    uint64_t       duration_s    = 20;
    bool           spin          = false;
    int            cpu           = -1;
    bool           tx_timestamps = true;
    bool           trade         = true;
    bool           mlock         = false;
    bool           verbose       = false;
    StrategyParams strat;
    RiskLimits     risk;
};

void usage(const char *prog) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "  --host IP              exchange host (default 127.0.0.1)\n"
        "  --order-port PORT      OUCH order entry (default 9100)\n"
        "  --snapshot-port PORT   snapshot service (default 9101)\n"
        "  --md-group IP          ITCH multicast group (default 239.255.0.2)\n"
        "  --md-port PORT         ITCH multicast port (default 9200)\n"
        "  --symbol SYM           symbol to trade (default PFRG)\n"
        "  --user NAME            login name, <= 6 chars (default TRADR1)\n"
        "  --duration S           run for S seconds (default 20)\n"
        "  --spin                 busy-poll instead of sleeping in epoll_wait\n"
        "  --cpu N                pin to CPU N\n"
        "  --mlock                mlockall(): no page faults on the hot path\n"
        "  --no-trade             feed handler + book only, never send orders\n"
        "  --no-tx-timestamps     skip SO_TIMESTAMPING on the order socket\n"
        "  --qty N                order size (default 100)\n"
        "  --imbalance PCT        signal threshold, 1..99 (default 60)\n"
        "  --cooldown-us N        min gap between signals (default 1000)\n"
        "  --max-pos N            risk: max |position| (default 2000)\n"
        "  --max-rate N           risk: max orders per second (default 200)\n"
        "  --verbose              print every fill and a status line each second\n",
        prog);
}

int parse_args(int argc, char **argv, TraderConfig &c) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                std::exit(EXIT_USAGE);
            }
            return argv[++i];
        };
        if (a == "--host") c.host = val();
        else if (a == "--order-port") c.order_port = std::atoi(val().c_str());
        else if (a == "--snapshot-port") c.snapshot_port = std::atoi(val().c_str());
        else if (a == "--md-group") c.md_group = val();
        else if (a == "--md-port") c.md_port = std::atoi(val().c_str());
        else if (a == "--symbol") c.symbol = val();
        else if (a == "--user") c.user = val();
        else if (a == "--duration") c.duration_s = std::strtoull(val().c_str(), nullptr, 10);
        else if (a == "--spin") c.spin = true;
        else if (a == "--cpu") c.cpu = std::atoi(val().c_str());
        else if (a == "--mlock") c.mlock = true;
        else if (a == "--no-trade") c.trade = false;
        else if (a == "--no-tx-timestamps") c.tx_timestamps = false;
        else if (a == "--qty") c.strat.order_qty = static_cast<Qty>(std::strtoul(val().c_str(), nullptr, 10));
        else if (a == "--imbalance") c.strat.imbalance_pct = std::atoll(val().c_str());
        else if (a == "--cooldown-us") c.strat.cooldown_ns = std::strtoull(val().c_str(), nullptr, 10) * 1000;
        else if (a == "--max-pos") c.risk.max_position = std::atoll(val().c_str());
        else if (a == "--max-rate") c.risk.max_orders_per_sec = static_cast<uint32_t>(std::strtoul(val().c_str(), nullptr, 10));
        else if (a == "--verbose") c.verbose = true;
        else if (a == "--help" || a == "-h") {
            usage(argv[0]);
            std::exit(EXIT_OK);
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", a.c_str());
            usage(argv[0]);
            return -1;
        }
    }
    if (c.symbol.empty() || c.symbol.size() > 8 || c.user.empty() || c.user.size() > 6) {
        std::fprintf(stderr, "--symbol must be 1..8 chars, --user 1..6 chars\n");
        return -1;
    }
    if (c.strat.imbalance_pct < 1 || c.strat.imbalance_pct > 99 || c.strat.order_qty == 0) {
        std::fprintf(stderr, "--imbalance must be 1..99 and --qty > 0\n");
        return -1;
    }
    c.strat.max_position = c.risk.max_position;
    if (c.risk.max_order_qty < c.strat.order_qty) {
        c.risk.max_order_qty = c.strat.order_qty;
    }
    if (!is_multicast_address(c.md_group)) {
        std::fprintf(stderr, "--md-group %s is not a multicast address\n", c.md_group.c_str());
        return -1;
    }
    return 0;
}

void set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    CHECK(fl, "fcntl(F_GETFL)");
    CHECK(fcntl(fd, F_SETFL, fl | O_NONBLOCK), "fcntl(F_SETFL)");
}

sockaddr_in make_addr(const std::string &ip, int port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) {
        throw SystemError("inet_pton(" + ip + ")", EINVAL);
    }
    return a;
}

Socket tcp_connect(const std::string &ip, int port) {
    Socket s(AF_INET, SOCK_STREAM, 0);
    CHECK(s.get(), "socket(tcp)");
    sockaddr_in a = make_addr(ip, port);
    CHECK(connect(s.get(), reinterpret_cast<const sockaddr *>(&a), sizeof(a)),
          "connect(" + ip + ":" + std::to_string(port) + ")");
    int one = 1;
    CHECK(setsockopt(s.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)), "TCP_NODELAY");
    return s;
}

bool send_all_blocking(int fd, const uint8_t *p, size_t n) {
    while (n > 0) {
        ssize_t k = send(fd, p, n, MSG_NOSIGNAL);
        if (k <= 0) {
            if (k < 0 && errno == EINTR) continue;
            return false;
        }
        p += k;
        n -= static_cast<size_t>(k);
    }
    return true;
}

size_t login_frame(uint8_t *out, size_t cap, const std::string &user) {
    soup::LoginRequest req;
    req.username           = user;
    req.password           = "pulse";
    req.requested_sequence = 1;
    uint8_t payload[soup::kLoginRequestLen];
    size_t n = soup::encode_login_request(req, payload, sizeof(payload));
    return soup::write_frame(out, cap, soup::kLoginRequest, payload, n);
}

/* Per-packet timestamps that may become an order's "trigger". */
struct Trigger {
    uint64_t exch_mid  = 0; /* ITCH timestamp of the last message that touched our book */
    uint64_t kernel_rx = 0; /* SO_TIMESTAMPING, 0 if unavailable */
    uint64_t user_rx   = 0; /* right after recvmsg() returned */
    uint64_t book_done = 0; /* after every message in the packet was applied */
};

struct LiveOrder {
    uint32_t user_ref     = 0;
    Side     side         = Side::Buy;
    Price    price        = 0;
    Qty      qty          = 0;
    Qty      leaves       = 0;
    uint64_t exch_id      = 0;
    bool     accepted     = false;
    uint64_t t_send_start = 0;
    uint64_t t_sent       = 0;
};

/* What we need to time an order once its TX stamp shows up - kept apart
 * from LiveOrder because the stamp can arrive after the order is done. */
struct TxPending {
    uint32_t tx_id        = 0;
    bool     used         = false;
    uint64_t t_kernel_rx  = 0;
    uint64_t t_send_start = 0;
};

class Trader {
public:
    explicit Trader(const TraderConfig &c)
        : cfg_(c),
          symbol_(make_symbol(c.symbol)),
          book_(BookConfig{}),
          orders_(1024),
          risk_(c.risk),
          strat_(c.strat),
          timer_(kTimerPeriodNs),
          signals_({SIGINT, SIGTERM}),
          s_feed_(1u << 19),
          s_wakeup_(1u << 19),
          s_book_(1u << 19),
          s_strategy_(1u << 16),
          s_send_(1u << 16),
          s_t2t_(1u << 16),
          s_tx_stack_(1u << 16),
          s_w2w_(1u << 16),
          s_gateway_(1u << 16),
          s_rtt_(1u << 16) {}

    int run();

private:
    static constexpr uint64_t kTagMd     = 1;
    static constexpr uint64_t kTagOe     = 2;
    static constexpr uint64_t kTagTimer  = 3;
    static constexpr uint64_t kTagSignal = 4;

    /* ---------------- market data ---------------- */

    void setup_md_socket() {
        md_ = Socket(AF_INET, SOCK_DGRAM, 0);
        CHECK(md_.get(), "socket(md)");
        int one = 1;
        /* several processes on one host may listen to the same feed */
        CHECK(setsockopt(md_.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)), "SO_REUSEADDR");
        /* A big receive buffer is the shock absorber: it holds the live
         * feed while we are busy (e.g. pulling a snapshot). The kernel
         * caps it at net.core.rmem_max; we print what we actually got. */
        int rcvbuf = kMdRcvBufBytes;
        (void)setsockopt(md_.get(), SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        socklen_t len = sizeof(rcvbuf);
        (void)getsockopt(md_.get(), SOL_SOCKET, SO_RCVBUF, &rcvbuf, &len);
        md_rcvbuf_ = rcvbuf;
        /* Bind to the GROUP address, not INADDR_ANY: then this socket only
         * gets datagrams sent to this group, not every group on the port. */
        sockaddr_in a = make_addr(cfg_.md_group, cfg_.md_port);
        CHECK(bind(md_.get(), reinterpret_cast<const sockaddr *>(&a), sizeof(a)), "bind(md)");
        multicast_join_group(md_.get(), cfg_.md_group);
        rx_ts_ = enable_rx_timestamps(md_.get());
        set_nonblocking(md_.get());
    }

    /* GLIMPSE-style snapshot over TCP, blocking with a timeout. While we
     * sit here, live ITCH packets pile up in md_'s kernel buffer. */
    bool snapshot() {
        try {
            Socket s = tcp_connect(cfg_.host, cfg_.snapshot_port);
            timeval tv{};
            tv.tv_sec = 2;
            (void)setsockopt(s.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            uint8_t frame[64];
            size_t fn = login_frame(frame, sizeof(frame), cfg_.user);
            if (!send_all_blocking(s.get(), frame, fn)) {
                return false;
            }
            book_.clear();
            locate_ = 0;
            size_t orders = 0;
            uint64_t next_seq = 0;
            bool done = false;
            soup::FrameReader r;
            while (!done) {
                ssize_t n = recv(s.get(), r.write_ptr(), r.write_space(), 0);
                if (n <= 0) {
                    std::fprintf(stderr, "[trader] snapshot: connection ended early\n");
                    return false;
                }
                r.commit(static_cast<size_t>(n));
                char type;
                const uint8_t *p;
                size_t len;
                while (r.next(type, p, len) == soup::FrameReader::Status::Frame) {
                    if (type == soup::kLoginRejected) {
                        return false;
                    }
                    if (type != soup::kSequencedData) {
                        continue;
                    }
                    auto msg = itch::decode(p, len);
                    if (!msg) {
                        continue;
                    }
                    if (auto *sd = std::get_if<itch::StockDirectory>(&*msg)) {
                        if (sd->stock == symbol_) {
                            locate_ = sd->locate;
                        }
                    } else if (auto *ao = std::get_if<itch::AddOrder>(&*msg)) {
                        if (locate_ != 0 && ao->locate == locate_) {
                            book_.add(ao->order_ref, ao->side, static_cast<Price>(ao->price),
                                      ao->shares);
                            ++orders;
                        }
                    } else if (auto *eos = std::get_if<itch::EndOfSnapshot>(&*msg)) {
                        next_seq = eos->next_sequence;
                        done = true;
                    }
                }
            }
            if (locate_ == 0) {
                std::fprintf(stderr, "[trader] symbol %s is not listed on the exchange\n",
                             cfg_.symbol.c_str());
                return false;
            }
            seq_.reset(next_seq);
            synced_ = true;
            ++snapshots_;
            std::printf("[trader] snapshot: %zu %s orders, resume incremental at seq %" PRIu64
                        "  (%s)\n",
                        orders, cfg_.symbol.c_str(), next_seq, top_of_book().c_str());
            std::fflush(stdout);
            return true;
        } catch (const SystemError &e) {
            std::fprintf(stderr, "[trader] snapshot: %s failed: %s\n", e.what(),
                         std::strerror(e.code()));
            return false;
        }
    }

    void on_md_readable() {
        alignas(64) uint8_t buf[2048];
        for (;;) {
            uint64_t t_kernel = 0;
            ssize_t n = recv_timestamped(md_.get(), buf, sizeof(buf), &t_kernel);
            if (n < 0) {
                return; /* EAGAIN: drained */
            }
            const uint64_t t_user = now_realtime_ns();
            handle_packet(buf, static_cast<size_t>(n), t_kernel, t_user);
        }
    }

    void handle_packet(const uint8_t *p, size_t n, uint64_t t_kernel, uint64_t t_user) {
        mold::Header h;
        if (!mold::parse_header(p, n, h)) {
            ++bad_packets_;
            return;
        }
        ++packets_;
        if (h.count == mold::kEndOfSession) {
            return;
        }
        if (!synced_) {
            return; /* snapshot pending (retried from the timer) */
        }
        if (h.count == 0) {
            ++heartbeats_;
            if (seq_.on_heartbeat(h.sequence) == SequenceTracker::Verdict::Gap) {
                ++gaps_;
                resync("heartbeat shows missed messages");
            }
            return;
        }

        Trigger trig;
        trig.kernel_rx = t_kernel;
        trig.user_rx   = t_user;
        bool touched = false, gap = false, inconsistent = false;
        const bool ok = mold::for_each_message(p, n, h, [&](uint64_t seq, const uint8_t *m, size_t len) {
            if (gap || inconsistent) {
                return;
            }
            switch (seq_.on_message(seq)) {
            case SequenceTracker::Verdict::Duplicate:
                ++duplicates_;
                return;
            case SequenceTracker::Verdict::Gap:
                gap = true;
                return;
            case SequenceTracker::Verdict::Apply:
                break;
            }
            ++messages_;
            auto msg = itch::decode(m, len);
            if (!msg) {
                ++bad_messages_;
                return;
            }
            switch (apply_itch(book_, locate_, *msg)) {
            case ApplyResult::Applied:
                touched = true;
                trig.exch_mid = itch_timestamp(*msg);
                break;
            case ApplyResult::Inconsistent:
                inconsistent = true;
                break;
            case ApplyResult::Ignored:
                if (auto *se = std::get_if<itch::SystemEvent>(&*msg)) {
                    if (se->event_code == itch::kEndOfMessages) {
                        exchange_closed_ = true;
                    }
                }
                break;
            }
        });
        if (!ok) {
            ++bad_packets_;
        }
        trig.book_done = now_realtime_ns();

        /* per-packet market-data latency */
        if (t_kernel != 0) {
            s_wakeup_.record(t_user - t_kernel);
            if (touched) {
                const int64_t feed = mid_diff(ns_since_midnight(t_kernel), trig.exch_mid);
                if (feed >= 0) {
                    s_feed_.record(static_cast<uint64_t>(feed));
                }
            }
        }
        s_book_.record(trig.book_done - t_user);

        if (gap) {
            ++gaps_;
            resync("sequence gap");
            return;
        }
        if (inconsistent) {
            ++inconsistencies_;
            resync("book inconsistency (unknown order id)");
            return;
        }
        if (touched && cfg_.trade && oe_up_) {
            maybe_trade(trig);
        }
    }

    void resync(const char *why) {
        synced_ = false;
        ++resyncs_;
        std::printf("[trader] GAP: %s (expected seq %" PRIu64 ") -> book STALE, pulling orders, "
                    "re-snapshotting\n",
                    why, seq_.expected());
        std::fflush(stdout);
        cancel_all();
        (void)snapshot(); /* on failure synced_ stays false; the timer retries */
    }

    std::string top_of_book() const {
        if (book_.empty(Side::Buy) || book_.empty(Side::Sell)) {
            return "one-sided book";
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "bid %s x %" PRIu64 " / ask %s x %" PRIu64,
                      format_price(book_.best_price(Side::Buy)).c_str(), book_.best_qty(Side::Buy),
                      format_price(book_.best_price(Side::Sell)).c_str(), book_.best_qty(Side::Sell));
        return buf;
    }

    Price mid_price() const {
        if (book_.empty(Side::Buy) || book_.empty(Side::Sell)) {
            return 0;
        }
        return (book_.best_price(Side::Buy) + book_.best_price(Side::Sell)) / 2;
    }

    /* ---------------- strategy -> risk -> order ---------------- */

    void maybe_trade(const Trigger &trig) {
        auto sig = strat_.on_book_update(book_, risk_.position(), orders_.size() > 0,
                                         trig.book_done);
        if (!sig) {
            return;
        }
        ++signal_count_;
        const RiskVerdict v =
            risk_.check(sig->side, sig->price, sig->qty, mid_price(), trig.book_done);
        if (v != RiskVerdict::Pass) {
            ++risk_blocked_[static_cast<int>(v)];
            return;
        }
        send_order(*sig, trig);
    }

    void send_order(const Signal &sig, const Trigger &trig) {
        LiveOrder o;
        o.user_ref = next_user_ref_++;
        o.side     = sig.side;
        o.price    = sig.price;
        o.qty      = sig.qty;
        o.leaves   = sig.qty;

        ouch::EnterOrder eo;
        eo.user_ref = o.user_ref;
        eo.side     = o.side;
        eo.qty      = o.qty;
        eo.symbol   = symbol_;
        eo.price    = o.price;
        eo.tif      = TimeInForce::IOC;
        char cl[16];
        std::snprintf(cl, sizeof(cl), "PF%u", o.user_ref);
        eo.clordid = ouch::make_clordid(cl);

        /* Encode straight into the frame buffer after the 3-byte SoupBin
         * header - no intermediate copy - then fill the header in. */
        uint8_t frame[64];
        const size_t pn = ouch::encode(eo, frame + soup::kFrameHeaderLen,
                                       sizeof(frame) - soup::kFrameHeaderLen);
        wire::put_u16(frame, static_cast<uint16_t>(pn + 1));
        frame[2] = static_cast<uint8_t>(soup::kUnsequencedData);
        const size_t fn = pn + soup::kFrameHeaderLen;

        o.t_send_start = now_realtime_ns();
        const bool whole = oe_send(frame, fn);
        o.t_sent = now_realtime_ns();

        if (!orders_.insert(o.user_ref, o)) {
            std::fprintf(stderr, "[trader] order table full\n");
        }
        risk_.on_order_sent(o.side, o.qty, trig.book_done);
        ++orders_sent_;

        s_strategy_.record(o.t_send_start - trig.book_done);
        s_send_.record(o.t_sent - o.t_send_start);
        if (trig.kernel_rx != 0) {
            s_t2t_.record(o.t_sent - trig.kernel_rx);
        }
        if (whole && tx_ts_) {
            TxPending &tp   = tx_ring_[tx_head_++ % kTxRing];
            tp.tx_id        = static_cast<uint32_t>(stream_bytes_ - 1);
            tp.used         = true;
            tp.t_kernel_rx  = trig.kernel_rx;
            tp.t_send_start = o.t_send_start;
        }
    }

    void send_cancel(uint32_t user_ref) {
        ouch::CancelOrder c;
        c.user_ref = user_ref;
        c.qty      = 0;
        uint8_t payload[ouch::len::CancelOrder];
        size_t pn = ouch::encode(c, payload, sizeof(payload));
        uint8_t frame[32];
        size_t fn = soup::write_frame(frame, sizeof(frame), soup::kUnsequencedData, payload, pn);
        (void)oe_send(frame, fn);
    }

    /* "Pull your quotes": cancel every open order (used when the book
     * goes stale - you cannot manage orders in a market you cannot see). */
    void cancel_all() {
        if (!oe_up_) {
            return;
        }
        std::vector<uint32_t> refs;
        orders_.for_each([&refs](uint64_t key, const LiveOrder &o) {
            if (o.leaves > 0) {
                refs.push_back(static_cast<uint32_t>(key));
            }
        });
        for (uint32_t r : refs) {
            send_cancel(r);
        }
    }

    /* Every byte to the order socket goes through here so stream_bytes_
     * stays exact - TX timestamp ids are byte offsets in this stream.
     * Returns true if the whole buffer went out in this one send(). */
    bool oe_send(const uint8_t *p, size_t n) {
        if (!oe_up_) {
            return false;
        }
        if (!oe_pending_.empty()) {
            oe_pending_.insert(oe_pending_.end(), p, p + n); /* preserve ordering */
            return false;
        }
        ssize_t k = send(oe_.get(), p, n, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (k == static_cast<ssize_t>(n)) {
            stream_bytes_ += n;
            last_oe_tx_ = now_ns();
            return true;
        }
        if (k < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::fprintf(stderr, "[trader] order entry send failed: %s\n", std::strerror(errno));
                oe_up_ = false;
                return false;
            }
            k = 0;
        }
        stream_bytes_ += static_cast<uint64_t>(k);
        oe_pending_.insert(oe_pending_.end(), p + k, p + n);
        ep_.modify(oe_.get(), EPOLLIN | EPOLLOUT, kTagOe);
        return false;
    }

    void flush_oe_pending() {
        while (!oe_pending_.empty()) {
            ssize_t k = send(oe_.get(), oe_pending_.data(), oe_pending_.size(),
                             MSG_NOSIGNAL | MSG_DONTWAIT);
            if (k <= 0) {
                if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    return;
                }
                oe_up_ = false;
                return;
            }
            stream_bytes_ += static_cast<uint64_t>(k);
            oe_pending_.erase(oe_pending_.begin(), oe_pending_.begin() + k);
        }
        ep_.modify(oe_.get(), EPOLLIN, kTagOe);
    }

    /* ---------------- order entry session ---------------- */

    void connect_order_entry() {
        oe_ = tcp_connect(cfg_.host, cfg_.order_port);
        /* Enable TX stamps right after connect, before the first byte, so
         * byte offset 0 of the id space is the first byte of the login. */
        tx_ts_ = cfg_.tx_timestamps && enable_tx_timestamps(oe_.get());
        oe_up_ = true;

        uint8_t frame[64];
        size_t fn = login_frame(frame, sizeof(frame), cfg_.user);
        if (!send_all_blocking(oe_.get(), frame, fn)) {
            throw SystemError("send(login)", errno);
        }
        stream_bytes_ += fn;
        last_oe_tx_ = now_ns();

        /* wait (blocking, 2 s) for Login Accepted */
        timeval tv{};
        tv.tv_sec = 2;
        (void)setsockopt(oe_.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        for (;;) {
            ssize_t n = recv(oe_.get(), oe_reader_.write_ptr(), oe_reader_.write_space(), 0);
            if (n <= 0) {
                throw SystemError("recv(login reply)", n == 0 ? ECONNRESET : errno);
            }
            oe_reader_.commit(static_cast<size_t>(n));
            char type;
            const uint8_t *p;
            size_t len;
            if (oe_reader_.next(type, p, len) == soup::FrameReader::Status::Frame) {
                if (type == soup::kLoginAccepted) {
                    soup::LoginAccepted acc;
                    (void)soup::decode_login_accepted(p, len, acc);
                    std::printf("[trader] logged in as %s, session %s, next seq %" PRIu64 "\n",
                                cfg_.user.c_str(), acc.session.c_str(), acc.sequence);
                    break;
                }
                throw SystemError("login rejected", EACCES);
            }
        }
        tv.tv_sec = 0;
        (void)setsockopt(oe_.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        set_nonblocking(oe_.get());
    }

    void on_oe_event(uint32_t events) {
        if (events & EPOLLERR) {
            /* With SO_TIMESTAMPING, EPOLLERR mostly means "TX stamps are
             * waiting on the error queue", not a broken connection. If
             * there were no stamps, ask the socket for a real error. */
            if (drain_tx_stamps() == 0) {
                int err = 0;
                socklen_t len = sizeof(err);
                if (getsockopt(oe_.get(), SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err != 0) {
                    std::fprintf(stderr, "[trader] order entry socket error: %s\n",
                                 std::strerror(err));
                    oe_up_ = false;
                    stop_  = true;
                    return;
                }
            }
        }
        if (events & EPOLLOUT) {
            flush_oe_pending();
        }
        if (events & (EPOLLIN | EPOLLHUP)) {
            read_oe();
        }
    }

    size_t drain_tx_stamps() {
        TxStamp stamps[32];
        size_t n, total = 0;
        while ((n = read_tx_timestamps(oe_.get(), stamps, 32)) > 0) {
            total += n;
            for (size_t i = 0; i < n; i++) {
                for (TxPending &tp : tx_ring_) {
                    if (tp.used && tp.tx_id == stamps[i].id) {
                        tp.used = false;
                        if (stamps[i].ns >= tp.t_send_start) {
                            s_tx_stack_.record(stamps[i].ns - tp.t_send_start);
                        }
                        if (tp.t_kernel_rx != 0 && stamps[i].ns >= tp.t_kernel_rx) {
                            s_w2w_.record(stamps[i].ns - tp.t_kernel_rx);
                        }
                        break;
                    }
                }
            }
            if (n < 32) {
                break;
            }
        }
        return total;
    }

    void read_oe() {
        for (;;) {
            ssize_t n = recv(oe_.get(), oe_reader_.write_ptr(), oe_reader_.write_space(), 0);
            if (n > 0) {
                oe_reader_.commit(static_cast<size_t>(n));
                const uint64_t t_rx = now_realtime_ns();
                last_oe_rx_ = now_ns();
                char type;
                const uint8_t *p;
                size_t len;
                soup::FrameReader::Status st;
                while ((st = oe_reader_.next(type, p, len)) == soup::FrameReader::Status::Frame) {
                    handle_oe_frame(type, p, len, t_rx);
                }
                if (st == soup::FrameReader::Status::Error) {
                    std::fprintf(stderr, "[trader] order entry framing error\n");
                    oe_up_ = false;
                    stop_ = true;
                    return;
                }
                continue;
            }
            if (n == 0) {
                std::printf("[trader] exchange closed the order entry connection\n");
                oe_up_ = false;
                stop_ = true;
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                oe_up_ = false;
                stop_ = true;
            }
            return;
        }
    }

    void handle_oe_frame(char type, const uint8_t *p, size_t len, uint64_t t_rx) {
        switch (type) {
        case soup::kSequencedData: {
            auto m = ouch::decode_outbound(p, len);
            if (!m) {
                ++bad_messages_;
                return;
            }
            std::visit(overloaded{
                           [&](const ouch::OrderAccepted &a) { on_accepted(a, t_rx); },
                           [&](const ouch::OrderExecuted &e) { on_executed(e); },
                           [&](const ouch::OrderCanceled &c) { on_canceled(c); },
                           [&](const ouch::OrderRejected &j) { on_rejected(j); },
                       },
                       *m);
            break;
        }
        case soup::kServerHeartbeat:
            break;
        case soup::kEndOfSession:
            std::printf("[trader] exchange: end of session\n");
            stop_ = true;
            break;
        default:
            break;
        }
    }

    void on_accepted(const ouch::OrderAccepted &a, uint64_t t_rx) {
        LiveOrder *o = orders_.find(a.user_ref);
        if (o == nullptr) {
            ++unknown_reports_;
            return;
        }
        o->accepted = true;
        o->exch_id  = a.order_ref;
        ++accepted_;
        /* Measured from send() START: on loopback the kernel runs the
         * exchange side's TCP receive inline inside our send() call, so
         * the exchange can accept the order before our send() returns. */
        const int64_t gw = mid_diff(a.timestamp, ns_since_midnight(o->t_send_start));
        if (gw >= 0) {
            s_gateway_.record(static_cast<uint64_t>(gw));
        }
        s_rtt_.record(t_rx - o->t_send_start);
    }

    void on_executed(const ouch::OrderExecuted &e) {
        LiveOrder *o = orders_.find(e.user_ref);
        if (o == nullptr) {
            ++unknown_reports_;
            return;
        }
        const Qty q = e.qty > o->leaves ? o->leaves : e.qty;
        o->leaves -= q;
        risk_.on_fill(o->side, q);
        const int64_t notional = e.price * static_cast<int64_t>(q);
        cash_ += o->side == Side::Buy ? -notional : notional;
        ++fills_;
        filled_shares_ += q;
        if (cfg_.verbose) {
            std::printf("[trader] FILL %s %u @ %s  position %" PRId64 "\n",
                        o->side == Side::Buy ? "BUY " : "SELL", q, format_price(e.price).c_str(),
                        risk_.position());
        }
        if (o->leaves == 0) {
            close_order(e.user_ref);
        }
    }

    void on_canceled(const ouch::OrderCanceled &c) {
        LiveOrder *o = orders_.find(c.user_ref);
        if (o == nullptr) {
            ++unknown_reports_;
            return;
        }
        const Qty q = c.qty > o->leaves ? o->leaves : c.qty;
        o->leaves -= q;
        risk_.on_cancelled(o->side, q);
        ++cancels_;
        if (o->leaves == 0) {
            close_order(c.user_ref);
        }
    }

    void on_rejected(const ouch::OrderRejected &j) {
        LiveOrder *o = orders_.find(j.user_ref);
        if (o == nullptr) {
            ++unknown_reports_;
            return;
        }
        ++rejects_;
        std::printf("[trader] order %u rejected, reason '%c'\n", j.user_ref, j.reason);
        risk_.on_cancelled(o->side, o->leaves);
        o->leaves = 0;
        close_order(j.user_ref);
    }

    void close_order(uint32_t user_ref) {
        risk_.on_order_closed();
        orders_.erase(user_ref);
    }

    /* ---------------- housekeeping ---------------- */

    void on_timer() {
        if (timer_.drain() == 0) {
            return;
        }
        const uint64_t now = now_ns();
        if (oe_up_ && now - last_oe_tx_ >= kHeartbeatNs) {
            uint8_t frame[3];
            size_t fn = soup::write_frame(frame, sizeof(frame), soup::kClientHeartbeat, nullptr, 0);
            (void)oe_send(frame, fn);
        }
        if (!synced_ && now - last_resync_try_ >= 500000000ULL) {
            last_resync_try_ = now;
            (void)snapshot();
        }
        if (cfg_.verbose && now - last_status_ >= 1000000000ULL) {
            last_status_ = now;
            std::printf("[trader] t=%2" PRIu64 "s pkts %" PRIu64 " msgs %" PRIu64
                        " gaps %" PRIu64 " | %s | orders %" PRIu64 " fills %" PRIu64
                        " pos %" PRId64 "\n",
                        static_cast<uint64_t>((now - start_ns_) / 1000000000ULL), packets_,
                        messages_, gaps_,
                        synced_ ? top_of_book().c_str() : "STALE", orders_sent_, fills_,
                        risk_.position());
            std::fflush(stdout);
        }
        if (now >= deadline_ns_) {
            stop_ = true;
        }
        if (exchange_closed_) {
            std::printf("[trader] exchange published end of messages\n");
            stop_ = true;
        }
    }

    void pump(int timeout_ms) {
        epoll_event ev[16];
        const int n = ep_.wait(ev, 16, timeout_ms);
        for (int i = 0; i < n; i++) {
            switch (ev[i].data.u64) {
            case kTagMd:
                on_md_readable();
                break;
            case kTagOe:
                on_oe_event(ev[i].events);
                break;
            case kTagTimer:
                on_timer();
                break;
            case kTagSignal:
                if (signals_.read_signal() != 0) {
                    stop_ = true;
                }
                break;
            default:
                break;
            }
        }
    }

    void print_row(const char *name, const LatencySampler &s) const {
        LatencySampler::Summary x = s.summarize();
        if (x.count == 0) {
            std::printf("  %-44s %9s\n", name, "n/a");
            return;
        }
        std::printf("  %-44s %9zu %8" PRIu64 " %8" PRIu64 " %8" PRIu64 " %8" PRIu64 " %9" PRIu64
                    "\n",
                    name, x.count, x.p50, x.p90, x.p99, x.p999, x.max);
    }

    void report() const {
        const double secs = static_cast<double>(now_ns() - start_ns_) / 1e9;
        std::printf("\n==== PulseForge trader report (%.1f s, %s%s) ====\n", secs,
                    cfg_.spin ? "busy-poll" : "epoll blocking",
                    cfg_.cpu >= 0 ? (", pinned to cpu " + std::to_string(cfg_.cpu)).c_str() : "");
        std::printf("market data  packets %" PRIu64 "  messages %" PRIu64 "  heartbeats %" PRIu64
                    "  duplicates %" PRIu64 "  gaps %" PRIu64 "  inconsistencies %" PRIu64
                    "  snapshots %" PRIu64 "\n",
                    packets_, messages_, heartbeats_, duplicates_, gaps_, inconsistencies_,
                    snapshots_);
        std::printf("             resyncs %" PRIu64 "  bad packets %" PRIu64 "  bad messages %" PRIu64
                    "  unknown exec reports %" PRIu64 "\n",
                    resyncs_, bad_packets_, bad_messages_, unknown_reports_);
        std::printf("             SO_RCVBUF %d bytes, kernel RX timestamps %s, TX timestamps %s\n",
                    md_rcvbuf_, rx_ts_ ? "ON (software)" : "unavailable",
                    tx_ts_ ? "ON (software)" : "off");
        std::printf("orders       signals %" PRIu64 "  sent %" PRIu64 "  accepted %" PRIu64
                    "  fills %" PRIu64 " (%" PRIu64 " shares)  cancels %" PRIu64
                    "  rejects %" PRIu64 "\n",
                    signal_count_, orders_sent_, accepted_, fills_, filled_shares_, cancels_,
                    rejects_);
        std::printf("risk blocks ");
        bool any = false;
        for (int i = 1; i < kRiskVerdictCount; i++) {
            if (risk_blocked_[i] != 0) {
                std::printf(" %s=%" PRIu64, to_string(static_cast<RiskVerdict>(i)), risk_blocked_[i]);
                any = true;
            }
        }
        std::printf("%s\n", any ? "" : " none");
        const Price mid = mid_price();
        const int64_t pos = risk_.position();
        const double pnl = static_cast<double>(cash_ + pos * mid) / static_cast<double>(kPriceScale);
        std::printf("position     %" PRId64 " shares, PnL $%.2f (marked at mid %s)\n", pos, pnl,
                    mid ? format_price(mid).c_str() : "n/a");

        std::printf("\nlatency in ns                                    count      p50      p90"
                    "      p99    p99.9       max\n");
        std::printf(" market data path (every packet)\n");
        print_row("exchange event -> kernel RX", s_feed_);
        print_row("kernel RX -> user space (wakeup + recvmsg)", s_wakeup_);
        print_row("user space -> book updated (decode+apply)", s_book_);
        std::printf(" order path (packets that triggered an order)\n");
        print_row("book updated -> send() (strategy+risk+enc)", s_strategy_);
        print_row("send() syscall", s_send_);
        print_row("TICK-TO-TRADE: kernel RX -> send() returned", s_t2t_);
        print_row("send() start -> driver TX stamp", s_tx_stack_);
        print_row("kernel RX -> driver TX (sw wire-to-wire)", s_w2w_);
        print_row("send() start -> exchange accepted (OUCH ts)", s_gateway_);
        print_row("send() start -> ack in user space (RTT)", s_rtt_);
        std::printf("\nAll stamps are CLOCK_REALTIME on one host. Kernel stamps are software\n"
                    "(SO_TIMESTAMPING); on a NIC with hardware timestamping the same code\n"
                    "path would read the PHY's clock instead.\n");
    }

    TraderConfig                 cfg_;
    Symbol                       symbol_;
    uint16_t                     locate_ = 0;
    Socket                       md_{-1};
    Socket                       oe_{-1};
    OrderBook                    book_;
    SequenceTracker              seq_;
    FlatIdMap<LiveOrder>         orders_;
    RiskGate                     risk_;
    ImbalanceStrategy            strat_;
    Epoll                        ep_;
    TimerFd                      timer_;
    SignalFd                     signals_;
    soup::FrameReader            oe_reader_;
    std::vector<uint8_t>         oe_pending_;
    std::array<TxPending, kTxRing> tx_ring_{};
    size_t                       tx_head_ = 0;
    uint64_t                     stream_bytes_ = 0;
    uint32_t                     next_user_ref_ = 1;
    bool synced_ = false, oe_up_ = false, rx_ts_ = false, tx_ts_ = false;
    bool stop_ = false, exchange_closed_ = false;
    int  md_rcvbuf_ = 0;
    int64_t cash_ = 0;
    uint64_t start_ns_ = 0, deadline_ns_ = 0, last_oe_tx_ = 0, last_oe_rx_ = 0;
    uint64_t last_resync_try_ = 0, last_status_ = 0;
    uint64_t packets_ = 0, messages_ = 0, heartbeats_ = 0, duplicates_ = 0, gaps_ = 0;
    uint64_t inconsistencies_ = 0, resyncs_ = 0, snapshots_ = 0, bad_packets_ = 0, bad_messages_ = 0;
    uint64_t signal_count_ = 0, orders_sent_ = 0, accepted_ = 0, fills_ = 0, filled_shares_ = 0;
    uint64_t cancels_ = 0, rejects_ = 0, unknown_reports_ = 0;
    uint64_t risk_blocked_[kRiskVerdictCount] = {};
    LatencySampler s_feed_, s_wakeup_, s_book_, s_strategy_, s_send_, s_t2t_, s_tx_stack_,
        s_w2w_, s_gateway_, s_rtt_;
};

int Trader::run() {
    if (cfg_.cpu >= 0 && pin_to_cpu(cfg_.cpu) != 0) {
        std::perror("pin_to_cpu");
    }
    if (cfg_.mlock) {
        /* Lock every current and future page in RAM: no page fault (and
         * no swap-in) can ever stall the hot path. Needs RLIMIT_MEMLOCK
         * or CAP_IPC_LOCK; we just warn if it fails. */
        if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
            std::perror("mlockall (try: ulimit -l unlimited)");
        }
    }
    std::printf("PulseForge trader: %s on %s:%d (orders) / %s:%d (market data)%s\n",
                cfg_.symbol.c_str(), cfg_.host.c_str(), cfg_.order_port, cfg_.md_group.c_str(),
                cfg_.md_port, cfg_.trade ? "" : "  [no-trade]");

    setup_md_socket(); /* 1. subscribe first ...           */
    if (!snapshot()) { /* 2. ... then snapshot ...          */
        std::fprintf(stderr, "[trader] initial snapshot failed - is bin/exchange running?\n");
        return EXIT_SYSFAIL;
    }
    connect_order_entry();

    ep_.add(md_.get(), EPOLLIN, kTagMd); /* 3. ... then drain the queued feed */
    ep_.add(oe_.get(), EPOLLIN, kTagOe);
    ep_.add(timer_.get(), EPOLLIN, kTagTimer);
    ep_.add(signals_.get(), EPOLLIN, kTagSignal);

    start_ns_    = now_ns();
    deadline_ns_ = start_ns_ + cfg_.duration_s * 1000000000ULL;
    std::printf("[trader] running for %" PRIu64 " s (%s)...\n", cfg_.duration_s,
                cfg_.spin ? "busy-poll" : "epoll blocking");
    std::fflush(stdout);

    const int timeout = cfg_.spin ? 0 : -1;
    while (!stop_) {
        pump(timeout);
    }

    /* Wind down: pull anything still open, give the acks a moment. */
    cancel_all();
    const uint64_t until = now_ns() + 200000000ULL;
    stop_ = false;
    while (oe_up_ && orders_.size() > 0 && now_ns() < until) {
        pump(10);
    }
    if (oe_up_) {
        uint8_t frame[3];
        size_t fn = soup::write_frame(frame, sizeof(frame), soup::kLogoutRequest, nullptr, 0);
        (void)oe_send(frame, fn);
        drain_tx_stamps();
    }
    report();
    return EXIT_OK;
}

} // namespace

int main(int argc, char **argv) {
    try {
        TraderConfig cfg;
        if (parse_args(argc, argv, cfg) != 0) {
            return EXIT_USAGE;
        }
        Trader t(cfg);
        return t.run();
    } catch (const SystemError &e) {
        std::fprintf(stderr, "trader: %s failed: %s\n", e.what(), std::strerror(e.code()));
        return EXIT_SYSFAIL;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "trader: %s\n", e.what());
        return EXIT_CFG;
    }
}
