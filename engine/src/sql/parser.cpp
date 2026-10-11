#include "querylab/sql/parser.h"

#include <charconv>
#include <cstdlib>

namespace querylab::sql {

namespace {

class Parser {
public:
    Parser(std::string_view src, std::vector<Token> tokens) : src_(src), toks_(std::move(tokens)) {}

    std::vector<Statement> script() {
        std::vector<Statement> out;
        while (true) {
            while (acceptPunct(";")) {
            }
            if (atEnd()) break;
            out.push_back(statement());
            if (!atEnd() && !checkPunct(";")) {
                fail("expected ';' or end of input after statement, found " + describe(cur()));
            }
        }
        return out;
    }

    Statement single() {
        while (acceptPunct(";")) {
        }
        if (atEnd()) fail("empty statement");
        Statement s = statement();
        while (acceptPunct(";")) {
        }
        if (!atEnd()) fail("unexpected " + describe(cur()) + " after end of statement");
        return s;
    }

    ExprPtr standaloneExpr() {
        ExprPtr e = expr();
        if (!atEnd()) fail("unexpected " + describe(cur()) + " after expression");
        return e;
    }

private:
    // ---- token helpers --------------------------------------------------
    const Token& cur() const { return toks_[pos_]; }
    const Token& peekTok(std::size_t k = 1) const { return toks_[std::min(pos_ + k, toks_.size() - 1)]; }
    bool atEnd() const { return cur().type == TokenType::End; }
    const Token& advance() {
        const Token& t = toks_[pos_];
        if (t.type != TokenType::End) ++pos_;
        lastEnd_ = t.span.end;
        return t;
    }

    bool checkKw(std::string_view kw) const { return cur().type == TokenType::Keyword && cur().value == kw; }
    bool checkKwAt(std::size_t k, std::string_view kw) const {
        const Token& t = peekTok(k);
        return t.type == TokenType::Keyword && t.value == kw;
    }
    bool acceptKw(std::string_view kw) {
        if (!checkKw(kw)) return false;
        advance();
        return true;
    }
    const Token& expectKw(std::string_view kw) {
        if (!checkKw(kw)) fail("expected " + std::string(kw) + ", found " + describe(cur()));
        return advance();
    }
    bool checkPunct(std::string_view p) const { return cur().type == TokenType::Punctuation && cur().value == p; }
    bool acceptPunct(std::string_view p) {
        if (!checkPunct(p)) return false;
        advance();
        return true;
    }
    const Token& expectPunct(std::string_view p) {
        if (!checkPunct(p)) fail("expected '" + std::string(p) + "', found " + describe(cur()));
        return advance();
    }
    bool checkOp(std::string_view op) const { return cur().type == TokenType::Operator && cur().value == op; }

    const Token& expectIdent(const char* what) {
        if (cur().type != TokenType::Identifier) {
            std::string msg = std::string("expected ") + what + ", found " + describe(cur());
            if (cur().type == TokenType::Keyword) msg += " (reserved words can be used as names if double-quoted)";
            fail(msg);
        }
        return advance();
    }

    static std::string describe(const Token& t) {
        switch (t.type) {
            case TokenType::End: return "end of input";
            case TokenType::Keyword: return "keyword " + t.value;
            case TokenType::Identifier: return "identifier '" + t.lexeme + "'";
            case TokenType::String: return "string " + t.lexeme;
            default: return "'" + t.lexeme + "'";
        }
    }

    [[noreturn]] void fail(const std::string& msg) const { throw QueryError(ErrorStage::Parser, msg, cur().span); }
    [[noreturn]] void failAt(const std::string& msg, const SourceSpan& s) const {
        throw QueryError(ErrorStage::Parser, msg, s);
    }

    SourceSpan spanFrom(const SourceSpan& start) const {
        SourceSpan s = start;
        s.end = std::max(lastEnd_, start.end);
        return s;
    }

    // ---- statements -----------------------------------------------------
    Statement statement() {
        const SourceSpan start = cur().span;
        Statement s;
        if (acceptKw("EXPLAIN")) s.explain = true;
        if (checkKw("SELECT")) {
            s.body = select();
        } else if (checkKw("CREATE")) {
            if (checkKwAt(1, "TABLE")) s.body = createTable();
            else if (checkKwAt(1, "INDEX") || (checkKwAt(1, "UNIQUE") && checkKwAt(2, "INDEX"))) s.body = createIndex();
            else { advance(); fail("expected TABLE or INDEX after CREATE, found " + describe(cur())); }
        } else if (checkKw("DROP")) {
            advance();
            if (acceptKw("TABLE")) {
                DropTableStmt d;
                if (acceptKw("IF")) { expectKw("EXISTS"); d.ifExists = true; }
                const Token& n = expectIdent("table name");
                d.name = n.value;
                d.nameSpan = n.span;
                s.body = std::move(d);
            } else if (acceptKw("INDEX")) {
                DropIndexStmt d;
                if (acceptKw("IF")) { expectKw("EXISTS"); d.ifExists = true; }
                const Token& n = expectIdent("index name");
                d.name = n.value;
                d.nameSpan = n.span;
                s.body = std::move(d);
            } else {
                fail("expected TABLE or INDEX after DROP, found " + describe(cur()));
            }
        } else if (checkKw("INSERT")) {
            s.body = insert();
        } else if (checkKw("UPDATE")) {
            s.body = update();
        } else if (checkKw("DELETE")) {
            s.body = del();
        } else {
            fail("expected a statement (SELECT, INSERT, CREATE, DROP, UPDATE, DELETE, EXPLAIN), found " + describe(cur()));
        }
        s.span = spanFrom(start);
        s.text = std::string(src_.substr(s.span.begin, s.span.end - s.span.begin));
        return s;
    }

    SelectStmt select() {
        expectKw("SELECT");
        SelectStmt s;
        if (acceptKw("DISTINCT")) s.distinct = true;
        else acceptKw("ALL");

        do {
            SelectItem item;
            const SourceSpan start = cur().span;
            item.expr = expr();
            if (acceptKw("AS")) {
                item.alias = expectIdent("column alias after AS").value;
            } else if (cur().type == TokenType::Identifier) {
                item.alias = advance().value;
            }
            item.span = spanFrom(start);
            s.items.push_back(std::move(item));
        } while (acceptPunct(","));

        if (acceptKw("FROM")) {
            s.from = tableRef();
            while (true) {
                const SourceSpan start = cur().span;
                if (acceptPunct(",")) {  // implicit cross join
                    JoinClause j;
                    j.type = JoinType::Inner;
                    j.table = tableRef();
                    j.span = spanFrom(start);
                    s.joins.push_back(std::move(j));
                    continue;
                }
                JoinType type;
                if (checkKw("JOIN") || checkKw("INNER")) {
                    type = JoinType::Inner;
                    acceptKw("INNER");
                    expectKw("JOIN");
                } else if (checkKw("LEFT")) {
                    type = JoinType::Left;
                    advance();
                    acceptKw("OUTER");
                    expectKw("JOIN");
                } else {
                    break;
                }
                JoinClause j;
                j.type = type;
                j.table = tableRef();
                expectKw("ON");
                j.on = expr();
                j.span = spanFrom(start);
                s.joins.push_back(std::move(j));
            }
        } else if (checkKw("JOIN") || checkKw("WHERE") || checkKw("GROUP")) {
            fail("expected FROM before " + describe(cur()));
        }

        if (acceptKw("WHERE")) s.where = expr();
        if (acceptKw("GROUP")) {
            expectKw("BY");
            do {
                s.groupBy.push_back(expr());
            } while (acceptPunct(","));
        }
        if (acceptKw("HAVING")) s.having = expr();
        if (acceptKw("ORDER")) {
            expectKw("BY");
            do {
                OrderItem o;
                const SourceSpan start = cur().span;
                o.expr = expr();
                if (acceptKw("DESC")) o.descending = true;
                else acceptKw("ASC");
                o.span = spanFrom(start);
                s.orderBy.push_back(std::move(o));
            } while (acceptPunct(","));
        }
        if (acceptKw("LIMIT")) {
            s.limitSpan = cur().span;
            s.limit = nonNegativeInt("LIMIT");
        }
        if (acceptKw("OFFSET")) {
            s.offsetSpan = cur().span;
            s.offset = nonNegativeInt("OFFSET");
        }
        if (checkPunct("(") || checkKw("SELECT")) fail("subqueries are not supported");
        return s;
    }

    std::int64_t nonNegativeInt(const char* clause) {
        if (cur().type != TokenType::Integer) fail(std::string(clause) + " expects a non-negative integer literal, found " + describe(cur()));
        const Token& t = advance();
        std::int64_t v = 0;
        auto [p, ec] = std::from_chars(t.value.data(), t.value.data() + t.value.size(), v);
        if (ec != std::errc() || p != t.value.data() + t.value.size()) failAt(std::string(clause) + " value is out of range", t.span);
        return v;
    }

    TableRef tableRef() {
        TableRef r;
        const Token& n = expectIdent("table name");
        r.name = n.value;
        r.span = n.span;
        if (acceptKw("AS")) {
            r.alias = expectIdent("table alias").value;
        } else if (cur().type == TokenType::Identifier) {
            r.alias = advance().value;
        }
        r.span = spanFrom(n.span);
        return r;
    }

    DataType typeName() {
        if (cur().type != TokenType::Keyword) fail("expected a column type (INTEGER, FLOAT, TEXT, BOOLEAN), found " + describe(cur()));
        const std::string kw = cur().value;
        DataType t;
        if (kw == "INTEGER" || kw == "INT" || kw == "BIGINT") t = DataType::Integer;
        else if (kw == "FLOAT" || kw == "REAL" || kw == "DOUBLE") t = DataType::Float;
        else if (kw == "TEXT" || kw == "VARCHAR") t = DataType::Text;
        else if (kw == "BOOLEAN" || kw == "BOOL") t = DataType::Boolean;
        else fail("expected a column type (INTEGER, FLOAT, TEXT, BOOLEAN), found " + describe(cur()));
        advance();
        if (kw == "VARCHAR" && acceptPunct("(")) {  // VARCHAR(n): length accepted, not enforced
            if (cur().type != TokenType::Integer) fail("expected length in VARCHAR(n)");
            advance();
            expectPunct(")");
        }
        return t;
    }

    CreateTableStmt createTable() {
        expectKw("CREATE");
        expectKw("TABLE");
        CreateTableStmt c;
        if (acceptKw("IF")) {
            expectKw("NOT");
            expectKw("EXISTS");
            c.ifNotExists = true;
        }
        const Token& n = expectIdent("table name");
        c.name = n.value;
        c.nameSpan = n.span;
        expectPunct("(");
        do {
            ColumnDef col;
            const Token& cn = expectIdent("column name");
            col.name = cn.value;
            col.type = typeName();
            col.span = spanFrom(cn.span);
            if (!checkPunct(",") && !checkPunct(")")) {
                fail("column constraints are not supported; expected ',' or ')', found " + describe(cur()));
            }
            c.columns.push_back(std::move(col));
        } while (acceptPunct(","));
        expectPunct(")");
        return c;
    }

    CreateIndexStmt createIndex() {
        expectKw("CREATE");
        CreateIndexStmt c;
        if (acceptKw("UNIQUE")) c.unique = true;
        expectKw("INDEX");
        if (acceptKw("IF")) {
            expectKw("NOT");
            expectKw("EXISTS");
            c.ifNotExists = true;
        }
        c.name = expectIdent("index name").value;
        expectKw("ON");
        const Token& t = expectIdent("table name");
        c.table = t.value;
        c.tableSpan = t.span;
        expectPunct("(");
        do {
            const Token& col = expectIdent("column name");
            c.columns.push_back(col.value);
            c.columnSpans.push_back(col.span);
            if (checkKw("ASC") || checkKw("DESC")) fail("per-column index ordering is not supported");
        } while (acceptPunct(","));
        expectPunct(")");
        return c;
    }

    InsertStmt insert() {
        expectKw("INSERT");
        expectKw("INTO");
        InsertStmt ins;
        const Token& t = expectIdent("table name");
        ins.table = t.value;
        ins.tableSpan = t.span;
        if (acceptPunct("(")) {
            do {
                const Token& c = expectIdent("column name");
                ins.columns.push_back(c.value);
                ins.columnSpans.push_back(c.span);
            } while (acceptPunct(","));
            expectPunct(")");
        }
        if (checkKw("SELECT")) fail("INSERT ... SELECT is not supported; use INSERT ... VALUES");
        expectKw("VALUES");
        do {
            expectPunct("(");
            std::vector<ExprPtr> row;
            do {
                row.push_back(expr());
            } while (acceptPunct(","));
            expectPunct(")");
            ins.rows.push_back(std::move(row));
        } while (acceptPunct(","));
        return ins;
    }

    UpdateStmt update() {
        expectKw("UPDATE");
        UpdateStmt u;
        const Token& t = expectIdent("table name");
        u.table = t.value;
        u.tableSpan = t.span;
        expectKw("SET");
        do {
            Assignment a;
            const Token& c = expectIdent("column name");
            a.column = c.value;
            a.span = c.span;
            if (!checkOp("=")) fail("expected '=' in SET clause, found " + describe(cur()));
            advance();
            a.value = expr();
            u.assignments.push_back(std::move(a));
        } while (acceptPunct(","));
        if (acceptKw("WHERE")) u.where = expr();
        return u;
    }

    DeleteStmt del() {
        expectKw("DELETE");
        expectKw("FROM");
        DeleteStmt d;
        const Token& t = expectIdent("table name");
        d.table = t.value;
        d.tableSpan = t.span;
        if (acceptKw("WHERE")) d.where = expr();
        return d;
    }

    // ---- expressions (lowest to highest precedence) ----------------------
    ExprPtr makeBinary(std::string op, ExprPtr l, ExprPtr r) {
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::Binary;
        e->op = std::move(op);
        e->span = SourceSpan::cover(l->span, r->span);
        e->children.push_back(std::move(l));
        e->children.push_back(std::move(r));
        return e;
    }

    ExprPtr expr() {
        if (++depth_ > 200) fail("expression is nested too deeply");
        ExprPtr e = orExpr();
        --depth_;
        return e;
    }

    ExprPtr orExpr() {
        ExprPtr l = andExpr();
        while (acceptKw("OR")) l = makeBinary("OR", std::move(l), andExpr());
        return l;
    }

    ExprPtr andExpr() {
        ExprPtr l = notExpr();
        while (acceptKw("AND")) l = makeBinary("AND", std::move(l), notExpr());
        return l;
    }

    ExprPtr notExpr() {
        if (checkKw("NOT")) {
            const SourceSpan start = advance().span;
            if (++depth_ > 200) fail("expression is nested too deeply");
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::Unary;
            e->op = "NOT";
            e->children.push_back(notExpr());
            --depth_;
            e->span = spanFrom(start);
            return e;
        }
        return predicate();
    }

    ExprPtr predicate() {
        ExprPtr l = additive();
        const SourceSpan start = l->span;
        if (cur().type == TokenType::Operator) {
            const std::string& op = cur().value;
            if (op == "=" || op == "<>" || op == "<" || op == "<=" || op == ">" || op == ">=") {
                std::string o = op;
                advance();
                ExprPtr r = additive();
                ExprPtr e = makeBinary(std::move(o), std::move(l), std::move(r));
                if (cur().type == TokenType::Operator &&
                    (cur().value == "=" || cur().value == "<>" || cur().value == "<" || cur().value == "<=" ||
                     cur().value == ">" || cur().value == ">=")) {
                    fail("comparison operators cannot be chained; use AND or parentheses");
                }
                return e;
            }
        }
        if (acceptKw("IS")) {
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::IsNull;
            e->negated = acceptKw("NOT");
            expectKw("NULL");
            e->children.push_back(std::move(l));
            e->span = spanFrom(start);
            return e;
        }
        bool negated = false;
        if (checkKw("NOT") && (checkKwAt(1, "BETWEEN") || checkKwAt(1, "IN") || checkKwAt(1, "LIKE"))) {
            advance();
            negated = true;
        }
        if (acceptKw("BETWEEN")) {
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::Between;
            e->negated = negated;
            e->children.push_back(std::move(l));
            e->children.push_back(additive());
            expectKw("AND");
            e->children.push_back(additive());
            e->span = spanFrom(start);
            return e;
        }
        if (acceptKw("IN")) {
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::InList;
            e->negated = negated;
            e->children.push_back(std::move(l));
            expectPunct("(");
            if (checkKw("SELECT")) fail("subqueries are not supported");
            do {
                e->children.push_back(expr());
            } while (acceptPunct(","));
            expectPunct(")");
            e->span = spanFrom(start);
            return e;
        }
        if (acceptKw("LIKE")) {
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::Like;
            e->negated = negated;
            e->children.push_back(std::move(l));
            e->children.push_back(additive());
            e->span = spanFrom(start);
            return e;
        }
        if (negated) fail("expected BETWEEN, IN or LIKE after NOT");
        return l;
    }

    ExprPtr additive() {
        ExprPtr l = multiplicative();
        while (checkOp("+") || checkOp("-") || checkOp("||")) {
            std::string op = advance().value;
            l = makeBinary(std::move(op), std::move(l), multiplicative());
        }
        return l;
    }

    ExprPtr multiplicative() {
        ExprPtr l = unary();
        while (checkOp("*") || checkOp("/") || checkOp("%")) {
            std::string op = advance().value;
            l = makeBinary(std::move(op), std::move(l), unary());
        }
        return l;
    }

    ExprPtr unary() {
        if (checkOp("-") || checkOp("+")) {
            const Token& t = advance();
            if (++depth_ > 200) fail("expression is nested too deeply");
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::Unary;
            e->op = t.value;
            e->children.push_back(unary());
            --depth_;
            e->span = spanFrom(t.span);
            return e;
        }
        return primary();
    }

    ExprPtr literal(Value v, const SourceSpan& span) {
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::Literal;
        e->literal = std::move(v);
        e->span = span;
        return e;
    }

    ExprPtr primary() {
        const Token& t = cur();
        switch (t.type) {
            case TokenType::Integer: {
                advance();
                std::int64_t v = 0;
                auto [p, ec] = std::from_chars(t.value.data(), t.value.data() + t.value.size(), v);
                if (ec != std::errc() || p != t.value.data() + t.value.size()) {
                    failAt("integer literal " + t.value + " is out of range for 64-bit INTEGER", t.span);
                }
                return literal(Value::integer(v), t.span);
            }
            case TokenType::Float: {
                advance();
                return literal(Value::floating(std::strtod(t.value.c_str(), nullptr)), t.span);
            }
            case TokenType::String:
                advance();
                return literal(Value::text(t.value), t.span);
            case TokenType::Keyword:
                if (t.value == "NULL") { advance(); return literal(Value::null(), t.span); }
                if (t.value == "TRUE") { advance(); return literal(Value::boolean(true), t.span); }
                if (t.value == "FALSE") { advance(); return literal(Value::boolean(false), t.span); }
                if (t.value == "SELECT") fail("subqueries are not supported");
                fail("expected an expression, found " + describe(t));
            case TokenType::Operator:
                if (t.value == "*") {
                    advance();
                    auto e = std::make_unique<Expr>();
                    e->kind = ExprKind::Star;
                    e->span = t.span;
                    return e;
                }
                fail("expected an expression, found " + describe(t));
            case TokenType::Punctuation:
                if (t.value == "(") {
                    const SourceSpan start = advance().span;
                    if (checkKw("SELECT")) fail("subqueries are not supported");
                    ExprPtr inner = expr();
                    expectPunct(")");
                    inner->span = spanFrom(start);  // include parentheses for source highlighting
                    return inner;
                }
                fail("expected an expression, found " + describe(t));
            case TokenType::Identifier:
                return identifierExpr();
            default:
                fail("expected an expression, found " + describe(t));
        }
    }

    ExprPtr identifierExpr() {
        const Token& first = advance();
        if (checkPunct("(")) {
            advance();
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::Function;
            e->name = first.value;
            if (checkOp("*")) {
                advance();
                e->starArg = true;
            } else if (!checkPunct(")")) {
                if (acceptKw("DISTINCT")) e->distinct = true;
                do {
                    e->children.push_back(expr());
                } while (acceptPunct(","));
            }
            expectPunct(")");
            e->span = spanFrom(first.span);
            return e;
        }
        if (acceptPunct(".")) {
            if (checkOp("*")) {
                advance();
                auto e = std::make_unique<Expr>();
                e->kind = ExprKind::Star;
                e->table = first.value;
                e->span = spanFrom(first.span);
                return e;
            }
            const Token& col = expectIdent("column name after '.'");
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::ColumnRef;
            e->table = first.value;
            e->column = col.value;
            e->span = spanFrom(first.span);
            return e;
        }
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::ColumnRef;
        e->column = first.value;
        e->span = first.span;
        return e;
    }

    std::string_view src_;
    std::vector<Token> toks_;
    std::size_t pos_ = 0;
    std::size_t lastEnd_ = 0;
    int depth_ = 0;
};

}  // namespace

std::vector<Statement> parseScript(std::string_view sql) { return Parser(sql, tokenize(sql)).script(); }

Statement parseStatement(std::string_view sql) { return Parser(sql, tokenize(sql)).single(); }

ExprPtr parseExpression(std::string_view sql) { return Parser(sql, tokenize(sql)).standaloneExpr(); }

}  // namespace querylab::sql
