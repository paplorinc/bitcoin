// Copyright (c) 2012-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <dbwrapper.h>

#include <logging.h>
#include <random.h>
#include <serialize.h>
#include <span.h>
#include <streams.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/strencodings.h>

#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/table.h"
#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <utility>

static auto CharCast(const std::byte* data) { return reinterpret_cast<const char*>(data); }

bool DestroyDB(const std::string& path_str)
{
    return rocksdb::DestroyDB(path_str, {}).ok();
}

/** Handle database error by throwing dbwrapper_error exception.
 */
static void HandleError(const rocksdb::Status& status)
{
    if (status.ok())
        return;
    const std::string errmsg = "Fatal RocksDB error: " + status.ToString();
    LogPrintf("%s\n", errmsg);
    LogPrintf("You can use -debug=rocksdb to get more complete diagnostic messages\n");
    throw dbwrapper_error(errmsg);
}

class CBitcoinRocksDBLogger : public rocksdb::Logger
{
public:
    // This code is adapted from posix_logger.h, which is why it is using vsprintf.
    // Please do not do this in normal code
    void Logv(const char * format, va_list ap) override {
        if (!LogAcceptCategory(BCLog::ROCKSDB, BCLog::Level::Debug)) {
            return;
            }
            char buffer[500];
            for (int iter = 0; iter < 2; iter++) {
                char* base;
                int bufsize;
                if (iter == 0) {
                    bufsize = sizeof(buffer);
                    base = buffer;
                }
                else {
                    bufsize = 30000;
                    base = new char[bufsize];
                }
                char* p = base;
                char* limit = base + bufsize;

                // Print the message
                if (p < limit) {
                    va_list backup_ap;
                    va_copy(backup_ap, ap);
                    // Do not use vsnprintf elsewhere in bitcoin source code, see above.
                    p += vsnprintf(p, limit - p, format, backup_ap);
                    va_end(backup_ap);
                }

                // Truncate to available space if necessary
                if (p >= limit) {
                    if (iter == 0) {
                        continue;       // Try again with larger buffer
                    }
                    else {
                        p = limit - 1;
                    }
                }

                // Add newline if necessary
                if (p == base || p[-1] != '\n') {
                    *p++ = '\n';
                }

                assert(p <= limit);
                base[std::min(bufsize - 1, (int)(p - base))] = '\0';
                LogDebug(BCLog::ROCKSDB, "%s\n", util::RemoveSuffixView(base, "\n"));
                if (base != buffer) {
                    delete[] base;
                }
                break;
            }
    }
};

static void SetMaxOpenFiles(rocksdb::Options* options)
{
    // On most platforms the default setting of max_open_files (which is 1000)
    // is optimal. On Windows using a large file count is OK because the handles
    // do not interfere with select() loops. On 64-bit Unix hosts this value is
    // also OK, because up to that amount RocksDB will use an mmap
    // implementation that does not use extra file descriptors (the fds are
    // closed after being mmap'ed).
    //
    // Increasing the value beyond the default is dangerous because RocksDB will
    // fall back to a non-mmap implementation when the file count is too large.
    // On 32-bit Unix host we should decrease the value because the handles use
    // up real fds, and we want to avoid fd exhaustion issues.
    //
    // See PR #12495 for further discussion.

    int default_open_files = options->max_open_files;
#ifndef WIN32
    if (sizeof(void*) < 8) {
        options->max_open_files = 64;
    }
#endif
    LogDebug(BCLog::ROCKSDB, "RocksDB using max_open_files=%d (default=%d)\n",
             options->max_open_files, default_open_files);
}

static rocksdb::Options GetOptions(size_t nCacheSize)
{
    rocksdb::Options options;
    // Set up BlockBasedTableOptions
    rocksdb::BlockBasedTableOptions table_options;
    table_options.block_cache = rocksdb::NewLRUCache(nCacheSize / 2);
    table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
    options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

    options.write_buffer_size = nCacheSize / 4;
    options.compression = rocksdb::kNoCompression;
    options.info_log = std::make_shared<CBitcoinRocksDBLogger>();
    options.paranoid_checks = true;
    SetMaxOpenFiles(&options);
    return options;
}

struct CDBBatch::WriteBatchImpl {
    rocksdb::WriteBatch batch;
};

CDBBatch::CDBBatch(const CDBWrapper& _parent)
    : parent{_parent},
      m_impl_batch{std::make_unique<CDBBatch::WriteBatchImpl>()} {};

CDBBatch::~CDBBatch() = default;

void CDBBatch::Clear()
{
    m_impl_batch->batch.Clear();
    size_estimate = 0;
}

void CDBBatch::WriteImpl(Span<const std::byte> key, DataStream& ssValue)
{
    rocksdb::Slice slKey(CharCast(key.data()), key.size());
    ssValue.Xor(dbwrapper_private::GetObfuscateKey(parent));
    rocksdb::Slice slValue(CharCast(ssValue.data()), ssValue.size());
    m_impl_batch->batch.Put(slKey, slValue);
    // RocksDB serializes writes as:
    // - byte: header
    // - varint: key length (1 byte up to 127B, 2 bytes up to 16383B, ...)
    // - byte[]: key
    // - varint: value length
    // - byte[]: value
    // The formula below assumes the key and value are both less than 16k.
    size_estimate += 3 + (slKey.size() > 127) + slKey.size() + (slValue.size() > 127) + slValue.size();
}

void CDBBatch::EraseImpl(Span<const std::byte> key)
{
    rocksdb::Slice slKey(CharCast(key.data()), key.size());
    m_impl_batch->batch.Delete(slKey);
    // RocksDB serializes erases as:
    // - byte: header
    // - varint: key length
    // - byte[]: key
    // The formula below assumes the key is less than 16kB.
    size_estimate += 2 + (slKey.size() > 127) + slKey.size();
}

struct RocksDBContext {
    //! custom environment this database is using (may be nullptr in case of default environment)
    rocksdb::Env* penv;

    //! database options used
    rocksdb::Options options;

    //! options used when reading from the database
    rocksdb::ReadOptions readoptions;

    //! options used when iterating over values of the database
    rocksdb::ReadOptions iteroptions;

    //! options used when writing to the database
    rocksdb::WriteOptions writeoptions;

    //! options used when sync writing to the database
    rocksdb::WriteOptions syncoptions;

    //! the database itself
    rocksdb::DB* pdb;
};

CDBWrapper::CDBWrapper(const DBParams& params)
    : m_db_context{std::make_unique<RocksDBContext>()}, m_name{fs::PathToString(params.path.stem())}, m_path{params.path}, m_is_memory{params.memory_only}
{
    DBContext().penv = nullptr;
    DBContext().readoptions.verify_checksums = true;
    DBContext().iteroptions.verify_checksums = true;
    DBContext().iteroptions.fill_cache = false;
    DBContext().syncoptions.sync = true;
    DBContext().options = GetOptions(params.cache_bytes);
    DBContext().options.create_if_missing = true;
    if (params.memory_only) {
        DBContext().penv = rocksdb::NewMemEnv(rocksdb::Env::Default());
        DBContext().options.env = DBContext().penv;
    } else {
        if (params.wipe_data) {
            LogPrintf("Wiping RocksDB in %s\n", fs::PathToString(params.path));
            rocksdb::Status result = rocksdb::DestroyDB(fs::PathToString(params.path), DBContext().options);
            HandleError(result);
        }
        TryCreateDirectories(params.path);
        LogPrintf("Opening RocksDB in %s\n", fs::PathToString(params.path));
    }
    // PathToString() return value is safe to pass to rocksdb open function,
    // because on POSIX rocksdb passes the byte string directly to ::open(), and
    // on Windows it converts from UTF-8 to UTF-16 before calling ::CreateFileW
    // (see env_posix.cc and env_windows.cc).
    rocksdb::Status status = rocksdb::DB::Open(DBContext().options, fs::PathToString(params.path), &DBContext().pdb);
    HandleError(status);
    LogPrintf("Opened RocksDB successfully\n");

    if (params.options.force_compact) {
        LogPrintf("Starting database compaction of %s\n", fs::PathToString(params.path));
        rocksdb::CompactRangeOptions compact_options;
        DBContext().pdb->CompactRange(compact_options, nullptr, nullptr);
        LogPrintf("Finished database compaction of %s\n", fs::PathToString(params.path));
    }

    // The base-case obfuscation key, which is a noop.
    obfuscate_key = std::vector<unsigned char>(OBFUSCATE_KEY_NUM_BYTES, '\000');

    bool key_exists = Read(OBFUSCATE_KEY_KEY, obfuscate_key);

    if (!key_exists && params.obfuscate && IsEmpty()) {
        // Initialize non-degenerate obfuscation if it won't upset
        // existing, non-obfuscated data.
        std::vector<unsigned char> new_key = CreateObfuscateKey();

        // Write `new_key` so we don't obfuscate the key with itself
        Write(OBFUSCATE_KEY_KEY, new_key);
        obfuscate_key = new_key;

        LogPrintf("Wrote new obfuscate key for %s: %s\n", fs::PathToString(params.path), HexStr(obfuscate_key));
    }

    LogPrintf("Using obfuscation key for %s: %s\n", fs::PathToString(params.path), HexStr(obfuscate_key));
}

CDBWrapper::~CDBWrapper()
{
    delete DBContext().pdb;
    DBContext().pdb = nullptr;

    if (DBContext().penv != nullptr) {
        delete DBContext().penv;
        DBContext().penv = nullptr;
    }
}

bool CDBWrapper::WriteBatch(CDBBatch& batch, bool fSync)
{
    const bool log_memory = LogAcceptCategory(BCLog::ROCKSDB, BCLog::Level::Debug);
    double mem_before = 0;
    if (log_memory) {
        mem_before = DynamicMemoryUsage() / 1024.0 / 1024;
    }
    rocksdb::Status status = DBContext().pdb->Write(fSync ? DBContext().syncoptions : DBContext().writeoptions, &batch.m_impl_batch->batch);
    HandleError(status);
    if (log_memory) {
        double mem_after = DynamicMemoryUsage() / 1024.0 / 1024;
        LogDebug(BCLog::ROCKSDB, "WriteBatch memory usage: db=%s, before=%.1fMiB, after=%.1fMiB\n",
                 m_name, mem_before, mem_after);
    }
    return true;
}

size_t CDBWrapper::DynamicMemoryUsage() const
{
    std::string memory;
    std::optional<size_t> parsed;
    if (!DBContext().pdb->GetProperty("rocksdb.approximate-memory-usage", &memory) || !(parsed = ToIntegral<size_t>(memory))) {
        LogDebug(BCLog::ROCKSDB, "Failed to get approximate-memory-usage property\n");
        return 0;
    }
    return parsed.value();
}

// Prefixed with null character to avoid collisions with other keys
//
// We must use a string constructor which specifies length so that we copy
// past the null-terminator.
const std::string CDBWrapper::OBFUSCATE_KEY_KEY("\000obfuscate_key", 14);

const unsigned int CDBWrapper::OBFUSCATE_KEY_NUM_BYTES = 8;

/**
 * Returns a string (consisting of 8 random bytes) suitable for use as an
 * obfuscating XOR key.
 */
std::vector<unsigned char> CDBWrapper::CreateObfuscateKey() const
{
    std::vector<uint8_t> ret(OBFUSCATE_KEY_NUM_BYTES);
    GetRandBytes(ret);
    return ret;
}

std::optional<std::string> CDBWrapper::ReadImpl(Span<const std::byte> key) const
{
    rocksdb::Slice slKey(CharCast(key.data()), key.size());
    std::string strValue;
    rocksdb::Status status = DBContext().pdb->Get(DBContext().readoptions, slKey, &strValue);
    if (!status.ok()) {
        if (status.IsNotFound())
            return std::nullopt;
        LogPrintf("RocksDB read failure: %s\n", status.ToString());
        HandleError(status);
    }
    return strValue;
}

bool CDBWrapper::ExistsImpl(Span<const std::byte> key) const
{
    rocksdb::Slice slKey(CharCast(key.data()), key.size());

    std::string strValue;
    rocksdb::Status status = DBContext().pdb->Get(DBContext().readoptions, slKey, &strValue);
    if (!status.ok()) {
        if (status.IsNotFound())
            return false;
        LogPrintf("RocksDB read failure: %s\n", status.ToString());
        HandleError(status);
    }
    return true;
}

size_t CDBWrapper::EstimateSizeImpl(Span<const std::byte> key1, Span<const std::byte> key2) const
{
    rocksdb::Slice slKey1(CharCast(key1.data()), key1.size());
    rocksdb::Slice slKey2(CharCast(key2.data()), key2.size());
    uint64_t size = 0;
    rocksdb::Range range(slKey1, slKey2);
    DBContext().pdb->GetApproximateSizes(&range, 1, &size);
    return size;
}

bool CDBWrapper::IsEmpty()
{
    std::unique_ptr<CDBIterator> it(NewIterator());
    it->SeekToFirst();
    return !(it->Valid());
}

struct CDBIterator::IteratorImpl {
    const std::unique_ptr<rocksdb::Iterator> iter;

    explicit IteratorImpl(rocksdb::Iterator* _iter) : iter{_iter} {}
};

CDBIterator::CDBIterator(const CDBWrapper& _parent, std::unique_ptr<IteratorImpl> _piter) : parent(_parent),
                                                                                            m_impl_iter(std::move(_piter)) {}

CDBIterator* CDBWrapper::NewIterator()
{
    return new CDBIterator{*this, std::make_unique<CDBIterator::IteratorImpl>(DBContext().pdb->NewIterator(DBContext().iteroptions))};
}

void CDBIterator::SeekImpl(Span<const std::byte> key)
{
    rocksdb::Slice slKey(CharCast(key.data()), key.size());
    m_impl_iter->iter->Seek(slKey);
}

Span<const std::byte> CDBIterator::GetKeyImpl() const
{
    return MakeByteSpan(m_impl_iter->iter->key());
}

Span<const std::byte> CDBIterator::GetValueImpl() const
{
    return MakeByteSpan(m_impl_iter->iter->value());
}

CDBIterator::~CDBIterator() = default;
bool CDBIterator::Valid() const { return m_impl_iter->iter->Valid(); }
void CDBIterator::SeekToFirst() { m_impl_iter->iter->SeekToFirst(); }
void CDBIterator::Next() { m_impl_iter->iter->Next(); }

namespace dbwrapper_private {

const std::vector<unsigned char>& GetObfuscateKey(const CDBWrapper &w)
{
    return w.obfuscate_key;
}

} // namespace dbwrapper_private
