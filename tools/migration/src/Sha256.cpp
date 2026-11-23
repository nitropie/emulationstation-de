//  SPDX-License-Identifier: MIT
//
//  SHA-256 implementation for the 6B staging helper. See Sha256.h.
//

#include "Sha256.h"

#include <algorithm>
#include <cstring>

namespace
{
    const std::uint32_t kConstants[64] {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
} // namespace

void MigrationSplit::Sha256::update(const unsigned char* data, std::size_t size)
{
    mLength += size;
    while (size > 0) {
        const std::size_t take {std::min(size, mChunk.size() - mUsed)};
        std::memcpy(mChunk.data() + mUsed, data, take);
        mUsed += take;
        data += take;
        size -= take;
        if (mUsed == mChunk.size()) {
            transform(mChunk.data());
            mUsed = 0;
        }
    }
}

void MigrationSplit::Sha256::update(const std::string& data)
{
    update(reinterpret_cast<const unsigned char*>(data.data()), data.size());
}

std::string MigrationSplit::Sha256::hex()
{
    const std::uint64_t bits {mLength * 8};
    const unsigned char pad {0x80};
    update(&pad, 1);
    const unsigned char zero {0};
    while (mUsed != 56)
        update(&zero, 1);
    unsigned char tail[8];
    for (int index {0}; index < 8; ++index)
        tail[index] = static_cast<unsigned char>(bits >> (56 - 8 * index));
    update(tail, 8);

    static const char* digits {"0123456789abcdef"};
    std::string result;
    result.reserve(64);
    for (std::uint32_t word : mState) {
        for (int shift {28}; shift >= 0; shift -= 4)
            result.push_back(digits[(word >> shift) & 0xF]);
    }
    return result;
}

std::uint32_t MigrationSplit::Sha256::rotate(std::uint32_t value, int bits)
{
    return (value >> bits) | (value << (32 - bits));
}

void MigrationSplit::Sha256::transform(const unsigned char* block)
{
    std::uint32_t words[64];
    for (int index {0}; index < 16; ++index) {
        words[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24) |
                       (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16) |
                       (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8) |
                       static_cast<std::uint32_t>(block[index * 4 + 3]);
    }
    for (int index {16}; index < 64; ++index) {
        const std::uint32_t s0 {rotate(words[index - 15], 7) ^
                                rotate(words[index - 15], 18) ^
                                (words[index - 15] >> 3)};
        const std::uint32_t s1 {rotate(words[index - 2], 17) ^
                                rotate(words[index - 2], 19) ^ (words[index - 2] >> 10)};
        words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }
    std::uint32_t a {mState[0]}, b {mState[1]}, c {mState[2]}, d {mState[3]};
    std::uint32_t e {mState[4]}, f {mState[5]}, g {mState[6]}, h {mState[7]};
    for (int index {0}; index < 64; ++index) {
        const std::uint32_t s1 {rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)};
        const std::uint32_t choice {(e & f) ^ (~e & g)};
        const std::uint32_t temp1 {h + s1 + choice + kConstants[index] + words[index]};
        const std::uint32_t s0 {rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)};
        const std::uint32_t majority {(a & b) ^ (a & c) ^ (b & c)};
        const std::uint32_t temp2 {s0 + majority};
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    mState[0] += a;
    mState[1] += b;
    mState[2] += c;
    mState[3] += d;
    mState[4] += e;
    mState[5] += f;
    mState[6] += g;
    mState[7] += h;
}
