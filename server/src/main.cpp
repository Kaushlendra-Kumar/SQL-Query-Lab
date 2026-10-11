// QueryLab HTTP server: exposes the engine through a versioned JSON API and
// optionally serves the built frontend.
//
//   querylab_server [--host 127.0.0.1] [--port 8080] [--db FILE] [--static DIR]
//                   [--dataset NAME --scale N] [--allow-origin URL]
#include <atomic>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>

#include <httplib.h>

#include "querylab/api/database.h"
#include "querylab/explain/ast_json.h"
#include "querylab/sql/parser.h"

using querylab::QueryError;
using querylab::api::Database;
using querylab::api::json;
using querylab::api::QueryOptions;

namespace {

constexpr std::size_t kMaxBodyBytes = 1 << 20;   // 1 MiB request bodies
constexpr std::size_t kMaxSqlBytes = 200 * 1024; // 200 KiB of SQL per request

httplib::Server* gServer = nullptr;

void sendJson(httplib::Response& res, const json& body, int status = 200) {
    res.status = status;
    res.set_content(body.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
}

void sendError(httplib::Response& res, int status, const std::string& stage, const std::string& message) {
    sendJson(res, json{{"success", false}, {"error", {{"stage", stage}, {"message", message}, {"display", stage + " error: " + message}}}}, status);
}

int statusFor(const QueryError& e) {
    const std::string msg = e.what();
    if (msg.find("does not exist") != std::string::npos) return 404;
    return 400;
}

/// Parses the JSON request body; sends a 400 and returns false on failure.
bool parseBody(const httplib::Request& req, httplib::Response& res, json& out) {
    if (req.body.size() > kMaxBodyBytes) {
        sendError(res, 413, "api", "request body exceeds 1 MiB");
        return false;
    }
    if (req.body.empty()) {
        out = json::object();
        return true;
    }
    try {
        out = json::parse(req.body);
    } catch (const json::parse_error& e) {
        sendError(res, 400, "api", std::string("malformed JSON: ") + e.what());
        return false;
    }
    if (!out.is_object()) {
        sendError(res, 400, "api", "request body must be a JSON object");
        return false;
    }
    return true;
}

/// Reads the mandatory "sql" field.
bool sqlField(const json& body, httplib::Response& res, std::string& sql) {
    if (!body.contains("sql") || !body["sql"].is_string()) {
        sendError(res, 400, "api", "field 'sql' (string) is required");
        return false;
    }
    sql = body["sql"].get<std::string>();
    if (sql.size() > kMaxSqlBytes) {
        sendError(res, 413, "api", "SQL text exceeds 200 KiB");
        return false;
    }
    return true;
}

bool parseOptions(const json& body, httplib::Response& res, QueryOptions& o) {
    if (!body.contains("options")) return true;
    const json& j = body["options"];
    if (!j.is_object()) {
        sendError(res, 400, "api", "'options' must be an object");
        return false;
    }
    try {
        o.timing = j.value("timing", true);
        o.recordEvents = j.value("recordEvents", true);
        o.disableIndexScans = j.value("disableIndexScans", false);
        o.disableHashJoin = j.value("disableHashJoin", false);
        o.coldCache = j.value("coldCache", false);
        const auto maxRows = j.value("maxRows", 0);
        if (maxRows < 0 || maxRows > 100000) {
            sendError(res, 400, "api", "options.maxRows must be between 0 and 100000");
            return false;
        }
        o.maxRows = static_cast<std::size_t>(maxRows);
    } catch (const json::type_error& e) {
        sendError(res, 400, "api", std::string("invalid option type: ") + e.what());
        return false;
    }
    return true;
}

bool validIdentifier(const std::string& s) {
    static const std::regex re("^[A-Za-z_][A-Za-z0-9_]{0,62}$");
    return std::regex_match(s, re);
}

/// Wraps a handler: converts engine/JSON errors into structured responses so
/// no request can crash the server.
template <class F>
auto guarded(F f) {
    return [f](const httplib::Request& req, httplib::Response& res) {
        try {
            f(req, res);
        } catch (const QueryError& e) {
            sendJson(res, json{{"success", false},
                               {"error", {{"stage", querylab::stageName(e.stage())}, {"message", e.what()}, {"display", e.describe()}}}},
                     statusFor(e));
        } catch (const json::exception& e) {
            sendError(res, 400, "api", std::string("invalid request: ") + e.what());
        } catch (const std::exception& e) {
            sendError(res, 500, "internal", e.what());
        }
    };
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1", dbPath, staticDir, dataset, allowOrigin;
    int port = 8080;
    std::size_t scale = 10000;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << a << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--host") host = val();
        else if (a == "--port") port = std::stoi(val());
        else if (a == "--db") dbPath = val();
        else if (a == "--static") staticDir = val();
        else if (a == "--dataset") dataset = val();
        else if (a == "--scale") scale = std::stoul(val());
        else if (a == "--allow-origin") allowOrigin = val();
        else if (a == "-h" || a == "--help") {
            std::cout << "usage: querylab_server [--host 127.0.0.1] [--port 8080] [--db FILE] [--static DIR]\n"
                         "                       [--dataset ecommerce|social|employees --scale N] [--allow-origin URL]\n";
            return 0;
        } else {
            std::cerr << "unknown option " << a << "\n";
            return 2;
        }
    }

    std::unique_ptr<Database> db;
    try {
        if (dbPath.empty()) {
            db = Database::inMemory();
        } else {
            const bool exists = std::filesystem::exists(dbPath);
            db = Database::openFile(dbPath, !exists);
        }
        if (!dataset.empty()) {
            auto j = db->loadDataset(dataset, scale);
            std::cout << "Loaded dataset " << dataset << ": " << j["tables"].dump() << "\n";
        }
    } catch (const QueryError& e) {
        std::cerr << e.describe() << "\n";
        return 1;
    }

    httplib::Server svr;
    gServer = &svr;
    svr.set_payload_max_length(kMaxBodyBytes);
    svr.new_task_queue = [] { return new httplib::ThreadPool(4); };

    // CORS: only for the Vite dev server (or an explicitly allowed origin).
    svr.set_post_routing_handler([allowOrigin](const httplib::Request& req, httplib::Response& res) {
        const std::string origin = req.get_header_value("Origin");
        static const std::regex local(R"(^http://(localhost|127\.0\.0\.1)(:\d+)?$)");
        if (!origin.empty() && (std::regex_match(origin, local) || origin == allowOrigin)) {
            res.set_header("Access-Control-Allow-Origin", origin);
            res.set_header("Vary", "Origin");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        }
        res.set_header("X-Content-Type-Options", "nosniff");
    });
    svr.Options(R"(/api/.*)", [](const httplib::Request&, httplib::Response& res) { res.status = 204; });

    const std::string v1 = "/api/v1";

    svr.Get(v1 + "/health", guarded([&](const httplib::Request&, httplib::Response& res) { sendJson(res, db->health()); }));

    svr.Post(v1 + "/query", guarded([&](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string sql;
        QueryOptions o;
        if (!parseBody(req, res, body) || !sqlField(body, res, sql) || !parseOptions(body, res, o)) return;
        sendJson(res, db->execute(sql, o));
    }));

    svr.Post(v1 + "/explain", guarded([&](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string sql;
        QueryOptions o;
        if (!parseBody(req, res, body) || !sqlField(body, res, sql) || !parseOptions(body, res, o)) return;
        sendJson(res, db->explain(sql, o));
    }));

    // Lightweight front-end stages for live editor feedback (no catalog access).
    svr.Post(v1 + "/parse", guarded([&](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string sql;
        if (!parseBody(req, res, body) || !sqlField(body, res, sql)) return;
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
        sendJson(res, out);
    }));

    svr.Get(v1 + "/catalog", guarded([&](const httplib::Request&, httplib::Response& res) { sendJson(res, db->catalogJson()); }));

    svr.Get(v1 + R"(/tables/([A-Za-z0-9_]+)/schema)", guarded([&](const httplib::Request& req, httplib::Response& res) {
        sendJson(res, db->tableSchema(req.matches[1]));
    }));

    svr.Get(v1 + R"(/tables/([A-Za-z0-9_]+)/rows)", guarded([&](const httplib::Request& req, httplib::Response& res) {
        std::size_t offset = 0, limit = 100;
        if (req.has_param("offset")) offset = std::stoul(req.get_param_value("offset"));
        if (req.has_param("limit")) limit = std::stoul(req.get_param_value("limit"));
        sendJson(res, db->tableRows(req.matches[1], offset, limit));
    }));

    svr.Get(v1 + R"(/tables/([A-Za-z0-9_]+)/pages/(\d+))", guarded([&](const httplib::Request& req, httplib::Response& res) {
        sendJson(res, db->pageDetail(req.matches[1], std::stoul(req.matches[2])));
    }));

    svr.Get(v1 + "/indexes", guarded([&](const httplib::Request&, httplib::Response& res) { sendJson(res, db->indexesJson()); }));

    svr.Get(v1 + R"(/indexes/([A-Za-z0-9_]+))", guarded([&](const httplib::Request& req, httplib::Response& res) {
        sendJson(res, db->indexDetail(req.matches[1]));
    }));

    // Creates an index from structured fields. Identifiers are validated, so
    // the generated statement cannot carry anything but a CREATE INDEX.
    svr.Post(v1 + "/indexes", guarded([&](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parseBody(req, res, body)) return;
        const std::string name = body.value("name", "");
        const std::string table = body.value("table", "");
        const json cols = body.value("columns", json::array());
        if (!validIdentifier(name) || !validIdentifier(table) || !cols.is_array() || cols.empty() || cols.size() > 8) {
            sendError(res, 400, "api", "expected {name, table, columns: [...]} with identifiers matching [A-Za-z_][A-Za-z0-9_]*");
            return;
        }
        std::string sql = std::string(body.value("unique", false) ? "CREATE UNIQUE INDEX " : "CREATE INDEX ") + name + " ON " + table + " (";
        for (std::size_t i = 0; i < cols.size(); ++i) {
            if (!cols[i].is_string() || !validIdentifier(cols[i].get<std::string>())) {
                sendError(res, 400, "api", "invalid column name in 'columns'");
                return;
            }
            sql += (i ? ", " : "") + cols[i].get<std::string>();
        }
        sendJson(res, db->execute(sql + ")"));
    }));

    svr.Delete(v1 + R"(/indexes/([A-Za-z0-9_]+))", guarded([&](const httplib::Request& req, httplib::Response& res) {
        sendJson(res, db->execute("DROP INDEX " + std::string(req.matches[1])));
    }));

    svr.Get(v1 + "/storage", guarded([&](const httplib::Request&, httplib::Response& res) { sendJson(res, db->storageJson()); }));
    svr.Get(v1 + "/datasets", guarded([&](const httplib::Request&, httplib::Response& res) { sendJson(res, db->datasetsJson()); }));

    svr.Post(v1 + R"(/datasets/([a-z]+)/load)", guarded([&](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parseBody(req, res, body)) return;
        const auto rowsWanted = body.value("scale", 10000);
        if (rowsWanted < 10 || rowsWanted > 1000000) {
            sendError(res, 400, "api", "scale must be between 10 and 1,000,000");
            return;
        }
        sendJson(res, db->loadDataset(req.matches[1], static_cast<std::size_t>(rowsWanted)));
    }));

    svr.Post(v1 + "/reset", guarded([&](const httplib::Request&, httplib::Response& res) { sendJson(res, db->reset()); }));

    svr.Post(v1 + "/benchmarks", guarded([&](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parseBody(req, res, body)) return;
        sendJson(res, db->benchmark(body));
    }));

    svr.Get(v1 + "/history", guarded([&](const httplib::Request&, httplib::Response& res) { sendJson(res, db->history()); }));
    svr.Get(v1 + "/settings", guarded([&](const httplib::Request&, httplib::Response& res) { sendJson(res, db->settingsJson()); }));
    svr.Put(v1 + "/settings", guarded([&](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parseBody(req, res, body)) return;
        sendJson(res, db->updateSettings(body));
    }));
    // Does not take the engine lock: flips an atomic checked by running operators.
    svr.Post(v1 + "/cancel", guarded([&](const httplib::Request&, httplib::Response& res) {
        db->cancel();
        sendJson(res, json{{"success", true}});
    }));

    svr.Get(R"(/api/.*)", [](const httplib::Request&, httplib::Response& res) { sendError(res, 404, "api", "unknown endpoint"); });
    svr.Post(R"(/api/.*)", [](const httplib::Request&, httplib::Response& res) { sendError(res, 404, "api", "unknown endpoint"); });

    if (!staticDir.empty()) {
        if (!svr.set_mount_point("/", staticDir)) {
            std::cerr << "static directory '" << staticDir << "' not found\n";
            return 1;
        }
        // Single-page app: unknown non-API paths serve index.html.
        svr.set_error_handler([staticDir](const httplib::Request& req, httplib::Response& res) {
            if (res.status == 404 && req.path.rfind("/api/", 0) != 0) {
                std::ifstream f(std::filesystem::path(staticDir) / "index.html", std::ios::binary);
                if (f) {
                    std::string html((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                    res.status = 200;
                    res.set_content(html, "text/html");
                }
            }
        });
    }

    std::signal(SIGINT, [](int) {
        if (gServer) gServer->stop();
    });
    std::signal(SIGTERM, [](int) {
        if (gServer) gServer->stop();
    });

    std::cout << "QueryLab server listening on http://" << host << ":" << port << " ("
              << (dbPath.empty() ? "in-memory database" : "database file " + dbPath) << ")\n";
    if (host != "127.0.0.1" && host != "localhost") {
        std::cout << "WARNING: listening on a non-loopback address. The API has no authentication; anyone who can reach it can run SQL.\n";
    }
    if (!svr.listen(host, port)) {
        std::cerr << "failed to listen on " << host << ":" << port << " (is the port in use?)\n";
        return 1;
    }
    db->checkpoint();
    std::cout << "server stopped\n";
    return 0;
}
