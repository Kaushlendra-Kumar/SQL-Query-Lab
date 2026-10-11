# QueryLab

QueryLab is a small relational database written from scratch in C++20, with a web interface that shows what the engine does with every query: the tokens, the syntax tree, the logical plan before and after each optimizer rule, the physical operators it chose, and what each operator actually did when it ran.

Nothing in the interface is mocked. Plans, row counts, page reads, timings, execution events, explanations and index suggestions all come from the engine's response for the query you just ran.

```
SQL text ─▶ Lexer ─▶ Parser ─▶ Binder ─▶ Optimizer ─▶ Physical planner ─▶ Executor
             tokens    AST     typed       rule-based     scan / join          volcano
                               logical     rewrites +     choices,             operators over
                               plan        cost model     costed               slotted pages + B+ trees
```

## Quick start

### Linux / macOS

Requirements: CMake 3.20+, a C++20 compiler (GCC 11+ or Clang 14+), Node.js 18+ for the web UI. Python 3 and SQLite development headers are optional (extra test suites).

```bash
scripts/start.sh                                    # builds everything on first run, then serves http://127.0.0.1:8080
scripts/start.sh --dataset ecommerce --scale 10000  # start with sample data loaded
scripts/start.sh --db shop.qldb                     # persistent database file instead of memory
```

On Ubuntu: `sudo apt install build-essential cmake nodejs npm` (and optionally `libsqlite3-dev python3`).

### Windows

Install Visual Studio 2022 Build Tools (C++ workload), CMake and Node.js 18+, then in PowerShell:

```powershell
.\scripts\start.ps1
.\scripts\start.ps1 --dataset ecommerce --scale 10000
```

The code is written to be portable (MSVC paths for overflow checks, `ws2_32` for sockets), but it was developed and tested on Linux with GCC 13 and Clang 18; the Windows build has not been run in this environment.

### Without the web UI

The engine is a standalone library with its own shell:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
build/engine/querylab_cli --dataset ecommerce --scale 10000 --rules --why \
  -c "SELECT * FROM orders WHERE customer_id = 42"
build/engine/querylab_cli          # interactive; type .help
```

### Development mode

`scripts/dev.sh` runs the C++ server on :8080 and the Vite dev server on :5173 with hot reload (`/api` is proxied).

## What you can do

- **Editor**: write SQL (Monaco, with table/column completion), run it or plan it without running, and see where in the pipeline it failed. Errors carry line and column and are marked in the editor.
- **Visual Explain**: interactive graphs of the physical plan, the logical plan (before and after optimization) and the syntax tree. Pipes between operators are sized by row count: the faint pipe is the estimate, the coloured one is what actually flowed. Click an operator for its estimated vs actual rows, cost, time, pages, index work, schemas and the optimizer decisions that shaped it. Click a token in the SQL to find its syntax-tree node.
- **Step-through execution**: replay the events the executor recorded on its real execution path (open, each row, done, hash table built, sort finished, close). The replay is labelled as such; it is not live streaming.
- **Optimizer**: every rule in order, whether it applied, why, the cost before and after, and side-by-side plans. Physical decisions list each alternative the planner costed.
- **Storage Explorer**: page maps with fill level and buffer-pool residency, slot-level page contents, buffer-pool statistics, per-column statistics with equi-depth histograms, and B+ tree structure. Inspection reads pages without touching the buffer pool, so looking never changes an experiment.
- **Benchmarks (performance lab)**: compare two queries or two strategies (index scans off, hash joins off, cold buffer pool) over repeated runs with medians and interquartile ranges, or run a guided index experiment: measure, create the index, measure again, compare.
- **Explanation and index advisor**: a plain-language account of the run built only from engine metadata, with general guidance labelled separately, and index suggestions that show their evidence, estimated benefit, write overhead, limitations and how to verify them.

## Sample data

Three deterministic datasets (same seed, same rows on every platform), each with a configurable size:

| Dataset | Tables | Scale controls |
|---|---|---|
| E-commerce | customers, products, orders, order_items | number of orders |
| Social network | users, posts, follows, comments | number of posts |
| Employee analytics | departments, employees, salaries | number of employees |

The data is deliberately skewed (popular customers, heavy-tailed likes, a few NULLs) so estimates and actuals differ in instructive ways. Practical scale is up to a few hundred thousand rows per table; see [docs/benchmarks.md](docs/benchmarks.md). QueryLab is an educational engine and makes no production-scale promises.

## SQL supported

`CREATE TABLE`, `CREATE [UNIQUE] INDEX` (composite), `DROP TABLE`, `DROP INDEX`, `INSERT ... VALUES`, `UPDATE`, `DELETE`, `EXPLAIN`, and `SELECT` with `DISTINCT`, `WHERE`, `AND/OR/NOT`, comparisons, `IS [NOT] NULL`, `[NOT] BETWEEN`, `[NOT] IN (list)`, `[NOT] LIKE`, arithmetic, `||`, `INNER JOIN`, `LEFT JOIN`, comma cross joins, aliases, `GROUP BY`, `HAVING`, `ORDER BY` (columns, aliases, positions, expressions), `LIMIT`, `OFFSET`, aggregates `COUNT/SUM/AVG/MIN/MAX` (with `DISTINCT`) and scalar functions `LOWER, UPPER, LENGTH, ABS, ROUND, COALESCE`. Types: `INTEGER`, `FLOAT`, `TEXT`, `BOOLEAN`, `NULL`, with SQL three-valued logic.

Not supported: subqueries, `INSERT ... SELECT`, `RIGHT/FULL` joins, constraints other than unique indexes, transactions. Full details: [docs/sql-dialect.md](docs/sql-dialect.md).

## Tests

```bash
scripts/test.sh        # C++ tests, SQLite differential tests, HTTP API tests, frontend tests
scripts/sanitize.sh    # C++ tests under AddressSanitizer + UndefinedBehaviorSanitizer
```

| Suite | What it covers |
|---|---|
| `querylab_tests` (doctest, 45 cases, ~2,000 assertions) | lexer, parser and precedence, AST, binder errors, NULL semantics, every SQL feature, joins, aggregates, UPDATE/DELETE with index maintenance, statement atomicity, slotted pages, buffer pool, randomized B+ tree tests, statistics, every optimizer rule (transformation and result equivalence), index selection, persistence and corruption detection, limits, benchmarks, advisor |
| `querylab_difftest` (needs SQLite) | ~1,900 random queries (predicates, ORDER BY/LIMIT, DISTINCT, GROUP BY/HAVING, inner/left/cross joins, with and without indexes) compared with SQLite |
| `server/tests/api_test.py` (needs Python 3) | every endpoint, malformed JSON, invalid options, oversized SQL, result truncation, CORS, validation |
| `frontend` (Vitest) | renders a real engine response: plan graph, inspector, explanation, replay |

## Project layout

```
engine/      C++ engine (library + CLI + tests)
  include/querylab/{common,sql,catalog,storage,index,planner,optimizer,execution,explain,api}
  src/...    one directory per module
  tests/     doctest suites; tests/differential/ compares with SQLite
server/      HTTP server (cpp-httplib) exposing /api/v1
frontend/    React + TypeScript + Vite + Tailwind + React Flow + Monaco + Recharts
third_party/ vendored single headers: cpp-httplib, nlohmann/json, doctest (licences included)
docs/        architecture, SQL dialect, optimizer and cost model, API, benchmarks
examples/    SQL walkthroughs
scripts/     build, start (Linux/macOS/Windows), dev, test, sanitize
```

## Documentation

- [Architecture](docs/architecture.md): modules, data flow, storage format, execution model
- [SQL dialect](docs/sql-dialect.md): exact syntax and semantics
- [Optimizer and cost model](docs/optimizer.md): rules, statistics, formulas, known limitations
- [HTTP API](docs/api.md): endpoints and response DTOs
- [Benchmarks](docs/benchmarks.md): methodology and measured baselines
- [Project status](PROJECT_STATUS.md): what is complete, decisions, next steps

## Security note

The server binds to 127.0.0.1 by default and has no authentication. Anyone who can reach it can run SQL, so do not expose it on a network you do not trust. Request bodies are capped at 1 MiB and SQL text at 200 KiB; queries are bounded by a time limit and a buffered-memory limit.
