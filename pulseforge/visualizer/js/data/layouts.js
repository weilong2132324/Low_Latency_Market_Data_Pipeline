/*
 * layouts.js - the in-memory layout of PulseForge's hottest C++ structs,
 * byte for byte, as the machine view's "cache line" level draws them.
 *
 * Why this file exists: the CPU never moves single bytes between cores -
 * it moves 64-byte cache lines. Whether two threads' data share a line
 * decides whether they run independently or ping-pong that line between
 * cores on every write (false sharing, ~40-100 ns per bounce). These
 * layouts are copied from the real headers; if you change a struct in
 * C++, change it here too (the static_asserts in the headers will catch
 * the C++ side).
 *
 * field.writer tags who writes it - the whole false-sharing story is
 * "are there two different writers on one 64-byte line?"
 */

export const CACHE_LINE = 64;

/* include/stats.hpp - deliberately padded into one line per writer. */
export const SHARED_STATS = {
  id: 'SharedStats',
  file: 'include/stats.hpp',
  size: 128,
  fields: [
    { name: 'received', offset: 0, size: 8, writer: 'receiver' },
    { name: 'invalid', offset: 8, size: 8, writer: 'receiver' },
    { name: 'sequence_gaps', offset: 16, size: 8, writer: 'receiver' },
    { name: 'reorders', offset: 24, size: 8, writer: 'receiver' },
    { name: 'queue_full', offset: 32, size: 8, writer: 'receiver' },
    { name: 'recovered', offset: 40, size: 8, writer: 'receiver' },
    { name: '_pad1[2]', offset: 48, size: 16, writer: 'pad' },
    { name: 'processed', offset: 64, size: 8, writer: 'workers' },
    { name: 'latency_total_ns', offset: 72, size: 8, writer: 'workers' },
    { name: 'latency_max_ns', offset: 80, size: 8, writer: 'workers' },
    { name: 'queue_depth', offset: 88, size: 8, writer: 'both' },
    { name: '_pad2[4]', offset: 96, size: 32, writer: 'pad' },
  ],
};

/* The same counters with the padding deleted - what false sharing looks
 * like. Toggle this in the cache-line view. */
export const SHARED_STATS_UNPADDED = {
  id: 'SharedStats (no padding - broken)',
  file: 'include/stats.hpp (hypothetical)',
  size: 80,
  fields: [
    { name: 'received', offset: 0, size: 8, writer: 'receiver' },
    { name: 'invalid', offset: 8, size: 8, writer: 'receiver' },
    { name: 'sequence_gaps', offset: 16, size: 8, writer: 'receiver' },
    { name: 'reorders', offset: 24, size: 8, writer: 'receiver' },
    { name: 'queue_full', offset: 32, size: 8, writer: 'receiver' },
    { name: 'recovered', offset: 40, size: 8, writer: 'receiver' },
    { name: 'processed', offset: 48, size: 8, writer: 'workers' },
    { name: 'latency_total_ns', offset: 56, size: 8, writer: 'workers' },
    { name: 'latency_max_ns', offset: 64, size: 8, writer: 'workers' },
    { name: 'queue_depth', offset: 72, size: 8, writer: 'both' },
  ],
};

/* include/spsc_ring.hpp - alignas(64) puts head_ and tail_ on their own
 * lines: the consumer owns one, the producer owns the other. */
export const SPSC_RING = {
  id: 'SpscRing',
  file: 'include/spsc_ring.hpp',
  size: 192,
  fields: [
    { name: 'head_ (atomic)', offset: 0, size: 8, writer: 'workers' },
    { name: '_pad1[56]', offset: 8, size: 56, writer: 'pad' },
    { name: 'tail_ (atomic)', offset: 64, size: 8, writer: 'receiver' },
    { name: '_pad2[56]', offset: 72, size: 56, writer: 'pad' },
    { name: 'closed_ (atomic)', offset: 128, size: 1, writer: 'receiver' },
    { name: '(alignment)', offset: 129, size: 63, writer: 'pad' },
  ],
};

/* include/message.hpp packed back-to-back in a ring buffer (std::vector).
 * 40 does not divide 64, so messages straddle cache lines. */
export function tickMessageRing(count = 8) {
  const fields = [];
  for (let i = 0; i < count; i++) {
    fields.push({ name: `msg[${i}]`, offset: i * 40, size: 40, writer: i % 2 ? 'receiver' : 'receiver-alt', msg: i });
  }
  return { id: 'TickMessage x8 in a ring', file: 'include/message.hpp', size: count * 40, fields };
}

/* include/shared_events.hpp - 32-byte Event, two per line, and several
 * writer threads append into neighbouring slots. False sharing by design
 * (it's the cold telemetry path, and fixing it would cost 2x memory). */
export function eventRing(count = 6) {
  const fields = [];
  const writers = ['receiver', 'workers', 'receiver', 'workers', 'workers', 'receiver'];
  for (let i = 0; i < count; i++) {
    fields.push({ name: `events[${i}]`, offset: i * 32, size: 32, writer: writers[i % writers.length] });
  }
  return { id: 'Event x6 in SharedEventRing', file: 'include/shared_events.hpp', size: count * 32, fields };
}

/* Which cache lines does a field touch? (A field that straddles two
 * lines costs two line fills to read.) */
export function linesTouched(field) {
  const first = Math.floor(field.offset / CACHE_LINE);
  const last = Math.floor((field.offset + field.size - 1) / CACHE_LINE);
  return { first, last, straddles: first !== last };
}

/* For each cache line: the set of distinct writers. >1 = false sharing. */
export function writersPerLine(layout) {
  const lines = Math.ceil(layout.size / CACHE_LINE);
  const result = [];
  for (let l = 0; l < lines; l++) {
    const writers = new Set();
    for (const f of layout.fields) {
      if (f.writer === 'pad') continue;
      const t = linesTouched(f);
      if (l >= t.first && l <= t.last) {
        writers.add(f.writer === 'receiver-alt' ? 'receiver' : f.writer === 'both' ? 'receiver+workers' : f.writer);
      }
    }
    const flat = new Set([...writers].flatMap((w) => w.split('+')));
    result.push({ line: l, writers: [...flat], falseSharing: flat.size > 1 });
  }
  return result;
}
