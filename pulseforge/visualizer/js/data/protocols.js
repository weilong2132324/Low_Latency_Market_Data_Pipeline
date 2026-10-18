/*
 * protocols.js - byte-exact header layouts for every protocol PulseForge
 * puts on the wire, plus the arithmetic that falls out of them.
 *
 * Why this file exists: "a UDP packet" is an abstraction. On the wire a
 * 40-byte TickMessage is 106 bytes of light, and every one of the 66
 * extra bytes was added by a specific layer for a specific reason. The
 * network-stack view draws these tables; reading them here first is the
 * fastest way to learn the stack.
 *
 * Each header: { name, layer, size, fields: [{ name, offset, size, note, live? }] }
 * `live(ctx)` (optional) returns the value this field would carry for
 * the running configuration - ctx = { config, link }.
 */

/* Application payload - must match include/message.hpp byte for byte. */
export const TICK_MESSAGE = {
  name: 'TickMessage (application payload)',
  layer: 'L7',
  size: 40,
  fields: [
    { name: 'sequence', offset: 0, size: 8, note: 'uint64_t, monotonic per run - how the receiver detects gaps' },
    { name: 'send_timestamp_ns', offset: 8, size: 8, note: 'CLOCK_MONOTONIC_RAW at send - end-to-end latency = now - this' },
    { name: 'symbol_id', offset: 16, size: 4, note: 'uint32_t instrument id' },
    { name: '(padding)', offset: 20, size: 4, pad: true, note: 'inserted by the compiler so `price` lands 8-byte aligned' },
    { name: 'price', offset: 24, size: 8, note: 'int64_t scaled integer - never a float for money' },
    { name: 'quantity', offset: 32, size: 4, note: 'uint32_t, must be > 0 (the BPF filter checks this in-kernel)' },
    { name: 'flags', offset: 36, size: 4, note: 'TICK_FLAG_TRADE | QUOTE | LAST' },
  ],
};

/* TCP gap-recovery request - include/snapshot_protocol.hpp */
export const SNAPSHOT_REQUEST = {
  name: 'SnapshotRequest (application payload)',
  layer: 'L7',
  size: 16,
  fields: [
    { name: 'from_seq', offset: 0, size: 8, note: 'first missing sequence number' },
    { name: 'count', offset: 8, size: 8, note: 'how many, capped at kMaxSnapshotRequestLen' },
  ],
};

/* RFC 6455 frame as src/ws_server.cpp builds it (server -> browser,
 * unmasked), carrying one telemetry JSON snapshot. Payload length
 * 126..65535 uses the 2-byte extended form. 354 bytes is the size of a
 * real frame with no new events (measured from telemetry_bridge). */
export const WS_FRAME = {
  name: 'WebSocket frame + JSON snapshot',
  layer: 'L7',
  size: 358,
  fields: [
    { name: 'FIN|RSV|opcode', offset: 0, size: 1, note: '0x81 = final fragment, opcode 1 = text', live: () => '0x81' },
    { name: 'MASK|len7', offset: 1, size: 1, note: 'MASK=0 (servers never mask), 126 = "16-bit length follows"' },
    { name: 'ext length', offset: 2, size: 2, note: 'JSON length, big-endian', live: () => 354 },
    { name: 'JSON body', offset: 4, size: 354, note: '{"stats":{...},"config":{...},"events":[...]} - grows with events' },
  ],
};

export const UDP_HEADER = {
  name: 'UDP header',
  layer: 'L4',
  size: 8,
  fields: [
    { name: 'source port', offset: 0, size: 2, note: 'ephemeral port the publisher\'s socket got' },
    { name: 'dest port', offset: 2, size: 2, note: 'the receiver\'s --port', live: (c) => c.config ? c.config.port : 9000 },
    { name: 'length', offset: 4, size: 2, note: 'header + payload = 8 + 40 = 48', live: () => 48 },
    { name: 'checksum', offset: 6, size: 2, note: 'optional in IPv4 (0 = none); NICs usually compute it (offload)' },
  ],
};

export const TCP_HEADER = {
  name: 'TCP header',
  layer: 'L4',
  size: 20,
  fields: [
    { name: 'source port', offset: 0, size: 2, note: 'receiver\'s ephemeral port' },
    { name: 'dest port', offset: 2, size: 2, note: 'publisher --snapshot-port', live: () => 9501 },
    { name: 'sequence number', offset: 4, size: 4, note: 'byte offset in the stream - what makes TCP ordered' },
    { name: 'ack number', offset: 8, size: 4, note: 'next byte expected from the peer - what makes TCP reliable' },
    { name: 'offset|flags', offset: 12, size: 2, note: 'SYN, ACK, PSH, FIN, RST ... header length' },
    { name: 'window', offset: 14, size: 2, note: 'receive window - flow control' },
    { name: 'checksum', offset: 16, size: 2, note: 'mandatory for TCP' },
    { name: 'urgent ptr', offset: 18, size: 2, note: 'effectively unused' },
  ],
};

export const IPV4_HEADER = {
  name: 'IPv4 header',
  layer: 'L3',
  size: 20,
  fields: [
    { name: 'version|IHL', offset: 0, size: 1, note: '0x45 = IPv4, 5 x 32-bit words (no options)', live: () => '0x45' },
    { name: 'DSCP|ECN', offset: 1, size: 1, note: 'QoS marking - some firms prioritise market data with it' },
    { name: 'total length', offset: 2, size: 2, note: 'IP header + everything above it' },
    { name: 'identification', offset: 4, size: 2, note: 'for fragment reassembly - irrelevant at 48 bytes' },
    { name: 'flags|frag off', offset: 6, size: 2, note: 'DF bit; fragmentation is a latency disaster, never let it happen' },
    { name: 'TTL', offset: 8, size: 1, note: 'publisher --ttl; multicast TTL 1 = never leaves the subnet', live: () => 1 },
    { name: 'protocol', offset: 9, size: 1, note: '17 = UDP, 6 = TCP', live: (c) => (c.link && c.link.l4 === 'tcp' ? 6 : 17) },
    { name: 'header checksum', offset: 10, size: 2, note: 'recomputed at every router hop (TTL changes)' },
    { name: 'source IP', offset: 12, size: 4, note: 'sender interface address' },
    {
      name: 'dest IP', offset: 16, size: 4,
      note: '224.0.0.0/4 = multicast group, otherwise a host',
      live: (c) => (c.link && c.link.dstIp ? c.link.dstIp(c.config) : '-'),
    },
  ],
};

export const ETHERNET_HEADER = {
  name: 'Ethernet II header (+ 4-byte FCS trailer)',
  layer: 'L2',
  size: 14,
  trailer: 4,
  fields: [
    {
      name: 'dest MAC', offset: 0, size: 6,
      note: 'for multicast: 01:00:5e + low 23 bits of the group IP',
      live: (c) => (c.link && c.link.dstMac ? c.link.dstMac(c.config) : '-'),
    },
    { name: 'source MAC', offset: 6, size: 6, note: 'sender NIC (yours is a Hyper-V synthetic NIC)' },
    { name: 'EtherType', offset: 12, size: 2, note: '0x0800 = IPv4 follows', live: () => '0x0800' },
  ],
};

/* Bytes the PHY adds that never show up in a packet capture. */
export const PHY_OVERHEAD = {
  preamble_sfd: 8, /* 7 x 0x55 + 0xD5: clock sync */
  inter_frame_gap: 12, /* mandatory idle time between frames */
  min_frame: 64, /* Ethernet minimum (dst..FCS); short frames get padded */
};

/*
 * Multicast IP -> MAC mapping (RFC 1112). Only 23 of the 28 group bits fit,
 * so 32 different groups share each MAC - the NIC accepts them all and the
 * kernel throws the extras away. A classic interview detail.
 */
export function multicastMac(ip) {
  const parts = String(ip || '239.255.0.1').split('.').map(Number);
  if (parts.length !== 4 || parts.some((n) => !(n >= 0 && n <= 255))) return '01:00:5e:00:00:00';
  const hex = (n) => n.toString(16).padStart(2, '0');
  return ['01', '00', '5e', hex(parts[1] & 0x7f), hex(parts[2]), hex(parts[3])].join(':');
}

/* Full on-the-wire footprint of one frame carrying `payload` bytes. */
export function frameBudget(payloadBytes, l4Bytes) {
  const l2l3l4 = ETHERNET_HEADER.size + IPV4_HEADER.size + l4Bytes;
  const frame = Math.max(PHY_OVERHEAD.min_frame, l2l3l4 + payloadBytes + ETHERNET_HEADER.trailer);
  const onWire = frame + PHY_OVERHEAD.preamble_sfd + PHY_OVERHEAD.inter_frame_gap;
  return { payload: payloadBytes, headers: l2l3l4 + ETHERNET_HEADER.trailer, frame, onWire };
}

/* bits / (Gbit/s) = nanoseconds. Serialization delay is pure physics:
 * no software trick makes a 10G link clock bits out faster. */
export function serializationNs(onWireBytes, gbps) {
  return (onWireBytes * 8) / gbps;
}

/* The three links drawn in the overview, and what each one carries. */
export const LINKS = {
  feed: {
    id: 'feed',
    title: 'Market-data feed  A → B',
    from: 'A', to: 'B',
    l4: 'udp',
    app: TICK_MESSAGE,
    l4Header: UDP_HEADER,
    lesson: 'link-feed',
    dstIp: (cfg) => (cfg && cfg.transport === 'multicast' ? cfg.multicast_group : '127.0.0.1'),
    dstMac: (cfg) =>
      cfg && cfg.transport === 'multicast' ? multicastMac(cfg.multicast_group) : 'receiver NIC MAC (unicast)',
  },
  recovery: {
    id: 'recovery',
    title: 'TCP gap recovery  B → A',
    from: 'B', to: 'A',
    l4: 'tcp',
    app: SNAPSHOT_REQUEST,
    l4Header: TCP_HEADER,
    lesson: 'link-recovery',
    dstIp: () => '127.0.0.1 (publisher host)',
    dstMac: () => 'publisher NIC MAC (unicast)',
  },
  telemetry: {
    id: 'telemetry',
    title: 'Telemetry  C → browser',
    from: 'C', to: 'you',
    l4: 'tcp',
    app: WS_FRAME,
    l4Header: TCP_HEADER,
    lesson: 'link-telemetry',
    dstIp: () => 'your browser',
    dstMac: () => 'unicast',
  },
};
