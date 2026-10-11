#include "querylab/api/database.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "querylab/api/datasets.h"
#include "querylab/explain/ast_json.h"
#include "querylab/explain/insights.h"
#include "querylab/explain/plan_json.h"
#include "querylab/optimizer/optimizer.h"
#include "querylab/planner/binder.h"
#include "querylab/planner/physical_plan.h"
#include "querylab/sql/parser.h"

namespace querylab::api {

using Clock = std::chrono::steady_clock;
using explain::spanToJson;
using explain::valueToJson;

namespace {

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

json errorJson(const QueryError& e) {
    json j{{"stage", stageName(e.stage())}, {"message", e.what()}, {"display", e.describe()}};
    if (e.span()) j["span"] = spanToJson(*e.span());
    return j;
}

// ---- persistence format ------------------------------------------------------------
// Page 0 (header), bytes after the 16-byte page header:
//   [16..24) magic "QLABDB\0\1"  [24..28) format version  [28..32) page size
//   [32..36) first catalog page  [36..40) catalog bytes    [40..44) page count
// Catalog pages: [16..20) next catalog page (0xFFFFFFFF = end), [20..22) bytes
// used, [22..4096) JSON payload fragment.
constexpr char kMagic[8] = {'Q', 'L', 'A', 'B', 'D', 'B', '\0', '\1'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kCatalogPayload = storage::kPageSize - 22;

void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint32_t get32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 |
           static_cast<std::uint32_t>(p[3]) << 24;
}

DataType parseType(const std::string& s) {
    if (s == "INTEGER") return DataType::Integer;
    if (s == "FLOAT") return DataType::Float;
    if (s == "TEXT") return DataType::Text;
    if (s == "BOOLEAN") return DataType::Boolean;
    throw QueryError(ErrorStage::Storage, "unknown column type '" + s + "' in catalog");
}

json statsJson(const catalog::ColumnStats& s) {
    json j{{"distinct", s.distinct()}, {"nulls", s.nullCount()}};
    if (auto mn = s.min()) j["min"] = valueToJson(*mn);
    if (auto mx = s.max()) j["max"] = valueToJson(*mx);
    return j;
}

bool compatibleForStorage(DataType value, DataType column) {
    if (value == DataType::Null || value == column) return true;
    return isNumeric(value) && isNumeric(column);
}

}  // namespace

std::string queryShape(const std::string& sql) {
    try {
        auto toks = sql::tokenize(sql);
        std::string s;
        for (const auto& t : toks) {
            if (t.type == sql::TokenType::End) break;
            if (t.type == sql::TokenType::Integer || t.type == sql::TokenType::Float || t.type == sql::TokenType::String) s += "?";
            else s += t.value.empty() ? t.lexeme : t.value;
            s += " ";
        }
        return s;
    } catch (const QueryError&) {
        return sql;
    }
}

// ---- construction --------------------------------------------------------------------

Database::Database(std::unique_ptr<storage::PageStore> store, EngineOptions opts, std::string path)
    : store_(std::move(store)), opts_(opts), path_(std::move(path)) {
    pool_ = std::make_unique<storage::BufferPool>(*store_, opts_.bufferPoolPages);
    catalog_ = std::make_unique<catalog::Catalog>(*pool_);
    timerOverheadNs_ = execution::timerOverheadNs();
}

Database::~Database() {
    try {
        if (persistent()) checkpoint();
    } catch (...) {
        // Destructors must not throw; the last explicit checkpoint stands.
    }
}

std::unique_ptr<Database> Database::inMemory(EngineOptions opts) {
    return std::unique_ptr<Database>(new Database(std::make_unique<storage::MemoryPageStore>(), opts, ""));
}

std::unique_ptr<Database> Database::openFile(const std::string& path, bool create, EngineOptions opts) {
    auto store = std::make_unique<storage::FilePageStore>(path, create);
    const bool fresh = store->pageCount() == 0;
    if (!create && fresh) throw QueryError(ErrorStage::Storage, "database file '" + path + "' is empty");
    if (fresh) store->allocate();  // page 0: header
    auto db = std::unique_ptr<Database>(new Database(std::move(store), opts, path));
    if (fresh) db->checkpoint();
    else db->loadFromFile();
    return db;
}

void Database::checkpoint() {
    std::lock_guard lock(mu_);
    if (!persistent()) return;
    auto& fs = static_cast<storage::FilePageStore&>(*store_);
    pool_->flushAll();

    json cat{{"tables", json::array()}, {"indexes", json::array()}};
    for (auto* t : catalog_->tables()) {
        json cols = json::array();
        for (const auto& c : t->columns) cols.push_back({{"name", c.name}, {"type", typeName(c.type)}});
        cat["tables"].push_back({{"id", t->id}, {"name", t->name}, {"columns", cols}, {"pages", t->heap->pages()}, {"rows", t->rowCount()}});
        for (auto* idx : t->indexes) {
            json icols = json::array();
            for (auto c : idx->columns) icols.push_back(t->columns[c].name);
            cat["indexes"].push_back({{"name", idx->name}, {"table", t->name}, {"columns", icols}, {"unique", idx->unique}});
        }
    }
    const std::string payload = cat.dump();
    const std::size_t needed = std::max<std::size_t>(1, (payload.size() + kCatalogPayload - 1) / kCatalogPayload);
    while (catalogPages_.size() < needed) catalogPages_.push_back(fs.allocate());
    for (std::size_t i = 0; i < needed; ++i) {
        storage::Page p;
        p.init(catalogPages_[i]);
        const std::size_t off = i * kCatalogPayload;
        const std::size_t len = std::min(kCatalogPayload, payload.size() - std::min(payload.size(), off));
        put32(p.raw() + 16, i + 1 < needed ? catalogPages_[i + 1] : storage::kInvalidPageId);
        p.raw()[20] = static_cast<std::uint8_t>(len);
        p.raw()[21] = static_cast<std::uint8_t>(len >> 8);
        if (len) std::memcpy(p.raw() + 22, payload.data() + off, len);
        fs.write(catalogPages_[i], p);
    }
    storage::Page header;
    header.init(0);
    std::memcpy(header.raw() + 16, kMagic, 8);
    put32(header.raw() + 24, kFormatVersion);
    put32(header.raw() + 28, static_cast<std::uint32_t>(storage::kPageSize));
    put32(header.raw() + 32, catalogPages_.front());
    put32(header.raw() + 36, static_cast<std::uint32_t>(payload.size()));
    put32(header.raw() + 40, static_cast<std::uint32_t>(fs.pageCount()));
    fs.write(0, header);  // header last: it points at the new catalog
    fs.sync();
}

void Database::loadFromFile() {
    auto& fs = static_cast<storage::FilePageStore&>(*store_);
    storage::Page header;
    fs.readRaw(0, header);
    if (std::memcmp(header.raw() + 16, kMagic, 8) != 0) {
        throw QueryError(ErrorStage::Storage, "'" + path_ + "' is not a QueryLab database (bad magic number)");
    }
    if (!header.verifyChecksum()) throw QueryError(ErrorStage::Storage, "database header checksum mismatch (file is corrupted)");
    if (get32(header.raw() + 24) != kFormatVersion) {
        throw QueryError(ErrorStage::Storage, "unsupported database format version " + std::to_string(get32(header.raw() + 24)));
    }
    if (get32(header.raw() + 28) != storage::kPageSize) throw QueryError(ErrorStage::Storage, "database page size mismatch");
    const std::uint32_t total = get32(header.raw() + 36);
    std::string payload;
    storage::PageId next = get32(header.raw() + 32);
    std::size_t guard = 0;
    while (next != storage::kInvalidPageId) {
        if (++guard > fs.pageCount()) throw QueryError(ErrorStage::Storage, "catalog page chain loops (file is corrupted)");
        storage::Page p;
        fs.read(next, p);
        catalogPages_.push_back(next);
        const std::size_t len = static_cast<std::size_t>(p.raw()[20]) | (static_cast<std::size_t>(p.raw()[21]) << 8);
        if (len > kCatalogPayload) throw QueryError(ErrorStage::Storage, "catalog page length is invalid (file is corrupted)");
        payload.append(reinterpret_cast<const char*>(p.raw() + 22), len);
        next = get32(p.raw() + 16);
    }
    if (payload.size() != total) throw QueryError(ErrorStage::Storage, "catalog size mismatch (file is corrupted)");
    json cat;
    try {
        cat = json::parse(payload);
        for (const auto& t : cat.at("tables")) {
            std::vector<catalog::ColumnInfo> cols;
            for (const auto& c : t.at("columns")) cols.push_back({c.at("name").get<std::string>(), parseType(c.at("type").get<std::string>())});
            for (auto pid : t.at("pages")) {
                if (pid.get<storage::PageId>() >= fs.pageCount()) throw QueryError(ErrorStage::Storage, "table page beyond end of file");
            }
            auto& info = catalog_->restoreTable(t.at("id").get<std::uint32_t>(), t.at("name").get<std::string>(), std::move(cols),
                                                t.at("pages").get<std::vector<storage::PageId>>(), t.at("rows").get<std::size_t>());
            catalog_->rebuildStats(info);  // statistics are derived, not stored
        }
        for (const auto& i : cat.at("indexes")) {
            // Indexes are rebuilt from the heap on open (documented trade-off).
            catalog_->createIndex(i.at("name").get<std::string>(), i.at("table").get<std::string>(),
                                  i.at("columns").get<std::vector<std::string>>(), i.at("unique").get<bool>());
        }
    } catch (const json::exception& e) {
        throw QueryError(ErrorStage::Storage, std::string("catalog is unreadable (file is corrupted): ") + e.what());
    }
}

// ---- statements --------------------------------------------------------------------------

json Database::execute(const std::string& sql, const QueryOptions& opts) {
    std::lock_guard lock(mu_);
    cancel_.store(false);
    json out{{"success", true}, {"results", json::array()}};

    std::vector<sql::Token> tokens;
    std::vector<sql::Statement> stmts;
    double lexMs = 0, parseMs = 0;
    auto t0 = Clock::now();
    try {
        tokens = sql::tokenize(sql, true);
        lexMs = msSince(t0);
        t0 = Clock::now();
        stmts = sql::parseScript(sql);
        parseMs = msSince(t0);
    } catch (const QueryError& e) {
        json r{{"queryId", nextQueryId_++}, {"index", 0}, {"sql", sql}, {"success", false}, {"error", errorJson(e)}};
        r["timeline"] = json::array({{{"stage", "lexer"}, {"ms", e.stage() == ErrorStage::Lexer ? msSince(t0) : lexMs}, {"ok", e.stage() != ErrorStage::Lexer}}});
        if (e.stage() == ErrorStage::Parser) {
            r["timeline"].push_back({{"stage", "parser"}, {"ms", msSince(t0)}, {"ok", false}});
            r["tokens"] = explain::tokensToJson(tokens);
        }
        out["results"].push_back(r);
        out["success"] = false;
        out["error"] = errorJson(e);
        return out;
    }
    if (stmts.empty()) {
        out["success"] = false;
        out["error"] = {{"stage", "parser"}, {"message", "no statements to execute"}, {"display", "parser error: no statements to execute"}};
        return out;
    }
    for (std::size_t i = 0; i < stmts.size(); ++i) {
        // Lex/parse time is for the whole script; attribute it to the first statement.
        json r = runStatement(stmts[i], tokens, opts, i == 0 ? lexMs : 0, i == 0 ? parseMs : 0);
        r["index"] = i;
        const bool ok = r["success"].get<bool>();
        out["results"].push_back(std::move(r));
        if (!ok) {
            out["success"] = false;
            out["error"] = out["results"].back()["error"];
            break;  // stop at the first failing statement
        }
    }
    return out;
}

json Database::explain(const std::string& sql, QueryOptions opts) {
    opts.explainOnly = true;
    return execute(sql, opts);
}

json Database::runStatement(const sql::Statement& st, const std::vector<sql::Token>& tokens, const QueryOptions& opts,
                            double lexMs, double parseMs) {
    json r{{"queryId", nextQueryId_++}, {"kind", st.kindName()}, {"sql", st.text}, {"span", spanToJson(st.span)},
           {"explain", st.explain || opts.explainOnly}, {"success", true}};
    r["timeline"] = json::array({{{"stage", "lexer"}, {"ms", lexMs}, {"ok", true}}, {{"stage", "parser"}, {"ms", parseMs}, {"ok", true}}});
    std::vector<sql::Token> mine;
    for (const auto& t : tokens) {
        if (t.type != sql::TokenType::End && t.span.begin >= st.span.begin && t.span.end <= st.span.end) mine.push_back(t);
    }
    r["tokens"] = explain::tokensToJson(mine);
    r["ast"] = explain::statementToJson(st);
    try {
        runQuery(st, opts, r);
    } catch (const QueryError& e) {
        r["success"] = false;
        r["error"] = errorJson(e);
        // Mark the stage that failed on the timeline.
        r["timeline"].push_back({{"stage", stageName(e.stage())}, {"ms", 0}, {"ok", false}});
    } catch (const std::exception& e) {
        r["success"] = false;
        r["error"] = {{"stage", "internal"}, {"message", std::string("internal error: ") + e.what()},
                      {"display", std::string("internal error: ") + e.what()}};
    }
    if (r["success"].get<bool>() && opts.recordHistory) recordHistory(st, r);
    return r;
}

json Database::runQuery(const sql::Statement& st, const QueryOptions& opts, json& r) {
    using namespace planner;
    const bool explainOnly = st.explain || opts.explainOnly;
    auto stage = [&](const char* name, double ms) { r["timeline"].push_back({{"stage", name}, {"ms", ms}, {"ok", true}}); };

    // ---- DDL / INSERT: no query plan ----------------------------------------------------
    if (std::holds_alternative<sql::CreateTableStmt>(st.body)) {
        const auto& c = std::get<sql::CreateTableStmt>(st.body);
        if (explainOnly) { r["message"] = "EXPLAIN is only supported for SELECT, UPDATE and DELETE"; return r; }
        auto t0 = Clock::now();
        if (catalog_->findTable(c.name)) {
            if (c.ifNotExists) {
                r["message"] = "table " + c.name + " already exists, skipped";
                return r;
            }
            throw QueryError(ErrorStage::Catalog, "table '" + c.name + "' already exists", c.nameSpan);
        }
        std::vector<catalog::ColumnInfo> cols;
        for (const auto& cd : c.columns) cols.push_back({cd.name, cd.type});
        catalog_->createTable(c.name, std::move(cols));
        stage("executor", msSince(t0));
        r["message"] = "created table " + c.name;
        if (persistent()) checkpoint();
        return r;
    }
    if (std::holds_alternative<sql::CreateIndexStmt>(st.body)) {
        const auto& c = std::get<sql::CreateIndexStmt>(st.body);
        if (explainOnly) { r["message"] = "EXPLAIN is only supported for SELECT, UPDATE and DELETE"; return r; }
        if (catalog_->findIndex(c.name) && c.ifNotExists) {
            r["message"] = "index " + c.name + " already exists, skipped";
            return r;
        }
        if (!catalog_->findTable(c.table)) throw QueryError(ErrorStage::Catalog, "table '" + c.table + "' does not exist", c.tableSpan);
        auto t0 = Clock::now();
        auto& idx = catalog_->createIndex(c.name, c.table, c.columns, c.unique);
        const double ms = msSince(t0);
        stage("executor", ms);
        r["message"] = "created index " + c.name + " on " + c.table + " (" + std::to_string(idx.tree->size()) + " entries, height " +
                       std::to_string(idx.tree->height()) + ") in " + std::to_string(ms).substr(0, 6) + " ms";
        r["affectedRows"] = idx.tree->size();
        if (persistent()) checkpoint();
        return r;
    }
    if (std::holds_alternative<sql::DropTableStmt>(st.body)) {
        const auto& d = std::get<sql::DropTableStmt>(st.body);
        if (explainOnly) { r["message"] = "EXPLAIN is only supported for SELECT, UPDATE and DELETE"; return r; }
        if (!catalog_->findTable(d.name)) {
            if (d.ifExists) { r["message"] = "table " + d.name + " does not exist, skipped"; return r; }
            throw QueryError(ErrorStage::Catalog, "table '" + d.name + "' does not exist", d.nameSpan);
        }
        catalog_->dropTable(d.name);
        r["message"] = "dropped table " + d.name;
        if (persistent()) checkpoint();
        return r;
    }
    if (std::holds_alternative<sql::DropIndexStmt>(st.body)) {
        const auto& d = std::get<sql::DropIndexStmt>(st.body);
        if (explainOnly) { r["message"] = "EXPLAIN is only supported for SELECT, UPDATE and DELETE"; return r; }
        if (!catalog_->findIndex(d.name)) {
            if (d.ifExists) { r["message"] = "index " + d.name + " does not exist, skipped"; return r; }
            throw QueryError(ErrorStage::Catalog, "index '" + d.name + "' does not exist", d.nameSpan);
        }
        catalog_->dropIndex(d.name);
        r["message"] = "dropped index " + d.name;
        if (persistent()) checkpoint();
        return r;
    }
    if (std::holds_alternative<sql::InsertStmt>(st.body)) {
        const auto& ins = std::get<sql::InsertStmt>(st.body);
        if (explainOnly) { r["message"] = "EXPLAIN is only supported for SELECT, UPDATE and DELETE"; return r; }
        auto* t = catalog_->findTable(ins.table);
        if (!t) throw QueryError(ErrorStage::Binder, "table '" + ins.table + "' does not exist", ins.tableSpan);
        auto t0 = Clock::now();
        // Map target columns.
        std::vector<std::size_t> targets;
        if (ins.columns.empty()) {
            for (std::size_t i = 0; i < t->columns.size(); ++i) targets.push_back(i);
        } else {
            for (std::size_t i = 0; i < ins.columns.size(); ++i) {
                auto pos = t->columnIndex(ins.columns[i]);
                if (!pos) throw QueryError(ErrorStage::Binder, "column '" + ins.columns[i] + "' does not exist in table '" + t->name + "'", ins.columnSpans[i]);
                if (std::find(targets.begin(), targets.end(), *pos) != targets.end()) {
                    throw QueryError(ErrorStage::Binder, "column '" + ins.columns[i] + "' specified more than once", ins.columnSpans[i]);
                }
                targets.push_back(*pos);
            }
        }
        IdGenerator ids;
        Binder binder(*catalog_, ids);
        std::vector<Row> rows;
        for (const auto& values : ins.rows) {
            if (values.size() != targets.size()) {
                throw QueryError(ErrorStage::Binder, "INSERT has " + std::to_string(values.size()) + " values but " + std::to_string(targets.size()) +
                                                         " target columns", values.empty() ? ins.tableSpan : values.front()->span);
            }
            Row row(t->columns.size());
            for (std::size_t i = 0; i < values.size(); ++i) {
                const auto& col = t->columns[targets[i]];
                Value v = binder.evalConstant(*values[i]);
                if (!compatibleForStorage(v.type(), col.type)) {
                    throw QueryError(ErrorStage::Binder, std::string("cannot insert ") + typeName(v.type()) + " value " + v.toSqlLiteral() + " into " +
                                                             typeName(col.type) + " column '" + col.name + "'", values[i]->span);
                }
                row[targets[i]] = coerceForStorage(v, col.type, col.name);
            }
            rows.push_back(std::move(row));
        }
        stage("binder", msSince(t0));
        t0 = Clock::now();
        // Statement-level atomicity: undo this statement's rows on failure.
        std::vector<std::pair<storage::Rid, Row>> done;
        try {
            for (auto& row : rows) done.emplace_back(catalog_->insertRow(*t, row), row);
        } catch (...) {
            for (auto it = done.rbegin(); it != done.rend(); ++it) catalog_->deleteRow(*t, it->first, it->second);
            throw;
        }
        stage("executor", msSince(t0));
        r["affectedRows"] = rows.size();
        r["message"] = "inserted " + std::to_string(rows.size()) + " row" + (rows.size() == 1 ? "" : "s") + " into " + t->name;
        if (persistent()) checkpoint();
        return r;
    }

    // ---- planned statements: SELECT / UPDATE / DELETE -----------------------------------
    IdGenerator ids;
    Binder binder(*catalog_, ids);
    auto t0 = Clock::now();
    LogicalPtr logical;
    std::vector<std::pair<std::size_t, BoundExprPtr>> assignments;
    const catalog::TableInfo* dmlTable = nullptr;
    if (st.isSelect()) {
        logical = binder.bindSelect(std::get<sql::SelectStmt>(st.body));
    } else if (std::holds_alternative<sql::DeleteStmt>(st.body)) {
        const auto& d = std::get<sql::DeleteStmt>(st.body);
        logical = binder.bindTableFilter(d.table, d.tableSpan, d.where.get());
        dmlTable = catalog_->findTable(d.table);
    } else {
        const auto& u = std::get<sql::UpdateStmt>(st.body);
        logical = binder.bindTableFilter(u.table, u.tableSpan, u.where.get());
        dmlTable = catalog_->findTable(u.table);
        const Schema& schema = logical->output;
        for (const auto& a : u.assignments) {
            auto pos = dmlTable->columnIndex(a.column);
            if (!pos) throw QueryError(ErrorStage::Binder, "column '" + a.column + "' does not exist in table '" + u.table + "'", a.span);
            for (const auto& prev : assignments) {
                if (prev.first == *pos) throw QueryError(ErrorStage::Binder, "column '" + a.column + "' assigned more than once", a.span);
            }
            auto e = binder.bindAgainst(*a.value, schema, u.table);
            const auto colType = dmlTable->columns[*pos].type;
            if (!compatibleForStorage(e->type, colType)) {
                throw QueryError(ErrorStage::Binder, std::string("cannot assign ") + typeName(e->type) + " to " + typeName(colType) +
                                                         " column '" + a.column + "'", a.value->span);
            }
            assignments.emplace_back(*pos, std::move(e));
        }
    }
    stage("binder", msSince(t0));

    t0 = Clock::now();
    optimizer::CostModel cost;
    cost.indexColumns(*logical);
    auto initial = logical->clone();
    cost.annotate(*initial);
    r["logicalPlan"] = explain::logicalToJson(*initial);
    std::vector<optimizer::OptimizerStep> trace;
    optimizer::Optimizer opt(cost, ids);
    auto optimized = opt.optimize(std::move(logical), trace);
    cost.indexColumns(*optimized);
    cost.annotate(*optimized);
    stage("optimizer", msSince(t0));

    t0 = Clock::now();
    PhysicalPlanner planner(cost, trace);
    planner.setIndexScansEnabled(!opts.disableIndexScans);
    planner.setHashJoinEnabled(!opts.disableHashJoin);
    PhysicalPtr physical;
    if (st.isSelect()) physical = planner.plan(*optimized);
    else if (std::holds_alternative<sql::DeleteStmt>(st.body)) physical = planner.planDelete(*optimized);
    else physical = planner.planUpdate(*optimized, std::move(assignments));
    stage("planner", msSince(t0));

    r["optimizedPlan"] = explain::logicalToJson(*optimized);
    r["optimizer"] = explain::optimizerTraceToJson(trace);
    json columns = json::array();
    if (st.isSelect()) {
        for (const auto& c : optimized->output) columns.push_back({{"name", c.name}, {"type", typeName(c.type)}});
    }
    r["columns"] = columns;

    explain::InsightInput in;
    in.statement = &st;
    in.plan = physical.get();
    in.trace = &trace;
    in.cost = &cost;
    in.catalog = catalog_.get();
    in.timerOverheadNs = timerOverheadNs_;
    in.queryFrequency = queryFrequency(queryShape(st.text)) + (explainOnly ? 0 : 1);

    if (explainOnly) {
        r["physicalPlan"] = explain::physicalToJson(*physical, nullptr, cost);
        r["explanation"] = explain::explainExecution(in);
        r["advisor"] = explain::adviseIndexes(in);
        r["rows"] = json::array();
        r["rowCount"] = 0;
        r["message"] = "plan only (EXPLAIN): the query was not executed";
        return r;
    }

    // ---- execute ------------------------------------------------------------------------
    if (opts.coldCache) pool_->evictAll();
    auto limits = opts_.limits;
    limits.timing = opts.timing;
    limits.recordEvents = opts.recordEvents;
    if (opts.maxRows) limits.maxResultRows = opts.maxRows;
    execution::ExecContext ctx(*catalog_, limits, &cancel_);
    const auto poolBefore = pool_->stats();
    t0 = Clock::now();
    execution::ExecutionResult res = execution::execute(*physical, ctx);
    const double execMs = msSince(t0);
    stage("executor", execMs);
    const auto poolDelta = pool_->stats() - poolBefore;

    json rows = json::array();
    for (const auto& row : res.rows) {
        json jr = json::array();
        for (const auto& v : row) jr.push_back(valueToJson(v));
        rows.push_back(std::move(jr));
    }
    r["rows"] = std::move(rows);
    r["rowCount"] = res.totalRows;
    r["truncated"] = res.truncated;
    r["affectedRows"] = res.affectedRows;
    if (!st.isSelect()) {
        r["message"] = std::string(st.kindName() == std::string("DELETE") ? "deleted " : "updated ") + std::to_string(res.affectedRows) +
                       " row" + (res.affectedRows == 1 ? "" : "s") + " in " + dmlTable->name;
    } else if (res.truncated) {
        r["message"] = "showing the first " + std::to_string(res.rows.size()) + " of " + std::to_string(res.totalRows) + " rows";
    }
    r["physicalPlan"] = explain::physicalToJson(*physical, &ctx.stats, cost);

    std::uint64_t scanned = 0, pages = 0, misses = 0, entries = 0, mem = 0;
    for (const auto& [id, s] : ctx.stats) {
        scanned += s.rowsScanned;
        pages += s.pagesRead;
        misses += s.pageMisses;
        entries += s.indexEntriesExamined;
        mem = std::max<std::uint64_t>(mem, s.peakMemoryBytes);
    }
    double planningMs = 0;
    for (const auto& s : r["timeline"]) {
        const std::string name = s["stage"];
        if (name != "executor") planningMs += s["ms"].get<double>();
    }
    r["metrics"] = {{"executionMs", execMs},
                    {"planningMs", planningMs},
                    {"totalEngineMs", execMs + planningMs},
                    {"rowsScanned", scanned},
                    {"rowsReturned", res.totalRows},
                    {"pagesRead", pages},
                    {"pageMisses", misses},
                    {"indexEntriesExamined", entries},
                    {"peakOperatorMemoryBytes", mem},
                    {"totalBufferedBytes", ctx.memoryBytes},
                    {"bufferPool",
                     {{"logicalReads", poolDelta.logicalReads},
                      {"hits", poolDelta.hits},
                      {"misses", poolDelta.misses},
                      {"evictions", poolDelta.evictions},
                      {"hitRate", poolDelta.logicalReads ? static_cast<double>(poolDelta.hits) / static_cast<double>(poolDelta.logicalReads) : 1.0},
                      {"capacityPages", pool_->capacity()}}},
                    {"coldCache", opts.coldCache},
                    {"timing", opts.timing},
                    {"timerCalls", ctx.timerCalls},
                    {"timerOverheadNs", timerOverheadNs_},
                    {"estimatedTimerOverheadMs", static_cast<double>(ctx.timerCalls) * timerOverheadNs_ / 1e6},
                    {"storage", persistent() ? "file" : "memory"}};
    json events = json::array();
    for (const auto& e : ctx.events) {
        events.push_back({{"seq", e.seq}, {"nodeId", e.nodeId}, {"type", e.type}, {"rows", e.rows}, {"tMs", static_cast<double>(e.tNs) / 1e6}});
    }
    r["events"] = {{"mode", "recorded"},
                   {"note", "Events were recorded on the real execution path and are replayed after the query finished."},
                   {"events", events},
                   {"truncated", ctx.eventsTruncated},
                   {"maxEvents", limits.maxEvents}};

    in.stats = &ctx.stats;
    in.executeMs = execMs;
    in.resultRows = res.totalRows;
    in.timerCalls = ctx.timerCalls;
    r["explanation"] = explain::explainExecution(in);
    r["advisor"] = explain::adviseIndexes(in);
    if (!st.isSelect() && persistent()) checkpoint();
    return r;
}

void Database::recordHistory(const sql::Statement& st, const json& r) {
    if (!st.isSelect() || r.value("explain", false)) return;
    const std::string shape = queryShape(st.text);
    ++shapeCounts_[shape];
    json h{{"queryId", r["queryId"]}, {"sql", st.text}, {"rowCount", r.value("rowCount", 0)},
           {"executionMs", r.contains("metrics") ? r["metrics"]["executionMs"] : json(0)},
           {"pagesRead", r.contains("metrics") ? r["metrics"]["pagesRead"] : json(0)},
           {"plan", r.contains("physicalPlan") ? r["physicalPlan"]["label"] : json("")},
           {"shapeCount", shapeCounts_[shape]}};
    history_.push_front(std::move(h));
    while (history_.size() > 50) history_.pop_back();
}

int Database::queryFrequency(const std::string& shape) const {
    auto it = shapeCounts_.find(shape);
    return it == shapeCounts_.end() ? 0 : it->second;
}

// ---- inspection endpoints -------------------------------------------------------------

json Database::catalogJson() {
    std::lock_guard lock(mu_);
    json tables = json::array();
    for (auto* t : catalog_->tables()) {
        json cols = json::array();
        for (std::size_t i = 0; i < t->columns.size(); ++i) {
            cols.push_back({{"name", t->columns[i].name}, {"type", typeName(t->columns[i].type)}, {"stats", statsJson(t->stats[i])}});
        }
        json idx = json::array();
        for (auto* ix : t->indexes) {
            json ic = json::array();
            for (auto c : ix->columns) ic.push_back(t->columns[c].name);
            idx.push_back({{"name", ix->name}, {"columns", ic}, {"unique", ix->unique}, {"height", ix->tree->height()},
                           {"entries", ix->tree->size()}, {"definition", ix->definition(*t)}});
        }
        tables.push_back({{"name", t->name}, {"rowCount", t->rowCount()}, {"pageCount", t->pageCount()}, {"columns", cols}, {"indexes", idx}});
    }
    return json{{"tables", tables}, {"storage", persistent() ? "file" : "memory"}, {"path", path_}};
}

json Database::tableSchema(const std::string& name) {
    std::lock_guard lock(mu_);
    auto* t = catalog_->findTable(name);
    if (!t) throw QueryError(ErrorStage::Catalog, "table '" + name + "' does not exist");
    json cols = json::array();
    for (std::size_t i = 0; i < t->columns.size(); ++i) {
        const auto& st = t->stats[i];
        json hist = json::array();
        for (const auto& b : st.histogram()) hist.push_back({{"low", valueToJson(b.low)}, {"high", valueToJson(b.high)}, {"count", b.count}});
        cols.push_back({{"name", t->columns[i].name}, {"type", typeName(t->columns[i].type)}, {"stats", statsJson(st)}, {"histogram", hist}});
    }
    json idx = json::array();
    for (auto* ix : t->indexes) idx.push_back(ix->name);
    return json{{"name", t->name}, {"rowCount", t->rowCount()}, {"pageCount", t->pageCount()}, {"columns", cols}, {"indexes", idx}};
}

json Database::tableRows(const std::string& name, std::size_t offset, std::size_t limit) {
    std::lock_guard lock(mu_);
    auto* t = catalog_->findTable(name);
    if (!t) throw QueryError(ErrorStage::Catalog, "table '" + name + "' does not exist");
    limit = std::min<std::size_t>(limit, 1000);
    json rows = json::array();
    std::size_t seen = 0;
    for (auto pid : t->heap->pages()) {
        if (rows.size() >= limit) break;
        auto g = pool_->fetch(pid);
        const auto& p = g.page();
        for (std::uint16_t s = 0; s < p.slotCount() && rows.size() < limit; ++s) {
            if (!p.isLive(s)) continue;
            if (seen++ < offset) continue;
            Row row = storage::codec::decodeRow(p.get(s), t->columns.size());
            json jr = json::array();
            for (const auto& v : row) jr.push_back(valueToJson(v));
            rows.push_back({{"rid", {{"page", pid}, {"slot", s}}}, {"values", jr}});
        }
    }
    json cols = json::array();
    for (const auto& c : t->columns) cols.push_back({{"name", c.name}, {"type", typeName(c.type)}});
    return json{{"table", name}, {"columns", cols}, {"rows", rows}, {"offset", offset}, {"total", t->rowCount()}};
}

json Database::indexesJson() {
    std::lock_guard lock(mu_);
    json arr = json::array();
    for (auto* ix : catalog_->indexes()) {
        auto* t = catalog_->findTable(ix->table);
        json ic = json::array();
        for (auto c : ix->columns) ic.push_back(t->columns[c].name);
        arr.push_back({{"name", ix->name}, {"table", ix->table}, {"columns", ic}, {"unique", ix->unique}, {"height", ix->tree->height()},
                       {"entries", ix->tree->size()}, {"nodes", ix->tree->nodeCount()}, {"definition", ix->definition(*t)}});
    }
    return arr;
}

json Database::indexDetail(const std::string& name) {
    std::lock_guard lock(mu_);
    auto* ix = catalog_->findIndex(name);
    if (!ix) throw QueryError(ErrorStage::Catalog, "index '" + name + "' does not exist");
    auto* t = catalog_->findTable(ix->table);
    json d = ix->tree->describe();
    d["name"] = ix->name;
    d["table"] = ix->table;
    d["definition"] = ix->definition(*t);
    d["approximateBytes"] = ix->tree->approximateBytes();
    d["valid"] = ix->tree->validate().empty();
    return d;
}

json Database::storageJson() {
    std::lock_guard lock(mu_);
    json tables = json::array();
    for (auto* t : catalog_->tables()) {
        std::size_t cached = 0, used = 0, dead = 0;
        for (auto pid : t->heap->pages()) {
            if (pool_->isCached(pid)) ++cached;
        }
        // Sample page fill (first 64 pages) without disturbing statistics too much.
        json pages = json::array();
        const auto& pids = t->heap->pages();
        for (std::size_t i = 0; i < pids.size() && i < 64; ++i) {
            const bool wasCached = pool_->isCached(pids[i]);
            auto g = pool_->fetch(pids[i]);
            const auto& p = g.page();
            const std::size_t live = p.liveCount();
            used += storage::kPageSize - p.freeSpace();
            dead += p.slotCount() - live;
            pages.push_back({{"pageId", pids[i]}, {"slots", p.slotCount()}, {"live", live}, {"freeBytes", p.freeSpace()}, {"cached", wasCached}});
        }
        tables.push_back({{"name", t->name}, {"pageCount", pids.size()}, {"rowCount", t->rowCount()},
                          {"rowsPerPage", pids.empty() ? 0.0 : static_cast<double>(t->rowCount()) / static_cast<double>(pids.size())},
                          {"cachedPages", cached}, {"pages", pages}, {"deadSlotsInSample", dead}});
    }
    const auto& s = pool_->stats();
    return json{{"mode", persistent() ? "file" : "memory"},
                {"path", path_},
                {"pageSize", storage::kPageSize},
                {"totalPages", store_->pageCount()},
                {"bufferPool",
                 {{"capacity", pool_->capacity()}, {"cached", pool_->cachedPages()}, {"logicalReads", s.logicalReads}, {"hits", s.hits},
                  {"misses", s.misses}, {"evictions", s.evictions}, {"writes", s.writes}}},
                {"tables", tables},
                {"note", persistent() ? "Pages are read from the database file on a buffer-pool miss (the OS file cache may still serve them)."
                                      : "In-memory mode: a buffer-pool miss copies the page from the in-memory page store, so misses are real "
                                        "events but cost far less than disk I/O."}};
}

json Database::pageDetail(const std::string& table, std::size_t pageIndex) {
    std::lock_guard lock(mu_);
    auto* t = catalog_->findTable(table);
    if (!t) throw QueryError(ErrorStage::Catalog, "table '" + table + "' does not exist");
    if (pageIndex >= t->heap->pages().size()) throw QueryError(ErrorStage::Api, "page index out of range");
    const auto pid = t->heap->pages()[pageIndex];
    const bool wasCached = pool_->isCached(pid);
    auto g = pool_->fetch(pid);
    const auto& p = g.page();
    json slots = json::array();
    for (std::uint16_t s = 0; s < p.slotCount(); ++s) {
        json slot{{"slot", s}, {"live", p.isLive(s)}};
        if (p.isLive(s)) {
            auto bytes = p.get(s);
            slot["bytes"] = bytes.size();
            Row row = storage::codec::decodeRow(bytes, t->columns.size());
            json vals = json::array();
            for (const auto& v : row) vals.push_back(valueToJson(v));
            slot["values"] = vals;
        }
        slots.push_back(std::move(slot));
    }
    return json{{"table", table}, {"pageIndex", pageIndex}, {"pageId", pid}, {"slotCount", p.slotCount()}, {"liveRows", p.liveCount()},
                {"freeBytes", p.freeSpace()}, {"pageSize", storage::kPageSize}, {"wasCached", wasCached}, {"slots", slots}};
}

json Database::datasetsJson() {
    json arr = json::array();
    for (const auto& d : availableDatasets()) {
        json q = json::array();
        for (const auto& [title, sql] : d.sampleQueries) q.push_back({{"title", title}, {"sql", sql}});
        arr.push_back({{"name", d.name}, {"title", d.title}, {"description", d.description}, {"tables", d.tables}, {"sampleQueries", q}});
    }
    return arr;
}

json Database::loadDataset(const std::string& name, std::size_t scale) {
    std::lock_guard lock(mu_);
    const auto t0 = Clock::now();
    json counts = api::loadDataset(*catalog_, name, scale);
    const double ms = msSince(t0);
    if (persistent()) checkpoint();
    return json{{"dataset", name}, {"scale", scale}, {"tables", counts}, {"ms", ms}};
}

json Database::reset() {
    std::lock_guard lock(mu_);
    for (auto* t : catalog_->tables()) catalog_->dropTable(t->name);
    shapeCounts_.clear();
    history_.clear();
    if (persistent()) checkpoint();
    return json{{"ok", true}};
}

json Database::history() {
    std::lock_guard lock(mu_);
    json arr = json::array();
    for (const auto& h : history_) arr.push_back(h);
    return arr;
}

json Database::settingsJson() {
    std::lock_guard lock(mu_);
    return json{{"bufferPoolPages", pool_->capacity()},
                {"timeoutMs", opts_.limits.timeout.count()},
                {"maxResultRows", opts_.limits.maxResultRows},
                {"maxEvents", opts_.limits.maxEvents},
                {"maxMemoryMB", opts_.limits.maxMemoryBytes / (1024 * 1024)},
                {"timerOverheadNs", timerOverheadNs_}};
}

json Database::updateSettings(const json& patch) {
    std::lock_guard lock(mu_);
    auto clampGet = [&](const char* key, std::int64_t lo, std::int64_t hi) -> std::optional<std::int64_t> {
        if (!patch.contains(key)) return std::nullopt;
        if (!patch[key].is_number_integer()) throw QueryError(ErrorStage::Api, std::string("setting '") + key + "' must be an integer");
        const auto v = patch[key].get<std::int64_t>();
        if (v < lo || v > hi) {
            throw QueryError(ErrorStage::Api, std::string("setting '") + key + "' must be between " + std::to_string(lo) + " and " + std::to_string(hi));
        }
        return v;
    };
    if (auto v = clampGet("bufferPoolPages", 4, 1 << 18)) {
        pool_->resize(static_cast<std::size_t>(*v));
        opts_.bufferPoolPages = static_cast<std::size_t>(*v);
    }
    if (auto v = clampGet("timeoutMs", 100, 600000)) opts_.limits.timeout = std::chrono::milliseconds(*v);
    if (auto v = clampGet("maxResultRows", 1, 100000)) opts_.limits.maxResultRows = static_cast<std::size_t>(*v);
    if (auto v = clampGet("maxEvents", 0, 50000)) opts_.limits.maxEvents = static_cast<std::size_t>(*v);
    if (auto v = clampGet("maxMemoryMB", 16, 8192)) opts_.limits.maxMemoryBytes = static_cast<std::size_t>(*v) * 1024 * 1024;
    return settingsJson();
}

json Database::health() {
    return json{{"status", "ok"}, {"engine", "QueryLab"}, {"version", "1.0.0"}, {"storage", persistent() ? "file" : "memory"}};
}

// ---- benchmarks ---------------------------------------------------------------------------

json Database::benchmark(const json& req) {
    std::lock_guard lock(mu_);
    const int iterations = req.value("iterations", 10);
    const int warmup = req.value("warmup", 2);
    if (iterations < 1 || iterations > 500) throw QueryError(ErrorStage::Api, "iterations must be between 1 and 500");
    if (warmup < 0 || warmup > 50) throw QueryError(ErrorStage::Api, "warmup must be between 0 and 50");

    // Variants: [{label, sql, disableIndexScans, coldCache, disableHashJoin}]
    json variants = req.contains("variants") ? req["variants"] : json::array();
    if (variants.empty() && req.contains("sql")) {
        variants.push_back({{"label", "query"}, {"sql", req["sql"]}, {"disableIndexScans", req.value("disableIndexScans", false)},
                            {"coldCache", req.value("coldCache", false)}});
    }
    if (variants.empty() || variants.size() > 4) throw QueryError(ErrorStage::Api, "provide 1 to 4 variants (or a 'sql' field)");

    json out = json::array();
    const auto budgetEnd = Clock::now() + std::chrono::seconds(30);
    for (const auto& v : variants) {
        const std::string sql = v.at("sql").get<std::string>();
        auto stmts = sql::parseScript(sql);
        if (stmts.size() != 1 || !stmts[0].isSelect()) throw QueryError(ErrorStage::Api, "benchmarks run exactly one SELECT statement per variant");
        QueryOptions qo;
        qo.disableIndexScans = v.value("disableIndexScans", false);
        qo.disableHashJoin = v.value("disableHashJoin", false);
        qo.coldCache = v.value("coldCache", false);
        qo.timing = false;        // no per-operator timers: measure total time only
        qo.recordEvents = false;
        qo.maxRows = 1;           // results are discarded; counting continues
        qo.recordHistory = false;

        std::vector<double> samples;
        std::vector<double> misses;
        json last;
        for (int i = 0; i < warmup + iterations; ++i) {
            if (Clock::now() > budgetEnd) break;
            json res = execute(sql, qo);
            if (!res["success"].get<bool>()) return json{{"success", false}, {"error", res["error"]}, {"variant", v.value("label", "")}};
            const auto& r = res["results"][0];
            if (i >= warmup) {
                samples.push_back(r["metrics"]["executionMs"].get<double>());
                misses.push_back(r["metrics"]["pageMisses"].get<double>());
            }
            last = r;
        }
        std::vector<double> sorted = samples;
        std::sort(sorted.begin(), sorted.end());
        auto quant = [&](double q) {
            if (sorted.empty()) return 0.0;
            const double pos = q * static_cast<double>(sorted.size() - 1);
            const auto lo = static_cast<std::size_t>(std::floor(pos));
            const auto hi = static_cast<std::size_t>(std::ceil(pos));
            return sorted[lo] + (sorted[hi] - sorted[lo]) * (pos - static_cast<double>(lo));
        };
        double mean = 0;
        for (double s : samples) mean += s;
        mean = samples.empty() ? 0 : mean / static_cast<double>(samples.size());
        double var = 0;
        for (double s : samples) var += (s - mean) * (s - mean);
        const double stddev = samples.size() > 1 ? std::sqrt(var / static_cast<double>(samples.size() - 1)) : 0.0;
        double avgMisses = 0;
        for (double m : misses) avgMisses += m;
        avgMisses = misses.empty() ? 0 : avgMisses / static_cast<double>(misses.size());

        out.push_back({{"label", v.value("label", "query")},
                       {"sql", sql},
                       {"options", {{"disableIndexScans", qo.disableIndexScans}, {"disableHashJoin", qo.disableHashJoin}, {"coldCache", qo.coldCache}}},
                       {"iterations", samples.size()},
                       {"warmup", warmup},
                       {"samplesMs", samples},
                       {"stats", {{"min", sorted.empty() ? 0 : sorted.front()}, {"p25", quant(0.25)}, {"median", quant(0.5)}, {"p75", quant(0.75)},
                                  {"max", sorted.empty() ? 0 : sorted.back()}, {"mean", mean}, {"stddev", stddev},
                                  {"cv", mean > 0 ? stddev / mean : 0}}},
                       {"rowsReturned", last["rowCount"]},
                       {"rowsScanned", last["metrics"]["rowsScanned"]},
                       {"pagesRead", last["metrics"]["pagesRead"]},
                       {"avgPageMisses", avgMisses},
                       {"indexEntriesExamined", last["metrics"]["indexEntriesExamined"]},
                       {"planningMs", last["metrics"]["planningMs"]},
                       {"plan", last["physicalPlan"]},
                       {"planLabel", last["physicalPlan"]["label"]}});
    }
    return json{{"success", true},
                {"variants", out},
                {"timerOverheadNs", timerOverheadNs_},
                {"notes", json::array({"Times are engine execution only (no parsing, planning, JSON encoding or network) with per-operator timers disabled.",
                                       "Warm-up runs are discarded. Medians are more robust than means against scheduling noise.",
                                       "Cold-cache runs empty QueryLab's buffer pool before each iteration; the operating system's file cache is not controlled.",
                                       "Results depend on this machine and its current load; repeat runs to check stability."})}};
}

}  // namespace querylab::api
