//  SPDX-License-Identifier: MIT
//
//  SHA-256 for the 6B staging helper. No library already linked into the
//  fork exposes a SHA-256 API (libcurl and libgit2 do not; OpenSSL is only a
//  transitive dependency of those, not a declared input), so the helper
//  carries its own implementation. It binds the bytes the tool reads to the
//  digest the caller already verified; it is not an authentication scheme and
//  no secret material is involved. The standard vectors in
//  tests/MigrationSplitTests.cpp cover the padding boundaries and multi-block
//  inputs.
//

#ifndef ES_TOOLS_MIGRATION_SHA256_H
#define ES_TOOLS_MIGRATION_SHA256_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace MigrationSplit
{
    class Sha256
    {
    public:
        void update(const unsigned char* data, std::size_t size);
        void update(const std::string& data);
        void update(const char* text) { update(std::string {text}); }
        // Finishes the digest. The instance must not be reused afterwards.
        std::string hex();

    private:
        static std::uint32_t rotate(std::uint32_t value, int bits);
        void transform(const unsigned char* block);

        std::array<std::uint32_t, 8> mState {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        std::array<unsigned char, 64> mChunk {};
        std::size_t mUsed {0};
        std::uint64_t mLength {0};
    };
} // namespace MigrationSplit

#endif // ES_TOOLS_MIGRATION_SHA256_H
