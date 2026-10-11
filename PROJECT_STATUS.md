# Project status

All eight milestones are implemented, compile without warnings (GCC 13 `-Wall -Wextra -Wpedantic -Wshadow`, Clang 18) and pass their tests.

| Milestone | Status | Acceptance check |
|---|---|---|
| 1. Foundation: CMake, typed values, lexer, parser, AST | Done | `querylab_cli` tokenizes and parses; lexer/parser/AST tests |
| 2. First working database: catalog, CREATE/INSERT, scans, WHERE, projection, LIMIT | Done | query tests |
| 3. Query planning: logical plans, rule-based optimizer, physical operators, statistics | Done | optimizer tests verify each rule's transformation and result equivalence |
| 4. Indexes and advanced SQL: B+ tree, index scans, ORDER BY, aggregates, GROUP BY, joins, costs | Done | plan changes from Seq Scan to Index Scan after CREATE INDEX (tested); randomized B+ tree tests; SQLite differential tests |
| 5. Backend API | Done | `server/tests/api_test.py` |
| 6. Visualizer | Done | UI connected to the real engine; Vitest renders a captured engine response |
| 7. Performance lab, advisor, explanations | Done | benchmark and advisor tests; index experiment exercised in the browser |
| 8. Persistence and polish | Done | persistence + corruption tests; ASan/UBSan clean; docs; `scripts/start.sh` |

## Build and test commands

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
ctest --test-dir build --output-on-failure                # C++ tests, SQLite difftest, API tests
(cd frontend && npm ci && npm test && npm run build)     # frontend tests and production build
scripts/sanitize.sh                                       # ASan + UBSan
scripts/start.sh --dataset ecommerce --scale 10000        # run everything on :8080
```

## Key decisions

- **Vendored single-header dependencies** (cpp-httplib, nlohmann/json, doctest) so the project builds offline on any platform. doctest replaces GoogleTest for that reason.
- **One `Expr` node type in the AST** with a kind tag keeps the parser, binder and serializer simple.
- **Column ids instead of names** after binding; ids are mapped to row slots once during physical planning.
- **In-memory B+ tree** with fanout 64, keyed on `(key, rid)` for exact duplicate handling. Indexes are rebuilt on open instead of being persisted: simpler, and the file format stays small; the cost is open time proportional to table size.
- **Statistics maintained incrementally** (exact frequencies internally) but the estimator only uses distinct counts, NULL counts, min/max and an equi-depth histogram, so estimation errors are realistic.
- **Volcano execution** with a non-virtual instrumentation wrapper; buffering operators (hash join build, sort, aggregate) materialize, and the replay is labelled as a replay of recorded events.
- **Checkpoint after each modifying statement** in file mode; no WAL, so durability against crashes is explicitly not claimed.
- **Inspection without side effects**: the storage explorer peeks at pages without touching the buffer pool.

## Known limitations / possible next steps

- Subqueries, `INSERT ... SELECT`, RIGHT/FULL joins, column constraints and transactions are not implemented.
- No WAL or crash recovery; deleted space inside pages is not reclaimed; pages of dropped tables are not reused.
- Index scans do not satisfy ORDER BY / GROUP BY and there is no index nested-loop join; the advisor states this rather than recommending such indexes.
- Join ordering is greedy and left-deep only.
- Windows support is written for (MSVC-specific code paths, `start.ps1`) but has not been built in this environment.
- Possible extensions: Top-N sort for ORDER BY + LIMIT, index-only scans, multi-column statistics, a WAL, live (streamed) execution events.
