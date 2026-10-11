#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "querylab/catalog/catalog.h"
#include "querylab/planner/physical_plan.h"

namespace querylab::execution {

/// Runtime statistics collected by every physical operator.
struct OperatorStats {
    int nodeId = 0;
    std::string name;
    std::uint64_t rowsIn = 0;          // rows received from children (scans: rows examined)
    std::uint64_t rowsOut = 0;
    std::uint64_t rowsFiltered = 0;    // rows rejected by this operator's predicate
    std::uint64_t rowsScanned = 0;     // scans: live rows examined
    std::uint64_t pagesRead = 0;       // buffer-pool page fetches by this operator
    std::uint64_t pageMisses = 0;      // of which had to be loaded from the page store
    std::uint64_t indexNodesVisited = 0;
    std::uint64_t indexEntriesExamined = 0;
    std::uint64_t nextCalls = 0;
    std::uint64_t buildRows = 0;       // hash join build side / sort / aggregate input
    std::uint64_t groups = 0;          // aggregate groups / distinct values / hash keys
    std::int64_t inclusiveNs = 0;      // time inside open/next/close incl. children
    std::uint64_t peakMemoryBytes = 0; // estimated bytes buffered by this operator
    bool opened = false;
    bool exhausted = false;
};

/// One engine event, recorded on the real execution path for replay.
struct ExecEvent {
    std::uint64_t seq = 0;
    int nodeId = 0;
    const char* type = "";  // static string: open | row | done | close | build_done | sort_done | aggregate_done
    std::uint64_t rows = 0;
    std::int64_t tNs = 0;  // since execution start
};

struct ExecLimits {
    std::chrono::milliseconds timeout{10000};
    std::size_t maxResultRows = 10000;  // rows kept for the response (counting continues)
    std::size_t maxEvents = 4000;
    std::size_t maxMemoryBytes = 512u * 1024u * 1024u;
    bool timing = true;        // per-operator timers (small overhead per row)
    bool recordEvents = true;
};

class ExecContext {
public:
    ExecContext(catalog::Catalog& cat, ExecLimits limits, const std::atomic<bool>* cancel = nullptr);

    catalog::Catalog& catalog;
    ExecLimits limits;
    std::map<int, OperatorStats> stats;
    std::vector<ExecEvent> events;
    bool eventsTruncated = false;
    std::uint64_t timerCalls = 0;
    std::uint64_t memoryBytes = 0;

    /// Throws QueryError(Executor) if the deadline passed or cancel was requested.
    void check();
    void event(int nodeId, const char* type, std::uint64_t rows);
    std::int64_t nowNs();
    void addMemory(std::uint64_t bytes, OperatorStats& st);

private:
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point deadline_;
    const std::atomic<bool>* cancel_;
};

struct ExecutionResult {
    std::vector<Row> rows;
    std::uint64_t totalRows = 0;
    bool truncated = false;
    std::uint64_t affectedRows = 0;
    std::int64_t executeNs = 0;
};

/// Builds operators for `plan` and runs them to completion (volcano model).
ExecutionResult execute(const planner::PhysicalNode& plan, ExecContext& ctx);

/// Measured cost of one steady_clock::now() call in ns (median of samples).
double timerOverheadNs();

}  // namespace querylab::execution
