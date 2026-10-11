#pragma once

#include <doctest.h>

#include <string>
#include <vector>

#include "querylab/api/database.h"

namespace qltest {

using querylab::api::json;

inline std::string cellText(const json& v) {
    if (v.is_null()) return "NULL";
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "TRUE" : "FALSE";
    if (v.is_number_float()) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.6g", v.get<double>());
        return buf;
    }
    return v.dump();
}

using Rows = std::vector<std::vector<std::string>>;

/// Runs a script, requires success, returns the last statement's result.
inline json run(querylab::api::Database& db, const std::string& sql, const querylab::api::QueryOptions& o = {}) {
    json out = db.execute(sql, o);
    if (!out["success"].get<bool>()) {
        FAIL_CHECK("query failed: " << sql << "\n  " << out["error"]["display"].get<std::string>());
        return out["results"].back();
    }
    return out["results"].back();
}

inline Rows rows(querylab::api::Database& db, const std::string& sql, const querylab::api::QueryOptions& o = {}) {
    json r = run(db, sql, o);
    Rows out;
    if (!r.contains("rows")) return out;
    for (const auto& row : r["rows"]) {
        std::vector<std::string> line;
        for (const auto& v : row) line.push_back(cellText(v));
        out.push_back(std::move(line));
    }
    return out;
}

/// Runs a script expected to fail; returns the error object.
inline json fails(querylab::api::Database& db, const std::string& sql) {
    json out = db.execute(sql);
    CHECK_MESSAGE(!out["success"].get<bool>(), "expected failure: " << sql);
    return out.contains("error") ? out["error"] : json::object();
}

/// Finds the first plan node (depth-first) with the given op name.
inline const json* findOp(const json& node, const std::string& op) {
    if (node.value("op", "") == op) return &node;
    for (const auto& c : node["children"]) {
        if (const json* f = findOp(c, op)) return f;
    }
    return nullptr;
}

inline void usersFixture(querylab::api::Database& db) {
    run(db,
        "CREATE TABLE users (id INTEGER, name TEXT, age INTEGER, city TEXT);"
        "INSERT INTO users VALUES (1, 'Aman', 24, 'Bengaluru'), (2, 'Priya', 29, 'Delhi'), (3, 'Ravi', NULL, 'Pune'),"
        "(4, 'Meera', 35, 'Delhi'), (5, 'Karan', 29, NULL);");
}

}  // namespace qltest
