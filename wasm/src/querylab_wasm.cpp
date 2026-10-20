// QueryLab in the browser: the engine compiled to WebAssembly, exposed through
// a single request handler that mirrors the HTTP server's /api/v1 routes, status
// codes and error bodies (see server/src/main.cpp). The frontend calls it from a
// Web Worker in place of fetch(), so the same UI runs with no backend at all.
//
//   handle("POST", "/query", "{\"sql\":\"SELECT 1\"}")
//     -> "200\n{...response body...}"
#include <memory>
#include <regex>
#include <string>

#include <emscripten/bind.h>

#include "querylab/api/database.h"
#include "querylab/explain/ast_json.h"
#include "querylab/sql/parser.h"

using querylab::QueryError;
using querylab::api::Database;
using querylab::api::json;
using querylab::api::QueryOptions;

namespace {

constexpr std::size_t kMaxBodyBytes = 1 << 20;   // same limits as the server
constexpr std::size_t kMaxSqlBytes = 200 * 1024;

std::unique_ptr<Database> gDb;

Database& db() {
    if (!gDb) gDb = Database::inMemory();
    return *gDb;
}

/// A response that ends the handler early: thrown by the validation helpers.
struct Reply {
    int status;
    json body;
};

Reply error(int status, const std::string& stage, const std::string& message) {
    return {status, json{{"success", false}, {"error", {{"stage", stage}, {"message", message}, {"display", stage + " error: " + message}}}}};
}

int statusFor(const QueryError& e) {
    const std::string msg = e.what();
    if (msg.find("does not exist") != std::string::npos) return 404;
    return 400;
}

json parseBody(const std::string& body) {
    if (body.size() > kMaxBodyBytes) throw error(413, "api", "request body exceeds 1 MiB");
    if (body.empty()) return json::object();
    json out;
    try {
        out = json::parse(body);
    } catch (const json::parse_error& e) {
        throw error(400, "api", std::string("malformed JSON: ") + e.what());
    }
    if (!out.is_object()) throw error(400, "api", "request body must be a JSON object");
    return out;
}

std::string sqlField(const json& body) {
    if (!body.contains("sql") || !body["sql"].is_string()) throw error(400, "api", "field 'sql' (string) is required");
    std::string sql = body["sql"].get<std::string>();
    if (sql.size() > kMaxSqlBytes) throw error(413, "api", "SQL text exceeds 200 KiB");
    return sql;
}

QueryOptions parseOptions(const json& body) {
    QueryOptions o;
    if (!body.contains("options")) return o;
    const json& j = body["options"];
    if (!j.is_object()) throw error(400, "api", "'options' must be an object");
    try {
        o.timing = j.value("timing", true);
        o.recordEvents = j.value("recordEvents", true);
        o.disableIndexScans = j.value("disableIndexScans", false);
        o.disableHashJoin = j.value("disableHashJoin", false);
        o.coldCache = j.value("coldCache", false);
        const auto maxRows = j.value("maxRows", 0);
        if (maxRows < 0 || maxRows > 100000) throw error(400, "api", "options.maxRows must be between 0 and 100000");
        o.maxRows = static_cast<std::size_t>(maxRows);
    } catch (const json::type_error& e) {
        throw error(400, "api", std::string("invalid option type: ") + e.what());
    }
    return o;
}

bool validIdentifier(const std::string& s) {
    static const std::regex re("^[A-Za-z_][A-Za-z0-9_]{0,62}$");
    return std::regex_match(s, re);
}

/// Value of `key` in a "a=1&b=2" query string, or `fallback`.
std::size_t queryParam(const std::string& query, const std::string& key, std::size_t fallback) {
    const std::regex re("(?:^|&)" + key + "=(\\d+)");
    std::smatch m;
    if (std::regex_search(query, m, re)) return std::stoul(m[1]);
    return fallback;
}

json parseSql(const std::string& sql) {
    json out{{"success", true}};
    try {
        out["tokens"] = querylab::explain::tokensToJson(querylab::sql::tokenize(sql, true));
        json asts = json::array();
        for (const auto& st : querylab::sql::parseScript(sql)) asts.push_back(querylab::explain::statementToJson(st));
        out["statements"] = asts;
    } catch (const QueryError& e) {
        out["success"] = false;
        out["error"] = {{"stage", querylab::stageName(e.stage())}, {"message", e.what()}, {"display", e.describe()}};
        if (e.span()) out["error"]["span"] = querylab::explain::spanToJson(*e.span());
    }
    return out;
}

json createIndex(const json& body) {
    const std::string name = body.value("name", "");
    const std::string table = body.value("table", "");
    const json cols = body.value("columns", json::array());
    if (!validIdentifier(name) || !validIdentifier(table) || !cols.is_array() || cols.empty() || cols.size() > 8) {
        throw error(400, "api", "expected {name, table, columns: [...]} with identifiers matching [A-Za-z_][A-Za-z0-9_]*");
    }
    std::string sql = std::string(body.value("unique", false) ? "CREATE UNIQUE INDEX " : "CREATE INDEX ") + name + " ON " + table + " (";
    for (std::size_t i = 0; i < cols.size(); ++i) {
        if (!cols[i].is_string() || !validIdentifier(cols[i].get<std::string>())) throw error(400, "api", "invalid column name in 'columns'");
        sql += (i ? ", " : "") + cols[i].get<std::string>();
    }
    return db().execute(sql + ")");
}

/// Routes one request. `path` is relative to /api/v1 and may carry a query string.
Reply route(const std::string& method, const std::string& fullPath, const std::string& rawBody) {
    const auto q = fullPath.find('?');
    const std::string path = fullPath.substr(0, q);
    const std::string query = q == std::string::npos ? "" : fullPath.substr(q + 1);
    static const std::regex tableRe(R"(^/tables/([A-Za-z0-9_]+)/(schema|rows)$)");
    static const std::regex pageRe(R"(^/tables/([A-Za-z0-9_]+)/pages/(\d+)$)");
    static const std::regex indexRe(R"(^/indexes/([A-Za-z0-9_]+)$)");
    static const std::regex datasetRe(R"(^/datasets/([a-z]+)/load$)");
    std::smatch m;

    if (method == "GET") {
        if (path == "/health") return {200, db().health()};
        if (path == "/catalog") return {200, db().catalogJson()};
        if (path == "/indexes") return {200, db().indexesJson()};
        if (path == "/storage") return {200, db().storageJson()};
        if (path == "/datasets") return {200, db().datasetsJson()};
        if (path == "/history") return {200, db().history()};
        if (path == "/settings") return {200, db().settingsJson()};
        if (std::regex_match(path, m, tableRe)) {
            if (m[2] == "schema") return {200, db().tableSchema(m[1])};
            return {200, db().tableRows(m[1], queryParam(query, "offset", 0), queryParam(query, "limit", 100))};
        }
        if (std::regex_match(path, m, pageRe)) return {200, db().pageDetail(m[1], std::stoul(m[2]))};
        if (std::regex_match(path, m, indexRe)) return {200, db().indexDetail(m[1])};
    } else if (method == "POST") {
        if (path == "/query" || path == "/explain") {
            const json body = parseBody(rawBody);
            const std::string sql = sqlField(body);
            const QueryOptions o = parseOptions(body);
            return {200, path == "/query" ? db().execute(sql, o) : db().explain(sql, o)};
        }
        if (path == "/parse") return {200, parseSql(sqlField(parseBody(rawBody)))};
        if (path == "/indexes") return {200, createIndex(parseBody(rawBody))};
        if (std::regex_match(path, m, datasetRe)) {
            const json body = parseBody(rawBody);
            const auto rowsWanted = body.value("scale", 10000);
            if (rowsWanted < 10 || rowsWanted > 1000000) throw error(400, "api", "scale must be between 10 and 1,000,000");
            return {200, db().loadDataset(m[1], static_cast<std::size_t>(rowsWanted))};
        }
        if (path == "/reset") return {200, db().reset()};
        if (path == "/benchmarks") return {200, db().benchmark(parseBody(rawBody))};
        // A query runs to completion before the worker sees the next message, so
        // there is nothing to interrupt; the engine's own timeout bounds each run.
        if (path == "/cancel") return {200, json{{"success", true}}};
    } else if (method == "PUT") {
        if (path == "/settings") return {200, db().updateSettings(parseBody(rawBody))};
    } else if (method == "DELETE") {
        if (std::regex_match(path, m, indexRe)) return {200, db().execute("DROP INDEX " + std::string(m[1]))};
    }
    return error(404, "api", "unknown endpoint");
}

std::string handle(const std::string& method, const std::string& path, const std::string& body) {
    Reply r{500, json::object()};
    try {
        r = route(method, path, body);
    } catch (const Reply& early) {
        r = early;
    } catch (const QueryError& e) {
        r = {statusFor(e), json{{"success", false}, {"error", {{"stage", querylab::stageName(e.stage())}, {"message", e.what()}, {"display", e.describe()}}}}};
    } catch (const json::exception& e) {
        r = error(400, "api", std::string("invalid request: ") + e.what());
    } catch (const std::exception& e) {
        r = error(500, "internal", e.what());
    }
    return std::to_string(r.status) + "\n" + r.body.dump(-1, ' ', false, json::error_handler_t::replace);
}

}  // namespace

EMSCRIPTEN_BINDINGS(querylab) {
    emscripten::function("handle", &handle);
}
