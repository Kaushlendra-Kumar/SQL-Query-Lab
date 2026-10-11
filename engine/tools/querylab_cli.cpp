#include <iostream>
#include <string>
#include "querylab/explain/ast_json.h"
#include "querylab/sql/parser.h"
int main(int argc, char** argv) {
    std::string sql = argc > 1 ? argv[1] : "SELECT name FROM users WHERE age > 25";
    try {
        for (const auto& t : querylab::sql::tokenize(sql))
            std::cout << querylab::sql::tokenTypeName(t.type) << "\t" << t.lexeme << "\n";
        auto st = querylab::sql::parseStatement(sql);
        std::cout << querylab::explain::astToText(querylab::explain::statementToJson(st));
    } catch (const querylab::QueryError& e) { std::cerr << e.describe() << "\n"; return 1; }
}
