#pragma once

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "querylab/catalog/catalog.h"
#include "querylab/execution/executor.h"
#include "querylab/sql/ast.h"
#include "querylab/sql/lexer.h"
#include "querylab/storage/buffer_pool.h"

namespace querylab::api {

using json = nlohmann::json;

struct EngineOptions {
    std::size_t bufferPoolPages = 256;  // 1 MiB of 4 KiB pages
    execution::ExecLimits limits;
};

struct QueryOptions {
    bool explainOnly = false;        // plan but do not execute (EXPLAIN)
    bool timing = true;              // per-operator timers
    bool recordEvents = true;        // execution events for replay
    bool disableIndexScans = false;  // what-if: force sequential scans
    bool disableHashJoin = false;    // what-if: force nested loops
    bool coldCache = false;          // empty the buffer pool before executing
    std::size_t maxRows = 0;         // 0 = engine default
    bool recordHistory = true;       // count toward workload statistics
};

/// The QueryLab engine facade. Thread-safe: statements are serialized by an
/// internal mutex (the engine has no concurrency control). `cancel()` may be
/// called from another thread to stop the running statement.
///
/// All results are returned as explicit JSON DTOs (see docs/api.md); no
/// internal object is serialized implicitly.
class Database {
public:
    /// Pure in-memory database (pages live in a MemoryPageStore).
    static std::unique_ptr<Database> inMemory(EngineOptions opts = {});
    /// File-backed database. Creates the file if `create` is true.
    static std::unique_ptr<Database> openFile(const std::string& path, bool create, EngineOptions opts = {});
    ~Database();

    /// Executes a ';'-separated script. Returns
    /// {success, results: [statement result...], error?}.
    json execute(const std::string& sql, const QueryOptions& opts = {});
    /// Plans every statement without executing it.
    json explain(const std::string& sql, QueryOptions opts = {});

    json catalogJson();
    json tableSchema(const std::string& table);
    json tableRows(const std::string& table, std::size_t offset, std::size_t limit);
    json indexesJson();
    json indexDetail(const std::string& name);
    json storageJson();
    json pageDetail(const std::string& table, std::size_t pageIndex);
    json datasetsJson();
    json loadDataset(const std::string& name, std::size_t scale);
    json reset();
    /// {sql | queries: [..], iterations, warmup, coldCache, disableIndexScans, variants: [...]}
    json benchmark(const json& request);
    json history();
    json settingsJson();
    json updateSettings(const json& patch);
    json health();

    void cancel() { cancel_.store(true); }
    /// Writes all dirty pages and the catalog (file mode only).
    void checkpoint();
    [[nodiscard]] bool persistent() const { return store_->persistent(); }

    catalog::Catalog& catalogRef() { return *catalog_; }

private:
    Database(std::unique_ptr<storage::PageStore> store, EngineOptions opts, std::string path);
    void loadFromFile();
    json runStatement(const sql::Statement& st, const std::vector<sql::Token>& tokens, const QueryOptions& opts, double lexMs,
                      double parseMs);
    json runQuery(const sql::Statement& st, const QueryOptions& opts, json& out);
    void recordHistory(const sql::Statement& st, const json& result);
    int queryFrequency(const std::string& shape) const;

    std::recursive_mutex mu_;
    std::unique_ptr<storage::PageStore> store_;
    std::unique_ptr<storage::BufferPool> pool_;
    std::unique_ptr<catalog::Catalog> catalog_;
    EngineOptions opts_;
    std::string path_;
    std::vector<storage::PageId> catalogPages_;
    std::atomic<bool> cancel_{false};
    std::uint64_t nextQueryId_ = 1;
    double timerOverheadNs_ = 0;
    std::map<std::string, int> shapeCounts_;
    std::deque<json> history_;
};

/// Replaces literals with '?' so queries differing only in constants share a
/// "shape" (used for workload frequency in the index advisor).
std::string queryShape(const std::string& sql);

}  // namespace querylab::api
