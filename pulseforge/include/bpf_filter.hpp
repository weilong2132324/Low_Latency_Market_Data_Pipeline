/*
 * bpf_filter.hpp - classic (cBPF) socket filter: drop invalid ticks in
 * the kernel, before recvfrom()/recvmmsg() ever copies them to us.
 *
 * Responsibility: this is the "kernel bypass" concept this project can
 * actually run under WSL2 without a dedicated NIC - no libbpf, no
 * clang, no XDP, just the same classic BPF machinery tcpdump/iptables
 * have used for decades (SO_ATTACH_FILTER, in Linux since 2.1).
 *
 * How it sees the packet: for a bound/connected UDP socket, a socket
 * filter installed with SO_ATTACH_FILTER runs against the data the
 * socket already demultiplexed to - i.e. byte 0 of "the packet" here IS
 * byte 0 of the TickMessage payload (IP/UDP headers are gone by this
 * point). The filter below reads the 4-byte `quantity` field at offset
 * 32 (see message.hpp's layout comment) and tells the kernel to drop
 * (truncate to 0 bytes, which recvfrom reports as a 0-length read we
 * then also discard) any datagram where quantity == 0 - the same
 * "invalid" check receiver.cpp already does in message_is_valid(), just
 * moved from userspace into the kernel's socket receive path.
 *
 * Program (classic BPF bytecode, by hand - no bpf_asm/tcpdump needed):
 *   ld   [32]         ; A = *(u32 *)(payload + 32)   (quantity)
 *   jeq  #0, drop, ok
 *   drop: ret #0       ; truncate packet to 0 bytes -> treated as empty
 *   ok:   ret #0xffff  ; accept up to 65535 bytes (i.e. the whole thing)
 *
 * Caveat, stated plainly: this filter only saves the copy-into-userspace
 * cost for already-invalid packets; it does not reduce latency for
 * valid ones, and on this project's loopback-only test traffic the
 * saving is unmeasurably small. The point is the mechanism, not a
 * benchmark win - see docs/kernel_bypass_next_steps.md for what a real
 * eBPF/XDP/DPDK setup buys you on real hardware instead.
 */

#ifndef PULSEFORGE_BPF_FILTER_HPP
#define PULSEFORGE_BPF_FILTER_HPP

#include <cstdint>

/* Byte offset of TickMessage::quantity - kept in sync with message.hpp's
 * documented layout (static_assert there catches any struct change). */
constexpr uint32_t kTickQuantityOffset = 32;

/* Attaches the drop-invalid-quantity filter described above to
 * `sockfd`. Throws SystemError if the kernel rejects it (e.g. no
 * CONFIG_NET socket filter support - essentially never on a modern
 * Linux, including WSL2's kernel). */
void bpf_attach_drop_invalid_quantity_filter(int sockfd);

#endif /* PULSEFORGE_BPF_FILTER_HPP */
