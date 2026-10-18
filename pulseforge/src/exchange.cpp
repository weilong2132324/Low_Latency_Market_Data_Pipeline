/*
 * exchange.cpp - "PFX", the PulseForge mock exchange.
 *
 * Responsibility: be the other side of the trading loop. One process,
 * one thread, one epoll loop that runs a real (if small) venue:
 *
 *   order entry  TCP :9100   SoupBinTCP + OUCH-style binary orders
 *                            (login, enter, cancel; accepted/executed/
 *                            canceled/rejected reports back)
 *   snapshot     TCP :9101   GLIMPSE-style: on login, stream every
 *                            resting order + "resume at sequence N"
 *   market data  UDP multicast 239.255.0.2:9200
 *                            ITCH-style order-by-order feed in
 *                            MoldUDP64 packets, heartbeats when idle
 *   matching     price-time priority (matching_engine.hpp) over one
 *                cache-friendly OrderBook per symbol (order_book.hpp)
 *   synthetic flow  built-in random participants (owner 0) add, cancel
 *                and take liquidity around a drifting fair price, so the
 *                book is always alive and the trader has something to
 *                react to
 *
 * Data flow for one client order:
 *   recv() bytes -> FrameReader -> 'U' frame -> ouch::decode_inbound
 *   -> MatchingEngine::submit -> EngineListener callbacks (this class):
 *        private: OUCH Accepted / Executed / Canceled  -> session out buffer
 *        public:  ITCH Add / Executed / Cancel / Delete -> MoldUDP64 packet
 *   -> end of loop iteration: flush session buffers (TCP), then flush
 *      the market data packet (UDP). Private reports go out before the
 *      public feed, like real venues: a firm learns about its own fill
 *      no later than the rest of the market learns about the trade.
 *
 * Why one thread: determinism (the same input order always produces the
 * same trades - required for audit and for replaying incidents), no
 * locks on the book, and the whole working set stays in one core's
 * cache. See matching_engine.hpp.
 *
 * Batching: everything produced during one epoll iteration goes out in
 * as few send() calls / UDP packets as possible. Fewer syscalls and
 * packets = more throughput, at the cost of a few microseconds of
 * latency for the first event in a batch. The trader's "exchange event
 * -> kernel rx" stage shows this cost.
 *
 * Usage: bin/exchange [--help]   (see usage() below)
 */

#include "common.hpp"
#include "cpu_util.hpp"
#include "event_loop.hpp"
#include "flat_id_map.hpp"
#include "itch.hpp"
#include "latency_sampler.hpp"
#include "matching_engine.hpp"
#include "multicast.hpp"
#include "ouch.hpp"
#include "soupbin.hpp"
#include "time_util.hpp"
#include "trading_types.hpp"

#include <arpa/inet.h>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr int         kDefaultOrderPort    = 9100;
constexpr int         kDefaultSnapshotPort = 9101;
constexpr int         kDefaultMdPort       = 9200;
constexpr const char *kDefaultMdGroup      = "239.255.0.2";
constexpr const char *kSessionName         = "PFX0000001"; /* 10 chars: MoldUDP64/SoupBin session */
constexpr uint64_t    kTimerPeriodNs       = 1000000ULL;       /* 1 ms housekeeping tick */
constexpr uint64_t    kHeartbeatNs         = 1000000000ULL;    /* 1 s idle -> heartbeat  */
constexpr uint64_t    kSessionTimeoutNs    = 15000000000ULL;   /* silent client -> drop  */
constexpr uint64_t    kStatsEveryNs        = 5000000000ULL;
constexpr size_t      kMaxSyntheticLive    = 400;  /* per symbol */
constexpr size_t      kMaxSessionOrders    = 16384;

struct ExchangeConfig {
    std::string              bind_ip       = "127.0.0.1";
    int                      order_port    = kDefaultOrderPort;
    int                      snapshot_port = kDefaultSnapshotPort;
    std::string              md_group      = kDefaultMdGroup;
    int                      md_port       = kDefaultMdPort;
    int                      md_ttl        = 1;
    std::vector<std::string> symbols       = {"PFRG", "ACME"};
    double                   flow_rate     = 2000.0; /* synthetic events/s per symbol */
    uint64_t                 drop_every    = 0;      /* drop every Nth md packet      */
    uint64_t                 duration_s    = 0;      /* 0 = run until Ctrl-C          */
    uint64_t                 seed          = 42;
    int                      cpu           = -1;
    SelfMatchPolicy          stp           = SelfMatchPolicy::CancelResting;
    bool                     quiet         = false;
    bool                     spin          = false; /* busy-poll epoll (timeout 0) */
};

void usage(const char *prog) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "  --bind IP            address for the TCP ports (default 127.0.0.1)\n"
        "  --order-port PORT    OUCH order entry (default %d)\n"
        "  --snapshot-port PORT GLIMPSE-style snapshot (default %d)\n"
        "  --md-group IP        ITCH multicast group (default %s)\n"
        "  --md-port PORT       ITCH multicast port (default %d)\n"
        "  --symbols A,B,...    listed symbols (default PFRG,ACME)\n"
        "  --flow-rate N        synthetic events/s per symbol (default 2000, 0 = off)\n"
        "  --drop-every N       drop every Nth market data packet (gap test, default off)\n"
        "  --duration S         stop after S seconds (default: run until Ctrl-C)\n"
        "  --seed N             synthetic flow RNG seed (default 42)\n"
        "  --stp resting|incoming  self-match prevention policy (default resting)\n"
        "  --cpu N              pin the exchange thread to CPU N\n"
        "  --spin               busy-poll instead of sleeping in epoll_wait (burns a core)\n"
        "  --quiet              no periodic stats\n",
        prog, kDefaultOrderPort, kDefaultSnapshotPort, kDefaultMdGroup, kDefaultMdPort);
}

std::vector<std::string> split_csv(const std::string &s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t comma = s.find(',', start);
        if (comma == std::string::npos) {
            comma = s.size();
        }
        if (comma > start) {
            out.push_back(s.substr(start, comma - start));
        }
        start = comma + 1;
    }
    return out;
}

int parse_args(int argc, char **argv, ExchangeConfig &cfg) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                std::exit(EXIT_USAGE);
            }
            return argv[++i];
        };
        if (a == "--bind") cfg.bind_ip = val();
        else if (a == "--order-port") cfg.order_port = std::atoi(val().c_str());
        else if (a == "--snapshot-port") cfg.snapshot_port = std::atoi(val().c_str());
        else if (a == "--md-group") cfg.md_group = val();
        else if (a == "--md-port") cfg.md_port = std::atoi(val().c_str());
        else if (a == "--symbols") cfg.symbols = split_csv(val());
        else if (a == "--flow-rate") cfg.flow_rate = std::atof(val().c_str());
        else if (a == "--drop-every") cfg.drop_every = std::strtoull(val().c_str(), nullptr, 10);
        else if (a == "--duration") cfg.duration_s = std::strtoull(val().c_str(), nullptr, 10);
        else if (a == "--seed") cfg.seed = std::strtoull(val().c_str(), nullptr, 10);
        else if (a == "--cpu") cfg.cpu = std::atoi(val().c_str());
        else if (a == "--quiet") cfg.quiet = true;
        else if (a == "--spin") cfg.spin = true;
        else if (a == "--stp") {
            std::string v = val();
            if (v == "resting") cfg.stp = SelfMatchPolicy::CancelResting;
            else if (v == "incoming") cfg.stp = SelfMatchPolicy::CancelIncoming;
            else { std::fprintf(stderr, "--stp must be resting or incoming\n"); return -1; }
        } else if (a == "--help" || a == "-h") {
            usage(argv[0]);
            std::exit(EXIT_OK);
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", a.c_str());
            usage(argv[0]);
            return -1;
        }
    }
    if (cfg.symbols.empty() || cfg.symbols.size() > 64) {
        std::fprintf(stderr, "--symbols: need 1..64 symbols\n");
        return -1;
    }
    for (const auto &s : cfg.symbols) {
        if (s.size() > 8) {
            std::fprintf(stderr, "symbol %s longer than 8 chars\n", s.c_str());
            return -1;
        }
    }
    if (!is_multicast_address(cfg.md_group)) {
        std::fprintf(stderr, "--md-group %s is not a multicast address\n", cfg.md_group.c_str());
        return -1;
    }
    return 0;
}

uint64_t mid_now() { return ns_since_midnight(now_realtime_ns()); }

void set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    CHECK(fl, "fcntl(F_GETFL)");
    CHECK(fcntl(fd, F_SETFL, fl | O_NONBLOCK), "fcntl(F_SETFL)");
}

Socket make_listener(const std::string &ip, int port) {
    Socket s(AF_INET, SOCK_STREAM, 0);
    CHECK(s.get(), "socket(listen)");
    int one = 1;
    CHECK(setsockopt(s.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)), "SO_REUSEADDR");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        throw SystemError("inet_pton(" + ip + ")", EINVAL);
    }
    CHECK(bind(s.get(), reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)),
          "bind(" + std::to_string(port) + ")");
    CHECK(listen(s.get(), 64), "listen");
    set_nonblocking(s.get());
    return s;
}

/* xorshift64: 3 shifts + 3 xors per number. std::mt19937_64 has 2.5 KB
 * of state and is overkill for synthetic order flow. Not for crypto. */
class Xorshift64 {
public:
    explicit Xorshift64(uint64_t seed) : s_(seed != 0 ? seed : 0x9E3779B97F4A7C15ULL) {}
    uint64_t next() {
        s_ ^= s_ << 13;
        s_ ^= s_ >> 7;
        s_ ^= s_ << 17;
        return s_;
    }
    uint64_t below(uint64_t n) { return next() % n; }
    bool coin() { return (next() & 1) != 0; }

private:
    uint64_t s_;
};

/* ---------------------------------------------------------------------
 * MarketDataFeed - the public ITCH feed: packs messages into MoldUDP64
 * packets and multicasts them. Sequence numbers are assigned here: the
 * k-th message of a packet whose header says sequence S is message S+k.
 * --------------------------------------------------------------------- */
class MarketDataFeed {
public:
    MarketDataFeed(const std::string &group, int port, int ttl, uint64_t drop_every)
        : sock_(AF_INET, SOCK_DGRAM, 0), pkt_(kSessionName), drop_every_(drop_every) {
        CHECK(sock_.get(), "socket(md)");
        multicast_set_ttl(sock_.get(), ttl);
        multicast_set_loop(sock_.get(), true); /* traders on this host must see it */
        dst_.sin_family = AF_INET;
        dst_.sin_port   = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET, group.c_str(), &dst_.sin_addr) != 1) {
            throw SystemError("inet_pton(" + group + ")", EINVAL);
        }
        pkt_.begin(next_seq_);
    }

    void publish(const uint8_t *msg, size_t n) {
        if (!pkt_.append(msg, n)) {
            flush();
            (void)pkt_.append(msg, n);
        }
        ++messages_;
    }

    template <typename M>
    void publish(const M &m) {
        uint8_t buf[itch::kMaxMessageLen];
        size_t n = itch::encode(m, buf, sizeof(buf));
        if (n > 0) {
            publish(buf, n);
        }
    }

    /* Send whatever is batched. Afterwards next_seq() is exactly the
     * sequence the next published message will get - the property the
     * snapshot service relies on. */
    void flush() {
        if (pkt_.count() == 0) {
            return;
        }
        pkt_.finish();
        ++packets_;
        if (drop_every_ > 0 && packets_ % drop_every_ == 0) {
            ++dropped_; /* simulated loss: sequence numbers are consumed, bytes never sent */
        } else {
            send_current();
        }
        next_seq_ += pkt_.count();
        pkt_.begin(next_seq_);
    }

    /* MoldUDP64 heartbeat: count 0, sequence = next expected. Lets a
     * receiver detect a lost final packet during a quiet period. */
    void heartbeat_if_idle(uint64_t now_mono) {
        if (pkt_.count() != 0 || now_mono - last_send_ns_ < kHeartbeatNs) {
            return;
        }
        pkt_.finish();
        send_current();
        ++heartbeats_;
    }

    uint64_t next_seq() const { return next_seq_; }
    uint64_t messages() const { return messages_; }
    uint64_t packets() const { return packets_; }
    uint64_t dropped() const { return dropped_; }
    uint64_t heartbeats() const { return heartbeats_; }

private:
    void send_current() {
        ssize_t n = sendto(sock_.get(), pkt_.data(), pkt_.size(), 0,
                           reinterpret_cast<const sockaddr *>(&dst_), sizeof(dst_));
        if (n != static_cast<ssize_t>(pkt_.size())) {
            ++send_errors_;
        }
        last_send_ns_ = now_ns();
    }

    Socket              sock_;
    sockaddr_in         dst_{};
    mold::PacketBuilder pkt_;
    uint64_t            next_seq_     = 1;
    uint64_t            drop_every_   = 0;
    uint64_t            messages_     = 0;
    uint64_t            packets_      = 0;
    uint64_t            dropped_      = 0;
    uint64_t            heartbeats_   = 0;
    uint64_t            send_errors_  = 0;
    uint64_t            last_send_ns_ = 0;
};

struct OrderHandle {
    uint16_t locate = 0;
    OrderId  id     = 0;
};

/* One TCP connection (an order-entry session or a snapshot request). */
struct Session {
    enum class Kind : uint8_t { OrderEntry, Snapshot };

    Session(uint32_t id_, Kind kind_, Socket sock_)
        : id(id_), kind(kind_), sock(std::move(sock_)),
          orders(kind_ == Kind::OrderEntry ? kMaxSessionOrders : 1) {}

    uint32_t             id;
    Kind                 kind;
    Socket               sock;
    std::string          user;
    soup::FrameReader    reader;
    std::vector<uint8_t> out;          /* bytes queued for send()      */
    size_t               out_off = 0;  /* how much of `out` is sent    */
    bool                 logged_in         = false;
    bool                 close_after_flush = false;
    bool                 dead              = false;
    bool                 want_write        = false; /* EPOLLOUT armed */
    bool                 dirty             = false; /* in dirty list  */
    uint32_t             last_user_ref     = 0;
    uint64_t             out_seq           = 0;     /* sequenced msgs sent */
    uint64_t             last_rx_ns        = 0;
    uint64_t             last_tx_ns        = 0;
    uint64_t             orders_in         = 0;
    FlatIdMap<OrderHandle> orders;     /* user_ref -> resting order    */
};

char reject_code(RejectReason r) {
    switch (r) {
    case RejectReason::BadPrice:      return ouch::kRejectInvalidPrice;
    case RejectReason::BadQty:        return ouch::kRejectInvalidQty;
    case RejectReason::UnknownSymbol: return ouch::kRejectInvalidSymbol;
    default:                          return ouch::kRejectOther;
    }
}

char cancel_code(CancelReason r) {
    switch (r) {
    case CancelReason::UserRequested:     return ouch::kCancelUserRequested;
    case CancelReason::ImmediateOrCancel: return ouch::kCancelImmediate;
    case CancelReason::SelfMatch:         return ouch::kCancelSelfMatch;
    case CancelReason::Supervisory:       return ouch::kCancelSupervisory;
    }
    return ouch::kCancelSupervisory;
}

/* ---------------------------------------------------------------------
 * Exchange - owns everything and IS the engine's listener.
 * `final`: nothing derives from it, so calls through an Exchange& can
 * be devirtualised by the compiler.
 * --------------------------------------------------------------------- */
class Exchange final : public EngineListener {
public:
    explicit Exchange(const ExchangeConfig &cfg)
        : cfg_(cfg),
          engine_(*this, cfg.stp),
          md_(cfg.md_group, cfg.md_port, cfg.md_ttl, cfg.drop_every),
          order_listen_(make_listener(cfg.bind_ip, cfg.order_port)),
          snap_listen_(make_listener(cfg.bind_ip, cfg.snapshot_port)),
          timer_(kTimerPeriodNs),
          signals_({SIGINT, SIGTERM}),
          rng_(cfg.seed),
          gateway_latency_(1u << 18) {
        BookConfig bc; /* $50.00 - $150.00, 1 cent ticks, 64k orders */
        for (const auto &s : cfg.symbols) {
            engine_.add_symbol(make_symbol(s), bc);
            Synth st;
            st.fair = 100 * kPriceScale;
            synth_.push_back(st);
        }
    }

    int run();

    /* ---- EngineListener: turn engine events into ITCH + OUCH ---------- */

    void on_accepted(uint16_t locate, const OrderRequest &req) override {
        Session *s = session_for(req.owner);
        if (s == nullptr) {
            return;
        }
        ouch::OrderAccepted m{};
        m.timestamp = mid_now();
        m.user_ref  = req.user_ref;
        m.side      = req.side;
        m.qty       = req.qty;
        m.symbol    = engine_.symbol(locate);
        m.price     = req.price;
        m.tif       = req.tif;
        m.order_ref = req.id; /* same id the public feed will use */
        m.clordid   = pending_clordid_;
        m.state     = 'L';
        queue_ouch(*s, m);
    }

    void on_rejected(uint16_t locate, const OrderRequest &req, RejectReason why) override {
        (void)locate;
        ++rejects_;
        if (Session *s = session_for(req.owner)) {
            send_reject(*s, req.user_ref, reject_code(why));
        }
    }

    void on_fill(uint16_t locate, const Fill &f) override {
        ++trades_;
        shares_traded_ += f.qty;
        const uint64_t ts = mid_now();

        /* public: only the RESTING order appears in ITCH ('E' references
         * the maker's order ref). The taker is anonymous unless it rests. */
        itch::OrderExecuted e{};
        e.locate          = locate;
        e.timestamp       = ts;
        e.order_ref       = f.maker_id;
        e.executed_shares = f.qty;
        e.match_number    = f.match_number;
        md_.publish(e);

        /* private: both sides get an execution report */
        if (Session *s = session_for(f.maker_owner)) {
            send_exec(*s, ts, f.maker_user_ref, f, ouch::kAddedLiquidity);
            if (f.maker_remaining == 0) {
                s->orders.erase(f.maker_user_ref);
            }
        }
        if (Session *s = session_for(f.taker_owner)) {
            send_exec(*s, ts, f.taker_user_ref, f, ouch::kRemovedLiquidity);
        }
    }

    void on_rested(uint16_t locate, const BookOrder &o) override {
        itch::AddOrder a{};
        a.locate    = locate;
        a.timestamp = mid_now();
        a.order_ref = o.id;
        a.side      = o.side;
        a.shares    = o.qty;
        a.stock     = engine_.symbol(locate);
        a.price     = static_cast<uint32_t>(o.price);
        md_.publish(a);
    }

    void on_resting_cancelled(uint16_t locate, const BookOrder &before, Qty cancelled,
                              CancelReason why) override {
        ++cancels_;
        const uint64_t ts = mid_now();
        const bool gone = cancelled == before.qty;
        if (gone) {
            itch::OrderDelete d{};
            d.locate    = locate;
            d.timestamp = ts;
            d.order_ref = before.id;
            md_.publish(d);
        } else {
            itch::OrderCancel x{};
            x.locate           = locate;
            x.timestamp        = ts;
            x.order_ref        = before.id;
            x.cancelled_shares = cancelled;
            md_.publish(x);
        }
        if (Session *s = session_for(before.owner)) {
            send_canceled(*s, ts, before.user_ref, cancelled, cancel_code(why));
            if (gone) {
                s->orders.erase(before.user_ref);
            }
        }
    }

    void on_taker_cancelled(uint16_t locate, const OrderRequest &req, Qty cancelled,
                            CancelReason why) override {
        (void)locate;
        if (Session *s = session_for(req.owner)) {
            send_canceled(*s, mid_now(), req.user_ref, cancelled, cancel_code(why));
        }
    }

private:
    /* epoll tags: small integers, never pointers (see event_loop.hpp) */
    static constexpr uint64_t kTagOrderListen    = 1;
    static constexpr uint64_t kTagSnapshotListen = 2;
    static constexpr uint64_t kTagTimer          = 3;
    static constexpr uint64_t kTagSignal         = 4;
    static constexpr uint64_t kTagSessionBase    = 1000;

    struct Synth {
        Price                fair  = 0;
        double               carry = 0.0; /* fractional events owed */
        std::vector<OrderId> live;        /* synthetic resting ids (lazily pruned) */
    };

    Session *session_for(uint32_t owner) {
        if (owner == 0) {
            return nullptr; /* synthetic flow has no session */
        }
        auto it = sessions_.find(owner);
        return it == sessions_.end() ? nullptr : it->second.get();
    }

    /* ---- output queueing ---- */

    void mark_dirty(Session &s) {
        if (!s.dirty) {
            s.dirty = true;
            dirty_.push_back(s.id);
        }
    }

    void queue_frame(Session &s, char type, const uint8_t *p, size_t n) {
        soup::append_frame(s.out, type, p, n);
        mark_dirty(s);
    }

    template <typename M>
    void queue_ouch(Session &s, const M &m) {
        uint8_t buf[ouch::kMaxMessageLen];
        size_t n = ouch::encode(m, buf, sizeof(buf));
        if (n > 0) {
            queue_frame(s, soup::kSequencedData, buf, n);
            ++s.out_seq;
        }
    }

    void send_reject(Session &s, uint32_t user_ref, char reason) {
        ouch::OrderRejected m{};
        m.timestamp = mid_now();
        m.user_ref  = user_ref;
        m.reason    = reason;
        m.clordid   = pending_clordid_;
        queue_ouch(s, m);
    }

    void send_exec(Session &s, uint64_t ts, uint32_t user_ref, const Fill &f, char liq) {
        ouch::OrderExecuted m{};
        m.timestamp    = ts;
        m.user_ref     = user_ref;
        m.qty          = f.qty;
        m.price        = f.price;
        m.liquidity    = liq;
        m.match_number = f.match_number;
        queue_ouch(s, m);
    }

    void send_canceled(Session &s, uint64_t ts, uint32_t user_ref, Qty qty, char reason) {
        ouch::OrderCanceled m{};
        m.timestamp = ts;
        m.user_ref  = user_ref;
        m.qty       = qty;
        m.reason    = reason;
        queue_ouch(s, m);
    }

    /* Non-blocking write of the session's queue. On EAGAIN (the kernel's
     * socket send buffer is full - a slow client) arm EPOLLOUT and finish
     * later; the event loop never blocks on one slow peer. */
    void flush_session(Session &s) {
        while (s.out_off < s.out.size()) {
            ssize_t n = send(s.sock.get(), s.out.data() + s.out_off, s.out.size() - s.out_off,
                             MSG_NOSIGNAL | MSG_DONTWAIT);
            if (n > 0) {
                s.out_off += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!s.want_write) {
                    ep_.modify(s.sock.get(), EPOLLIN | EPOLLOUT, kTagSessionBase + s.id);
                    s.want_write = true;
                }
                return;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            s.dead = true;
            return;
        }
        s.out.clear();
        s.out_off    = 0;
        s.last_tx_ns = now_ns();
        if (s.want_write) {
            ep_.modify(s.sock.get(), EPOLLIN, kTagSessionBase + s.id);
            s.want_write = false;
        }
        if (s.close_after_flush) {
            s.dead = true;
        }
    }

    void flush_dirty_sessions() {
        for (uint32_t id : dirty_) {
            auto it = sessions_.find(id);
            if (it == sessions_.end()) {
                continue; /* closed since it was queued */
            }
            it->second->dirty = false;
            flush_session(*it->second);
        }
        dirty_.clear();
    }

    void reap_dead_sessions() {
        std::vector<uint32_t> dead;
        for (auto &kv : sessions_) {
            if (kv.second->dead) {
                dead.push_back(kv.first);
            }
        }
        for (uint32_t id : dead) {
            close_session(id);
        }
    }

    void close_session(uint32_t id) {
        auto it = sessions_.find(id);
        if (it == sessions_.end()) {
            return;
        }
        Session &s = *it->second;
        /* Cancel-on-disconnect: a firm whose connection drops must not be
         * left with live orders it can no longer manage. Collect first,
         * then cancel: the cancel callbacks erase from s.orders, and
         * modifying a container while iterating it is a classic bug. */
        std::vector<OrderHandle> live;
        s.orders.for_each([&live](uint64_t, const OrderHandle &h) { live.push_back(h); });
        for (const OrderHandle &h : live) {
            engine_.cancel(h.locate, h.id, 0, s.id, CancelReason::Supervisory);
        }
        if (!cfg_.quiet || s.kind == Session::Kind::OrderEntry) {
            std::printf("[exchange] session %u (%s%s) closed, %zu resting orders cancelled\n",
                        s.id, s.kind == Session::Kind::Snapshot ? "snapshot" : "order entry",
                        s.user.empty() ? "" : (" " + s.user).c_str(), live.size());
        }
        ep_.remove(s.sock.get());
        sessions_.erase(it); /* unique_ptr frees it; Socket closes the fd */
    }

    /* ---- input ---- */

    void accept_all(int listen_fd, Session::Kind kind) {
        for (;;) {
            int fd = accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) {
                return; /* EAGAIN: accepted everything pending */
            }
            Socket sock(fd);
            int one = 1;
            /* TCP_NODELAY: disable Nagle's algorithm, which holds small
             * writes back (up to ~40 ms with delayed ACKs) hoping to
             * coalesce them. For order entry every message is small and
             * urgent - Nagle is pure latency. */
            (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            const uint32_t id = next_session_id_++;
            auto s = std::make_unique<Session>(id, kind, std::move(sock));
            s->last_rx_ns = now_ns();
            ep_.add(fd, EPOLLIN, kTagSessionBase + id);
            sessions_.emplace(id, std::move(s));
        }
    }

    void on_session_event(uint32_t id, uint32_t events) {
        auto it = sessions_.find(id);
        if (it == sessions_.end()) {
            return; /* closed earlier in this same epoll batch */
        }
        Session &s = *it->second;
        if (events & EPOLLIN) {
            read_session(s);
        }
        if ((events & EPOLLOUT) && !s.dead) {
            flush_session(s);
        }
        if (events & (EPOLLERR | EPOLLHUP)) {
            s.dead = true;
        }
    }

    void read_session(Session &s) {
        for (;;) {
            uint8_t *dst = s.reader.write_ptr();
            size_t room  = s.reader.write_space();
            if (room == 0) {
                s.dead = true;
                return;
            }
            ssize_t n = recv(s.sock.get(), dst, room, 0);
            if (n > 0) {
                s.reader.commit(static_cast<size_t>(n));
                s.last_rx_ns = now_ns();
                char type;
                const uint8_t *payload;
                size_t len;
                soup::FrameReader::Status st;
                while ((st = s.reader.next(type, payload, len)) == soup::FrameReader::Status::Frame) {
                    handle_frame(s, type, payload, len);
                    if (s.dead) {
                        return;
                    }
                }
                if (st == soup::FrameReader::Status::Error) {
                    std::printf("[exchange] session %u: framing error, disconnecting\n", s.id);
                    s.dead = true;
                    return;
                }
                continue;
            }
            if (n == 0) {
                s.dead = true; /* orderly shutdown by the peer */
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                s.dead = true;
            }
            return;
        }
    }

    void handle_frame(Session &s, char type, const uint8_t *p, size_t n) {
        if (s.kind == Session::Kind::Snapshot) {
            if (type == soup::kLoginRequest) {
                send_snapshot(s);
            }
            return;
        }
        switch (type) {
        case soup::kLoginRequest:
            handle_login(s, p, n);
            break;
        case soup::kUnsequencedData: {
            if (!s.logged_in) {
                s.dead = true; /* data before login: protocol violation */
                return;
            }
            auto msg = ouch::decode_inbound(p, n);
            if (!msg) {
                ++bad_messages_;
                return;
            }
            std::visit(overloaded{
                           [&](const ouch::EnterOrder &m) { handle_enter(s, m); },
                           [&](const ouch::CancelOrder &m) { handle_cancel(s, m); },
                       },
                       *msg);
            break;
        }
        case soup::kClientHeartbeat:
            break; /* last_rx_ns already refreshed */
        case soup::kLogoutRequest:
            queue_frame(s, soup::kEndOfSession, nullptr, 0);
            s.close_after_flush = true;
            break;
        default:
            ++bad_messages_;
            break;
        }
    }

    void handle_login(Session &s, const uint8_t *p, size_t n) {
        soup::LoginRequest req;
        if (s.logged_in || !soup::decode_login_request(p, n, req) || req.username.empty()) {
            const uint8_t reason = 'A'; /* not authorised */
            queue_frame(s, soup::kLoginRejected, &reason, 1);
            s.close_after_flush = true;
            return;
        }
        s.logged_in = true;
        s.user      = req.username;
        soup::LoginAccepted acc;
        acc.session  = kSessionName;
        acc.sequence = s.out_seq + 1;
        uint8_t buf[soup::kLoginAcceptedLen];
        size_t len = soup::encode_login_accepted(acc, buf, sizeof(buf));
        queue_frame(s, soup::kLoginAccepted, buf, len);
        std::printf("[exchange] session %u: login accepted for user '%s'\n", s.id, s.user.c_str());
    }

    void handle_enter(Session &s, const ouch::EnterOrder &m) {
        const uint64_t t0 = now_ns();
        ++s.orders_in;
        ++client_orders_;
        pending_clordid_ = m.clordid; /* engine callbacks below are synchronous */

        /* OUCH 5.0 rule: UserRefNum strictly increasing per session. An
         * O(1) duplicate/replay check, no lookup needed. */
        if (m.user_ref <= s.last_user_ref) {
            ++rejects_;
            send_reject(s, m.user_ref, ouch::kRejectInvalidUserRef);
            return;
        }
        s.last_user_ref = m.user_ref;

        const uint16_t locate = engine_.find_symbol(m.symbol);
        if (locate == 0) {
            ++rejects_;
            send_reject(s, m.user_ref, ouch::kRejectInvalidSymbol);
            return;
        }
        OrderRequest req;
        req.id       = next_order_id_++;
        req.side     = m.side;
        req.price    = m.price;
        req.qty      = m.qty;
        req.tif      = m.tif;
        req.owner    = s.id;
        req.user_ref = m.user_ref;

        OrderHandle h;
        h.locate = locate;
        h.id     = req.id;
        if (!s.orders.insert(m.user_ref, h)) {
            ++rejects_;
            send_reject(s, m.user_ref, ouch::kRejectOther); /* session order table full */
            return;
        }
        engine_.submit(locate, req);
        if (engine_.book(locate).find(req.id) == nullptr) {
            s.orders.erase(m.user_ref); /* fully traded / IOC / rejected: nothing rests */
        }
        gateway_latency_.record(now_ns() - t0);
    }

    void handle_cancel(Session &s, const ouch::CancelOrder &m) {
        const OrderHandle *h = s.orders.find(m.user_ref);
        if (h == nullptr) {
            ++late_cancels_; /* already filled or cancelled: "too late to cancel" */
            return;
        }
        const OrderHandle copy = *h; /* the engine callback may erase *h */
        engine_.cancel(copy.locate, copy.id, m.qty, s.id, CancelReason::UserRequested);
    }

    /* GLIMPSE-style snapshot: every symbol's directory entry, then every
     * resting order in priority order, then "resume incremental at N". */
    void send_snapshot(Session &s) {
        md_.flush(); /* make the sequence boundary exact: everything < N is in the books */
        const uint64_t next = md_.next_seq();
        const uint64_t ts = mid_now();
        uint8_t buf[itch::kMaxMessageLen];
        size_t orders = 0;
        for (uint16_t loc = 1; loc <= engine_.symbol_count(); loc++) {
            itch::StockDirectory sd{};
            sd.locate         = loc;
            sd.timestamp      = ts;
            sd.stock          = engine_.symbol(loc);
            sd.round_lot_size = 100;
            queue_frame(s, soup::kSequencedData, buf, itch::encode(sd, buf, sizeof(buf)));
        }
        for (uint16_t loc = 1; loc <= engine_.symbol_count(); loc++) {
            engine_.book(loc).for_each_order([&](const BookOrder &o) {
                itch::AddOrder a{};
                a.locate    = loc;
                a.timestamp = ts;
                a.order_ref = o.id;
                a.side      = o.side;
                a.shares    = o.qty;
                a.stock     = engine_.symbol(loc);
                a.price     = static_cast<uint32_t>(o.price);
                queue_frame(s, soup::kSequencedData, buf, itch::encode(a, buf, sizeof(buf)));
                ++orders;
            });
        }
        itch::EndOfSnapshot eos{next};
        queue_frame(s, soup::kSequencedData, buf, itch::encode(eos, buf, sizeof(buf)));
        queue_frame(s, soup::kEndOfSession, nullptr, 0);
        s.close_after_flush = true;
        ++snapshots_;
        std::printf("[exchange] snapshot served: %zu orders, resume at seq %" PRIu64 "\n",
                    orders, next);
    }

    /* ---- synthetic order flow ---- */

    void synth_step(uint16_t locate) {
        Synth &st = synth_[locate - 1];
        const OrderBook &book = engine_.book(locate);
        const BookConfig &bc  = book.config();
        const Price tick      = bc.tick_size;

        /* fair value: a slow random walk, kept away from the band edges */
        if (rng_.below(100) < 6) {
            st.fair += rng_.coin() ? tick : -tick;
            if (st.fair < bc.min_price + 50 * tick) st.fair = bc.min_price + 50 * tick;
            if (st.fair > bc.max_price - 50 * tick) st.fair = bc.max_price - 50 * tick;
        }

        OrderRequest req;
        req.owner = 0;
        const uint64_t u = rng_.below(100);
        if (st.live.size() < 20 || (u < 55 && st.live.size() < kMaxSyntheticLive)) {
            /* passive: rest 1-8 ticks away from fair */
            req.side  = rng_.coin() ? Side::Buy : Side::Sell;
            const Price k = static_cast<Price>(1 + rng_.below(8));
            req.price = req.side == Side::Buy ? st.fair - k * tick : st.fair + k * tick;
            req.qty   = static_cast<Qty>(100 * (1 + rng_.below(5)));
            req.tif   = TimeInForce::Day;
            req.id    = next_order_id_++;
            engine_.submit(locate, req);
            if (book.find(req.id) != nullptr) {
                st.live.push_back(req.id);
            }
        } else if (u < 88 && !st.live.empty()) {
            /* cancel (sometimes only partially) a random synthetic order */
            const size_t idx = static_cast<size_t>(rng_.below(st.live.size()));
            const OrderId id = st.live[idx];
            const BookOrder *o = book.find(id);
            if (o != nullptr && o->qty >= 200 && rng_.below(3) == 0) {
                engine_.cancel(locate, id, (o->qty / 200) * 100, 0); /* halve it: ITCH 'X' */
            } else {
                st.live[idx] = st.live.back(); /* swap-and-pop: O(1) unordered erase */
                st.live.pop_back();
                if (o != nullptr) {
                    engine_.cancel(locate, id, 0, 0); /* ITCH 'D' */
                }
            }
        } else {
            /* aggressive IOC through fair: produces trades */
            req.side  = rng_.coin() ? Side::Buy : Side::Sell;
            req.price = req.side == Side::Buy ? st.fair + 3 * tick : st.fair - 3 * tick;
            req.qty   = static_cast<Qty>(100 * (1 + rng_.below(3)));
            req.tif   = TimeInForce::IOC;
            req.id    = next_order_id_++;
            engine_.submit(locate, req);
        }
    }

    void on_timer() {
        const uint64_t ticks = timer_.drain();
        if (ticks == 0) {
            return;
        }
        const uint64_t now = now_ns();
        if (cfg_.flow_rate > 0) {
            const double per_tick = cfg_.flow_rate * static_cast<double>(kTimerPeriodNs) / 1e9;
            for (uint16_t loc = 1; loc <= engine_.symbol_count(); loc++) {
                Synth &st = synth_[loc - 1];
                st.carry += per_tick * static_cast<double>(ticks > 10 ? 10 : ticks);
                while (st.carry >= 1.0) {
                    st.carry -= 1.0;
                    synth_step(loc);
                }
            }
        }
        md_.heartbeat_if_idle(now);
        for (auto &kv : sessions_) {
            Session &s = *kv.second;
            if (s.kind != Session::Kind::OrderEntry || !s.logged_in) {
                continue;
            }
            if (now - s.last_rx_ns > kSessionTimeoutNs) {
                std::printf("[exchange] session %u: no heartbeat for 15 s, disconnecting\n", s.id);
                s.dead = true;
            } else if (now - s.last_tx_ns >= kHeartbeatNs && s.out.empty()) {
                queue_frame(s, soup::kServerHeartbeat, nullptr, 0);
            }
        }
        if (!cfg_.quiet && now - last_stats_ns_ >= kStatsEveryNs) {
            last_stats_ns_ = now;
            print_stats(now);
        }
        if (deadline_ns_ != 0 && now >= deadline_ns_) {
            stop_ = true;
        }
    }

    void print_stats(uint64_t now) {
        std::printf("[exchange] t=%3" PRIu64 "s  orders %" PRIu64 " (clients %" PRIu64
                    ")  trades %" PRIu64 "  md msgs %" PRIu64 " pkts %" PRIu64
                    " dropped %" PRIu64 "  sessions %zu\n",
                    static_cast<uint64_t>((now - start_ns_) / 1000000000ULL), next_order_id_ - 1,
                    client_orders_,
                    trades_, md_.messages(), md_.packets(), md_.dropped(), sessions_.size());
        for (uint16_t loc = 1; loc <= engine_.symbol_count(); loc++) {
            const OrderBook &b = engine_.book(loc);
            std::string bid = b.empty(Side::Buy) ? "-" : format_price(b.best_price(Side::Buy));
            std::string ask = b.empty(Side::Sell) ? "-" : format_price(b.best_price(Side::Sell));
            std::printf("           %-8s bid %s x %-6" PRIu64 " ask %s x %-6" PRIu64
                        " resting %zu\n",
                        symbol_str(engine_.symbol(loc)).c_str(), bid.c_str(),
                        b.best_qty(Side::Buy), ask.c_str(), b.best_qty(Side::Sell),
                        b.order_count());
        }
        std::fflush(stdout);
    }

    ExchangeConfig                                         cfg_;
    MatchingEngine                                         engine_;
    MarketDataFeed                                         md_;
    Epoll                                                  ep_;
    Socket                                                 order_listen_;
    Socket                                                 snap_listen_;
    TimerFd                                                timer_;
    SignalFd                                               signals_;
    Xorshift64                                             rng_;
    std::unordered_map<uint32_t, std::unique_ptr<Session>> sessions_;
    std::vector<uint32_t>                                  dirty_;
    std::vector<Synth>                                     synth_;
    ouch::ClOrdId                                          pending_clordid_{};
    uint32_t                                               next_session_id_ = 1;
    OrderId                                                next_order_id_   = 1;
    LatencySampler                                         gateway_latency_;
    uint64_t client_orders_ = 0, trades_ = 0, shares_traded_ = 0, cancels_ = 0, rejects_ = 0;
    uint64_t late_cancels_ = 0, bad_messages_ = 0, snapshots_ = 0;
    uint64_t start_ns_ = 0, last_stats_ns_ = 0, deadline_ns_ = 0;
    bool     stop_ = false;
};

int Exchange::run() {
    if (cfg_.cpu >= 0) {
        if (pin_to_cpu(cfg_.cpu) != 0) {
            std::perror("pin_to_cpu");
        }
    }
    std::printf("PulseForge exchange (PFX)\n");
    std::printf("  order entry  tcp %s:%d   (SoupBinTCP + OUCH-style)\n", cfg_.bind_ip.c_str(),
                cfg_.order_port);
    std::printf("  snapshot     tcp %s:%d   (GLIMPSE-style)\n", cfg_.bind_ip.c_str(),
                cfg_.snapshot_port);
    std::printf("  market data  udp %s:%d (ITCH-style over MoldUDP64)\n", cfg_.md_group.c_str(),
                cfg_.md_port);
    std::printf("  symbols     ");
    for (uint16_t loc = 1; loc <= engine_.symbol_count(); loc++) {
        std::printf(" %s(locate %u)", symbol_str(engine_.symbol(loc)).c_str(), loc);
    }
    std::printf("\n  synthetic flow %.0f events/s/symbol  drop-every %" PRIu64 "  loop %s\n",
                cfg_.flow_rate, cfg_.drop_every, cfg_.spin ? "busy-poll" : "epoll blocking");
    std::fflush(stdout);

    start_ns_ = last_stats_ns_ = now_ns();
    deadline_ns_ = cfg_.duration_s ? start_ns_ + cfg_.duration_s * 1000000000ULL : 0;

    /* Start of day on the public feed. */
    const uint64_t ts = mid_now();
    md_.publish(itch::SystemEvent{0, ts, itch::kStartOfMessages});
    for (uint16_t loc = 1; loc <= engine_.symbol_count(); loc++) {
        md_.publish(itch::StockDirectory{loc, ts, engine_.symbol(loc), 100});
    }
    md_.publish(itch::SystemEvent{0, ts, itch::kStartOfMarketHours});
    md_.flush();

    ep_.add(order_listen_.get(), EPOLLIN, kTagOrderListen);
    ep_.add(snap_listen_.get(), EPOLLIN, kTagSnapshotListen);
    ep_.add(timer_.get(), EPOLLIN, kTagTimer);
    ep_.add(signals_.get(), EPOLLIN, kTagSignal);

    epoll_event events[64];
    const int timeout = cfg_.spin ? 0 : -1;
    while (!stop_) {
        const int n = ep_.wait(events, 64, timeout);
        for (int i = 0; i < n; i++) {
            const uint64_t tag = events[i].data.u64;
            if (tag == kTagOrderListen) {
                accept_all(order_listen_.get(), Session::Kind::OrderEntry);
            } else if (tag == kTagSnapshotListen) {
                accept_all(snap_listen_.get(), Session::Kind::Snapshot);
            } else if (tag == kTagTimer) {
                on_timer();
            } else if (tag == kTagSignal) {
                if (signals_.read_signal() != 0) {
                    stop_ = true;
                }
            } else if (tag >= kTagSessionBase) {
                on_session_event(static_cast<uint32_t>(tag - kTagSessionBase), events[i].events);
            }
        }
        reap_dead_sessions();
        flush_dirty_sessions(); /* private reports first ...      */
        md_.flush();            /* ... then the public feed        */
    }

    /* End of day: tell the feed, tell every session. */
    const uint64_t te = mid_now();
    md_.publish(itch::SystemEvent{0, te, itch::kEndOfMarketHours});
    md_.publish(itch::SystemEvent{0, te, itch::kEndOfMessages});
    md_.flush();
    for (auto &kv : sessions_) {
        queue_frame(*kv.second, soup::kEndOfSession, nullptr, 0);
    }
    flush_dirty_sessions();

    const double secs = static_cast<double>(now_ns() - start_ns_) / 1e9;
    std::printf("\n==== PulseForge exchange summary (%.1f s) ====\n", secs);
    std::printf("orders      %" PRIu64 " total (%" PRIu64 " from clients), rejects %" PRIu64
                ", late cancels %" PRIu64 "\n",
                next_order_id_ - 1, client_orders_, rejects_, late_cancels_);
    std::printf("trades      %" PRIu64 " (%" PRIu64 " shares), cancels %" PRIu64 "\n", trades_,
                shares_traded_, cancels_);
    std::printf("market data %" PRIu64 " messages in %" PRIu64 " packets (%.1f msgs/pkt), %" PRIu64
                " dropped (simulated), %" PRIu64 " heartbeats\n",
                md_.messages(), md_.packets(),
                md_.packets() ? static_cast<double>(md_.messages()) / static_cast<double>(md_.packets()) : 0.0,
                md_.dropped(), md_.heartbeats());
    std::printf("snapshots   %" PRIu64 " served\n", snapshots_);
    LatencySampler::Summary g = gateway_latency_.summarize();
    if (g.count > 0) {
        std::printf("gateway     client order decode -> engine done -> reports queued: "
                    "p50 %" PRIu64 " ns  p99 %" PRIu64 " ns  max %" PRIu64 " ns (%zu orders)\n",
                    g.p50, g.p99, g.max, g.count);
    }
    return EXIT_OK;
}

} // namespace

int main(int argc, char **argv) {
    try {
        ExchangeConfig cfg;
        if (parse_args(argc, argv, cfg) != 0) {
            return EXIT_USAGE;
        }
        Exchange ex(cfg);
        return ex.run();
    } catch (const SystemError &e) {
        std::fprintf(stderr, "exchange: %s failed: %s\n", e.what(), std::strerror(e.code()));
        return EXIT_SYSFAIL;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "exchange: %s\n", e.what());
        return EXIT_CFG;
    }
}
