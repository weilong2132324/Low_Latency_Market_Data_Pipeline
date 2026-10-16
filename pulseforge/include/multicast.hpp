/*
 * multicast.hpp - UDP multicast setup helpers.
 *
 * Responsibility: the real shape of a market-data feed is one sender,
 * many subscribers, no per-subscriber fan-out cost on the sender - that
 * is exactly what IP multicast gives you and unicast UDP does not (a
 * unicast publisher that wants N subscribers must open N sockets and
 * pay N x the send cost). This header wraps the handful of setsockopt
 * calls needed on each side:
 *   - sender:   IP_MULTICAST_TTL (how many router hops the packet may
 *               cross), IP_MULTICAST_LOOP (whether a sender on the same
 *               host sees its own packets looped back - handy for this
 *               lab, since publisher and receiver(s) run on one box).
 *   - receiver: IP_ADD_MEMBERSHIP (join the group so the kernel starts
 *               delivering that destination address's datagrams).
 *
 * Error handling: these functions follow the project's CHECK()/
 * SystemError convention (common.hpp), not perror+bool, so callers can
 * use the same try/catch as every other syscall in this codebase.
 *
 * Group addresses: 224.0.0.0-239.255.255.255 is the IPv4 multicast
 * range; this project defaults to 239.255.0.1, inside the
 * administratively-scoped 239.0.0.0/8 block meant for private/local use
 * (RFC 2365) - never route this out to the internet.
 */

#ifndef PULSEFORGE_MULTICAST_HPP
#define PULSEFORGE_MULTICAST_HPP

#include "common.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>

constexpr const char *kDefaultMulticastGroup = "239.255.0.1";

/* True if `ip` parses as an address in 224.0.0.0/4 (the IPv4 multicast
 * range). Used to validate --group / give a clear error instead of a
 * confusing setsockopt failure. */
inline bool is_multicast_address(const std::string &ip) {
    in_addr addr{};
    if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) {
        return false;
    }
    uint32_t host_order = ntohl(addr.s_addr);
    return (host_order >> 28) == 0xE; /* top 4 bits == 1110 */
}

/* Sender-side: how many router hops a multicast datagram may cross.
 * ttl=1 (the default almost everyone wants) means "this subnet only". */
inline void multicast_set_ttl(int sockfd, int ttl) {
    CHECK(setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)),
          "setsockopt(IP_MULTICAST_TTL)");
}

/* Sender-side: whether a copy of the sender's own packets is looped
 * back to receivers on the same host. Must be enabled for this lab's
 * publisher/receiver to talk to each other over loopback multicast. */
inline void multicast_set_loop(int sockfd, bool enable) {
    unsigned char loop = enable ? 1 : 0;
    CHECK(setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop,
                     sizeof(loop)),
          "setsockopt(IP_MULTICAST_LOOP)");
}

/* Receiver-side: join a multicast group on the default interface so the
 * kernel starts delivering its datagrams to this socket. */
inline void multicast_join_group(int sockfd, const std::string &group_ip) {
    ip_mreq mreq{};
    if (inet_pton(AF_INET, group_ip.c_str(), &mreq.imr_multiaddr) != 1) {
        throw SystemError("inet_pton(" + group_ip + ")", EINVAL);
    }
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    CHECK(setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
                     sizeof(mreq)),
          "setsockopt(IP_ADD_MEMBERSHIP)");
}

#endif /* PULSEFORGE_MULTICAST_HPP */
