/*
 * sha1.hpp - minimal SHA-1 implementation.
 *
 * Responsibility: SHA-1 is one ingredient of the RFC 6455 WebSocket
 * handshake (Sec-WebSocket-Accept = base64(sha1(key + magic GUID))) -
 * this exists purely so ws_server.hpp has no third-party dependency,
 * matching the rest of this project's "no external libraries" rule.
 * SHA-1 is cryptographically broken for anything security-sensitive;
 * it is used here ONLY because the WebSocket spec itself mandates it
 * for this one handshake step, not as a security primitive.
 *
 * Textbook implementation (FIPS 180-4), nothing hand-optimized.
 */

#ifndef PULSEFORGE_SHA1_HPP
#define PULSEFORGE_SHA1_HPP

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

inline std::array<uint8_t, 20> sha1(const std::string &input) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE,
             h3 = 0x10325476, h4 = 0xC3D2E1F0;

    std::string msg = input;
    uint64_t    bit_len = static_cast<uint64_t>(msg.size()) * 8;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) {
        msg.push_back(static_cast<char>(0x00));
    }
    for (int i = 7; i >= 0; i--) {
        msg.push_back(static_cast<char>((bit_len >> (i * 8)) & 0xff));
    }

    auto rotl = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = (static_cast<uint8_t>(msg[chunk + i * 4]) << 24) |
                   (static_cast<uint8_t>(msg[chunk + i * 4 + 1]) << 16) |
                   (static_cast<uint8_t>(msg[chunk + i * 4 + 2]) << 8) |
                   (static_cast<uint8_t>(msg[chunk + i * 4 + 3]));
        }
        for (int i = 16; i < 80; i++) {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::array<uint8_t, 20> digest{};
    uint32_t                hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; i++) {
        digest[i * 4]     = static_cast<uint8_t>((hs[i] >> 24) & 0xff);
        digest[i * 4 + 1] = static_cast<uint8_t>((hs[i] >> 16) & 0xff);
        digest[i * 4 + 2] = static_cast<uint8_t>((hs[i] >> 8) & 0xff);
        digest[i * 4 + 3] = static_cast<uint8_t>(hs[i] & 0xff);
    }
    return digest;
}

#endif /* PULSEFORGE_SHA1_HPP */
