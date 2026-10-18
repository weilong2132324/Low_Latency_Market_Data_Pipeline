/*
 * wire.hpp - explicit big-endian ("network byte order") serialization.
 *
 * Responsibility: every binary protocol in the trading loop (ITCH,
 * MoldUDP64, SoupBinTCP, OUCH) is a sequence of fixed-width big-endian
 * integers and space-padded ASCII fields. This header is the only code
 * that turns those bytes into numbers and back.
 *
 * Why not just memcpy a packed struct onto the wire (like message.hpp's
 * TickMessage does)?
 *   1. Endianness: x86 and ARM are little-endian; these protocols are
 *      big-endian. A struct memcpy would send the bytes backwards.
 *   2. Padding/alignment: the compiler inserts padding to align fields;
 *      a 1-byte side followed by a 4-byte qty would get 3 invisible
 *      bytes. __attribute__((packed)) removes them but then field access
 *      is unaligned, and it is compiler-specific.
 *   3. Strict aliasing: reinterpret_cast'ing a byte buffer to a struct
 *      and reading through it is undefined behaviour in C++.
 *   Shifts like (p[0] << 8) | p[1] are portable, alignment-safe and
 *   aliasing-safe - and g++ -O2 compiles get_u32 to a single MOVBE or
 *   MOV+BSWAP, so the "slow-looking" version costs nothing.
 *
 * Bounds checking: Writer/Reader never touch memory past the buffer.
 * Instead of throwing (exceptions on the hot path are a latency
 * landmine) they latch ok() == false and turn every later call into a
 * no-op, so you check once at the end: "if (!r.ok()) drop the message".
 *
 * Thread-safety: free functions are pure; a Writer/Reader instance is
 * single-threaded (it is a cursor over one buffer).
 */

#ifndef PULSEFORGE_WIRE_HPP
#define PULSEFORGE_WIRE_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace wire {

/* ---- raw put/get at a known-valid address ---------------------------- */

inline void put_u8(uint8_t *p, uint8_t v) { p[0] = v; }

inline void put_u16(uint8_t *p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

inline void put_u32(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

/* 6-byte integer: ITCH timestamps are 48-bit "ns since midnight"
 * (a day is 8.64e13 ns, which needs 47 bits - 6 bytes saves 2 bytes per
 * message versus a u64, which adds up at millions of messages/second). */
inline void put_u48(uint8_t *p, uint64_t v) {
    for (int i = 5; i >= 0; i--) {
        p[i] = static_cast<uint8_t>(v);
        v >>= 8;
    }
}

inline void put_u64(uint8_t *p, uint64_t v) {
    for (int i = 7; i >= 0; i--) {
        p[i] = static_cast<uint8_t>(v);
        v >>= 8;
    }
}

inline uint16_t get_u16(const uint8_t *p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

inline uint32_t get_u32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline uint64_t get_u48(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 6; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

inline uint64_t get_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

/* "Alpha" fields: left-justified ASCII, padded on the right with spaces
 * to a fixed width (ITCH/OUCH/SoupBinTCP all use this). */
inline void put_alpha(uint8_t *p, size_t width, std::string_view s) {
    size_t n = s.size() < width ? s.size() : width;
    std::memcpy(p, s.data(), n);
    std::memset(p + n, ' ', width - n);
}

/* View of an alpha field without the trailing pad. Points INTO the
 * buffer - valid only as long as the buffer is. */
inline std::string_view get_alpha(const uint8_t *p, size_t width) {
    size_t n = width;
    while (n > 0 && p[n - 1] == ' ') {
        --n;
    }
    return std::string_view(reinterpret_cast<const char *>(p), n);
}

/* ---- bounds-checked cursors ------------------------------------------ */

class Writer {
public:
    Writer(uint8_t *buf, size_t cap) noexcept : buf_(buf), cap_(cap) {}

    void u8(uint8_t v) { if (uint8_t *p = take(1)) put_u8(p, v); }
    void ch(char c) { u8(static_cast<uint8_t>(c)); }
    void u16(uint16_t v) { if (uint8_t *p = take(2)) put_u16(p, v); }
    void u32(uint32_t v) { if (uint8_t *p = take(4)) put_u32(p, v); }
    void u48(uint64_t v) { if (uint8_t *p = take(6)) put_u48(p, v); }
    void u64(uint64_t v) { if (uint8_t *p = take(8)) put_u64(p, v); }
    void i64(int64_t v) { u64(static_cast<uint64_t>(v)); } /* two's complement */
    void alpha(std::string_view s, size_t width) {
        if (uint8_t *p = take(width)) put_alpha(p, width, s);
    }
    void bytes(const void *src, size_t n) {
        if (uint8_t *p = take(n)) std::memcpy(p, src, n);
    }

    size_t size() const noexcept { return len_; }
    bool ok() const noexcept { return ok_; }

private:
    uint8_t *take(size_t n) {
        if (!ok_ || cap_ - len_ < n) {
            ok_ = false;
            return nullptr;
        }
        uint8_t *p = buf_ + len_;
        len_ += n;
        return p;
    }

    uint8_t *buf_;
    size_t   cap_;
    size_t   len_ = 0;
    bool     ok_  = true;
};

class Reader {
public:
    Reader(const uint8_t *buf, size_t len) noexcept : buf_(buf), len_(len) {}

    uint8_t u8() { const uint8_t *p = take(1); return p ? p[0] : 0; }
    char ch() { return static_cast<char>(u8()); }
    uint16_t u16() { const uint8_t *p = take(2); return p ? get_u16(p) : 0; }
    uint32_t u32() { const uint8_t *p = take(4); return p ? get_u32(p) : 0; }
    uint64_t u48() { const uint8_t *p = take(6); return p ? get_u48(p) : 0; }
    uint64_t u64() { const uint8_t *p = take(8); return p ? get_u64(p) : 0; }
    int64_t i64() { return static_cast<int64_t>(u64()); }
    std::string_view alpha(size_t width) {
        const uint8_t *p = take(width);
        return p ? get_alpha(p, width) : std::string_view();
    }
    void bytes(void *dst, size_t n) {
        if (const uint8_t *p = take(n)) std::memcpy(dst, p, n);
    }
    void skip(size_t n) { (void)take(n); }

    size_t remaining() const noexcept { return ok_ ? len_ - pos_ : 0; }
    bool ok() const noexcept { return ok_; }

private:
    const uint8_t *take(size_t n) {
        if (!ok_ || len_ - pos_ < n) {
            ok_ = false;
            return nullptr;
        }
        const uint8_t *p = buf_ + pos_;
        pos_ += n;
        return p;
    }

    const uint8_t *buf_;
    size_t         len_;
    size_t         pos_ = 0;
    bool           ok_  = true;
};

} // namespace wire

#endif /* PULSEFORGE_WIRE_HPP */
