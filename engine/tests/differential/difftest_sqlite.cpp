// Differential testing against SQLite on the shared SQL subset.
//
// Random tables and random queries (predicates with AND/OR/NOT, comparisons,
// IS NULL, BETWEEN, IN, arithmetic; joins; GROUP BY aggregates) are run on
// both engines and the result multisets compared. Dialect differences are
// avoided by construction: no division (SQLite returns NULL on /0), no LIKE
// (SQLite is case-insensitive), small integers (no overflow).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "querylab/api/database.h"
#include "querylab/api/datasets.h"

using querylab::api::Database;
using querylab::api::json;
using querylab::api::Rng;
using Rows = std::vector<std::vector<std::string>>;

namespace {

std::string norm(double d) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6g", d);  // tolerate summation-order rounding
    return buf;
}

std::string normJson(const json& v) {
    if (v.is_null()) return "NULL";
    if (v.is_string()) return "'" + v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "1" : "0";
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    return norm(v.get<double>());
}

class Lite {
public:
    Lite() { sqlite3_open(":memory:", &db_); }
    ~Lite() { sqlite3_close(db_); }
    void exec(const std::string& sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            std::string m = err ? err : "?";
            sqlite3_free(err);
            FAIL("sqlite: " << m << " in " << sql);
        }
    }
    Rows query(const std::string& sql) {
        sqlite3_stmt* st = nullptr;
        Rows out;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
            FAIL("sqlite prepare: " << sqlite3_errmsg(db_) << " in " << sql);
            return out;
        }
        while (sqlite3_step(st) == SQLITE_ROW) {
            std::vector<std::string> row;
            for (int i = 0; i < sqlite3_column_count(st); ++i) {
                switch (sqlite3_column_type(st, i)) {
                    case SQLITE_NULL: row.push_back("NULL"); break;
                    case SQLITE_INTEGER: row.push_back(std::to_string(sqlite3_column_int64(st, i))); break;
                    case SQLITE_FLOAT: row.push_back(norm(sqlite3_column_double(st, i))); break;
                    default: row.push_back("'" + std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, i)))); break;
                }
            }
            out.push_back(std::move(row));
        }
        sqlite3_finalize(st);
        return out;
    }

private:
    sqlite3* db_ = nullptr;
};

Rows ql(Database& db, const std::string& sql) {
    json out = db.execute(sql);
    Rows rows;
    if (!out["success"].get<bool>()) {
        FAIL_CHECK("querylab: " << out["error"]["display"].get<std::string>() << " in " << sql);
        return rows;
    }
    for (const auto& r : out["results"].back()["rows"]) {
        std::vector<std::string> row;
        for (const auto& v : r) row.push_back(normJson(v));
        rows.push_back(std::move(row));
    }
    return rows;
}

class Gen {
public:
    explicit Gen(std::uint64_t seed) : r_(seed) {}

    std::string intLit() { return std::to_string(r_.range(-3, 12)); }
    std::string textLit() { return "'" + std::string(1, static_cast<char>('a' + r_.range(0, 5))) + "'"; }
    std::string maybeNull(const std::string& v) { return r_.range(0, 9) == 0 ? "NULL" : v; }

    std::string numExpr(const std::string& t, int depth) {
        const auto k = r_.range(0, depth > 1 ? 2 : 5);
        if (k == 0 || depth > 2) {
            const auto c = r_.range(0, 3);
            if (c == 0) return t + "a";
            if (c == 1) return t + "b";
            if (c == 2) return t + "f";
            return intLit();
        }
        static const char* ops[] = {"+", "-", "*"};
        return "(" + numExpr(t, depth + 1) + " " + ops[r_.range(0, 2)] + " " + numExpr(t, depth + 1) + ")";
    }

    std::string pred(const std::string& t, int depth = 0) {
        const auto k = r_.range(0, depth >= 3 ? 5 : 8);
        static const char* cmp[] = {"=", "<>", "<", "<=", ">", ">="};
        switch (k) {
            case 0: return numExpr(t, 1) + " " + cmp[r_.range(0, 5)] + " " + maybeNull(intLit());
            case 1: return t + "c " + cmp[r_.range(0, 5)] + " " + maybeNull(textLit());
            case 2: return t + (r_.range(0, 1) ? "a" : "c") + (r_.range(0, 1) ? " IS NULL" : " IS NOT NULL");
            case 3: return t + "b" + (r_.range(0, 1) ? " NOT" : "") + " BETWEEN " + intLit() + " AND " + maybeNull(intLit());
            case 4: return t + "a" + (r_.range(0, 1) ? " NOT" : "") + " IN (" + intLit() + ", " + maybeNull(intLit()) + ", " + intLit() + ")";
            case 5: return t + "a " + cmp[r_.range(0, 5)] + " " + t + "b";
            case 6: return "(" + pred(t, depth + 1) + " AND " + pred(t, depth + 1) + ")";
            case 7: return "(" + pred(t, depth + 1) + " OR " + pred(t, depth + 1) + ")";
            default: return "NOT (" + pred(t, depth + 1) + ")";
        }
    }

    Rng& rng() { return r_; }

private:
    Rng r_;
};

void makeTables(Database& q, Lite& s, Rng& r, int n) {
    for (const char* name : {"t", "u"}) {
        const std::string ddl = std::string("CREATE TABLE ") + name + " (a INTEGER, b INTEGER, c TEXT, f FLOAT)";
        q.execute(ddl);
        s.exec(ddl);
        std::string ins = std::string("INSERT INTO ") + name + " VALUES ";
        for (int i = 0; i < n; ++i) {
            auto v = [&](std::int64_t lo, std::int64_t hi) {
                return r.range(0, 7) == 0 ? std::string("NULL") : std::to_string(r.range(lo, hi));
            };
            const std::string c = r.range(0, 7) == 0 ? "NULL" : "'" + std::string(1, static_cast<char>('a' + r.range(0, 5))) + "'";
            const std::string f = r.range(0, 7) == 0 ? "NULL" : std::to_string(r.range(-20, 20)) + ".5";
            ins += (i ? "," : "") + std::string("(") + v(0, 10) + "," + v(-2, 10) + "," + c + "," + f + ")";
        }
        q.execute(ins);
        s.exec(ins);
    }
}

void compare(Database& q, Lite& s, const std::string& sql, bool ordered) {
    Rows a = ql(q, sql);
    Rows b = s.query(sql);
    if (!ordered) {
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
    }
    CHECK_MESSAGE(a == b, "mismatch for: " << sql << " (querylab " << a.size() << " rows, sqlite " << b.size() << " rows)");
}

}  // namespace

TEST_CASE("differential: random predicates (with and without indexes)") {
    for (std::uint64_t seed = 1; seed <= 4; ++seed) {
        auto q = Database::inMemory();
        Lite s;
        Gen g(seed);
        makeTables(*q, s, g.rng(), 300);
        if (seed % 2 == 0) {
            // Same queries must give the same answers whatever indexes exist.
            q->execute("CREATE INDEX ia ON t(a); CREATE INDEX ibc ON t(b, c); CREATE INDEX iuf ON u(f)");
        }
        for (int i = 0; i < 150; ++i) {
            const std::string p = g.pred("");
            compare(*q, s, "SELECT a, b, c, f FROM t WHERE " + p, false);
        }
    }
}

TEST_CASE("differential: ORDER BY, LIMIT, DISTINCT") {
    auto q = Database::inMemory();
    Lite s;
    Gen g(99);
    makeTables(*q, s, g.rng(), 200);
    for (int i = 0; i < 60; ++i) {
        const std::string p = g.pred("");
        compare(*q, s, "SELECT a, b, c, f FROM t WHERE " + p + " ORDER BY a DESC, b, c, f DESC LIMIT 15 OFFSET 2", true);
        compare(*q, s, "SELECT DISTINCT a, c FROM t WHERE " + p, false);
    }
}

TEST_CASE("differential: aggregates and GROUP BY / HAVING") {
    auto q = Database::inMemory();
    Lite s;
    Gen g(7);
    makeTables(*q, s, g.rng(), 400);
    for (int i = 0; i < 60; ++i) {
        const std::string p = g.pred("");
        compare(*q, s, "SELECT c, COUNT(*), COUNT(a), SUM(b), MIN(f), MAX(a), SUM(f) FROM t WHERE " + p + " GROUP BY c", false);
        compare(*q, s, "SELECT a, COUNT(DISTINCT c), AVG(b) FROM t GROUP BY a HAVING COUNT(*) > " + std::to_string(i % 5) + " AND " + "MIN(b) < " +
                           std::to_string(i % 9),
                false);
        compare(*q, s, "SELECT COUNT(*), SUM(a), AVG(f) FROM t WHERE " + p, false);
    }
}

TEST_CASE("differential: joins") {
    for (std::uint64_t seed = 11; seed <= 12; ++seed) {
        auto q = Database::inMemory();
        Lite s;
        Gen g(seed);
        makeTables(*q, s, g.rng(), 120);
        for (int i = 0; i < 40; ++i) {
            const std::string p1 = g.pred("t.");
            const std::string p2 = g.pred("u.");
            compare(*q, s, "SELECT t.a, t.c, u.b, u.f FROM t JOIN u ON t.a = u.a WHERE " + p1 + " AND " + p2, false);
            compare(*q, s, "SELECT t.a, t.b, u.c FROM t LEFT JOIN u ON t.b = u.b AND " + p2 + " WHERE " + p1, false);
            compare(*q, s, "SELECT t.c, COUNT(*), SUM(u.b) FROM t JOIN u ON t.c = u.c AND t.a < u.a GROUP BY t.c", false);
            compare(*q, s, "SELECT COUNT(*) FROM t, u WHERE t.a = u.b AND " + p1, false);
        }
    }
}

TEST_CASE("differential: sample datasets") {
    // The deterministic datasets themselves, compared through SQL dumps.
    auto q = Database::inMemory();
    q->loadDataset("ecommerce", 800);
    Lite s;
    for (const char* table : {"customers", "products", "orders", "order_items"}) {
        auto schema = q->tableSchema(table);
        std::string ddl = std::string("CREATE TABLE ") + table + " (";
        for (std::size_t i = 0; i < schema["columns"].size(); ++i) {
            ddl += (i ? ", " : "") + schema["columns"][i]["name"].get<std::string>() + " " + schema["columns"][i]["type"].get<std::string>();
        }
        s.exec(ddl + ")");
        auto rowsJ = q->execute(std::string("SELECT * FROM ") + table, {})["results"][0]["rows"];
        s.exec("BEGIN");
        for (const auto& r : rowsJ) {
            std::string ins = std::string("INSERT INTO ") + table + " VALUES (";
            for (std::size_t i = 0; i < r.size(); ++i) {
                std::string v;
                if (r[i].is_null()) v = "NULL";
                else if (r[i].is_string()) v = "'" + r[i].get<std::string>() + "'";
                else v = r[i].dump();
                ins += (i ? ", " : "") + v;
            }
            s.exec(ins + ")");
        }
        s.exec("COMMIT");
    }
    for (const auto& d : querylab::api::availableDatasets()) {
        if (d.name != "ecommerce") continue;
        for (const auto& [title, sql] : d.sampleQueries) {
            // LIMIT over tied sort keys may legitimately pick different rows.
            if (sql.rfind("CREATE", 0) == 0 || sql.find("LIMIT") != std::string::npos) continue;
            compare(*q, s, sql, false);
        }
    }
}
