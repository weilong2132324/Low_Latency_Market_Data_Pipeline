/*
 * net_timestamp.hpp - kernel packet timestamps via SO_TIMESTAMPING.
 *
 * Responsibility: answer "WHEN did this packet really arrive / leave?"
 * more precisely than calling clock_gettime() in user space.
 *
 * The problem: if you timestamp a packet after recvmsg() returns, your
 * "receive time" includes everything that happened in between - the
 * interrupt, the softirq, the socket wakeup, the scheduler putting your
 * thread back on a CPU, the syscall. Under epoll that can be 5-50 us and
 * it hides exactly the delay you are trying to measure.
 *
 * SO_TIMESTAMPING asks the kernel to stamp the packet at a fixed point
 * in the network stack and hand the stamp back as ancillary data (a
 * "cmsg") alongside the payload:
 *
 *   RX software (SOF_TIMESTAMPING_RX_SOFTWARE): stamped when the packet
 *       enters the network stack (netif_receive_skb, just after the
 *       driver). user_rx_time - kernel_rx_time = your wakeup latency.
 *   TX software (SOF_TIMESTAMPING_TX_SOFTWARE): stamped when the driver
 *       hands the packet to the NIC (skb_tx_timestamp). Delivered later
 *       on the socket's ERROR QUEUE (recvmsg with MSG_ERRQUEUE), because
 *       the send() call returned long before the packet left.
 *   Hardware (SOF_TIMESTAMPING_RX/TX_HARDWARE + RAW_HARDWARE): stamped
 *       by the NIC itself, at the PHY, from its own PTP-disciplined clock
 *       - nanosecond-accurate "on the wire" time. Needs a NIC that
 *       supports it (check: `ethtool -T eth0`) and an ioctl
 *       (SIOCSHWTSTAMP) to switch it on; WSL2's virtual NIC does not, so
 *       this lab uses the software stamps. The API is identical - the
 *       stamp just lands in ts[2] instead of ts[0] of scm_timestamping.
 *
 * Clock: software stamps are CLOCK_REALTIME, so compare them with
 * now_realtime_ns(), never with the CLOCK_MONOTONIC_RAW now_ns().
 *
 * TX stamp matching (SOF_TIMESTAMPING_OPT_ID): each stamp carries an id.
 * For a TCP socket the id is a BYTE offset: the stream position of the
 * last byte of the send() it belongs to, counted from when the option
 * was enabled. So if you have sent 1000 bytes in total and send a 44-byte
 * order, its stamp arrives with id 1043. The trader keeps a small table
 * of (expected id -> order) to match them up.
 *
 * Thread-safety: functions only touch the fd you pass.
 */

#ifndef PULSEFORGE_NET_TIMESTAMP_HPP
#define PULSEFORGE_NET_TIMESTAMP_HPP

#include <cstddef>
#include <cstdint>
#include <sys/types.h>

/* Ask for software RX timestamps on a (UDP) socket. false = unsupported. */
bool enable_rx_timestamps(int fd);

/* recvmsg() one datagram. *kernel_ns = the kernel RX stamp
 * (CLOCK_REALTIME ns) or 0 if none came with the packet. Returns bytes,
 * or -1 with errno (EAGAIN when a non-blocking socket is empty). */
ssize_t recv_timestamped(int fd, void *buf, size_t cap, uint64_t *kernel_ns);

/* Ask for software TX timestamps with byte-offset ids on a CONNECTED
 * TCP socket (the kernel rejects OPT_ID on an unconnected TCP socket).
 * false = unsupported. */
bool enable_tx_timestamps(int fd);

struct TxStamp {
    uint32_t id; /* byte offset of the last byte of the timestamped send */
    uint64_t ns; /* CLOCK_REALTIME */
};

/* Drain up to `max` TX stamps from the error queue (non-blocking).
 * Returns how many were stored in out. */
size_t read_tx_timestamps(int fd, TxStamp *out, size_t max);

#endif /* PULSEFORGE_NET_TIMESTAMP_HPP */
