/*
 * snapshot_server.hpp - TCP gap-recovery server embedded in the
 * publisher (see snapshot_protocol.hpp for the "why TCP here" story).
 *
 * Ownership: owns its listen socket and background accept thread.
 * start() spawns the thread; the destructor (via stop()) signals it to
 * exit and joins it - no dangling thread on process exit.
 *
 * Thread-safety: record() is called from the publisher's main send
 * loop; the accept thread calls serve_request() per client. Both go
 * through one std::mutex guarding the ring - this is NOT a hot path
 * (one mutex op per generated message is fine; it is not per *sent*
 * UDP datagram pacing-wise, since this runs once per sequence number
 * regardless of transport, and gap recovery itself is rare by
 * definition), so a plain mutex is the right tool, unlike the queues
 * this project otherwise goes lock-free for.
 *
 * Limits, stated plainly: single-threaded accept loop, one client
 * served at a time, synchronously. Fine for this lab's "occasional gap
 * repair" traffic pattern; not a production design for many concurrent
 * recovering subscribers.
 */

#ifndef PULSEFORGE_SNAPSHOT_SERVER_HPP
#define PULSEFORGE_SNAPSHOT_SERVER_HPP

#include "message.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class Socket; /* common.hpp */

class SnapshotServer {
public:
    SnapshotServer();
    ~SnapshotServer();

    SnapshotServer(const SnapshotServer &)            = delete;
    SnapshotServer &operator=(const SnapshotServer &) = delete;

    /* Binds, listens, and spawns the accept thread. Throws SystemError
     * on failure. */
    void start(int port);

    /* Signals the accept thread to exit and joins it. Safe to call
     * more than once (including implicitly via the destructor). */
    void stop();

    /* Publisher-side: record a just-generated message (called for
     * EVERY sequence number, even ones --drop-every will skip sending
     * over UDP - the ring models the authoritative source, not the
     * lossy wire). */
    void record(const TickMessage &msg);

private:
    void accept_loop(int port);
    void handle_client(int client_fd);
    std::vector<TickMessage> serve_request(uint64_t from_seq, uint64_t count);

    std::mutex              ring_mu_;
    std::vector<TickMessage> ring_;
    std::vector<uint64_t>    slot_seq_; /* seq currently in each slot, or UINT64_MAX if empty */

    std::atomic<bool>        stop_{false};
    std::thread              thread_;
    std::unique_ptr<Socket>  listen_sock_;
};

#endif /* PULSEFORGE_SNAPSHOT_SERVER_HPP */
