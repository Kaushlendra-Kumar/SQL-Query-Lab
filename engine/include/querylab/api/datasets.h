#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "querylab/catalog/catalog.h"

namespace querylab::api {

/// Deterministic sample data. A tiny splitmix64 generator is used instead of
/// <random> distributions, whose output differs between standard libraries,
/// so the same seed produces identical data on every platform.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : s_(seed) {}
    std::uint64_t next();
    /// Uniform integer in [lo, hi].
    std::int64_t range(std::int64_t lo, std::int64_t hi);
    double unit();  // [0, 1)
    /// Zipf-like skew: small values are much more likely (for realistic
    /// "popular customer" distributions).
    std::int64_t skewed(std::int64_t lo, std::int64_t hi, double skew);
    template <class T>
    const T& pick(const std::vector<T>& v) { return v[static_cast<std::size_t>(range(0, static_cast<std::int64_t>(v.size()) - 1))]; }

private:
    std::uint64_t s_;
};

struct DatasetInfo {
    std::string name;
    std::string title;
    std::string description;
    std::vector<std::string> tables;
    std::vector<std::pair<std::string, std::string>> sampleQueries;  // (title, sql)
};

std::vector<DatasetInfo> availableDatasets();

/// Creates the dataset's tables (dropping existing tables with the same
/// names) and fills them. `scale` is the row count of the dataset's main fact
/// table (orders / posts / employees). Returns {table: rows}.
nlohmann::json loadDataset(catalog::Catalog& catalog, const std::string& name, std::size_t scale, std::uint64_t seed = 42);

}  // namespace querylab::api
