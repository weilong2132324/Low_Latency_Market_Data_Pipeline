/*
 * snapshot_client.hpp - receiver-side client for the TCP gap-recovery
 * channel (see snapshot_protocol.hpp).
 *
 * request_snapshot_recovery() is meant to be run via std::async from
 * the receive loop when a sequence gap is detected - it blocks on a
 * real TCP connect/send/recv, which is exactly why it must NOT run on
 * the hot receive-loop thread directly. receiver.cpp dispatches it as
 * `std::async(std::launch::async, request_snapshot_recovery, ...)` and
 * polls the returned std::future with a non-blocking wait_for(0) from
 * the main loop, so recovered messages are still only ever pushed into
 * the queue backend by the single producer thread (preserving the
 * SPSC/Disruptor single-writer invariant documented in
 * queue_backend.hpp) - the async task's only job is the slow network
 * round trip, not touching the queue itself.
 *
 * Never throws: any failure (connect refused, partial read, etc.)
 * is reported by returning an empty vector - a best-effort repair,
 * not a guaranteed one, same honesty as every other "racy by design"
 * comment in this codebase.
 */

#ifndef PULSEFORGE_SNAPSHOT_CLIENT_HPP
#define PULSEFORGE_SNAPSHOT_CLIENT_HPP

#include "message.hpp"

#include <cstdint>
#include <string>
#include <vector>

std::vector<TickMessage> request_snapshot_recovery(std::string host, int port,
                                                    uint64_t from_seq,
                                                    uint64_t count);

#endif /* PULSEFORGE_SNAPSHOT_CLIENT_HPP */
