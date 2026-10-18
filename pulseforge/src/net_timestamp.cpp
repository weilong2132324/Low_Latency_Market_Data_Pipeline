/*
 * net_timestamp.cpp - SO_TIMESTAMPING plumbing. See net_timestamp.hpp.
 *
 * Ancillary data ("control messages", cmsg) primer: recvmsg() can return,
 * next to the payload, a buffer of typed records - each a cmsghdr
 * {len, level, type} followed by data. The CMSG_* macros walk them.
 * CMSG_DATA() may be unaligned for the struct inside, so we memcpy out
 * of it instead of casting the pointer (casting = undefined behaviour
 * on strict-alignment CPUs, and an aliasing violation everywhere).
 */

#include "net_timestamp.hpp"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>

namespace {

uint64_t to_ns(const timespec &ts) {
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

} // namespace

bool enable_rx_timestamps(int fd) {
    /* RX_SOFTWARE = generate stamps on receive; SOFTWARE = report
     * software stamps to user space (generation and reporting are
     * separate flags). */
    unsigned flags = SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
    return setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) == 0;
}

ssize_t recv_timestamped(int fd, void *buf, size_t cap, uint64_t *kernel_ns) {
    iovec iov{};
    iov.iov_base = buf;
    iov.iov_len  = cap;

    /* Room for one scm_timestamping (3 timespecs) plus headers. */
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(scm_timestamping)) + 64];

    msghdr msg{};
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = control;
    msg.msg_controllen = sizeof(control);

    ssize_t n = recvmsg(fd, &msg, 0);
    *kernel_ns = 0;
    if (n < 0) {
        return n;
    }
    for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPING) {
            scm_timestamping ts;
            std::memcpy(&ts, CMSG_DATA(c), sizeof(ts));
            /* ts[0] = software, ts[1] = legacy (unused), ts[2] = hardware */
            *kernel_ns = to_ns(ts.ts[0]);
        }
    }
    return n;
}

bool enable_tx_timestamps(int fd) {
    /* TX_SOFTWARE : stamp when the driver hands the packet to the device
     * SOFTWARE    : report software stamps
     * OPT_ID      : tag each stamp with an id (byte offset for TCP)
     * OPT_TSONLY  : do not loop the packet payload back with the stamp
     *               (we only want the time - saves a copy per send) */
    unsigned flags = SOF_TIMESTAMPING_TX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE |
                     SOF_TIMESTAMPING_OPT_ID | SOF_TIMESTAMPING_OPT_TSONLY;
    return setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) == 0;
}

size_t read_tx_timestamps(int fd, TxStamp *out, size_t max) {
    size_t got = 0;
    while (got < max) {
        alignas(cmsghdr) char control[512];
        char dummy[1];
        iovec iov{};
        iov.iov_base = dummy;
        iov.iov_len  = sizeof(dummy);
        msghdr msg{};
        msg.msg_iov        = &iov;
        msg.msg_iovlen     = 1;
        msg.msg_control    = control;
        msg.msg_controllen = sizeof(control);

        if (recvmsg(fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT) < 0) {
            break; /* EAGAIN: error queue empty */
        }
        bool have_ts = false, have_id = false;
        TxStamp s{};
        for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPING) {
                scm_timestamping ts;
                std::memcpy(&ts, CMSG_DATA(c), sizeof(ts));
                s.ns    = to_ns(ts.ts[0]);
                have_ts = s.ns != 0;
            } else if ((c->cmsg_level == SOL_IP && c->cmsg_type == IP_RECVERR) ||
                       (c->cmsg_level == SOL_IPV6 && c->cmsg_type == IPV6_RECVERR)) {
                sock_extended_err err;
                std::memcpy(&err, CMSG_DATA(c), sizeof(err));
                /* A timestamp report is an "error" with errno ENOMSG
                 * and origin TIMESTAMPING; anything else is a real error. */
                if (err.ee_errno == ENOMSG && err.ee_origin == SO_EE_ORIGIN_TIMESTAMPING &&
                    err.ee_info == SCM_TSTAMP_SND) {
                    s.id    = err.ee_data;
                    have_id = true;
                }
            }
        }
        if (have_ts && have_id) {
            out[got++] = s;
        }
    }
    return got;
}
