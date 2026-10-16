/*
 * snapshot_protocol.hpp - wire format for the TCP gap-recovery channel.
 *
 * Responsibility: this is the project's answer to "when do you use TCP
 * instead of UDP" - the live tick feed stays UDP (fire-and-forget,
 * minimum latency, loss is tolerable and detected downstream), but
 * repairing a detected gap needs exactly what TCP gives you for free:
 * a reliable, ordered, connection-oriented request/response. Re-sending
 * the repair over UDP would just reintroduce the same loss risk for the
 * thing meant to fix loss.
 *
 * Shape: the publisher ALSO runs a small TCP server (see
 * publisher.cpp's --snapshot-port) that remembers the last
 * kSnapshotRingCapacity generated messages - "generated", not "sent":
 * the ring is fed before the --drop-every check, so it models an
 * upstream sequencer/exchange that has every message; only the UDP
 * *wire* between publisher and receiver loses packets, which is also
 * how a real market-data gateway relates to a real exchange feed. The
 * receiver, on detecting a sequence gap, connects, asks for the missing
 * range, and replays whatever comes back into its own queue - counted
 * separately in SharedStats::recovered, never folded into `received`.
 *
 * Wire format (raw structs, same simplicity/caveats as message.hpp -
 * one machine, one architecture, no byte-order handling):
 *   client -> server: one SnapshotRequest
 *   server -> client: one SnapshotResponseHeader, then
 *                      header.returned_count TickMessage structs
 * A `returned_count` of 0 means "none of that range is in the ring
 * anymore" (evicted) or "that range includes sequences never
 * generated" - the receiver just leaves those as unrecoverable gaps.
 */

#ifndef PULSEFORGE_SNAPSHOT_PROTOCOL_HPP
#define PULSEFORGE_SNAPSHOT_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>

constexpr int    kDefaultSnapshotPort     = 9001;
constexpr size_t kSnapshotRingCapacity    = 8192;
constexpr uint64_t kMaxSnapshotRequestLen = 4096; /* cap one request's size */

struct SnapshotRequest {
    uint64_t from_seq; /* first sequence number wanted      */
    uint64_t count;     /* how many sequential messages, capped at kMaxSnapshotRequestLen */
};

struct SnapshotResponseHeader {
    uint64_t returned_count; /* number of TickMessage structs that follow */
};

#endif /* PULSEFORGE_SNAPSHOT_PROTOCOL_HPP */
