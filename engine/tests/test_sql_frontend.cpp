// Lexer, parser and AST serialization tests.
#include <doctest.h>

#include "querylab/explain/ast_json.h"
#include "querylab/sql/lexer.h"
#include "querylab/sql/parser.h"

using namespace querylab;
using namespace querylab::sql;

static QueryError captureError(auto&& fn) {
    try {
        fn();
    } catch (const QueryError& e) {
        return e;
    }
    FAIL("expected QueryError");
    return QueryError(ErrorStage::Api, "unreachable");
}

TEST_CASE("lexer: keywords, identifiers, literals, operators") {
    auto toks = tokenize("SELECT Name, age FROM Users WHERE age >= 25.5 AND city <> 'Delhi';");
    REQUIRE(toks.size() == 16);
    CHECK(toks[0].type == TokenType::Keyword);
    CHECK(toks[0].value == "SELECT");
    CHECK(toks[1].type == TokenType::Identifier);
    CHECK(toks[1].value == "name");  // unquoted identifiers are case-insensitive
    CHECK(toks[1].lexeme == "Name");
    CHECK(toks[8].type == TokenType::Operator);
    CHECK(toks[8].value == ">=");
    CHECK(toks[9].type == TokenType::Float);
    CHECK(toks[12].value == "<>");
    CHECK(toks[13].type == TokenType::String);
    CHECK(toks[13].value == "Delhi");
    CHECK(toks.back().type == TokenType::End);
}

TEST_CASE("lexer: source locations across lines") {
    auto toks = tokenize("SELECT a\n  FROM t");
    CHECK(toks[2].value == "FROM");
    CHECK(toks[2].span.line == 2);
    CHECK(toks[2].span.column == 3);
    CHECK(toks[2].span.begin == 11);
    CHECK(toks[2].span.end == 15);
}

TEST_CASE("lexer: escaped quotes, quoted identifiers, != normalization") {
    auto toks = tokenize("'it''s' \"Mixed Case\" a != b");
    CHECK(toks[0].value == "it's");
    CHECK(toks[1].type == TokenType::Identifier);
    CHECK(toks[1].value == "Mixed Case");
    CHECK(toks[3].value == "<>");
}

TEST_CASE("lexer: comments are skipped unless requested") {
    const char* sql = "SELECT 1 -- trailing\n/* block\ncomment */ + 2";
    auto toks = tokenize(sql);
    CHECK(toks.size() == 5);
    auto withComments = tokenize(sql, true);
    CHECK(withComments.size() == 7);
    CHECK(withComments[2].type == TokenType::Comment);
}

TEST_CASE("lexer: numeric forms") {
    auto toks = tokenize("1 1.5 .5 2e3 7E-2 3.");
    CHECK(toks[0].type == TokenType::Integer);
    for (int i = 1; i <= 5; ++i) CHECK(toks[i].type == TokenType::Float);
}

TEST_CASE("lexer: errors carry locations") {
    auto e = captureError([] { tokenize("SELECT 'abc"); });
    CHECK(e.stage() == ErrorStage::Lexer);
    CHECK(std::string(e.what()).find("unterminated string") != std::string::npos);
    CHECK(e.span()->column == 8);

    auto e2 = captureError([] { tokenize("SELECT a\nFROM t WHERE x # 1"); });
    CHECK(e2.span()->line == 2);
    CHECK(e2.span()->column == 16);
    CHECK_THROWS_AS(tokenize("/* never closed"), QueryError);
    CHECK_THROWS_AS(tokenize("SELECT 12abc"), QueryError);
    CHECK_THROWS_AS(tokenize("SELECT 1e"), QueryError);
    CHECK_THROWS_AS(tokenize("SELECT \"\""), QueryError);
}

TEST_CASE("parser: operator precedence") {
    CHECK(parseExpression("1 + 2 * 3")->toString() == "1 + (2 * 3)");
    CHECK(parseExpression("(1 + 2) * 3")->toString() == "(1 + 2) * 3");
    CHECK(parseExpression("a OR b AND c")->toString() == "a OR (b AND c)");
    CHECK(parseExpression("NOT a = 1 AND b")->toString() == "(NOT a = 1) AND b");
    CHECK(parseExpression("a - b - c")->toString() == "(a - b) - c");
    CHECK(parseExpression("-a * b")->toString() == "-a * b");
    CHECK(parseExpression("x > 1 + 2")->toString() == "x > (1 + 2)");
    CHECK(parseExpression("a IS NOT NULL OR b IS NULL")->toString() == "(a IS NOT NULL) OR (b IS NULL)");
    CHECK(parseExpression("a NOT BETWEEN 1 AND 5 AND b")->toString() == "(a NOT BETWEEN 1 AND 5) AND b");
    CHECK(parseExpression("a IN (1, 2, 3)")->toString() == "a IN (1, 2, 3)");
    CHECK(parseExpression("name NOT LIKE 'A%'")->toString() == "name NOT LIKE 'A%'");
    CHECK(parseExpression("count(DISTINCT x)")->toString() == "COUNT(DISTINCT x)");
    CHECK(parseExpression("COUNT(*)")->toString() == "COUNT(*)");
    CHECK(parseExpression("u.name || 'x'")->toString() == "u.name || 'x'");
}

TEST_CASE("parser: full SELECT") {
    auto st = parseStatement(
        "SELECT DISTINCT u.name AS n, COUNT(*) total FROM users u "
        "LEFT JOIN posts AS p ON p.user_id = u.id "
        "WHERE u.age > 25 GROUP BY u.name HAVING COUNT(*) > 1 "
        "ORDER BY total DESC, n LIMIT 10 OFFSET 5;");
    REQUIRE(st.isSelect());
    const auto& s = std::get<SelectStmt>(st.body);
    CHECK(s.distinct);
    REQUIRE(s.items.size() == 2);
    CHECK(s.items[0].alias == "n");
    CHECK(s.items[1].alias == "total");
    CHECK(s.from->name == "users");
    CHECK(s.from->alias == "u");
    REQUIRE(s.joins.size() == 1);
    CHECK(s.joins[0].type == JoinType::Left);
    CHECK(s.joins[0].table.alias == "p");
    CHECK(s.where->toString() == "u.age > 25");
    CHECK(s.groupBy.size() == 1);
    CHECK(s.having);
    REQUIRE(s.orderBy.size() == 2);
    CHECK(s.orderBy[0].descending);
    CHECK_FALSE(s.orderBy[1].descending);
    CHECK(*s.limit == 10);
    CHECK(*s.offset == 5);
}

TEST_CASE("parser: DDL and DML") {
    auto stmts = parseScript(
        "CREATE TABLE users (id INTEGER, name TEXT, score FLOAT, active BOOLEAN, note VARCHAR(20));"
        "CREATE UNIQUE INDEX idx_u ON users (id, name);"
        "INSERT INTO users (id, name) VALUES (1, 'Aman'), (2, 'Priya');"
        "UPDATE users SET score = score + 1, active = TRUE WHERE id = 1;"
        "DELETE FROM users WHERE id = 2;"
        "DROP INDEX IF EXISTS idx_u; DROP TABLE users; EXPLAIN SELECT 1");
    REQUIRE(stmts.size() == 8);
    const auto& ct = std::get<CreateTableStmt>(stmts[0].body);
    REQUIRE(ct.columns.size() == 5);
    CHECK(ct.columns[2].type == DataType::Float);
    CHECK(ct.columns[3].type == DataType::Boolean);
    CHECK(ct.columns[4].type == DataType::Text);
    const auto& ci = std::get<CreateIndexStmt>(stmts[1].body);
    CHECK(ci.unique);
    CHECK(ci.columns == std::vector<std::string>{"id", "name"});
    const auto& ins = std::get<InsertStmt>(stmts[2].body);
    CHECK(ins.rows.size() == 2);
    CHECK(ins.columns.size() == 2);
    CHECK(std::get<UpdateStmt>(stmts[3].body).assignments.size() == 2);
    CHECK(std::get<DropIndexStmt>(stmts[5].body).ifExists);
    CHECK(stmts[7].explain);
    CHECK(stmts[2].text.rfind("INSERT", 0) == 0);
}

TEST_CASE("parser: syntax errors with line and column") {
    auto e = captureError([] { parseStatement("SELECT name\nFROM users\nWHERE age >"); });
    CHECK(e.stage() == ErrorStage::Parser);
    CHECK(e.span()->line == 3);
    CHECK(std::string(e.what()).find("expected an expression") != std::string::npos);

    CHECK_THROWS_AS(parseStatement("SELECT FROM users"), QueryError);
    CHECK_THROWS_AS(parseStatement("SELECT a FROM"), QueryError);
    CHECK_THROWS_AS(parseStatement("SELECT a FROM t WHERE"), QueryError);
    CHECK_THROWS_AS(parseStatement("SELECT a FROM t LIMIT -1"), QueryError);
    CHECK_THROWS_AS(parseStatement("SELECT a FROM t WHERE a = (SELECT 1)"), QueryError);
    CHECK_THROWS_AS(parseStatement("SELECT a FROM t WHERE 1 < 2 < 3"), QueryError);
    CHECK_THROWS_AS(parseStatement("CREATE TABLE t (id INTEGER PRIMARY)"), QueryError);
    CHECK_THROWS_AS(parseStatement("SELECT 1; SELECT 2"), QueryError);
    CHECK_THROWS_AS(parseStatement("SELECT 99999999999999999999"), QueryError);
    CHECK_THROWS_AS(parseStatement("   "), QueryError);
    std::string deep(500, '(');
    CHECK_THROWS_AS(parseStatement("SELECT " + deep), QueryError);
}

TEST_CASE("AST JSON mirrors the SQL structure") {
    auto st = parseStatement("SELECT name FROM users WHERE age > 25");
    auto j = explain::statementToJson(st);
    CHECK(j["type"] == "SelectStatement");
    REQUIRE(j["children"].size() == 3);
    CHECK(j["children"][0]["type"] == "SelectList");
    CHECK(j["children"][1]["type"] == "From");
    CHECK(j["children"][2]["type"] == "Where");
    const auto& cmp = j["children"][2]["children"][0];
    CHECK(cmp["type"] == "BinaryExpression");
    CHECK(cmp["details"]["operator"] == ">");
    CHECK(cmp["span"]["begin"] == 29);
    CHECK(cmp["span"]["end"] == 37);
    CHECK(cmp["children"][1]["details"]["value"] == 25);
    auto text = explain::astToText(j);
    CHECK(text.find("└── WHERE") != std::string::npos);
}

TEST_CASE("Value total order and hashing") {
    CHECK(Value::compareTotal(Value::null(), Value::integer(0)) < 0);
    CHECK(Value::compareTotal(Value::integer(1), Value::floating(1.0)) == 0);
    CHECK(Value::integer(1).hash() == Value::floating(1.0).hash());
    CHECK(Value::compareTotal(Value::integer(2), Value::floating(1.5)) > 0);
    CHECK(Value::compareTotal(Value::text("a"), Value::integer(100)) > 0);
    CHECK(Value::text("it's").toSqlLiteral() == "'it''s'");
    CHECK(Value::floating(2.0).toString() == "2.0");
    CHECK_THROWS_AS(coerceForStorage(Value::text("x"), DataType::Integer, "c"), QueryError);
    CHECK(coerceForStorage(Value::integer(3), DataType::Float, "c").type() == DataType::Float);
}
