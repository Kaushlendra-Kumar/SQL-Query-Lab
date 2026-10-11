// Optimizer rules: each must (1) transform the plan as intended and
// (2) preserve results. Results are compared with the rule's effect disabled
// where possible, or against known answers.
#include "test_helpers.h"

#include "querylab/optimizer/optimizer.h"
#include "querylab/planner/binder.h"
#include "querylab/sql/parser.h"

using namespace qltest;
using namespace querylab;
using querylab::api::Database;

namespace {

struct Planned {
    planner::LogicalPtr before;
    planner::LogicalPtr after;
    optimizer::Optimizer::RuleResult result;
};

/// Binds `sql` and applies exactly one rule (after `prior` rules).
Planned applyRule(Database& db, const std::string& sql, const std::string& rule, const std::vector<std::string>& prior = {}) {
    auto st = sql::parseStatement(sql);
    planner::IdGenerator ids;
    planner::Binder binder(db.catalogRef(), ids);
    auto plan = binder.bindSelect(std::get<sql::SelectStmt>(st.body));
    optimizer::CostModel cost;
    cost.indexColumns(*plan);
    optimizer::Optimizer opt(cost, ids);
    auto apply = [&](const std::string& name) {
        if (name == "ConstantFolding") return opt.constantFolding(plan);
        if (name == "FilterSimplification") return opt.filterSimplification(plan);
        if (name == "PredicatePushdown") return opt.predicatePushdown(plan);
        if (name == "JoinReordering") return opt.joinReordering(plan);
        return opt.projectionPushdown(plan);
    };
    for (const auto& p : prior) apply(p);
    Planned out;
    out.before = plan->clone();
    out.result = apply(rule);
    out.after = std::move(plan);
    return out;
}

const json& step(const json& result, const std::string& rule) {
    for (const auto& s : result["optimizer"]) {
        if (s["rule"] == rule) return s;
    }
    static json none;
    FAIL("rule not in trace: " << rule);
    return none;
}

}  // namespace

TEST_CASE("constant folding evaluates constant sub-expressions safely") {
    auto db = Database::inMemory();
    usersFixture(*db);
    auto p = applyRule(*db, "SELECT name FROM users WHERE age > 20 + 5 * 1", "ConstantFolding");
    CHECK(p.result.applied);
    CHECK(p.after->toTreeString().find("users.age > 25") != std::string::npos);
    // Division by zero must not be folded (it would turn a never-evaluated
    // expression into a planning error).
    auto q = applyRule(*db, "SELECT name FROM users WHERE age > 100 AND 1 / 0 = 1", "ConstantFolding");
    CHECK(q.after->toTreeString().find("1 / 0") != std::string::npos);
    CHECK(rows(*db, "SELECT name FROM users WHERE age > 20 + 5 * 1 ORDER BY name") == Rows{{"Karan"}, {"Meera"}, {"Priya"}});
}

TEST_CASE("filter simplification preserves three-valued logic") {
    auto db = Database::inMemory();
    usersFixture(*db);
    auto p = applyRule(*db, "SELECT id FROM users WHERE TRUE AND age > 25", "FilterSimplification");
    CHECK(p.result.applied);
    CHECK(p.after->toTreeString().find("TRUE") == std::string::npos);
    auto q = applyRule(*db, "SELECT id FROM users WHERE age > 25 AND 1 = 2", "FilterSimplification", {"ConstantFolding"});
    CHECK(q.after->toTreeString().find("Empty") != std::string::npos);
    auto r = run(*db, "SELECT id FROM users WHERE age > 25 AND 1 = 2");
    CHECK(r["rows"].empty());
    CHECK(findOp(r["physicalPlan"], "Empty Result") != nullptr);
    CHECK(findOp(r["physicalPlan"], "Seq Scan") == nullptr);  // no data read at all
    // Global aggregate over an empty input still returns one row.
    CHECK(rows(*db, "SELECT COUNT(*) FROM users WHERE FALSE") == Rows{{"0"}});
    CHECK(rows(*db, "SELECT COUNT(*) FROM users WHERE NULL") == Rows{{"0"}});
    // NOT NOT x == x even when x is NULL.
    CHECK(rows(*db, "SELECT COUNT(*) FROM users WHERE NOT NOT (age > 25)") == Rows{{"3"}});
    CHECK(rows(*db, "SELECT COUNT(*) FROM users WHERE age > 25 OR TRUE") == Rows{{"5"}});
}

TEST_CASE("predicate pushdown moves filters into scans and join inputs") {
    auto db = Database::inMemory();
    usersFixture(*db);
    run(*db, "CREATE TABLE posts (id INTEGER, user_id INTEGER, title TEXT); INSERT INTO posts VALUES (10, 1, 'a'), (11, 2, 'b'), (12, 2, 'c')");

    auto p = applyRule(*db, "SELECT name FROM users WHERE age > 25", "PredicatePushdown");
    CHECK(p.before->toTreeString().find("Filter(users.age > 25)") != std::string::npos);
    CHECK(p.after->toTreeString().find("Filter") == std::string::npos);
    CHECK(p.after->toTreeString().find("Scan(users WHERE users.age > 25)") != std::string::npos);

    auto j = applyRule(*db, "SELECT u.name FROM users u JOIN posts p ON p.user_id = u.id WHERE u.age > 25 AND p.title = 'b'",
                       "PredicatePushdown");
    const std::string tree = j.after->toTreeString();
    CHECK(tree.find("Scan(users AS u WHERE u.age > 25)") != std::string::npos);
    CHECK(tree.find("Scan(posts AS p WHERE p.title = 'b')") != std::string::npos);
    CHECK(tree.find("InnerJoin(p.user_id = u.id)") != std::string::npos);

    // Cross join + WHERE equality becomes a join condition (enables hash join).
    auto c = run(*db, "SELECT COUNT(*) FROM users u, posts p WHERE u.id = p.user_id");
    CHECK(c["rows"][0][0] == 3);
    const json* join = findOp(c["optimizedPlan"], "Join");
    REQUIRE(join);
    CHECK((*join)["details"]["condition"] == "u.id = p.user_id");
    CHECK(findOp(c["optimizedPlan"], "Filter") == nullptr);

    // LEFT JOIN: a WHERE condition on the right side must stay above the join.
    auto l = applyRule(*db, "SELECT u.id FROM users u LEFT JOIN posts p ON p.user_id = u.id WHERE p.id IS NULL", "PredicatePushdown");
    const std::string lt = l.after->toTreeString();
    CHECK(lt.find("Filter(p.id IS NULL)") != std::string::npos);
    CHECK(lt.find("Scan(posts AS p)") != std::string::npos);
    CHECK(rows(*db, "SELECT u.id FROM users u LEFT JOIN posts p ON p.user_id = u.id WHERE p.id IS NULL ORDER BY u.id") ==
          Rows{{"3"}, {"4"}, {"5"}});
    // ...but its left-side conditions can be pushed into the left input.
    auto l2 = applyRule(*db, "SELECT u.id FROM users u LEFT JOIN posts p ON p.user_id = u.id WHERE u.age > 30", "PredicatePushdown");
    CHECK(l2.after->toTreeString().find("Scan(users AS u WHERE u.age > 30)") != std::string::npos);
}

TEST_CASE("projection pushdown reads only needed columns") {
    auto db = Database::inMemory();
    usersFixture(*db);
    auto p = applyRule(*db, "SELECT name FROM users WHERE age > 25", "ProjectionPushdown", {"PredicatePushdown"});
    CHECK(p.result.applied);
    REQUIRE(p.result.details.size() == 1);
    CHECK(p.result.details[0].find("reads 2 of 4 columns (name, age)") != std::string::npos);
    auto r = run(*db, "SELECT COUNT(*) FROM users");
    const json* scan = findOp(r["physicalPlan"], "Seq Scan");
    REQUIRE(scan);
    CHECK((*scan)["details"]["columnsRead"].empty());
    CHECK(r["rows"][0][0] == 5);
    // DISTINCT needs every projected column; results stay correct.
    CHECK(rows(*db, "SELECT DISTINCT city FROM users WHERE city IS NOT NULL ORDER BY city") == Rows{{"Bengaluru"}, {"Delhi"}, {"Pune"}});
}

TEST_CASE("join reordering starts from the smallest input and preserves results") {
    auto db = Database::inMemory();
    run(*db, "CREATE TABLE big (id INTEGER, k INTEGER); CREATE TABLE mid (id INTEGER, k INTEGER); CREATE TABLE tiny (id INTEGER, k INTEGER);");
    std::string ins = "INSERT INTO big VALUES ";
    for (int i = 0; i < 2000; ++i) ins += (i ? "," : "") + std::string("(") + std::to_string(i) + "," + std::to_string(i % 50) + ")";
    run(*db, ins);
    ins = "INSERT INTO mid VALUES ";
    for (int i = 0; i < 200; ++i) ins += (i ? "," : "") + std::string("(") + std::to_string(i) + "," + std::to_string(i % 50) + ")";
    run(*db, ins);
    run(*db, "INSERT INTO tiny VALUES (1, 1), (2, 2), (3, 3)");
    const std::string q = "SELECT COUNT(*) FROM big b JOIN mid m ON m.k = b.k JOIN tiny t ON t.id = m.id";
    auto p = applyRule(*db, q, "JoinReordering", {"PredicatePushdown"});
    CHECK(p.result.applied);
    // The first (deepest-left) input is now the tiny table.
    const planner::LogicalNode* n = p.after.get();
    while (!n->children.empty()) n = n->children[0].get();
    CHECK(n->alias == "t");
    // Same answer as with the written order: tiny ids 1..3 match mid ids 1..3
    // (k = 1..3), each matching 40 big rows.
    CHECK(rows(*db, q) == Rows{{"120"}});
    const json full = run(*db, q);
    const auto& s = step(full, "JoinReordering");
    CHECK(s["applied"] == true);
    CHECK(s["costAfter"].get<double>() < s["costBefore"].get<double>());
}

TEST_CASE("index selection: equality, range, composite prefix, and refusing unselective scans") {
    auto db = Database::inMemory();
    db->loadDataset("ecommerce", 5000);
    const std::string q = "SELECT * FROM orders WHERE customer_id = 42";
    auto before = run(*db, q);
    CHECK(findOp(before["physicalPlan"], "Seq Scan") != nullptr);
    run(*db, "CREATE INDEX idx_orders_customer ON orders(customer_id)");
    auto after = run(*db, q);
    const json* idx = findOp(after["physicalPlan"], "Index Scan");
    REQUIRE(idx);
    CHECK((*idx)["details"]["index"] == "idx_orders_customer");
    CHECK(after["rows"] == before["rows"]);  // same rows (same physical order is not guaranteed in general, but holds here)
    CHECK((*idx)["actual"]["rowsScanned"].get<int>() == static_cast<int>(after["rowCount"].get<int>()));
    CHECK(after["metrics"]["pagesRead"].get<int>() < before["metrics"]["pagesRead"].get<int>());

    // An unselective range keeps the sequential scan; trace explains why.
    auto wide = run(*db, "SELECT COUNT(*) FROM orders WHERE customer_id > 5");
    CHECK(findOp(wide["physicalPlan"], "Seq Scan") != nullptr);
    CHECK(step(wide, "AccessPathSelection")["reason"].get<std::string>().find("not selective") != std::string::npos);
    // A narrow range uses the index, with identical results to a forced seq scan.
    querylab::api::QueryOptions noIdx;
    noIdx.disableIndexScans = true;
    const std::string rq = "SELECT id FROM orders WHERE customer_id BETWEEN 10 AND 12 AND status = 'delivered' ORDER BY id";
    auto viaIdx = run(*db, rq);
    CHECK(findOp(viaIdx["physicalPlan"], "Index Scan") != nullptr);
    CHECK(viaIdx["rows"] == run(*db, rq, noIdx)["rows"]);
    for (const char* op : {"<", "<=", ">", ">="}) {
        const std::string qq = std::string("SELECT id FROM orders WHERE customer_id ") + op + " 3 AND customer_id <> 1 ORDER BY id";
        CHECK(run(*db, qq)["rows"] == run(*db, qq, noIdx)["rows"]);
    }
    // Composite index: equality prefix + range on the second column.
    run(*db, "CREATE INDEX idx_cust_total ON orders(customer_id, total)");
    const std::string cq = "SELECT id FROM orders WHERE customer_id = 7 AND total > 100 ORDER BY id";
    auto comp = run(*db, cq);
    const json* ci = findOp(comp["physicalPlan"], "Index Scan");
    REQUIRE(ci);
    CHECK((*ci)["details"]["index"] == "idx_cust_total");
    CHECK(comp["rows"] == run(*db, cq, noIdx)["rows"]);
    // A predicate only on the second column cannot use the composite index.
    auto second = run(*db, "SELECT COUNT(*) FROM orders WHERE total > 999999");
    CHECK(findOp(second["physicalPlan"], "Index Scan") == nullptr);
}

TEST_CASE("estimates are reported separately from actuals; mis-estimates are flagged") {
    auto db = Database::inMemory();
    db->loadDataset("ecommerce", 5000);
    auto r = run(*db, "SELECT COUNT(*) FROM orders WHERE status = 'delivered'");
    const json* scan = findOp(r["physicalPlan"], "Seq Scan");
    REQUIRE(scan);
    const double est = (*scan)["estimates"]["rows"];
    const double act = (*scan)["actual"]["rowsOut"];
    // Uniformity assumption: 1/6 of rows estimated, ~3/7 actually delivered.
    CHECK(est < act);
    CHECK((*scan)["details"]["selectivityEstimate"].get<std::string>().find("uniformity") != std::string::npos);
    bool flagged = false;
    for (const auto& w : r["explanation"]["estimateWarnings"]) flagged = flagged || w["nodeId"] == (*scan)["id"];
    CHECK(flagged);
}

TEST_CASE("hash join vs nested loop choice and build side") {
    auto db = Database::inMemory();
    db->loadDataset("ecommerce", 2000);
    auto r = run(*db, "SELECT COUNT(*) FROM orders o JOIN customers c ON o.customer_id = c.id");
    const json* hj = findOp(r["physicalPlan"], "Hash Join");
    REQUIRE(hj);
    // Build side (right child) should be the smaller customers table.
    CHECK((*hj)["children"][1]["details"]["table"] == "customers");
    querylab::api::QueryOptions nl;
    nl.disableHashJoin = true;
    auto r2 = run(*db, "SELECT COUNT(*) FROM orders o JOIN customers c ON o.customer_id = c.id", nl);
    CHECK(findOp(r2["physicalPlan"], "Nested Loop Join") != nullptr);
    CHECK(r2["rows"] == r["rows"]);
}
