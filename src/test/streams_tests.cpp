// Copyright (c) 2012-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <streams.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <util/fs.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

using namespace std::string_literals;

BOOST_FIXTURE_TEST_SUITE(streams_tests, BasicTestingSetup)

inline void Xor2(std::span<std::byte> write, const uint64_t key)
{
    assert(key);
    for (uint64_t raw; write.size();) {
        const auto chunk_size = std::min(size_t{8}, write.size());
        memcpy(&raw, write.data(), chunk_size);
        raw ^= key;
        memcpy(write.data(), &raw, chunk_size);
        write = write.subspan(chunk_size);
    }
}

inline void Xor2(std::span<std::byte> write, const uint64_t key, const size_t key_offset)
{
    if (key == 0) return;

    size_t key_rotation = 8 * key_offset;
    if constexpr (std::endian::native == std::endian::big) key_rotation *= -1;

    Xor2(write, std::rotr(key, key_rotation));
}

BOOST_AUTO_TEST_CASE(xor_roundtrip_random_chunks)
{
    auto apply_random_xor_chunks{[](std::span<std::byte> write, const uint64_t key, FastRandomContext& rng) {
        for (size_t offset{0}; offset < write.size();) {
            const size_t chunk_size{1 + rng.randrange(write.size() - offset)};
            Xor2(write.subspan(offset, chunk_size), key, offset);
            offset += chunk_size;
        }
    }};

    FastRandomContext rng{/*fDeterministic=*/false};
    for (size_t test{0}; test < 100; ++test) {
        const size_t write_size{1 + rng.randrange(100U)};
        const std::vector original{rng.randbytes<std::byte>(write_size)};
        std::vector roundtrip{original};

        std::vector key_vector{rng.randbytes<std::byte>(8)};
        uint64_t key;
        std::memcpy(&key, key_vector.data(), 8);

        apply_random_xor_chunks(roundtrip, key, rng);
        BOOST_CHECK_EQUAL(original != roundtrip, key != 0);
        apply_random_xor_chunks(roundtrip, key, rng);
        BOOST_CHECK(original == roundtrip);
    }
}

BOOST_AUTO_TEST_CASE(xor_bytes_reference)
{
    auto expected_xor{[](std::span<std::byte> write, const std::span<const std::byte> key, size_t key_offset) {
        for (auto& b : write) {
            b ^= key[key_offset++ % key.size()];
        }
    }};

    FastRandomContext rng{/*fDeterministic=*/false};
    for (size_t test{0}; test < 100; ++test) {
        const size_t write_size{1 + rng.randrange(100U)};
        const size_t key_offset{rng.randrange(3 * 8U)}; // Should wrap around

        std::vector key_vector{rng.randbytes<std::byte>(8)};
        uint64_t key;
        std::memcpy(&key, key_vector.data(), 8);

        std::vector expected{rng.randbytes<std::byte>(write_size)};
        std::vector actual{expected};

        expected_xor(expected, key_vector, key_offset);
        Xor2(actual, key, key_offset);

        BOOST_CHECK_EQUAL_COLLECTIONS(expected.begin(), expected.end(), actual.begin(), actual.end());
    }
}

BOOST_AUTO_TEST_SUITE_END()
