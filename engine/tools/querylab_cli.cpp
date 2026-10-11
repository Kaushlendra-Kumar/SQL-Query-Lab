// QueryLab command-line shell: runs the engine without the server or UI.
//
//   querylab_cli                         interactive shell, in-memory database
//   querylab_cli --db shop.qldb          open (or create) a database file
//   querylab_cli --dataset ecommerce --scale 10000 -c "SELECT ..."
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

#include "querylab/api/database.h"
#include "querylab/explain/ast_json.h"
#include "querylab/sql/parser.h"

using querylab::api::Database;
using querylab::api::json;
using querylab::api::QueryOptions;

namespace {

struct Settings {
    bool showPlan = true;
    bool showRules = false;
    bool showExplain = false;
};

std::string cell(const json& v) {
    if (v.is_null()) return "NULL";
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "TRUE" : "FALSE";
    if (v.is_number_float()) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.15g", v.get<double>());
        std::string s(buf);
        if (s.find_first_of(".eE") == std::string::npos) s += ".0";
        return s;
    }
    return v.dump();
}

std::size_t displayWidth(const std::string& s) {
    std::size_t w = 0;
    for (unsigned char c : s) w += (c & 0xC0) != 0x80;  // count UTF-8 code points
    return w;
}

void printTable(const json& columns, const json& rows) {
    std::vector<std::size_t> w;
    for (const auto& c : columns) w.push_back(displayWidth(c["name"].get<std::string>()));
    std::vector<std::vector<std::string>> cells;
    for (const auto& r : rows) {
        std::vector<std::string> line;
        for (std::size_t i = 0; i < r.size(); ++i) {
            line.push_back(cell(r[i]));
            if (i < w.size()) w[i] = std::min<std::size_t>(40, std::max(w[i], displayWidth(line.back())));
        }
        cells.push_back(std::move(line));
    }
    auto pad = [](std::string s, std::size_t width) {
        if (displayWidth(s) > width) {
            s = s.substr(0, width > 1 ? width - 1 : 0) + "…";
        }
        return s + std::string(width > displayWidth(s) ? width - displayWidth(s) : 0, ' ');
    };
    std::string sep = "+";
    for (auto x : w) sep += std::string(x + 2, '-') + "+";
    std::cout << sep << "\n|";
    for (std::size_t i = 0; i < w.size(); ++i) std::cout << " " << pad(columns[i]["name"], w[i]) << " |";
    std::cout << "\n" << sep << "\n";
    for (const auto& line : cells) {
        std::cout << "|";
        for (std::size_t i = 0; i < w.size(); ++i) std::cout << " " << pad(i < line.size() ? line[i] : "", w[i]) << " |";
        std::cout << "\n";
    }
    std::cout << sep << "\n";
}

void printPlan(const json& n, const std::string& prefix, bool last, bool root) {
    std::cout << (root ? "" : prefix + (last ? "└── " : "├── ")) << n["label"].get<std::string>();
    char buf[160];
    if (n.contains("actual") && n["actual"].value("executed", false)) {
        const auto& a = n["actual"];
        std::snprintf(buf, sizeof buf, "  (est %.0f rows | actual %llu rows, %.3f ms", n["estimates"]["rows"].get<double>(),
                      static_cast<unsigned long long>(a["rowsOut"].get<std::uint64_t>()), a["timeMs"].get<double>());
        std::cout << buf;
        if (a["pagesRead"].get<std::uint64_t>() > 0) std::cout << ", " << a["pagesRead"] << " pages";
        if (a["indexEntriesExamined"].get<std::uint64_t>() > 0) std::cout << ", " << a["indexEntriesExamined"] << " index entries";
        std::cout << ")";
    } else {
        std::snprintf(buf, sizeof buf, "  (est %.0f rows, cost %.1f)", n["estimates"]["rows"].get<double>(), n["estimates"]["cost"].get<double>());
        std::cout << buf;
    }
    std::cout << "\n";
    const auto& ch = n["children"];
    for (std::size_t i = 0; i < ch.size(); ++i) printPlan(ch[i], root ? "" : prefix + (last ? "    " : "│   "), i + 1 == ch.size(), false);
}

void printResult(const json& out, const Settings& s) {
    for (const auto& r : out["results"]) {
        if (!r.value("success", false)) {
            std::cout << "Error: " << r["error"]["display"].get<std::string>() << "\n";
            if (r["error"].contains("span")) {
                // Point at the error in the statement text.
                const auto& sp = r["error"]["span"];
                std::cout << "  at line " << sp["line"] << ", column " << sp["column"] << "\n";
            }
            continue;
        }
        if (r.contains("rows") && r.value("kind", "") == "SELECT" && !r.value("explain", false)) {
            printTable(r["columns"], r["rows"]);
            std::cout << r["rowCount"] << " row(s)";
            if (r.contains("metrics")) {
                const auto& m = r["metrics"];
                char buf[200];
                std::snprintf(buf, sizeof buf, " in %.3f ms (engine) — %llu rows examined, %llu page reads (%llu misses)",
                              m["executionMs"].get<double>(), static_cast<unsigned long long>(m["rowsScanned"].get<std::uint64_t>()),
                              static_cast<unsigned long long>(m["pagesRead"].get<std::uint64_t>()),
                              static_cast<unsigned long long>(m["pageMisses"].get<std::uint64_t>()));
                std::cout << buf;
            }
            std::cout << "\n";
        }
        if (r.contains("message")) std::cout << r["message"].get<std::string>() << "\n";
        if (r.contains("physicalPlan") && (s.showPlan || r.value("explain", false))) {
            std::cout << "\nPhysical plan:\n";
            printPlan(r["physicalPlan"], "", true, true);
        }
        if (r.contains("optimizer") && s.showRules) {
            std::cout << "\nOptimizer:\n";
            for (const auto& st : r["optimizer"]) {
                std::cout << "  " << (st["applied"].get<bool>() ? "[applied] " : "[skipped] ") << st["rule"].get<std::string>() << ": "
                          << st["reason"].get<std::string>() << "\n";
                for (const auto& d : st["details"]) std::cout << "      - " << d.get<std::string>() << "\n";
                for (const auto& a : st["alternatives"]) {
                    char buf[64];
                    std::snprintf(buf, sizeof buf, "%.1f", a["cost"].get<double>());
                    std::cout << "      " << (a["chosen"].get<bool>() ? "* " : "  ") << a["name"].get<std::string>()
                              << (a["cost"].get<double>() > 0 ? " cost " + std::string(buf) : "") << " — " << a["detail"].get<std::string>() << "\n";
                }
            }
        }
        if (r.contains("explanation") && s.showExplain) {
            std::cout << "\nExplanation: " << r["explanation"]["summary"].get<std::string>() << "\n";
            for (const auto& f : r["explanation"]["facts"]) std::cout << "  • " << f["text"].get<std::string>() << "\n";
            for (const auto& g : r["explanation"]["guidance"]) std::cout << "  (guidance) " << g.get<std::string>() << "\n";
            for (const auto& sug : r["advisor"]["suggestions"]) std::cout << "  Suggested: " << sug["definition"].get<std::string>() << "\n";
            for (const auto& n : r["advisor"]["notes"]) std::cout << "  Note: " << n["text"].get<std::string>() << "\n";
        }
        std::cout << "\n";
    }
}

void help() {
    std::cout << "Enter SQL terminated by ';'. Commands:\n"
                 "  .tables                 list tables\n"
                 "  .schema TABLE           columns, statistics and indexes\n"
                 "  .load NAME [SCALE]      load a sample dataset (ecommerce, social, employees)\n"
                 "  .tokens SQL             show the token stream\n"
                 "  .ast SQL                show the parsed AST\n"
                 "  .explain SQL            plan without executing\n"
                 "  .bench N SQL            run SQL N times and report timing statistics\n"
                 "  .plan on|off            show the physical plan after each query (default on)\n"
                 "  .rules on|off           show the optimizer trace\n"
                 "  .why on|off             show the plain-language explanation and index advice\n"
                 "  .save                   checkpoint to the database file (file mode)\n"
                 "  .help  .quit\n";
}

bool command(Database& db, const std::string& line, Settings& s) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    std::string rest;
    std::getline(in, rest);
    if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
    auto onoff = [&](bool& flag) {
        flag = rest.find("on") != std::string::npos;
        std::cout << cmd << " " << (flag ? "on" : "off") << "\n";
    };
    try {
        if (cmd == ".quit" || cmd == ".exit") return false;
        if (cmd == ".help") help();
        else if (cmd == ".tables") {
            for (const auto& t : db.catalogJson()["tables"]) std::cout << "  " << t["name"].get<std::string>() << " (" << t["rowCount"] << " rows, " << t["pageCount"] << " pages)\n";
        } else if (cmd == ".schema") {
            auto j = db.tableSchema(rest);
            for (const auto& c : j["columns"]) {
                std::cout << "  " << c["name"].get<std::string>() << " " << c["type"].get<std::string>() << "  distinct=" << c["stats"]["distinct"]
                          << " nulls=" << c["stats"]["nulls"] << "\n";
            }
            for (const auto& i : j["indexes"]) std::cout << "  index " << i.get<std::string>() << "\n";
        } else if (cmd == ".load") {
            std::istringstream a(rest);
            std::string name;
            std::size_t scale = 10000;
            a >> name >> scale;
            auto j = db.loadDataset(name, scale);
            std::cout << "loaded " << name << ": " << j["tables"].dump() << " in " << j["ms"] << " ms\n";
        } else if (cmd == ".tokens") {
            for (const auto& t : querylab::sql::tokenize(rest, true)) {
                std::cout << "  " << querylab::sql::tokenTypeName(t.type) << "\t" << t.lexeme << "\t@" << t.span.line << ":" << t.span.column << "\n";
            }
        } else if (cmd == ".ast") {
            std::cout << querylab::explain::astToText(querylab::explain::statementToJson(querylab::sql::parseStatement(rest)));
        } else if (cmd == ".explain") {
            Settings t = s;
            t.showPlan = true;
            printResult(db.explain(rest), t);
        } else if (cmd == ".bench") {
            std::istringstream a(rest);
            int n = 10;
            a >> n;
            std::string sql;
            std::getline(a, sql);
            auto j = db.benchmark(json{{"sql", sql}, {"iterations", n}});
            if (!j["success"].get<bool>()) {
                std::cout << "Error: " << j["error"]["display"].get<std::string>() << "\n";
            } else {
                const auto& st = j["variants"][0]["stats"];
                std::printf("  %d runs: median %.3f ms (p25 %.3f, p75 %.3f, min %.3f, max %.3f, cv %.1f%%)\n", n, st["median"].get<double>(),
                            st["p25"].get<double>(), st["p75"].get<double>(), st["min"].get<double>(), st["max"].get<double>(),
                            st["cv"].get<double>() * 100);
                std::cout << "  plan: " << j["variants"][0]["planLabel"].get<std::string>() << ", pages read per run: "
                          << j["variants"][0]["pagesRead"] << "\n";
            }
        } else if (cmd == ".plan") onoff(s.showPlan);
        else if (cmd == ".rules") onoff(s.showRules);
        else if (cmd == ".why") onoff(s.showExplain);
        else if (cmd == ".save") {
            db.checkpoint();
            std::cout << (db.persistent() ? "saved\n" : "in-memory database: nothing to save\n");
        } else {
            std::cout << "unknown command " << cmd << " (try .help)\n";
        }
    } catch (const querylab::QueryError& e) {
        std::cout << "Error: " << e.describe() << "\n";
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string dbPath, dataset, oneShot;
    std::size_t scale = 10000;
    Settings settings;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << a << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--db") dbPath = val();
        else if (a == "--dataset") dataset = val();
        else if (a == "--scale") scale = std::stoul(val());
        else if (a == "-c") oneShot = val();
        else if (a == "--rules") settings.showRules = true;
        else if (a == "--why") settings.showExplain = true;
        else if (a == "--no-plan") settings.showPlan = false;
        else if (a == "-h" || a == "--help") {
            std::cout << "usage: querylab_cli [--db FILE] [--dataset NAME --scale N] [-c SQL] [--rules] [--why] [--no-plan]\n";
            return 0;
        } else {
            std::cerr << "unknown option " << a << "\n";
            return 2;
        }
    }
    std::unique_ptr<Database> db;
    try {
        if (dbPath.empty()) db = Database::inMemory();
        else {
            std::FILE* f = std::fopen(dbPath.c_str(), "rb");
            const bool exists = f != nullptr;
            if (f) std::fclose(f);
            db = Database::openFile(dbPath, !exists);
        }
        if (!dataset.empty()) {
            auto j = db->loadDataset(dataset, scale);
            if (oneShot.empty()) std::cout << "loaded " << dataset << ": " << j["tables"].dump() << "\n";
        }
    } catch (const querylab::QueryError& e) {
        std::cerr << e.describe() << "\n";
        return 1;
    }
    if (!oneShot.empty()) {
        auto out = db->execute(oneShot);
        printResult(out, settings);
        return out["success"].get<bool>() ? 0 : 1;
    }
    std::cout << "QueryLab shell — " << (dbPath.empty() ? "in-memory database" : "file " + dbPath) << ". Type .help for commands.\n";
    std::string buffer, line;
    while (true) {
        std::cout << (buffer.empty() ? "querylab> " : "     ...> ") << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (buffer.empty() && !line.empty() && line[0] == '.') {
            if (!command(*db, line, settings)) break;
            continue;
        }
        buffer += line + "\n";
        if (line.find(';') == std::string::npos) continue;
        printResult(db->execute(buffer), settings);
        buffer.clear();
    }
    return 0;
}
