#include "bpf_filter.hpp"

#include "common.hpp"

#include <array>
#include <linux/filter.h>
#include <sys/socket.h>

void bpf_attach_drop_invalid_quantity_filter(int sockfd) {
    static const std::array<sock_filter, 4> kProgram = {{
        /* A = *(u32 *)(payload + kTickQuantityOffset)  -- "quantity" */
        {BPF_LD | BPF_W | BPF_ABS, 0, 0, kTickQuantityOffset},
        /* if A == 0: fall through to "drop"; else: jump 1 to "accept" */
        {BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0},
        /* drop: truncate the packet to 0 bytes */
        {BPF_RET | BPF_K, 0, 0, 0},
        /* accept: keep up to 65535 bytes (i.e. the whole datagram) */
        {BPF_RET | BPF_K, 0, 0, 0xffff},
    }};

    sock_fprog prog{};
    prog.len    = static_cast<unsigned short>(kProgram.size());
    prog.filter = const_cast<sock_filter *>(kProgram.data());

    CHECK(setsockopt(sockfd, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof(prog)),
          "setsockopt(SO_ATTACH_FILTER)");
}
