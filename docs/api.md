# HTTP API (v1)

Base URL: `http://127.0.0.1:8080/api/v1`. All bodies are JSON. The server listens on loopback by default and has no authentication.

## Conventions

- Malformed requests (bad JSON, wrong field types, out-of-range options) return **400** with `{success: false, error: {stage: "api", message, display}}`.
- Unknown tables, indexes or endpoints return **404**; bodies over 1 MiB or SQL over 200 KiB return **413**.
- A query that fails inside the engine is not an HTTP error: `/query` returns **200** with `success: false` and a structured error `{stage, message, display, span?}`, where `stage` is one of `lexer, parser, binder, catalog, optimizer, planner, executor, storage` and `span` is `{begin, end, line, column}` into the submitted SQL.
- Unexpected internal failures return **500**; the server keeps running.

## Endpoints

| Method | Path | Purpose |
|---|---|---|
| GET | `/health` | `{status, engine, version, storage}` |
| POST | `/query` | Execute a script: `{sql, options?}` |
| POST | `/explain` | Plan every statement without executing: `{sql, options?}` |
| POST | `/parse` | Tokens and ASTs only (no catalog access), for live editor feedback |
| GET | `/catalog` | Tables, columns with statistics, indexes |
| GET | `/tables/{t}/schema` | Columns, statistics and histograms |
| GET | `/tables/{t}/rows?offset&limit` | Stored rows with their RIDs (limit ≤ 1000) |
| GET | `/tables/{t}/pages/{i}` | Slot-level contents of the table's i-th page |
| GET | `/indexes` | All indexes |
| GET | `/indexes/{name}` | B+ tree structure summary |
| POST | `/indexes` | `{name, table, columns[], unique?}`; identifiers validated against `[A-Za-z_][A-Za-z0-9_]*` |
| DELETE | `/indexes/{name}` | Drop an index |
| GET | `/storage` | Buffer pool and page-level storage summary |
| GET | `/datasets` | Available sample datasets and their sample queries |
| POST | `/datasets/{name}/load` | `{scale}` (10 to 1,000,000) |
| POST | `/reset` | Drop every table |
| POST | `/benchmarks` | Repeated timed runs, see below |
| GET | `/history` | Recent successful SELECTs (most recent first) |
| GET / PUT | `/settings` | `bufferPoolPages` (4–262144), `timeoutMs` (100–600000), `maxResultRows` (1–100000), `maxEvents` (0–50000), `maxMemoryMB` (16–8192) |
| POST | `/cancel` | Stop the running statement (does not wait for the engine lock) |

Inspection endpoints (`/storage`, `/tables/.../pages`, `/tables/.../rows`) read pages without loading them into the buffer pool and do not change its statistics.

### Query options

```json
{ "timing": true, "recordEvents": true, "disableIndexScans": false,
  "disableHashJoin": false, "coldCache": false, "maxRows": 0 }
```

`maxRows` (0–100000, 0 = server setting) caps returned rows; the engine still counts every row.

### Query response

```jsonc
{
  "success": true,
  "error": { ... },                // first failing statement, if any
  "results": [ {                   // one per executed statement, stops at the first failure
    "queryId": 7, "index": 0, "kind": "SELECT", "sql": "...", "span": {...}, "explain": false,
    "success": true,
    "timeline": [ {"stage": "lexer", "ms": 0.02, "ok": true}, ... ],    // lexer, parser, binder, optimizer, planner, executor
    "tokens": [ {"type", "lexeme", "value", "span"} ],
    "ast": { "id", "type", "label", "span", "details", "children" },
    "logicalPlan":   PlanNode,    // as bound, before optimization, with estimates
    "optimizedPlan": PlanNode,    // after the logical rules
    "physicalPlan":  PlanNode,    // operators that ran, with "actual" statistics
    "optimizer": [ { "rule", "phase", "applied", "reason", "details", "costBefore", "costAfter",
                     "alternatives": [ {"name", "cost", "chosen", "detail"} ], "nodeIds",
                     "before"?: PlanNode, "after"?: PlanNode } ],
    "columns": [ {"name", "type"} ], "rows": [[...]], "rowCount": 19, "truncated": false,
    "affectedRows": 0, "message"?: "...",
    "metrics": { "executionMs", "planningMs", "rowsScanned", "rowsReturned", "pagesRead", "pageMisses",
                 "indexEntriesExamined", "bufferPool": {...}, "timerCalls", "timerOverheadNs",
                 "estimatedTimerOverheadMs", "coldCache", "timing", "storage" },
    "events": { "mode": "recorded", "note", "events": [ {"seq", "nodeId", "type", "rows", "tMs"} ], "truncated" },
    "explanation": { "summary", "facts": [ {"topic", "text", "nodeId"?} ], "guidance": [], "estimateWarnings": [], "hotspot" },
    "advisor": { "suggestions": [ {"table", "columns", "definition", "evidence", "expectedBenefit",
                                   "estimatedCost": {"current", "withIndex"}, "overhead", "limitations", "verify"} ],
                 "notes": [ {"kind", "nodeId", "text"} ] }
  } ]
}
```

`PlanNode`:

```jsonc
{ "id", "logicalId"?, "kind": "logical" | "physical", "op", "label", "description",
  "details": { ... operator specific: table, columnsRead, filter, selectivityEstimate, index, indexCondition, hashKeys, ... },
  "output": [ {"id", "name", "type", "qualifier"?, "source"?} ],
  "estimates": { "rows", "cost", "selfCost" },
  "actual"?: { "executed", "completed", "rowsIn", "rowsOut", "rowsFiltered", "rowsScanned", "pagesRead", "pageMisses",
               "indexNodesVisited", "indexEntriesExamined", "nextCalls", "buildRows", "groups",
               "timeMs", "selfTimeMs", "peakMemoryBytes" },
  "notes": [], "children": [] }
```

Estimated values live under `estimates` and measured values under `actual`; they are never mixed. `completed: false` means a parent stopped pulling rows early (for example under LIMIT).

### Benchmarks

```json
POST /benchmarks
{ "iterations": 15, "warmup": 2,
  "variants": [ {"label": "A", "sql": "SELECT ...", "disableIndexScans": false, "coldCache": false, "disableHashJoin": false},
                {"label": "B", "sql": "SELECT ...", "disableIndexScans": true} ] }
```

Each variant must be a single SELECT (1 to 4 variants, 1–500 iterations, 0–50 warm-up runs, about 30 seconds of total budget). Per-operator timers and event recording are off. The response gives every sample, min / p25 / median / p75 / max / mean / standard deviation / coefficient of variation, rows returned and examined, page reads, average buffer-pool misses, index entries examined, planning time and the plan of the last run. Benchmark runs are not added to the query history.
