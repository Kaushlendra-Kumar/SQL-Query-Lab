// End-to-end SQL execution: results checked against hand-computed answers.
#include "test_helpers.h"

using namespace qltest;
using querylab::api::Database;

TEST_CASE("basic SELECT, WHERE, ORDER BY, LIMIT/OFFSET") {
    auto db = Database::inMemory();
    usersFixture(*db);
    CHECK(rows(*db, "SELECT name, age FROM users WHERE age > 25 ORDER BY age DESC, name LIMIT 10") ==
          Rows{{"Meera", "35"}, {"Karan", "29"}, {"Priya", "29"}});
    CHECK(rows(*db, "SELECT id FROM users ORDER BY id LIMIT 2 OFFSET 1") == Rows{{"2"}, {"3"}});
    CHECK(rows(*db, "SELECT id FROM users ORDER BY id OFFSET 4") == Rows{{"5"}});
    CHECK(rows(*db, "SELECT id FROM users LIMIT 0").empty());
    CHECK(rows(*db, "SELECT * FROM users WHERE id = 1") == Rows{{"1", "Aman", "24", "Bengaluru"}});
    CHECK(rows(*db, "SELECT u.* FROM users u WHERE u.id = 2") == Rows{{"2", "Priya", "29", "Delhi"}});
    CHECK(rows(*db, "SELECT 1 + 2 AS three, 'a' || 'b', 7 / 2, 7.0 / 2, 7 % 3") == Rows{{"3", "ab", "3", "3.5", "1"}});
    CHECK(rows(*db, "SELECT id FROM users ORDER BY 1 DESC LIMIT 1") == Rows{{"5"}});
    // ORDER BY an expression that is not selected (hidden sort column).
    CHECK(rows(*db, "SELECT name FROM users WHERE age IS NOT NULL ORDER BY age * -1, id") ==
          Rows{{"Meera"}, {"Priya"}, {"Karan"}, {"Aman"}});
    // NULL sorts first ascending, last descending.
    CHECK(rows(*db, "SELECT id FROM users ORDER BY age, id")[0] == std::vector<std::string>{"3"});
    CHECK(rows(*db, "SELECT id FROM users ORDER BY age DESC, id").back() == std::vector<std::string>{"3"});
}

TEST_CASE("three-valued logic and NULL semantics") {
    auto db = Database::inMemory();
    usersFixture(*db);
    // WHERE keeps only TRUE: NULL age is neither > 25 nor <= 25.
    CHECK(rows(*db, "SELECT COUNT(*) FROM users WHERE age > 25") == Rows{{"3"}});
    CHECK(rows(*db, "SELECT COUNT(*) FROM users WHERE age <= 25") == Rows{{"1"}});
    CHECK(rows(*db, "SELECT COUNT(*) FROM users WHERE NOT (age > 25)") == Rows{{"1"}});
    CHECK(rows(*db, "SELECT COUNT(*) FROM users WHERE age = NULL") == Rows{{"0"}});
    CHECK(rows(*db, "SELECT id FROM users WHERE age IS NULL") == Rows{{"3"}});
    CHECK(rows(*db, "SELECT id FROM users WHERE city IS NULL OR age IS NULL ORDER BY id") == Rows{{"3"}, {"5"}});
    CHECK(rows(*db, "SELECT NULL AND FALSE, NULL AND TRUE, NULL OR TRUE, NULL OR FALSE, NOT NULL") ==
          Rows{{"FALSE", "NULL", "TRUE", "NULL", "NULL"}});
    CHECK(rows(*db, "SELECT NULL + 1, NULL = NULL, NULL IS NULL, 1 IN (2, NULL), 1 IN (1, NULL), 3 NOT IN (1, NULL)") ==
          Rows{{"NULL", "NULL", "TRUE", "NULL", "TRUE", "NULL"}});
    CHECK(rows(*db, "SELECT 5 BETWEEN 1 AND NULL, 5 BETWEEN 6 AND NULL") == Rows{{"NULL", "FALSE"}});
    CHECK(rows(*db, "SELECT COALESCE(NULL, NULL, 3), COALESCE(age, -1) FROM users WHERE id = 3") == Rows{{"3", "-1"}});
}

TEST_CASE("operators and scalar functions") {
    auto db = Database::inMemory();
    usersFixture(*db);
    CHECK(rows(*db, "SELECT name FROM users WHERE name LIKE 'P%' OR name LIKE '_a%' ORDER BY name") == Rows{{"Karan"}, {"Priya"}, {"Ravi"}});
    CHECK(rows(*db, "SELECT name FROM users WHERE name NOT LIKE '%a%' ORDER BY name").empty());
    CHECK(rows(*db, "SELECT id FROM users WHERE age BETWEEN 25 AND 30 ORDER BY id") == Rows{{"2"}, {"5"}});
    CHECK(rows(*db, "SELECT id FROM users WHERE id IN (1, 3, 99) ORDER BY id") == Rows{{"1"}, {"3"}});
    CHECK(rows(*db, "SELECT UPPER(name), LOWER(city), LENGTH(name), ABS(-3), ROUND(2.567, 2) FROM users WHERE id = 1") ==
          Rows{{"AMAN", "bengaluru", "4", "3", "2.57"}});
    CHECK(rows(*db, "SELECT -age, +age FROM users WHERE id = 1") == Rows{{"-24", "24"}});
    CHECK(rows(*db, "SELECT 1 < 2, 2.5 >= 2, 'b' > 'a', TRUE = FALSE, 3 <> 3") == Rows{{"TRUE", "TRUE", "TRUE", "FALSE", "FALSE"}});
}

TEST_CASE("aggregates, GROUP BY, HAVING, DISTINCT") {
    auto db = Database::inMemory();
    usersFixture(*db);
    CHECK(rows(*db, "SELECT COUNT(*), COUNT(age), SUM(age), MIN(age), MAX(age), AVG(age) FROM users") ==
          Rows{{"5", "4", "117", "24", "35", "29.25"}});
    CHECK(rows(*db, "SELECT city, COUNT(*) AS n FROM users GROUP BY city ORDER BY n DESC, city") ==
          Rows{{"Delhi", "2"}, {"NULL", "1"}, {"Bengaluru", "1"}, {"Pune", "1"}});
    CHECK(rows(*db, "SELECT city FROM users GROUP BY city HAVING COUNT(*) > 1") == Rows{{"Delhi"}});
    CHECK(rows(*db, "SELECT age, COUNT(*) FROM users GROUP BY age HAVING age > 25 ORDER BY age") == Rows{{"29", "2"}, {"35", "1"}});
    CHECK(rows(*db, "SELECT COUNT(DISTINCT age), SUM(DISTINCT age) FROM users") == Rows{{"3", "88"}});
    CHECK(rows(*db, "SELECT DISTINCT age FROM users WHERE age IS NOT NULL ORDER BY age") == Rows{{"24"}, {"29"}, {"35"}});
    CHECK(rows(*db, "SELECT DISTINCT city FROM users ORDER BY city").size() == 4);
    // Aggregates over no rows: COUNT gives 0, the others NULL; grouped -> no rows.
    CHECK(rows(*db, "SELECT COUNT(*), SUM(age), MAX(name) FROM users WHERE id > 100") == Rows{{"0", "NULL", "NULL"}});
    CHECK(rows(*db, "SELECT city, COUNT(*) FROM users WHERE id > 100 GROUP BY city").empty());
    // Expressions over aggregates and group keys, alias and ordinal references.
    CHECK(rows(*db, "SELECT city AS c, MAX(age) - MIN(age) AS spread FROM users WHERE city = 'Delhi' GROUP BY c") == Rows{{"Delhi", "6"}});
    CHECK(rows(*db, "SELECT city, COUNT(*) FROM users GROUP BY 1 ORDER BY 2 DESC, 1 LIMIT 1") == Rows{{"Delhi", "2"}});
    CHECK(rows(*db, "SELECT age + 1 AS a1, COUNT(*) FROM users GROUP BY age + 1 ORDER BY a1 LIMIT 1") == Rows{{"NULL", "1"}});
    CHECK(rows(*db, "SELECT SUM(age) / COUNT(age) FROM users") == Rows{{"29"}});
}

TEST_CASE("INNER JOIN, LEFT JOIN, cross join and self join") {
    auto db = Database::inMemory();
    usersFixture(*db);
    run(*db,
        "CREATE TABLE posts (id INTEGER, user_id INTEGER, title TEXT);"
        "INSERT INTO posts VALUES (10, 1, 'hello'), (11, 1, 'again'), (12, 2, 'hi'), (13, 99, 'orphan'), (14, NULL, 'anon');");
    CHECK(rows(*db, "SELECT u.name, p.title FROM users u JOIN posts p ON p.user_id = u.id ORDER BY p.id") ==
          Rows{{"Aman", "hello"}, {"Aman", "again"}, {"Priya", "hi"}});
    CHECK(rows(*db, "SELECT u.id, p.id FROM users u LEFT JOIN posts p ON p.user_id = u.id ORDER BY u.id, p.id") ==
          Rows{{"1", "10"}, {"1", "11"}, {"2", "12"}, {"3", "NULL"}, {"4", "NULL"}, {"5", "NULL"}});
    // Users without posts.
    CHECK(rows(*db, "SELECT u.id FROM users u LEFT JOIN posts p ON p.user_id = u.id WHERE p.id IS NULL ORDER BY u.id") ==
          Rows{{"3"}, {"4"}, {"5"}});
    // ON-clause condition on the right side of a LEFT JOIN filters only matches.
    CHECK(rows(*db, "SELECT u.id, p.title FROM users u LEFT JOIN posts p ON p.user_id = u.id AND p.title = 'again' "
                    "WHERE u.id <= 2 ORDER BY u.id") == Rows{{"1", "again"}, {"2", "NULL"}});
    // Non-equi join -> nested loop.
    auto r = run(*db, "SELECT COUNT(*) FROM users a JOIN users b ON a.age < b.age");
    CHECK(r["rows"][0][0] == 5);
    CHECK(findOp(r["physicalPlan"], "Nested Loop Join") != nullptr);
    CHECK(rows(*db, "SELECT COUNT(*) FROM users, posts") == Rows{{"25"}});
    CHECK(rows(*db, "SELECT COUNT(*) FROM users u, posts p WHERE u.id = p.user_id") == Rows{{"3"}});
    CHECK(rows(*db, "SELECT u.name, COUNT(p.id) AS posts FROM users u LEFT JOIN posts p ON p.user_id = u.id "
                    "GROUP BY u.name ORDER BY posts DESC, u.name LIMIT 2") == Rows{{"Aman", "2"}, {"Priya", "1"}});
    // Three-way join.
    run(*db, "CREATE TABLE likes (post_id INTEGER, user_id INTEGER); INSERT INTO likes VALUES (10, 2), (10, 4), (12, 1);");
    CHECK(rows(*db, "SELECT a.name, p.title, f.name FROM users a JOIN posts p ON p.user_id = a.id "
                    "JOIN likes l ON l.post_id = p.id JOIN users f ON f.id = l.user_id ORDER BY p.id, f.name") ==
          Rows{{"Aman", "hello", "Meera"}, {"Aman", "hello", "Priya"}, {"Priya", "hi", "Aman"}});
}

TEST_CASE("UPDATE and DELETE maintain rows, indexes and statistics") {
    auto db = Database::inMemory();
    usersFixture(*db);
    run(*db, "CREATE INDEX idx_age ON users(age);");
    auto u = run(*db, "UPDATE users SET age = age + 1, city = 'Mumbai' WHERE city = 'Delhi'");
    CHECK(u["affectedRows"] == 2);
    CHECK(rows(*db, "SELECT id, age, city FROM users WHERE city = 'Mumbai' ORDER BY id") == Rows{{"2", "30", "Mumbai"}, {"4", "36", "Mumbai"}});
    CHECK(rows(*db, "SELECT id FROM users WHERE age = 30") == Rows{{"2"}});  // via the maintained index
    CHECK(rows(*db, "SELECT COUNT(*) FROM users WHERE age = 29") == Rows{{"1"}});
    auto d = run(*db, "DELETE FROM users WHERE age < 30");
    CHECK(d["affectedRows"] == 2);
    CHECK(rows(*db, "SELECT id FROM users ORDER BY id") == Rows{{"2"}, {"3"}, {"4"}});
    CHECK(run(*db, "DELETE FROM users")["affectedRows"] == 3);
    CHECK(rows(*db, "SELECT COUNT(*) FROM users") == Rows{{"0"}});
    // Large values move rows to new pages; RIDs in the index must follow.
    run(*db, "INSERT INTO users VALUES (9, 'x', 1, 'c')");
    run(*db, "UPDATE users SET name = 'a much longer name than before that cannot fit in place' WHERE id = 9");
    CHECK(rows(*db, "SELECT name FROM users WHERE age = 1")[0][0].size() > 40);
}

TEST_CASE("statement-level atomicity and constraints") {
    auto db = Database::inMemory();
    run(*db, "CREATE TABLE t (id INTEGER, v TEXT); CREATE UNIQUE INDEX t_id ON t(id); INSERT INTO t VALUES (1, 'a'), (2, 'b')");
    auto e = fails(*db, "INSERT INTO t VALUES (3, 'c'), (1, 'dup')");
    CHECK(e["message"].get<std::string>().find("unique") != std::string::npos);
    CHECK(rows(*db, "SELECT COUNT(*) FROM t") == Rows{{"2"}});  // row 3 was rolled back
    fails(*db, "UPDATE t SET id = 2 WHERE id = 1");
    CHECK(rows(*db, "SELECT id FROM t ORDER BY id") == Rows{{"1"}, {"2"}});
    run(*db, "INSERT INTO t VALUES (NULL, 'x'), (NULL, 'y')");  // NULLs never conflict
    fails(*db, "CREATE UNIQUE INDEX t_v ON t(v); INSERT INTO t VALUES (5, 'a')");
    run(*db, "INSERT INTO t (v) VALUES ('only v')");
    CHECK(rows(*db, "SELECT id FROM t WHERE v = 'only v'") == Rows{{"NULL"}});
    run(*db, "CREATE TABLE f (x FLOAT, i INTEGER); INSERT INTO f VALUES (1, 2.0)");
    CHECK(rows(*db, "SELECT x, i FROM f") == Rows{{"1", "2"}});
    fails(*db, "INSERT INTO f VALUES (1, 2.5)");
    fails(*db, "INSERT INTO f VALUES ('a', 1)");
    fails(*db, "SELECT 9223372036854775807 + 1");
    fails(*db, "SELECT 1 / 0");
}

TEST_CASE("binder errors are specific and located") {
    auto db = Database::inMemory();
    usersFixture(*db);
    auto e = fails(*db, "SELECT nmae FROM users");
    CHECK(e["stage"] == "binder");
    CHECK(e["message"].get<std::string>().find("column 'nmae' does not exist") != std::string::npos);
    CHECK(e["span"]["column"] == 8);
    CHECK(fails(*db, "SELECT * FROM nope")["message"].get<std::string>().find("does not exist") != std::string::npos);
    run(*db, "CREATE TABLE posts (id INTEGER, user_id INTEGER)");
    CHECK(fails(*db, "SELECT id FROM users u JOIN posts p ON p.user_id = u.id")["message"].get<std::string>().find("ambiguous") !=
          std::string::npos);
    CHECK(fails(*db, "SELECT users.id FROM users u")["message"].get<std::string>().find("aliased") != std::string::npos);
    CHECK(fails(*db, "SELECT name, COUNT(*) FROM users")["message"].get<std::string>().find("GROUP BY") != std::string::npos);
    CHECK(fails(*db, "SELECT id FROM users WHERE COUNT(*) > 1")["message"].get<std::string>().find("not allowed") != std::string::npos);
    CHECK(fails(*db, "SELECT name + 1 FROM users")["message"].get<std::string>().find("numeric") != std::string::npos);
    CHECK(fails(*db, "SELECT id FROM users WHERE name = 5")["message"].get<std::string>().find("cannot compare") != std::string::npos);
    CHECK(fails(*db, "SELECT id FROM users WHERE age")["message"].get<std::string>().find("BOOLEAN") != std::string::npos);
    CHECK(fails(*db, "SELECT foo(id) FROM users")["message"].get<std::string>().find("does not exist") != std::string::npos);
    CHECK(fails(*db, "SELECT SUM(name) FROM users")["message"].get<std::string>().find("numeric") != std::string::npos);
    CHECK(fails(*db, "SELECT DISTINCT name FROM users ORDER BY age")["message"].get<std::string>().find("DISTINCT") != std::string::npos);
    CHECK(fails(*db, "INSERT INTO users VALUES (1, 2)")["message"].get<std::string>().find("values") != std::string::npos);
    CHECK(fails(*db, "INSERT INTO users (id, name) VALUES (1, Aman)")["message"].get<std::string>().find("did you mean") != std::string::npos);
    CHECK(fails(*db, "UPDATE users SET name = 3")["message"].get<std::string>().find("cannot assign") != std::string::npos);
    CHECK(fails(*db, "SELECT id FROM users ORDER BY 9")["message"].get<std::string>().find("not in the select list") != std::string::npos);
    CHECK(fails(*db, "CREATE TABLE users (x INTEGER)")["stage"] == "catalog");
    run(*db, "CREATE TABLE IF NOT EXISTS users (x INTEGER); DROP TABLE IF EXISTS nothing; DROP INDEX IF EXISTS nothing");
}

TEST_CASE("multi-statement scripts stop at the first error") {
    auto db = Database::inMemory();
    auto out = db->execute("CREATE TABLE a (x INTEGER); INSERT INTO a VALUES (1); SELECT y FROM a; INSERT INTO a VALUES (2)");
    CHECK_FALSE(out["success"].get<bool>());
    CHECK(out["results"].size() == 3);
    CHECK(rows(*db, "SELECT COUNT(*) FROM a") == Rows{{"1"}});
    auto parse = db->execute("SELECT 1; SELEC 2");
    CHECK(parse["error"]["stage"] == "parser");
    CHECK(parse["results"].size() == 1);
}
