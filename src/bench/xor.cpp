// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <bench/bench.h>
#include <random.h>
#include <span.h>
#include <streams.h>

#include <cstddef>
#include <vector>

static void XorSmall(benchmark::Bench& bench)
{
    FastRandomContext frc{/*fDeterministic=*/true};
    auto data1{frc.randbytes<std::byte>(1)};
    auto data2{frc.randbytes<std::byte>(4)};
    auto key{frc.rand64()};
    std::vector<std::byte> key_bytes(8);
    std::memcpy(&key, key_bytes.data(), 8);

    bench.batch(data1.size() + data2.size()).unit("byte").run([&] {
        util::Xor(data1, key_bytes);
        ankerl::nanobench::doNotOptimizeAway(data1);
        util::Xor(data2, key_bytes);
        ankerl::nanobench::doNotOptimizeAway(data2);
    });
}

static void Xor(benchmark::Bench& bench)
{
    FastRandomContext frc{/*fDeterministic=*/true};
    auto data{frc.randbytes<std::byte>(1004)};
    auto key{frc.rand64()};
    std::vector<std::byte> key_bytes(8);
    std::memcpy(&key, key_bytes.data(), 8);

    bench.batch(data.size() * 10).unit("byte").run([&] {
        for (size_t offset = 0; offset < 10; ++offset) {
            util::Xor(data, key_bytes, offset);
        }
        ankerl::nanobench::doNotOptimizeAway(data);
    });
}

static void AutoFileXor(benchmark::Bench& bench)
{
    FastRandomContext frc{/*fDeterministic=*/true};
    auto data{frc.randbytes<std::byte>(4'000'000)};
    auto key{frc.rand64()};
    std::vector<std::byte> key_bytes(8);
    std::memcpy(&key, key_bytes.data(), 8);

    const fs::path test_path = fs::temp_directory_path() / "xor_benchmark.dat";
    AutoFile f{fsbridge::fopen(test_path, "wb+"), key_bytes};
    bench.batch(data.size()).unit("byte").run([&] {
        f.Truncate(0);
        f << data; // xor through serialization
    });
    try { fs::remove(test_path); } catch (const fs::filesystem_error&) {}
}

BENCHMARK(XorSmall, benchmark::PriorityLevel::HIGH);
BENCHMARK(Xor, benchmark::PriorityLevel::HIGH);
BENCHMARK(AutoFileXor, benchmark::PriorityLevel::HIGH);
