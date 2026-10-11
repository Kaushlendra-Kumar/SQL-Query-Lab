// Engine facade: response DTOs, persistence, limits, benchmarks, advisor.
#include "test_helpers.h"

#include <filesystem>
#include <fstream>

#include "querylab/api/datasets.h"

using namespace qltest;
using querylab::api::Database;
namespace fs = std::filesystem;

namespace {
std::string tempPath(const std::string& name) {
    auto p = fs::temp_directory_path() / ("querylab_test_" + name + ".qldb");
    fs::remove(p);
    return p.string();
}
}  // namespace

TEST_CASE("query response contains every pipeline artifact") {
    auto db = Database::inMemory();
    usersFixture(*db);
    auto r = run(*db, "SELECT name FROM users WHERE age > 25 ORDER BY name");
    for (const char* key : {"queryId", "tokens", "ast", "logicalPlan", "optimizedPlan", "physicalPlan", "optimizer", "columns", "rows",
                            "metrics", "events", "explanation", "advisor", "timeline"}) {
        CHECK_MESSAGE(r.contains(key), key);
    }
    std::vector<std::string> stages;
    for (const auto& s : r["timeline"]) stages.push_back(s["stage"]);
    CHECK(stages == std::vector<std::string>{"lexer", "parser", "binder", "optimizer", "planner", "executor"});
    CHECK(r["columns"][0]["name"] == "name");
    CHECK(r["columns"][0]["type"] == "TEXT");
    // Physical nodes carry both estimates and actuals; actual rows are real.
    const json* scan = findOp(r["physicalPlan"], "Seq Scan");
    REQUIRE(scan);
    CHECK((*scan)["actual"]["rowsScanned"] == 5);
    CHECK((*scan)["actual"]["rowsOut"] == 3);
    CHECK((*scan)["actual"]["rowsFiltered"] == 2);
    CHECK((*scan)["estimates"].contains("rows"));
    // Events follow the real execution: open before rows, close at the end.
    const auto& ev = r["events"]["events"];
    REQUIRE(ev.size() > 4);
    CHECK(ev[0]["type"] == "open");
    CHECK(ev.back()["type"] == "close");
    int scanRows = 0;
    for (const auto& e : ev) {
        if (e["nodeId"] == (*scan)["id"] && e["type"] == "row") ++scanRows;
    }
    CHECK(scanRows == 3);
    // Tokens include the statement's comment tokens too.
    auto t = run(*db, "SELECT /* note */ 1");
    CHECK(t["tokens"][1]["type"] == "comment");
}

TEST_CASE("EXPLAIN plans without executing") {
    auto db = Database::inMemory();
    usersFixture(*db);
    auto r = run(*db, "EXPLAIN SELECT name FROM users WHERE age > 25");
    CHECK(r["explain"] == true);
    CHECK(r["rows"].empty());
    CHECK_FALSE(r["physicalPlan"].contains("actual"));
    CHECK_FALSE(r.contains("metrics"));
    auto e = db->explain("DELETE FROM users WHERE id = 1");
    CHECK(e["success"] == true);
    CHECK(rows(*db, "SELECT COUNT(*) FROM users") == Rows{{"5"}});  // nothing deleted
}

TEST_CASE("result size and time limits") {
    auto db = Database::inMemory();
    run(*db, "CREATE TABLE n (x INTEGER)");
    std::string ins = "INSERT INTO n VALUES ";
    for (int i = 0; i < 300; ++i) ins += (i ? "," : "") + std::string("(") + std::to_string(i) + ")";
    run(*db, ins);
    querylab::api::QueryOptions o;
    o.maxRows = 50;
    auto r = run(*db, "SELECT * FROM n", o);
    CHECK(r["rows"].size() == 50);
    CHECK(r["rowCount"] == 300);
    CHECK(r["truncated"] == true);
    // A runaway cross join is stopped by the time limit, not the process.
    db->updateSettings({{"timeoutMs", 200}});
    auto slow = db->execute("SELECT COUNT(*) FROM n a, n b, n c, n d");
    CHECK_FALSE(slow["success"].get<bool>());
    CHECK(slow["error"]["message"].get<std::string>().find("time limit") != std::string::npos);
    // The engine is still usable afterwards.
    CHECK(rows(*db, "SELECT COUNT(*) FROM n") == Rows{{"300"}});
    CHECK_THROWS_AS(db->updateSettings({{"timeoutMs", 1}}), querylab::QueryError);
}

TEST_CASE("persistence: data, indexes and statistics survive reopening") {
    const auto path = tempPath("persist");
    {
        auto db = Database::openFile(path, true);
        usersFixture(*db);
        run(*db, "CREATE INDEX idx_city ON users(city); CREATE UNIQUE INDEX idx_id ON users(id); UPDATE users SET age = 30 WHERE id = 1; DELETE FROM users WHERE id = 5;");
        std::string ins = "INSERT INTO users VALUES ";
        for (int i = 100; i < 1100; ++i) ins += (i > 100 ? "," : "") + std::string("(") + std::to_string(i) + ", 'n" + std::to_string(i) + "', 50, 'Goa')";
        run(*db, ins);
    }
    {
        auto db = Database::openFile(path, false);
        CHECK(rows(*db, "SELECT COUNT(*) FROM users") == Rows{{"1004"}});
        CHECK(rows(*db, "SELECT age FROM users WHERE id = 1") == Rows{{"30"}});
        CHECK(rows(*db, "SELECT id FROM users WHERE city = 'Delhi' ORDER BY id") == Rows{{"2"}, {"4"}});
        auto r = run(*db, "SELECT name FROM users WHERE id = 4");
        CHECK(r["rows"] == json::parse("[[\"Meera\"]]"));
        CHECK(findOp(r["physicalPlan"], "Index Scan") != nullptr);  // index rebuilt on open
        CHECK(db->indexesJson().size() == 2);
        CHECK(fails(*db, "INSERT INTO users VALUES (4, 'dup', 1, 'x')")["message"].get<std::string>().find("unique") != std::string::npos);
        auto cat = db->catalogJson();
        CHECK(cat["tables"][0]["columns"][3]["stats"]["distinct"] == 4);  // stats rebuilt
        run(*db, "INSERT INTO users VALUES (2000, 'late', 1, 'X')");
    }
    {
        auto db = Database::openFile(path, false);
        CHECK(rows(*db, "SELECT name FROM users WHERE id = 2000") == Rows{{"late"}});
    }
    fs::remove(path);
}

TEST_CASE("persistence: corrupted and invalid files are rejected") {
    const auto path = tempPath("corrupt");
    {
        auto db = Database::openFile(path, true);
        usersFixture(*db);
    }
    {
        // Flip a byte inside the first data page.
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(static_cast<std::streamoff>(querylab::storage::kPageSize) * 1 + 3000);
        char c = 0;
        f.read(&c, 1);
        f.seekp(static_cast<std::streamoff>(querylab::storage::kPageSize) * 1 + 3000);
        c = static_cast<char>(c ^ 0x5A);
        f.write(&c, 1);
    }
    bool rejected = false;
    try {
        auto db = Database::openFile(path, false);
        auto out = db->execute("SELECT * FROM users");
        rejected = !out["success"].get<bool>() && out["error"]["message"].get<std::string>().find("checksum") != std::string::npos;
    } catch (const querylab::QueryError& e) {
        rejected = std::string(e.what()).find("checksum") != std::string::npos;
    }
    CHECK(rejected);
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << "this is not a database";
    }
    CHECK_THROWS_AS(Database::openFile(path, false), querylab::QueryError);
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        std::string junk(querylab::storage::kPageSize, 'x');
        f << junk;
    }
    CHECK_THROWS_AS(Database::openFile(path, false), querylab::QueryError);
    fs::remove(path);
    CHECK_THROWS_AS(Database::openFile(path, false), querylab::QueryError);
}

TEST_CASE("datasets are deterministic and well formed") {
    auto a = Database::inMemory();
    auto b = Database::inMemory();
    a->loadDataset("social", 500);
    b->loadDataset("social", 500);
    CHECK(run(*a, "SELECT SUM(likes), COUNT(*) FROM posts")["rows"] == run(*b, "SELECT SUM(likes), COUNT(*) FROM posts")["rows"]);
    a->loadDataset("employees", 300);
    CHECK(rows(*a, "SELECT COUNT(*) FROM salaries") == Rows{{"1200"}});
    // Every sample query of every dataset runs.
    auto db = Database::inMemory();
    for (const auto& d : querylab::api::availableDatasets()) {
        db->loadDataset(d.name, 1000);
        for (const auto& [title, sql] : d.sampleQueries) {
            auto out = db->execute(sql);
            CHECK_MESSAGE(out["success"].get<bool>(), d.name << ": " << title << " -> " << out.value("error", json::object()).dump());
        }
    }
    CHECK_THROWS_AS(db->loadDataset("nope", 100), querylab::QueryError);
    CHECK_THROWS_AS(db->loadDataset("social", 5), querylab::QueryError);
}

TEST_CASE("benchmarks report distributions for each variant") {
    auto db = Database::inMemory();
    db->loadDataset("ecommerce", 3000);
    run(*db, "CREATE INDEX idx_orders_customer ON orders(customer_id)");
    auto b = db->benchmark({{"iterations", 5},
                            {"warmup", 1},
                            {"variants", json::array({{{"label", "index"}, {"sql", "SELECT * FROM orders WHERE customer_id = 7"}},
                                                      {{"label", "seq"}, {"sql", "SELECT * FROM orders WHERE customer_id = 7"}, {"disableIndexScans", true}},
                                                      {{"label", "cold"}, {"sql", "SELECT * FROM orders WHERE customer_id = 7"}, {"coldCache", true}}})}});
    REQUIRE(b["success"] == true);
    REQUIRE(b["variants"].size() == 3);
    const auto& idx = b["variants"][0];
    const auto& seq = b["variants"][1];
    CHECK(idx["samplesMs"].size() == 5);
    CHECK(idx["stats"]["median"].get<double>() >= idx["stats"]["min"].get<double>());
    CHECK(idx["rowsReturned"] == seq["rowsReturned"]);
    CHECK(idx["pagesRead"].get<int>() < seq["pagesRead"].get<int>());
    CHECK(findOp(idx["plan"], "Index Scan") != nullptr);
    CHECK(findOp(seq["plan"], "Index Scan") == nullptr);
    CHECK(b["variants"][2]["avgPageMisses"].get<double>() > 0);
    CHECK_THROWS_AS(db->benchmark({{"sql", "DELETE FROM orders"}}), querylab::QueryError);
    CHECK_THROWS_AS(db->benchmark({{"sql", "SELECT 1"}, {"iterations", 0}}), querylab::QueryError);
    // Benchmarks do not count as workload history.
    CHECK(db->history().empty());
}

TEST_CASE("index advisor: grounded suggestions and honest notes") {
    auto db = Database::inMemory();
    db->loadDataset("ecommerce", 5000);
    auto r = run(*db, "SELECT * FROM orders WHERE customer_id = 42");
    const auto& sug = r["advisor"]["suggestions"];
    REQUIRE(sug.size() == 1);
    CHECK(sug[0]["definition"] == "CREATE INDEX idx_orders_customer_id ON orders (customer_id)");
    CHECK(sug[0]["estimatedCost"]["withIndex"].get<double>() < sug[0]["estimatedCost"]["current"].get<double>());
    CHECK(sug[0]["verify"].size() >= 3);
    // Unselective filter: no suggestion, explained in a note.
    auto w = run(*db, "SELECT COUNT(*) FROM orders WHERE total > 1");
    CHECK(w["advisor"]["suggestions"].empty());
    CHECK(w["advisor"]["notes"][0]["kind"] == "unselective");
    // Equality + range: composite index with equality column first.
    auto c = run(*db, "SELECT id FROM orders WHERE total > 1500 AND customer_id = 3");
    REQUIRE(c["advisor"]["suggestions"].size() == 1);
    CHECK(c["advisor"]["suggestions"][0]["columns"] == json::array({"customer_id", "total"}));
    // After creating the index the advisor stops suggesting it.
    run(*db, "CREATE INDEX idx_orders_customer_id ON orders(customer_id)");
    auto again = run(*db, "SELECT * FROM orders WHERE customer_id = 42");
    CHECK(again["advisor"]["suggestions"].empty());
    // Workload frequency is tracked by query shape (constants ignored).
    run(*db, "SELECT * FROM orders WHERE status = 'returned' AND id < 50");
    auto f = run(*db, "SELECT * FROM orders WHERE status = 'cancelled' AND id < 60");
    bool sawFreq = false;
    for (const auto& s : f["advisor"]["suggestions"]) {
        for (const auto& e : s["evidence"]) sawFreq = sawFreq || e.get<std::string>().find("ran 2 times") != std::string::npos;
    }
    CHECK(sawFreq);
    CHECK(querylab::api::queryShape("SELECT * FROM t WHERE a = 1") == querylab::api::queryShape("select * from t where a = 99"));
}

TEST_CASE("inspection endpoints") {
    auto db = Database::inMemory();
    usersFixture(*db);
    run(*db, "CREATE INDEX idx_age ON users(age)");
    auto cat = db->catalogJson();
    CHECK(cat["tables"][0]["name"] == "users");
    CHECK(cat["tables"][0]["indexes"][0]["name"] == "idx_age");
    auto rowsJ = db->tableRows("users", 1, 2);
    CHECK(rowsJ["rows"].size() == 2);
    CHECK(rowsJ["rows"][0]["values"][0] == 2);
    auto schema = db->tableSchema("users");
    CHECK(schema["columns"][2]["stats"]["nulls"] == 1);
    auto idx = db->indexDetail("idx_age");
    CHECK(idx["valid"] == true);
    CHECK(idx["entries"] == 5);
    db->updateSettings({{"bufferPoolPages", 8}});  // empties the pool
    auto before = db->storageJson()["bufferPool"];
    auto storage = db->storageJson();
    (void)db->pageDetail("users", 0);
    (void)db->tableRows("users", 0, 10);
    auto after = db->storageJson()["bufferPool"];
    CHECK(storage["tables"][0]["pageCount"] == 1);
    // Inspecting storage must not change buffer-pool contents or statistics.
    CHECK(before == after);
    CHECK(after["cached"] == 0);
    auto page = db->pageDetail("users", 0);
    CHECK(page["liveRows"] == 5);
    CHECK_THROWS_AS(db->pageDetail("users", 9), querylab::QueryError);
    CHECK_THROWS_AS(db->tableRows("nope", 0, 1), querylab::QueryError);
    CHECK(db->history().size() == 0);
    run(*db, "SELECT * FROM users");
    CHECK(db->history().size() == 1);
    db->reset();
    CHECK(db->catalogJson()["tables"].empty());
}
