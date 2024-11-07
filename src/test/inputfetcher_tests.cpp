// Copyright (c) 2024-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <common/system.h>
#include <inputfetcher.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/transaction_identifier.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_set>

BOOST_AUTO_TEST_SUITE(inputfetcher_tests)

struct InputFetcherTest : BasicTestingSetup {
    CBlock CreateBlock(int32_t num_txs)
    {
        CBlock block;
        CMutableTransaction coinbase;
        coinbase.vin.emplace_back();
        block.vtx.push_back(MakeTransactionRef(coinbase));

        Txid prevhash{Txid::FromUint256(uint256(1))};

        for (auto i{1}; i < num_txs; ++i) {
            CMutableTransaction tx;
            const auto txid{m_rng.randbool() ? Txid::FromUint256(uint256(i)) : prevhash};
            tx.vin.emplace_back(COutPoint(txid, 0));
            prevhash = tx.GetHash();
            block.vtx.push_back(MakeTransactionRef(tx));
        }

        return block;
    }
};

BOOST_FIXTURE_TEST_CASE(fetch_inputs, InputFetcherTest)
{
    SeedRandomForTest(SeedRand::ZEROS);

    const auto cores{GetNumCores()};
    const auto num_txs{m_rng.randrange(cores * 10)};
    const auto block{CreateBlock(num_txs)};
    const auto batch_size{m_rng.randrange<int32_t>(block.vtx.size() * 2)};
    const auto worker_threads{m_rng.randrange(cores * 2)};
    InputFetcher fetcher{batch_size, worker_threads};

    for (auto i{0}; i < 3; ++i) {
        CCoinsView dummy;
        CCoinsViewCache db(&dummy);

        for (const auto& tx : block.vtx) {
            for (const auto& in : tx->vin) {
                auto outpoint{in.prevout};
                Coin coin{};
                coin.out.nValue = 1;
                db.EmplaceCoinInternalDANGER(std::move(outpoint), std::move(coin));
            }
        }

        CCoinsViewCache cache(&db);
        fetcher.FetchInputs(cache, db, block);

        std::unordered_set<Txid, SaltedTxidHasher> txids{};
        txids.reserve(block.vtx.size() - 1);

        for (const auto& tx : block.vtx) {
            if (tx->IsCoinBase()) {
                BOOST_CHECK(!cache.HaveCoinInCache(tx->vin[0].prevout));
            } else {
                for (const auto& in : tx->vin) {
                    const auto& outpoint{in.prevout};
                    const auto have{cache.HaveCoinInCache(outpoint)};
                    const auto should_have{!txids.contains(outpoint.hash) && worker_threads > 1};
                    BOOST_CHECK(should_have ? have : !have);
                }
                txids.emplace(tx->GetHash());
            }
        }
    }
}

// Test for the case where a block spends coins that are spent in the cache, but
// the spentness has not been flushed to the db. So the input fetcher will fetch
// the coin from the db since HaveCoinInCache will return false for an existing
// but spent coin. However, the fetched coin will fail to be inserted into the
// cache because the emplace call in EmplaceCoinInternalDANGER will not insert
// the unspent coin due to the collision with the already spent coin in the map.
BOOST_FIXTURE_TEST_CASE(fetch_no_double_spend, InputFetcherTest)
{
    SeedRandomForTest(SeedRand::ZEROS);

    const auto cores{GetNumCores()};
    const auto num_txs{m_rng.randrange(cores * 10)};
    const auto block{CreateBlock(num_txs)};
    const auto batch_size{m_rng.randrange<int32_t>(block.vtx.size() * 2)};
    const auto worker_threads{m_rng.randrange(cores * 2)};
    InputFetcher fetcher{batch_size, worker_threads};

    for (auto i{0}; i < 3; ++i) {
        CCoinsView dummy;
        CCoinsViewCache db(&dummy);

        for (const auto& tx : block.vtx) {
            for (const auto& in : tx->vin) {
                auto outpoint{in.prevout};
                Coin coin{};
                coin.out.nValue = 1;
                db.EmplaceCoinInternalDANGER(std::move(outpoint), std::move(coin));
            }
        }

        CCoinsViewCache cache(&db);

        // Add all inputs as spent already in cache
        for (const auto& tx : block.vtx) {
            for (const auto& in : tx->vin) {
                auto outpoint{in.prevout};
                Coin coin{};
                cache.EmplaceCoinInternalDANGER(std::move(outpoint), std::move(coin));
            }
        }

        fetcher.FetchInputs(cache, db, block);

        // Coins are still spent, even though they exist unspent in the parent db
        for (const auto& tx : block.vtx) {
            for (const auto& in : tx->vin) {
                BOOST_CHECK(!cache.HaveCoinInCache(in.prevout));
            }
        }
    }
}

BOOST_FIXTURE_TEST_CASE(fetch_no_inputs, InputFetcherTest)
{
    SeedRandomForTest(SeedRand::ZEROS);

    const auto cores{GetNumCores()};
    const auto num_txs{m_rng.randrange(cores * 10)};
    const auto block{CreateBlock(num_txs)};
    const auto batch_size{m_rng.randrange<int32_t>(block.vtx.size() * 2)};
    const auto worker_threads{m_rng.randrange(cores * 2)};
    InputFetcher fetcher{batch_size, worker_threads};

    for (auto i{0}; i < 3; ++i) {
        CCoinsView db;
        CCoinsViewCache cache(&db);
        fetcher.FetchInputs(cache, db, block);

        for (const auto& tx : block.vtx) {
            for (const auto& in : tx->vin) {
                BOOST_CHECK(!cache.HaveCoinInCache(in.prevout));
            }
        }
    }
}

class ThrowCoinsView : public CCoinsView
{
    std::optional<Coin> GetCoin(const COutPoint& outpoint) const override
    {
        throw std::runtime_error("database error");
    }
};

BOOST_FIXTURE_TEST_CASE(fetch_input_exceptions, InputFetcherTest)
{
    SeedRandomForTest(SeedRand::ZEROS);

    const auto cores{GetNumCores()};
    const auto num_txs{m_rng.randrange(cores * 10)};
    const auto block{CreateBlock(num_txs)};
    const auto batch_size{m_rng.randrange<int32_t>(block.vtx.size() * 2)};
    const auto worker_threads{m_rng.randrange(cores * 2)};
    InputFetcher fetcher{batch_size, worker_threads};

    for (auto i{0}; i < 3; ++i) {
        ThrowCoinsView db;
        CCoinsViewCache cache(&db);
        fetcher.FetchInputs(cache, db, block);

        for (const auto& tx : block.vtx) {
            for (const auto& in : tx->vin) {
                BOOST_CHECK(!cache.HaveCoinInCache(in.prevout));
            }
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
