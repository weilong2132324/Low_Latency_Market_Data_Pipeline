/*
 * lessons.js - every piece of teaching text in the visualizer, in one file.
 *
 * Read this file top to bottom and you have read the course: it goes from
 * the three machines, down through the protocol stack to the wire, and
 * down through the hardware to a single 64-byte cache line.
 *
 * Shape of a lesson (all fields optional except title):
 *   tag       short category chip shown above the title
 *   title
 *   summary   2-3 sentences: what this thing is
 *   points    the ideas that matter for low-latency work
 *   latency   typical order-of-magnitude numbers (NOT measurements -
 *             real values depend on hardware; measure your own)
 *   code      [{ file, note }] where PulseForge implements / uses it
 *   tryIt     shell commands to run on your own machine (WSL2)
 *   interview { q, a } (or a list of them) - questions you should be able to answer
 *
 * Text conventions: `backticks` render as inline code.
 */

export const LESSONS = {
  /* ---------------------------------------------------------- overview */
  overview: {
    tag: 'Start here',
    title: 'Three machines, one market-data pipeline',
    summary:
      "An exchange gateway (A) multicasts ticks to a trading host (B) that validates, queues and processes them, while a monitoring host (C) streams telemetry to you. Every box and every cable here is clickable - each click goes one layer deeper.",
    points: [
      "Click a machine to open it up: chassis → motherboard → CPU die → a single cache line.",
      "Click a cable to see its protocol stack: application → UDP/TCP → IP → Ethernet → NIC → wire.",
      "Orange = hot path (every tick flows through it, nanoseconds matter). Blue = cold path (observability and recovery, off the critical path).",
      "In this lab all three machines are processes on your laptop talking over loopback - the same code runs unchanged across three real servers.",
    ],
    tryIt: ["make && make topology   # build + export your real CPU layout", "bash scripts/run_basic.sh"],
    interview: {
      q: "Walk me through what happens between an exchange sending a price and your strategy seeing it.",
      a: "Exchange matching engine → multicast feed (UDP) → switch → your NIC (DMA into a ring) → driver/kernel or a kernel-bypass library → your feed handler (decode, gap detect) → in-process queue → strategy thread. Each hop has a latency budget; this visualizer walks every one.",
    },
  },

  'machine-A': {
    tag: 'Machine A',
    title: 'Exchange gateway (publisher)',
    summary:
      "Stands in for the exchange side of a feed: it generates ticks, stamps each with a send timestamp and a sequence number, and multicasts them. It also keeps a recovery ring so a receiver that missed packets can ask for them again over TCP.",
    points: [
      "Real exchanges publish two identical multicast feeds (A/B lines) on separate networks; receivers arbitrate between them to survive single-path loss.",
      "Sequence numbers are the contract: they let any receiver detect loss without acknowledgements.",
      "Click the machine to open it up and see the hardware this process runs on.",
    ],
    code: [
      { file: 'src/publisher.cpp', note: 'pacing loop, drift-free sleep_until_ns()' },
      { file: 'src/snapshot_server.cpp', note: 'TCP recovery server on its own thread' },
    ],
  },

  'machine-B': {
    tag: 'Machine B - hot path',
    title: 'Trading receiver',
    summary:
      "The box that matters. It receives every tick, validates it, detects gaps, and hands it to worker threads through a queue. In a real firm this is the colocated server running the feed handler and strategy, where every microsecond is money.",
    points: [
      "Everything on this machine is tuned for the p99.9, not the average: one 50 µs stall can mean a missed trade.",
      "The kernel's UDP socket buffer is the first queue; PulseForge's queue backend is the second. Backpressure has to go somewhere.",
      "Open it up to see which core each thread should live on and why.",
    ],
    code: [
      { file: 'src/receiver.cpp', note: 'receive loop, gap detection, recovery dispatch' },
      { file: 'include/queue_backend.hpp', note: 'factory over mutex / SPSC / Disruptor queues' },
    ],
  },

  'machine-C': {
    tag: 'Machine C - cold path',
    title: 'Monitoring host',
    summary:
      "Observability lives off the critical path. The telemetry bridge reads the receiver's shared-memory counters and event ring and streams JSON to this browser over a hand-written WebSocket server.",
    points: [
      "Rule one of production monitoring for latency-sensitive systems: the hot path must never block on observability. PulseForge records an event with one relaxed fetch_add and a few stores - no locks, no syscalls.",
      "Shared memory doesn't cross real machines; in production an agent on host B would ship metrics over the network instead.",
    ],
    code: [
      { file: 'src/telemetry_bridge.cpp', note: 'polls shm, builds JSON, broadcasts' },
      { file: 'include/shared_events.hpp', note: 'the lock-free event ring the hot path writes into' },
    ],
  },

  /* -------------------------------------------------------- components */
  'comp-publisher': {
    tag: 'Software - Machine A',
    title: 'Publisher process',
    summary: "Builds a 40-byte TickMessage per sequence number and sends it with one `sendto()` per tick, paced to a target rate.",
    points: [
      "Pacing uses an absolute deadline (`deadline += interval; sleep_until(deadline)`) so timing errors don't accumulate - a relative `sleep(interval)` drifts.",
      "PulseForge once had a real bug here: deadlines came from CLOCK_MONOTONIC_RAW but were slept on CLOCK_MONOTONIC, so pacing silently stopped working. Clocks are a classic source of production bugs.",
      "`--drop-every N` simulates wire loss; the snapshot ring still records every message, like a real exchange's retransmission server.",
    ],
    code: [
      { file: 'src/publisher.cpp', note: 'main send loop' },
      { file: 'include/time_util.hpp', note: 'now_ns() vs now_ns_monotonic() - read the comment' },
    ],
    tryIt: ["./bin/publisher --port 9000 --rate 10000 --count 10000 --drop-every 7"],
    interview: {
      q: "Why would you never use the wall clock (CLOCK_REALTIME) to measure latency?",
      a: "NTP and manual changes can step or slew it, so intervals can go negative or jump. Use a monotonic clock for intervals; for cross-machine latency you need synchronised clocks (PTP) and hardware timestamps.",
    },
  },

  'comp-receiver': {
    tag: 'Software - Machine B (hot path)',
    title: 'Receiver loop',
    summary:
      "A single thread that drains the UDP socket, validates each datagram, tracks the last sequence number to detect gaps and reorders, and pushes into the queue backend. It is the only producer.",
    points: [
      "`--batch N` switches from one `recvfrom()` per packet to `recvmmsg()`, pulling up to N datagrams per syscall - the fixed syscall cost is amortised. It needs `MSG_WAITFORONE`, or it waits for the batch to fill (see the socket layer lesson for the bug this caused).",
      "`--busy-poll US` sets SO_BUSY_POLL so the driver polls instead of waiting for an interrupt (needs a NAPI-capable driver).",
      "`--bpf-filter` attaches a classic BPF program that drops zero-quantity ticks inside the kernel, before any copy to userspace.",
      "Only this thread ever calls `try_push()`; async TCP recovery results come back to it through futures, preserving the single-producer invariant SPSC and Disruptor queues depend on.",
    ],
    latency: "recvfrom() on a warm socket ~1-2 µs; recvmmsg amortises that across the batch.",
    code: [
      { file: 'src/receiver.cpp', note: 'process_one(), drain_recoveries(), recvmmsg path' },
      { file: 'include/bpf_filter.hpp', note: 'the hand-written BPF bytecode, explained' },
    ],
    tryIt: ["./bin/receiver --port 9000 --batch 32 --busy-poll 50 --bpf-filter --telemetry"],
    interview: {
      q: "You see sequence gaps on the receiver but the publisher says it sent everything. Where do you look?",
      a: "Kernel socket buffer overflow (`netstat -su` RcvbufErrors / `/proc/net/snmp`), NIC ring drops (`ethtool -S` rx_missed / rx_no_buffer), a slow consumer causing backpressure, then the network (switch counters). Work from the receiver outward.",
    },
  },

  'comp-queue': {
    tag: 'Software - Machine B (hot path)',
    title: 'Queue backend (a ring buffer)',
    summary:
      "The handoff between the receiver and the workers. Drawn as a ring because that's what it is: fixed slots in a pre-allocated array, a write cursor and a read cursor chasing each other. Click a slot to see its state.",
    points: [
      "mutex: std::mutex + two condition variables - correct, blocking, MPMC-safe; contended lock costs microseconds (futex syscalls).",
      "spsc: lock-free single-producer/single-consumer ring. Just two atomics with acquire/release ordering; capacity rounded to a power of two so `index & mask` replaces `%`.",
      "disruptor: one writer, many readers, each with its own cursor - every worker sees every message (broadcast), the shape of a real feed fan-out.",
      "No allocation ever happens on push/pop: the slots were allocated once at startup.",
    ],
    latency: "uncontended mutex lock+unlock ~20-50 ns; contended: microseconds. SPSC push/pop handoff ~50-100 ns between cores (one cache-line transfer each way).",
    code: [
      { file: 'include/spsc_ring.hpp', note: 'why acquire/release and not relaxed' },
      { file: 'include/mpmc_fanout.hpp', note: 'Disruptor-style broadcast, slowest-reader backpressure' },
      { file: 'docs/memory_ordering.md', note: 'the memory model, as used in this codebase' },
    ],
    tryIt: ["make bin/queue_bench && ./bin/queue_bench --count 200000 --workers 2"],
    interview: {
      q: "Why does the SPSC ring need acquire/release, when the stats counters get away with relaxed?",
      a: "The consumer must see the message bytes written before the producer published the new tail. Release on the tail store and acquire on the tail load create a happens-before edge. Stats counters are independent values - only atomicity matters, not ordering.",
    },
  },

  'comp-workers': {
    tag: 'Software - Machine B (hot path)',
    title: 'Worker threads',
    summary:
      "Consumers that pop ticks and fold them into shared statistics. Click a single worker to see that thread's own processed count and latency - accumulated from the event stream, which the aggregate counters can't give you.",
    points: [
      "Each worker keeps local totals and folds them into atomics with `memory_order_relaxed`: atomic, but no fences.",
      "Running max uses a compare-exchange loop - the standard lock-free pattern for 'update if bigger'.",
      "`--slow-consumer-us` injects a delay to show backpressure: queue fills, receiver retries, socket buffer fills, kernel drops.",
    ],
    code: [{ file: 'src/worker.cpp', note: 'pop, measure, fold into stats' }],
    tryIt: ["bash scripts/run_slow_consumer.sh   # watch queue_full climb"],
  },

  'comp-telemetry': {
    tag: 'Software - Machine C (cold path)',
    title: 'Telemetry bridge',
    summary:
      "A separate process that maps the receiver's shared memory read-only-in-spirit, polls it every 50 ms, and pushes JSON frames to every connected browser.",
    points: [
      "The WebSocket server is hand-rolled: HTTP upgrade handshake, SHA-1 + Base64 of the client key, then unmasked text frames. ~200 lines, no library.",
      "The cursor into the event ring only advances while a browser is connected - an earlier version drained events into a broadcast nobody heard.",
      "Another real bug: closing or reloading the browser tab killed the bridge. Writing to a socket whose peer has gone raises SIGPIPE, whose default action terminates the process. Every `send()` now passes `MSG_NOSIGNAL`, so it just fails with EPIPE and the dead client is dropped.",
    ],
    interview: {
      q: "A long-running TCP server dies silently every few hours with no core dump and no log line. What's your first suspect?",
      a: "SIGPIPE: a client disconnected and the server wrote to the dead socket. The default action kills the process without a core. Fix with MSG_NOSIGNAL on send(), or signal(SIGPIPE, SIG_IGN) at startup, then handle EPIPE. Confirm by checking the exit status (killed by signal 13).",
    },
    code: [
      { file: 'src/ws_server.cpp', note: 'RFC 6455 handshake and framing' },
      { file: 'include/sha1.hpp', note: 'SHA-1 from the spec (needed only for the handshake)' },
    ],
  },

  /* ------------------------------------------------------------- links */
  'link-feed': {
    tag: 'Network link',
    title: 'UDP multicast market-data feed',
    summary:
      "Fire-and-forget datagrams addressed to a multicast group. The sender pays for one send no matter how many receivers have joined - the network replicates the packet. Click to walk its protocol stack.",
    points: [
      "Why UDP: no handshake, no retransmission, no head-of-line blocking. A late packet is worthless; it's better to detect the gap and recover out of band.",
      "Why multicast: one exchange, thousands of subscribers. Unicast would mean thousands of sends per tick.",
      "Receivers join with `IP_ADD_MEMBERSHIP`, which makes the kernel send an IGMP report so switches start forwarding the group to that port.",
    ],
    code: [
      { file: 'include/multicast.hpp', note: 'TTL, loopback, IP_ADD_MEMBERSHIP' },
      { file: 'src/publisher.cpp', note: '--transport multicast --group 239.255.0.5' },
    ],
    tryIt: ["ip maddr show eth0   # multicast groups your NIC is listening to"],
    interview: {
      q: "TCP is reliable, so why do exchanges publish market data over UDP?",
      a: "Reliability costs latency: retransmission and in-order delivery mean one lost packet stalls everything behind it (head-of-line blocking). With sequence numbers plus a separate recovery channel, a receiver gets everything else immediately and repairs the gap on the side.",
    },
  },

  'link-recovery': {
    tag: 'Network link',
    title: 'TCP gap recovery',
    summary:
      "When the receiver sees sequence N+3 after N, it asks the publisher for N+1..N+2 over a separate TCP connection. Here reliability is exactly what you want, and latency matters less because it's off the hot path.",
    points: [
      "The request runs on a `std::async` thread so the receive loop never blocks on a TCP round trip.",
      "In-flight recoveries are capped (`kMaxPendingRecoveries`): during a loss storm, one-thread-per-gap becomes a thundering herd that causes more loss than it repairs.",
      "Real venues offer both: a TCP retransmission service for small gaps and a snapshot feed to rebuild an order book from scratch.",
    ],
    code: [
      { file: 'include/snapshot_protocol.hpp', note: 'wire format + the why-TCP rationale' },
      { file: 'src/snapshot_client.cpp', note: 'blocking client, run under std::async' },
    ],
    interview: {
      q: "What is TCP_NODELAY and when do you want it?",
      a: "It disables Nagle's algorithm, which delays small writes to coalesce them. Any latency-sensitive request/response protocol (order entry, recovery requests) sets it; otherwise a small message can wait for an ACK - up to tens of milliseconds with delayed ACKs.",
    },
  },

  'link-telemetry': {
    tag: 'Network link',
    title: 'WebSocket telemetry',
    summary: "TCP connection, upgraded from HTTP to WebSocket, carrying one JSON text frame every 50 ms to this page.",
    points: [
      "The HTTP Upgrade handshake proves the server speaks WebSocket: it must return base64(sha1(key + a fixed GUID)).",
      "Server-to-client frames are never masked; client-to-server frames always are (to stop cache-poisoning attacks on proxies).",
    ],
    code: [{ file: 'src/ws_server.cpp', note: 'build_text_frame(): FIN, opcode, 7/16/64-bit lengths' }],
  },

  /* ----------------------------------------------------- network stack */
  'net-stack': {
    tag: 'Network stack',
    title: 'Encapsulation: how 40 bytes become 106',
    summary:
      "Going down the stack, each layer wraps what it got from above in its own header. Going up, each layer strips its header and hands the rest up. Click any layer to see its exact bytes, filled in with this run's values.",
    points: [
      "The dashed line is the user/kernel boundary - crossing it is a syscall. Everything below it runs in the kernel (or in the NIC).",
      "Toggle KERNEL BYPASS to see what DPDK / ef_vi / AF_XDP actually skip: the kernel's socket, protocol and driver layers. The headers are still on the wire - they are just parsed by a userspace library instead.",
      "Honesty note: in this lab everything is 127.0.0.1, so packets go through the loopback device and never touch a NIC or a wire. This view shows the path between two real machines.",
    ],
  },

  'net-app': {
    tag: 'L7 Application - user space',
    title: 'TickMessage: the payload',
    summary:
      "The bytes your program actually cares about. PulseForge sends its C++ struct as-is, which is only safe because both ends are the same binary on the same CPU architecture.",
    points: [
      "Look at the 4 padding bytes at offset 20: the compiler inserted them so the int64 `price` is 8-byte aligned. Reordering fields (8-byte ones first) would remove them.",
      "Real feeds use explicit binary formats with fixed byte order - NASDAQ ITCH, CME MDP3 / SBE (Simple Binary Encoding) - decoded with zero-copy readers, never JSON.",
      "Prices are scaled integers. Floating point cannot represent 0.1 exactly.",
    ],
    code: [{ file: 'include/message.hpp', note: 'layout walk-through + static_assert(sizeof == 40)' }],
    interview: {
      q: "Why is sending a raw C++ struct over the network dangerous in general?",
      a: "Padding, field order, alignment and endianness are compiler/ABI/CPU specific - a different compiler flag or a big-endian peer reads garbage. Production protocols define every byte explicitly and version the schema.",
    },
  },

  'net-socket': {
    tag: 'Socket layer - the syscall boundary',
    title: 'Sockets, syscalls and the receive buffer',
    summary:
      "`recvfrom()` crosses from user space into the kernel, copies one datagram out of the socket's receive queue into your buffer, and returns. That crossing, and that copy, are what kernel bypass removes.",
    points: [
      "The socket receive buffer (SO_RCVBUF, 4 MiB in PulseForge) is the first queue in the system: bursts park here when your code is busy. When it overflows, the kernel drops silently and bumps a counter.",
      "recvmmsg (`--batch`) amortises the syscall: one entry into the kernel, many datagrams out.",
      "A real PulseForge bug, found with this visualizer: a blocking `recvmmsg()` waits until ALL N slots are filled. At 400 msgs/s with `--batch 16`, every tick sat ~20 ms waiting for its batch-mates (avg latency 19.4 ms). Passing `MSG_WAITFORONE` - block for the first datagram, then take whatever is already queued - brought it back to ~0.26 ms. Batching must amortise syscalls, never add waiting.",
      "A socket filter (`--bpf-filter`) runs here, on the kernel side - rejected packets are never copied to you.",
      "Signals: the receiver installs SIGINT without SA_RESTART so a blocked recvfrom returns EINTR and the loop can exit cleanly.",
    ],
    latency: "bare syscall ~100-300 ns (more with Spectre/Meltdown mitigations); recvfrom incl. copy ~1-2 µs.",
    code: [
      { file: 'src/receiver.cpp', note: 'SO_RCVBUF, recvmmsg batch path, SIGINT without SA_RESTART' },
      { file: 'src/bpf_filter.cpp', note: 'SO_ATTACH_FILTER' },
    ],
    tryIt: ["netstat -su | grep -i -E 'errors|buffer'", "cat /proc/net/udp   # per-socket rx_queue + drops columns"],
    interview: [{
      q: "After enabling batched receives, throughput is fine but average latency jumped from 50 µs to 20 ms at low message rates. What happened?",
      a: "The batched receive is waiting for the batch to fill. At low rates that wait dominates: batch / rate = 16 / 400 per second = 40 ms worst case, ~20 ms average. Fix: MSG_WAITFORONE (or a short timeout) so the call returns as soon as at least one datagram is available. Lesson: always check latency at LOW load too, not just throughput at high load.",
    }, {
      q: "Your UDP receiver drops packets under bursts but CPU is only 30%. What do you change first?",
      a: "Check RcvbufErrors; raise SO_RCVBUF (and net.core.rmem_max) to absorb the burst, then reduce per-packet cost (recvmmsg, busy polling), and make sure the receiving thread is pinned and not being preempted.",
    }],
  },

  'net-udp': {
    tag: 'L4 Transport',
    title: 'UDP: 8 bytes, no promises',
    summary:
      "Ports, a length, a checksum. That's all. No connection, no ordering, no retransmission, no congestion control - which is exactly why it's fast and predictable.",
    points: [
      "The kernel matches the destination port to your bound socket - that's the demultiplexing step.",
      "Checksums are usually computed and verified by the NIC (checksum offload), not the CPU.",
    ],
    code: [{ file: 'src/receiver.cpp', note: 'bind() to INADDR_ANY for multicast' }],
  },

  'net-tcp': {
    tag: 'L4 Transport',
    title: 'TCP: 20 bytes of guarantees',
    summary:
      "Sequence and acknowledgement numbers turn a lossy network into an ordered, reliable byte stream - at the cost of a handshake, retransmission timers and head-of-line blocking.",
    points: [
      "A lost segment stalls every byte after it until the retransmission arrives - fatal for a live feed, fine for a recovery request.",
      "TCP is a byte stream: one recv() can return half a message or two. snapshot_client.cpp loops in `recv_full()` for exactly this reason.",
      "Nagle's algorithm + delayed ACKs can add tens of milliseconds to small writes; latency-sensitive code sets TCP_NODELAY.",
    ],
    code: [{ file: 'src/snapshot_client.cpp', note: 'recv_full() / send_full() loops' }],
  },

  'net-ip': {
    tag: 'L3 Network',
    title: 'IPv4 and multicast groups',
    summary:
      "IP gets a packet from one host to another (or to a group). The destination 239.255.0.5 is not a machine - it's a multicast group in the administratively-scoped 239/8 range, like a radio frequency anyone can tune into.",
    points: [
      "224.0.0.0/4 is multicast. 239.0.0.0/8 is for private use (RFC 2365) - the one to use in a lab.",
      "TTL: decremented at each router; multicast with TTL 1 never leaves the local subnet (`--ttl`).",
      "IGMP: joining a group makes the host announce it, so switches (IGMP snooping) and routers (PIM) forward the group only where it's wanted.",
      "Never fragment: one lost fragment loses the whole datagram, and reassembly adds latency. Keep packets under the MTU.",
    ],
    code: [{ file: 'include/multicast.hpp', note: 'is_multicast_address(), multicast_join_group()' }],
    tryIt: ["cat /proc/net/igmp"],
  },

  'net-eth': {
    tag: 'L2 Data link',
    title: 'Ethernet frames and multicast MACs',
    summary:
      "Ethernet moves frames between NICs on the same segment using MAC addresses. A multicast IP maps onto a multicast MAC, so the NIC itself can filter groups in hardware.",
    points: [
      "Group IP → MAC: `01:00:5e` + the low 23 bits of the IP. Only 23 of the 28 group bits fit, so 32 groups share each MAC; the kernel discards the ones you didn't join.",
      "Frames are at least 64 bytes (dst MAC to FCS); our 86-byte frame is above that. The FCS is a CRC-32 the NIC computes and checks.",
      "Switches: store-and-forward waits for the whole frame and checks the FCS; cut-through starts forwarding after reading the destination MAC - the HFT choice.",
    ],
    latency: "commodity cut-through switch ~300-500 ns; specialised low-latency/L1 switches ~5-50 ns.",
    tryIt: ["ip link show eth0", "ethtool -S eth0 | head -40   # NIC counters (limited on hv_netvsc)"],
    interview: {
      q: "What's the multicast MAC for group 239.255.0.5?",
      a: "01:00:5e:7f:00:05 - 01:00:5e followed by the low 23 bits of the address (255 & 0x7f = 0x7f, then 0x00, 0x05).",
    },
  },

  'net-driver': {
    tag: 'Kernel driver / NAPI',
    title: 'Interrupts, NAPI and busy polling',
    summary:
      "When a frame lands, the NIC raises an interrupt; the driver schedules NAPI, which polls the RX ring in a softirq, builds an sk_buff and pushes it up the stack. Every one of those steps costs time and can be delayed by whatever else the core is doing.",
    points: [
      "Interrupt coalescing (`ethtool -C`) trades latency for throughput: the NIC waits for several packets before interrupting. Latency-sensitive boxes turn it down or off.",
      "SO_BUSY_POLL (`--busy-poll`) lets your thread poll the driver's queue directly while it waits - skipping the interrupt and softirq scheduling.",
      "Pin the NIC queue's IRQ to the same core (or NUMA node) as the thread consuming it: `/proc/irq/N/smp_affinity`.",
    ],
    latency: "interrupt + softirq + wakeup path ~2-10 µs; busy polling removes most of the wakeup.",
    code: [{ file: 'src/receiver.cpp', note: 'SO_BUSY_POLL (soft-fails where the driver can’t do it)' }],
    tryIt: ["cat /proc/interrupts | head -20", "cat /proc/softirqs | grep NET_RX"],
  },

  'net-nic': {
    tag: 'L1/L2 Hardware - NIC',
    title: 'The network interface card',
    summary:
      "The NIC receives bits from the PHY, checks the frame, and DMAs it over PCIe straight into a ring of buffers in host memory - no CPU involved - then writes a descriptor saying 'slot 17 is full'.",
    points: [
      "RX/TX rings: circular arrays of descriptors, exactly like PulseForge's SPSC ring - the NIC is the producer, the driver (or a bypass library) is the consumer.",
      "RSS hashes flows across several queues so several cores can receive in parallel. Your eth0 has 8 RX queues.",
      "Hardware timestamping stamps each frame as it arrives at the PHY - the only honest way to measure wire-to-application latency.",
      "On Intel servers, DDIO DMAs packets directly into L3 cache instead of DRAM, saving a memory round trip.",
      "Your eth0 driver is `hv_netvsc`: a Hyper-V synthetic NIC. No real hardware, no DPDK. That's the honest limit of a WSL2 lab.",
    ],
    latency: "PCIe DMA + descriptor write ~0.5-1 µs.",
    code: [{ file: 'src/hw_topology.cpp', note: 'reads driver, queues, NUMA node from /sys/class/net' }],
    tryIt: ["ls /sys/class/net/eth0/queues", "ethtool -i eth0   # driver info"],
  },

  'net-phy': {
    tag: 'L1 Physical',
    title: 'PHY, serialization and the speed of light',
    summary:
      "The PHY turns the frame into a signal on fibre or copper. Two physical costs are fixed: serialization (how long it takes to clock the bits out) and propagation (how long light takes to travel).",
    points: [
      "Serialization of our frame: (86-byte frame + 8 preamble + 12 inter-frame gap) x 8 bits ÷ 10 Gbit/s = 84.8 ns. At 25 Gbit/s: 33.9 ns. Faster links are a latency win, not just bandwidth.",
      "Light in fibre covers ~1 m every 5 ns (it travels at ~2/3 c in glass). Every metre of cable in the colo is measured.",
      "Over long distances, microwave towers beat fibre: Chicago–New Jersey is roughly 13 ms round trip on the best fibre route vs ~8 ms by microwave, because radio travels at ~c in air and takes straighter paths.",
    ],
    latency: "serialization 34-85 ns (25G/10G); propagation ~5 ns/m in fibre, ~3.3 ns/m in air.",
    interview: {
      q: "Your strategy server is 30 m of fibre further from the exchange than a competitor's. How much slower are you?",
      a: "About 150 ns one way (30 m x 5 ns/m) - every tick, forever. That's why colocation cabinets are allocated with equal-length cables.",
    },
  },

  'net-switch': {
    tag: 'L2 device',
    title: 'The switch in the middle',
    summary:
      "Between two real machines sits at least one switch. It reads the destination MAC and forwards the frame; for multicast it uses IGMP snooping to send each group only to ports that joined it.",
    points: [
      "Cut-through switches start transmitting before the frame has fully arrived; store-and-forward waits for the FCS.",
      "Layer-1 switches (pure signal replication, no MAC lookup) are used to fan an exchange feed out to many servers in a few nanoseconds.",
    ],
    latency: "~300-500 ns commodity cut-through; ~5 ns layer-1.",
  },

  'net-bypass': {
    tag: 'Kernel bypass',
    title: 'What kernel bypass actually bypasses',
    summary:
      "The NIC's rings are mapped straight into your process. Your thread spins on the RX ring, sees a new descriptor, and reads the frame - no interrupt, no syscall, no sk_buff, no copy. The kernel's socket, IP, UDP and driver layers are skipped; their job (parsing headers) moves into a userspace library.",
    points: [
      "DPDK: generic userspace poll-mode drivers. Solarflare/AMD ef_vi and OpenOnload: vendor-specific, often lowest latency, OpenOnload even keeps the BSD socket API. AF_XDP: the in-kernel middle ground.",
      "Costs: a core burned at 100% spinning, you reimplement what the kernel did (ARP, IGMP, sometimes TCP), and you need supported hardware.",
      "PulseForge can't do this in WSL2 (virtual NIC). docs/kernel_bypass_next_steps.md explains what you'd need on bare metal.",
    ],
    latency: "wire-to-application ~1-2 µs with kernel bypass vs ~5-20 µs through the kernel (typical, hardware dependent).",
    code: [
      { file: 'docs/kernel_bypass_next_steps.md', note: 'the honest write-up' },
      { file: 'include/bpf_filter.hpp', note: 'the bypass-adjacent thing that DOES run here' },
    ],
    interview: {
      q: "Kernel bypass removes the kernel. What new problems does that create?",
      a: "You own the poll loop (a dedicated spinning core), you must handle protocol duties the kernel did (ARP, IGMP joins), standard tools like tcpdump/netstat no longer see your traffic, and you need supported NICs and drivers. Observability is the big SRE cost.",
    },
  },

  /* ---------------------------------------------------------- hardware */
  'hw-chassis': {
    tag: 'Hardware - chassis',
    title: 'The server',
    summary:
      "A 1U rack server in a colocation cage, a few metres of cable from the exchange's matching engine. Click the lid to take it off.",
    points: [
      "Colocation: firms rent rack space inside the exchange's data centre to cut the distance to nanoseconds of cable.",
      "BIOS settings matter as much as code: disable deep C-states and frequency scaling, turn off SMT on the hottest cores, disable power-saving on PCIe links.",
    ],
    tryIt: ["lscpu", "cat /proc/cpuinfo | grep MHz | head"],
  },

  'hw-board': {
    tag: 'Hardware - motherboard',
    title: 'Motherboard: sockets, memory, PCIe',
    summary:
      "The CPU socket(s), the memory channels hanging off each socket, and the PCIe slots where the NIC lives. On a multi-socket server, each socket with its memory is a NUMA node. Click the CPU to lift it out.",
    points: [
      "NUMA: memory attached to your own socket is local (~80-100 ns); the other socket's memory is remote (~1.5-2x slower) and crosses the inter-socket link.",
      "PCIe lanes belong to one socket. A NIC in a slot wired to socket 1, read by a thread on socket 0, pays a cross-socket trip for every packet. Rule: keep the NIC, the memory and the threads on the same node.",
      "Your machine has one NUMA node, so this lesson is about the servers you'll work on, not your laptop.",
    ],
    latency: "local DRAM ~80-100 ns; remote NUMA DRAM ~130-200 ns.",
    code: [{ file: 'src/hw_topology.cpp', note: 'reads /sys/devices/system/node/*' }],
    tryIt: ["numactl --hardware   # (sudo apt install numactl)", "cat /sys/class/net/eth0/device/numa_node"],
    interview: {
      q: "How do you find which NUMA node a NIC is on, and why do you care?",
      a: "`cat /sys/class/net/<if>/device/numa_node` (or lstopo). You pin the feed-handler threads and allocate their buffers on that node so DMA'd packets and the code reading them never cross the socket interconnect.",
    },
  },

  'hw-dram': {
    tag: 'Hardware - memory',
    title: 'DRAM',
    summary:
      "Main memory: big, cheap, and about 100x slower than L1 cache. A cache miss that goes all the way to DRAM costs ~80-100 ns - long enough to execute hundreds of instructions.",
    points: [
      "The hot path's working set should fit in L1/L2. Every pointer chase into a cold node is a potential 100 ns stall.",
      "Virtual-to-physical translation is cached in the TLB; a miss walks page tables (more memory accesses). Huge pages (2 MiB) cover 512x more memory per TLB entry.",
      "Pre-allocate at startup and pre-fault (touch) pages so the first real message doesn't pay for a page fault.",
    ],
    latency: "~80-100 ns per miss; TLB miss page walk adds ~10-50 ns more.",
    code: [
      { file: 'include/hugepage_alloc.hpp', note: 'MAP_HUGETLB with fallback' },
      { file: 'include/mem_pool.hpp', note: 'no new/delete after startup' },
    ],
    tryIt: ["grep -i huge /proc/meminfo"],
  },

  'hw-pcie': {
    tag: 'Hardware - PCIe',
    title: 'PCIe: the NIC’s road to memory',
    summary:
      "The NIC sits on PCIe lanes that belong to a CPU socket. Packets travel NIC → PCIe → the CPU's root complex → memory (or L3 with DDIO) by DMA, with no CPU instructions involved.",
    points: [
      "Each PCIe read by the CPU (e.g. reading a NIC register) is a round trip of several hundred ns - drivers avoid them in the fast path.",
      "Kernel-bypass libraries map the NIC's descriptor rings into your process precisely so you read them from memory, not from the device.",
    ],
    latency: "PCIe round trip ~300-1000 ns.",
  },

  'hw-nic': {
    tag: 'Hardware - NIC',
    title: 'The NIC, from the inside',
    summary:
      "Ports (SFP+ cages for fibre/twinax), a PHY, a MAC, packet buffers, and DMA engines that fill RX rings in host memory. Your machine's NIC is shown with its real driver, speed and queue count.",
    points: [
      "Look at the driver name: `mlx5_core`, `ixgbe`, `i40e`, `sfc` are real NICs with kernel-bypass support; `hv_netvsc` / `virtio_net` are virtual NICs inside a VM.",
      "Each RX queue has its own interrupt; RSS spreads flows across them. A single multicast feed is one flow, so it all lands on one queue - and one core.",
    ],
    code: [{ file: 'src/hw_topology.cpp', note: '/sys/class/net/<if>/queues, device/driver' }],
    tryIt: ["ethtool -i eth0", "ls /sys/class/net/eth0/queues"],
  },

  'hw-die': {
    tag: 'Hardware - CPU die',
    title: 'Inside the CPU: cores and caches',
    summary:
      "Each physical core has private L1 (split into instruction and data) and L2 caches; all cores share a large L3. Hyperthreads (SMT siblings) are two logical CPUs sharing one core and its private caches. The highlighted cores show where PulseForge's threads should run.",
    points: [
      "Pin hot threads with `taskset`/`sched_setaffinity` (PulseForge: `--pin-cpu`) so the scheduler never migrates them and their caches stay warm.",
      "One hot thread per physical core: leave the SMT sibling idle, or two hot threads will compete for the same L1/L2 and execution units.",
      "Isolate the hot cores from the scheduler with the kernel boot options `isolcpus=`, `nohz_full=` and `rcu_nocbs=`; leave CPU 0 for the OS and interrupts.",
      "Click a core for its caches; click a hot core's L1d to zoom into a cache line.",
    ],
    latency: "L1 ~1 ns (4-5 cycles), L2 ~3-4 ns, L3 ~10-20 ns, DRAM ~80-100 ns.",
    code: [
      { file: 'src/cpu_util.cpp', note: 'pin_to_cpu() via sched_setaffinity' },
      { file: 'src/hw_topology.cpp', note: 'how core/sibling/cache sharing is discovered' },
    ],
    tryIt: ["lscpu -e   # logical CPU -> core -> socket", "cat /sys/devices/system/cpu/cpu0/topology/thread_siblings_list", "taskset -c 2 ./bin/receiver --pin-cpu 2"],
    interview: {
      q: "Two threads on SMT siblings run 30% slower than on separate cores. Why?",
      a: "Siblings share one core's execution units, L1/L2 caches, TLB and branch predictors. Two hot threads evict each other's data and contend for ports. For latency-critical threads you give each a full physical core.",
    },
  },

  'hw-core': {
    tag: 'Hardware - core',
    title: 'One physical core',
    summary:
      "An out-of-order pipeline that can retire several instructions per cycle - as long as its data is in L1. Its private caches and the logical CPUs (SMT threads) it exposes are shown here from your real topology.",
    points: [
      "A cache miss to DRAM stalls the core for ~300+ cycles; branch mispredictions cost ~15-20 cycles each. Predictable, cache-resident code is fast code.",
      "Context switches flush a lot of this state - another reason hot threads are pinned and never block.",
    ],
  },

  'hw-l1': {
    tag: 'Hardware - L1 cache',
    title: 'L1 data cache',
    summary:
      "The smallest, fastest cache - tens of KB, ~4 cycles. Data moves in and out of it in 64-byte cache lines, never single bytes. Click to zoom into one of those lines.",
    points: [
      "Associativity (ways) limits where a line can live; pathological strides (e.g. 4 KiB apart) can thrash a set even when the cache isn't full.",
      "Layout your hot structures so everything one operation needs sits in as few lines as possible.",
    ],
    latency: "~4-5 cycles ≈ 1 ns.",
  },

  'hw-l2': {
    tag: 'Hardware - L2 cache',
    title: 'L2 cache',
    summary: "Private per core, typically 1-2 MB, ~12-14 cycles. Catches what falls out of L1.",
    latency: "~3-4 ns.",
  },

  'hw-l3': {
    tag: 'Hardware - L3 cache',
    title: 'Shared L3 (last-level cache)',
    summary:
      "Shared by all cores on the die. When one core writes a line another core holds, the coherence protocol (MESI and friends) invalidates the other copy - and the next read has to fetch it again. That traffic is what false sharing is made of.",
    points: [
      "Other processes on the same socket share L3 with you - a noisy neighbour can evict your hot data. Some firms partition L3 with Intel CAT (resctrl).",
      "Core-to-core transfer of a modified line ~40-100 ns: the real cost of 'just an atomic increment' when two cores fight over it.",
    ],
    latency: "~10-20 ns hit; cross-core cache-line transfer ~40-100 ns.",
  },

  'hw-cacheline': {
    tag: 'Hardware - cache line',
    title: 'One 64-byte cache line',
    summary:
      "The unit of everything: memory moves in lines, coherence works on lines, and two threads writing different variables on the same line still fight over it. The bars show a real PulseForge struct mapped onto lines, coloured by which thread writes each field.",
    points: [
      "False sharing: two writers on one line = the line ping-pongs between their cores on every write, even though they never touch the same variable.",
      "Fix: pad or `alignas(64)` so each writer owns its lines - exactly what SharedStats and SpscRing do.",
      "Look closely at SharedStats line 1: `queue_depth` is written by both the receiver and the workers. PulseForge still has a little false sharing there - a real improvement you could make.",
      "Use the toggle to delete the padding and watch the line start bouncing.",
    ],
    latency: "a contended line bouncing between two cores costs ~40-100 ns per write.",
    code: [
      { file: 'include/stats.hpp', note: '_pad1/_pad2 + static_assert(sizeof == 128)' },
      { file: 'include/spsc_ring.hpp', note: 'alignas(64) head_/tail_' },
      { file: 'src/race_demo.cpp', note: 'a counter shared by 8 threads' },
    ],
    tryIt: ["perf c2c record ./bin/queue_bench && perf c2c report   # finds false sharing (needs perf)"],
    interview: {
      q: "What is false sharing and how do you detect it?",
      a: "Independent variables written by different cores that live on the same cache line, so coherence traffic serialises the writers. Detect with `perf c2c` (HITM events), or suspect it when throughput drops as you add threads. Fix with padding/alignas(64) or per-thread data.",
    },
  },

  'hw-cacheline-A': {
    tag: 'Hardware - cache line',
    title: 'TickMessages straddling cache lines',
    summary:
      "The publisher's and the queue's buffers hold 40-byte TickMessages back to back. 40 doesn't divide 64, so half of them straddle two cache lines - reading one message then costs two line fills.",
    points: [
      "Of every 8 messages (320 bytes = 5 lines), messages 1, 3, 4 and 6 cross a line boundary.",
      "Fixes: pad the message to 64 bytes (wastes 24 bytes each), or shrink it to 32 (two per line). Real feed handlers often store a 64-byte aligned 'normalised' record per update.",
    ],
    code: [{ file: 'include/message.hpp', note: 'TICK_MESSAGE_EXPECTED_SIZE = 40' }],
  },

  'hw-cacheline-C': {
    tag: 'Hardware - cache line',
    title: 'The event ring: false sharing on purpose',
    summary:
      "Each telemetry Event is 32 bytes, so two share a line - and the receiver and the workers append into neighbouring slots. That is false sharing, and here it's an acceptable trade: it's the cold path, and padding each event to 64 bytes would double the ring's memory.",
    points: [
      "Engineering is choosing where to pay. The hot-path SharedStats is padded; the cold-path event ring isn't.",
    ],
    code: [{ file: 'include/shared_events.hpp', note: 'struct Event (32 bytes), event_ring_record()' }],
  },

  'hw-memctrl': {
    tag: 'Hardware - uncore',
    title: 'Memory controller and I/O',
    summary:
      "The parts of the die that aren't cores: memory controllers (one per DRAM channel group) and the PCIe root complex the NIC's DMA arrives through.",
    points: ["On AMD chips these live on a separate I/O die; on Intel they're on the same mesh as the cores. Either way, the NIC's packets enter the CPU here."],
  },
};

export function lesson(id) {
  return LESSONS[id] || { tag: 'Missing lesson', title: id, summary: 'No lesson text has been written for this yet - add one to js/data/lessons.js.' };
}
