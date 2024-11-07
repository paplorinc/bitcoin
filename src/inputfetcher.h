// Copyright (c) 2024-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INPUTFETCHER_H
#define BITCOIN_INPUTFETCHER_H

#include <coins.h>
#include <sync.h>
#include <tinyformat.h>
#include <txdb.h>
#include <util/hasher.h>
#include <util/threadnames.h>
#include <util/transaction_identifier.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <unordered_set>
#include <vector>

/**
 * Input fetcher for fetching inputs from the CoinsDB and inserting
 * into the CoinsTip.
 *
 * The main thread pushes batches of outpoints
 * onto the queue, where they are fetched by N worker threads. The resulting
 * coins are pushed onto another queue after they are read from disk. When
 * the main is done adding outpoints, it starts writing the results of the read
 * queue to the cache.
 */
class InputFetcher
{
private:
    //! Mutex to protect the inner state
    Mutex m_mutex{};
    //! Worker threads block on this when out of work
    std::condition_variable m_worker_cv{};
    //! Main thread blocks on this when out of work
    std::condition_variable m_main_cv{};
    //! The queue of outpoints to be fetched from disk.
    //! As the order of outpoints doesn't matter, it is used as a LIFO (stack)
    std::vector<COutPoint> m_outpoints GUARDED_BY(m_mutex){};
    //! The queue of pairs to be written to the cache.
    std::vector<std::pair<COutPoint, Coin>> m_pairs GUARDED_BY(m_mutex){};
    /**
     * Number of outpoint fetches that haven't completed yet.
     * This includes outpoints that are no longer queued, but still in the
     * worker's own batches.
     */
    int32_t m_in_flight_fetches_count GUARDED_BY(m_mutex){0};
    //! The number of worker threads that are waiting on m_worker_cv
    int32_t m_idle_worker_count GUARDED_BY(m_mutex){0};
    //! The maximum number of outpoints to be processed in one batch
    const int32_t m_batch_size;
    //! DB coins view to fetch from.
    const CCoinsView* m_db{nullptr};

    std::vector<std::thread> m_worker_threads;
    bool m_request_stop GUARDED_BY(m_mutex){false};

    //! Internal function that does the fetching from disk.
    void Loop() noexcept EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        int32_t local_in_flight_fetches_count{0};
        std::vector<std::pair<COutPoint, Coin>> local_pairs{};
        std::vector<COutPoint> local_outpoints{};
        local_outpoints.reserve(m_batch_size);
        do {
            local_outpoints.clear();
            {
                WAIT_LOCK(m_mutex, lock);
                // first do the clean-up of the previous loop run (allowing us to do
                // it in the same critsect) in_flight_fetches_count will only be
                // truthy after first run.
                if (local_in_flight_fetches_count) {
                    if (m_pairs.empty()) {
                        m_pairs = std::move(local_pairs);
                    } else {
                        m_pairs.reserve(m_pairs.size() + local_pairs.size());
                        m_pairs.insert(m_pairs.end(),
                                       std::make_move_iterator(local_pairs.begin()),
                                       std::make_move_iterator(local_pairs.end()));
                    }
                    m_in_flight_fetches_count -= local_in_flight_fetches_count;
                    m_main_cv.notify_one();
                }

                // logically, the do loop starts here
                while (m_outpoints.empty()) {
                    if (m_request_stop) {
                        return;
                    }
                    ++m_idle_worker_count;
                    m_worker_cv.wait(lock);
                    --m_idle_worker_count;
                }

                // Assign a batch of outpoints to this thread, and erase them
                // from the global queue
                local_in_flight_fetches_count =
                    std::max(1,
                             std::min(m_batch_size,
                                      static_cast<int32_t>(m_outpoints.size() /
                                                            (m_worker_threads.size() +
                                                             m_idle_worker_count))));
                const auto start_it{m_outpoints.end() - local_in_flight_fetches_count};
                local_outpoints.assign(std::make_move_iterator(start_it),
                                       std::make_move_iterator(m_outpoints.end()));
                m_outpoints.erase(start_it, m_outpoints.end());
            }

            local_pairs.clear();
            local_pairs.reserve(local_outpoints.size());
            try {
                for (const COutPoint& outpoint : local_outpoints) {
                    if (auto coin{m_db->GetCoin(outpoint)}; coin) {
                        local_pairs.emplace_back(outpoint, std::move(*coin));
                    } else {
                        // Missing an input, just break. This block will fail
                        // validation, so no point in continuing to get coins.
                        break;
                    }
                }
            } catch (const std::runtime_error& e) {
                // Database error
                // This will be handled later in validation.
                // Continue for now so the main thread can proceed.
            }
        } while (true);
    }

    //! Add a batch of outpoints to the queue
    void Add(std::vector<COutPoint>& outpoints) noexcept
        EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        if (outpoints.empty()) {
            return;
        }
        const auto size{outpoints.size()};
        {
            LOCK(m_mutex);
            m_in_flight_fetches_count += size;
            if (m_outpoints.empty()) {
                m_outpoints = std::move(outpoints);
            } else {
                m_outpoints.reserve(m_outpoints.size() + size);
                m_outpoints.insert(m_outpoints.end(),
                                   std::make_move_iterator(outpoints.begin()),
                                   std::make_move_iterator(outpoints.end()));
            }
        }
        if (size == 1) {
            m_worker_cv.notify_one();
        } else {
            m_worker_cv.notify_all();
        }
    }

public:

    //! Create a new input fetcher
    explicit InputFetcher(int32_t batch_size, int32_t worker_thread_count) noexcept
        : m_batch_size(batch_size)
    {
        if (worker_thread_count < 2) {
            // Don't do anything if there are less than 2 worker threads.
            // It is faster to lookup missing inputs single threaded
            // instead of queuing them up to 1 thread, and if there are no
            // worker threads nothing will fetch the inputs and we will hang.
            return;
        }
        m_worker_threads.reserve(worker_thread_count);
        for (auto n{0}; n < worker_thread_count; ++n) {
            m_worker_threads.emplace_back([this, n]() {
                util::ThreadRename(strprintf("inputfetch.%i", n));
                Loop();
            });
        }
    }

    // Since this class manages its own resources, which is a thread
    // pool `m_worker_threads`, copy and move operations are not appropriate.
    InputFetcher(const InputFetcher&) = delete;
    InputFetcher& operator=(const InputFetcher&) = delete;
    InputFetcher(InputFetcher&&) = delete;
    InputFetcher& operator=(InputFetcher&&) = delete;

    //! Fetch all block inputs from db, and insert into cache.
    void FetchInputs(CCoinsViewCache& cache,
                     const CCoinsView& db,
                     const CBlock& block) noexcept
        EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        if (m_worker_threads.empty()) {
            return;
        }

        // Set the db to use for this block.
        m_db = &db;

        std::vector<COutPoint> buffer{};
        const auto buffer_size{m_worker_threads.size()};
        buffer.reserve(buffer_size);

        std::unordered_set<Txid, SaltedTxidHasher> txids{};
        txids.reserve(block.vtx.size() - 1);

        // Loop through the inputs of the block and add them to the queue
        for (const auto& tx : block.vtx) {
            if (tx->IsCoinBase()) {
                continue;
            }

            for (const auto& in : tx->vin) {
                const auto& outpoint{in.prevout};
                // If an input spends an outpoint from earlier in the
                // block, it won't be in the cache yet but it also won't be
                // in the db either.
                if (txids.contains(outpoint.hash)) {
                    continue;
                }
                if (cache.HaveCoinInCache(outpoint)) {
                    continue;
                }
                buffer.emplace_back(outpoint);
                if (buffer.size() == buffer_size) {
                    Add(buffer);
                    buffer.clear();
                    buffer.reserve(buffer_size);
                }
            }
            txids.emplace(tx->GetHash());
        }
        Add(buffer);

        // Insert fetched coins into the cache
        std::vector<std::pair<COutPoint, Coin>> local_pairs{};
        do {
            {
                WAIT_LOCK(m_mutex, lock);
                while (m_pairs.empty()) {
                    if (m_in_flight_fetches_count == 0) {
                        return;
                    }
                    m_main_cv.wait(lock);
                }
                local_pairs = std::move(m_pairs);
                m_pairs.clear();
            }
            for (auto&& [outpoint, coin] : local_pairs) {
                cache.EmplaceCoinInternalDANGER(std::move(outpoint),
                                                std::move(coin),
                                                /*set_dirty=*/false);
            }
        } while (true);
    }

    ~InputFetcher()
    {
        WITH_LOCK(m_mutex, m_request_stop = true);
        m_worker_cv.notify_all();
        for (std::thread& t : m_worker_threads) {
            t.join();
        }
    }
};

#endif // BITCOIN_INPUTFETCHER_H
