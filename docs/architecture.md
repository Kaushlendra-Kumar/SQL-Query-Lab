# Architecture

QueryLab has three parts with one-way dependencies:

```
frontend (React)  ──HTTP/JSON──▶  server (cpp-httplib)  ──▶  engine (C++20 library)
                                                              ▲
                                       querylab_cli ──────────┘
```

The engine knows nothing about HTTP or the UI. The server is a thin adapter that validates requests and calls `api::Database`. The CLI uses the same facade.

## Engine modules

| Module | Responsibility | Key types |
|---|---|---|
| `common` | typed values, total ordering, errors with source spans | `Value`, `DataType`, `QueryError`, `SourceSpan` |
| `sql` | lexer, recursive-descent parser, AST | `Token`, `Expr`, `Statement` |
| `storage` | 4 KB slotted pages, row codec, page stores, buffer pool, heap files | `Page`, `BufferPool`, `PageGuard`, `TableHeap`, `Rid` |
| `index` | B+ tree over composite keys | `BPlusTree`, `Cursor` |
| `catalog` | tables, indexes, per-column statistics; keeps heap, indexes and stats consistent | `Catalog`, `TableInfo`, `IndexInfo`, `ColumnStats` |
| `planner` | binder (names, types, aggregates), bound expressions and evaluation, logical plan, physical planner | `Binder`, `BoundExpr`, `LogicalNode`, `PhysicalPlanner`, `PhysicalNode` |
| `optimizer` | rule-based rewrites and the cost model | `Optimizer`, `CostModel`, `OptimizerStep` |
| `execution` | volcano operators with instrumentation, limits, event recording | `ExecContext`, `OperatorStats`, `ExecEvent` |
| `explain` | explicit JSON DTO serializers, plain-language explanation, index advisor | `logicalToJson`, `physicalToJson`, `explainExecution`, `adviseIndexes` |
| `api` | `Database` facade, persistence, datasets, benchmarks | `Database`, `QueryOptions` |

## Query lifecycle

1. **Lexer** (`sql/lexer.cpp`) produces tokens with byte offsets, line and column. Unterminated strings or comments, malformed numbers and stray characters raise `QueryError(Lexer)` at the exact location. `''` inside strings and `""` inside quoted identifiers are unescaped. Unquoted identifiers are case-insensitive.
2. **Parser** (`sql/parser.cpp`) is recursive descent with explicit precedence levels: `OR < AND < NOT < comparison/IS/BETWEEN/IN/LIKE < + - || < * / % < unary`. Every AST node keeps its source span; the AST viewer highlights it. Nesting depth is capped at 200 to keep the parser's stack bounded.
3. **Binder** (`planner/binder.cpp`) resolves tables, aliases and columns (reporting ambiguous and unknown names), checks types, validates aggregate usage (GROUP BY rules, no aggregates in WHERE), expands `*`, resolves ORDER BY aliases/positions/hidden expressions, and produces the canonical logical plan. Every column gets a query-wide integer id; expressions refer to ids, never names.
4. **Optimizer** (`optimizer/optimizer.cpp`) applies five rules in order and records a trace entry for each (see [optimizer.md](optimizer.md)).
5. **Physical planner** (`planner/physical_plan.cpp`) picks sequential or index scans, hash or nested-loop joins and hash-join build sides by cost, and resolves column ids to row positions once.
6. **Executor** (`execution/executor.cpp`) builds operators and pulls rows with `open / next / close`.

Each stage is timed separately; the timings form the pipeline timeline in the UI.

## Storage

### Pages

All table data lives in fixed 4,096-byte slotted pages:

```
 0        4          8           10           12      16
 | pageId | checksum | slotCount | dataStart  | (res) | slot directory → ...      ... ← row data |
                                                        {u16 offset, u16 length}
```

The slot directory grows from the front and row images from the back. A slot with length 0 is a tombstone, so a row id `(page, slot)` stays valid until that row is deleted. Updates are written in place when the new image fits, otherwise the row moves and every index is updated with its new RID. Deleted space is not compacted (a documented simplification).

Row images encode each value as a one-byte tag plus payload (8-byte little-endian integers and IEEE doubles, length-prefixed UTF-8 text). The codec can skip columns without allocating them, which scans use for late materialization: columns used by the scan's filter are decoded first, the rest only for rows that pass.

### Buffer pool

`BufferPool` is a bounded LRU cache of page frames (default 256 pages, 1 MiB) in front of a `PageStore`. `PageGuard` pins a frame for as long as it is held (RAII), so pinned pages are never evicted. Dirty frames are written back on eviction and on checkpoint. Logical reads, hits, misses, evictions and write-backs are counted; scan operators attribute their own page reads and misses.

In memory mode the store is a `MemoryPageStore`: a miss copies the page into a frame, so misses are real events but cost a `memcpy`, not disk I/O. Cold-cache experiments empty QueryLab's pool; they cannot empty the operating system's file cache.

### Persistence (file mode)

`--db FILE` uses a `FilePageStore`: page *N* is at byte offset *N* × 4096.

- Page 0 is the header: magic `QLABDB\0\1`, format version, page size, first catalog page, catalog byte length, page count.
- The catalog (tables, columns, page lists, row counts, index definitions) is JSON stored in a chain of catalog pages (`next` pointer, length, payload).
- Every page written to the file carries an FNV-1a checksum and its own page id; reads verify both. Bad magic, a version mismatch, a truncated file, a broken catalog chain or a checksum mismatch are reported as storage errors.
- Indexes are not stored; they are rebuilt from the heap when the file is opened. Column statistics are recomputed the same way.
- The engine checkpoints after every statement that changes data or schema: dirty pages are flushed, the catalog chain rewritten, and the header written last.

This is **not** crash-safe. There is no write-ahead log, so a crash in the middle of a checkpoint can leave a file whose pages and catalog disagree; checksums will usually detect the damage but cannot repair it. Statements are atomic only against errors raised while they run (an in-memory undo log rolls back a failing INSERT, UPDATE or DELETE), not against crashes. There are no transactions and no concurrency control: the facade serializes statements with a mutex.

## B+ tree

`index::BPlusTree` stores `(key, rid)` entries with keys being rows of one or more values, fanout 64. Ordering on the full `(key, rid)` pair makes every entry unique, so duplicate keys need no special leaf structure and deleting one duplicate is exact. Leaves are linked for range scans. `seek(prefix, inclusive)` descends to the first entry whose key prefix is ≥ (or >) the bound; the index scan then walks the leaf chain until the equality prefix or upper bound no longer matches. Deletion does not rebalance (underfull nodes are allowed); `validate()` checks ordering, separator bounds, uniform leaf depth, leaf links and size, and randomized tests exercise it.

Unique indexes reject a second non-NULL key; multiple NULLs are allowed, as in standard SQL.

## Execution model

Operators implement the volcano iterator interface. A non-virtual wrapper around each operator's `open/next/close` measures inclusive time with `steady_clock`, counts calls and rows, records events, and checks the time limit and cancellation flag every 256 calls (nested loops check every 4,096 comparisons). Self time is inclusive time minus the children's inclusive time.

| Operator | Notes |
|---|---|
| Seq Scan | page at a time, predicate evaluated during decoding |
| Index Scan | B+ tree seek, then one heap fetch per matching entry; residual filter afterwards |
| Filter, Project, Limit | streaming; Limit stops pulling once satisfied |
| Nested Loop Join | buffers the right input; any condition; LEFT JOIN pads with NULLs |
| Hash Join | builds on the right input (the planner swaps inputs to build on the smaller side for inner joins); NULL keys never match |
| Hash Aggregate | one hash entry per group; COUNT(*) over no rows still yields one row |
| Hash Distinct | streaming with a hash set |
| Sort | buffers everything, stable sort, NULLs first ascending |
| Update / Delete | collect target RIDs first (avoids the Halloween problem), then modify heap, indexes and statistics, with undo on failure |

Memory buffered by joins, sorts, aggregates and DISTINCT is estimated and capped (default 512 MiB). Results beyond `maxResultRows` are counted but not returned.

### Instrumentation and its limits

Timers add two clock reads per operator call. The engine measures the cost of a clock read at start-up (typically 20–40 ns) and reports the number of reads and the estimated overhead with every result. Very short queries are dominated by this overhead and by scheduling noise; the benchmark endpoint switches per-operator timers off and reports medians and interquartile ranges over repeated runs. All reported times are engine time only: parsing and planning are reported separately, and JSON encoding, HTTP and rendering are not included.

### Execution events

The executor records `open`, `row`, `done`, `close`, `build_done`, `sort_done` and `aggregate_done` events with timestamps, in the order they happened. Row events are capped (4,000 by default); lifecycle events are always kept. The UI replays these events after the query finishes and says so.

## Complexity of major components

| Component | Cost |
|---|---|
| Lexing, parsing | O(n) in the SQL length |
| Binding | O(expression size × columns in scope) |
| B+ tree insert / seek | O(log_f N) node visits, f = 64; range scan adds O(k) for k results |
| Seq Scan | O(pages + rows) |
| Index Scan | O(log N + k) index work plus k heap page fetches |
| Hash Join | O(build + probe + output) expected, O(build) memory |
| Nested Loop Join | O(left × right) comparisons, O(right) memory |
| Sort | O(n log n), O(n) memory |
| Hash Aggregate / Distinct | O(n) expected, O(groups) memory |
| Statistics maintenance | O(log d) per value per insert/delete (d distinct values) |
| Histogram rebuild | O(d), lazily after changes |

## API layer

`api::Database` serializes statements with a recursive mutex and returns explicit JSON DTOs built by the `explain` serializers; internal objects are never dumped generically. `cancel()` flips an atomic flag checked by running operators, so it works from another thread while a query holds the mutex. See [api.md](api.md).
