# Benchmarks

## Methodology

The performance lab and `POST /api/v1/benchmarks` follow the same rules:

- Every variant runs on the same in-process data with the same settings, one after another.
- Warm-up runs are discarded; the remaining runs are reported individually and summarized as min, p25, median, p75, max, mean, standard deviation and coefficient of variation. The UI calls one variant faster only when the interquartile ranges do not overlap.
- Timing covers engine execution only. Parsing and planning are reported separately; JSON encoding, HTTP and rendering are excluded. Per-operator timers are switched off to avoid their overhead (about 20–40 ns per clock read, measured at start-up and reported).
- Cold-cache runs empty QueryLab's buffer pool before each iteration. In memory mode a miss is a memory copy; in file mode the operating system's file cache is not controlled, so "cold" means cold for QueryLab only.
- Work counters (rows examined, page reads, misses, index entries) are exact and deterministic. At small scales they are a better guide than milliseconds.

No benchmark result is ever invented: every number in the UI comes from a run on your machine.

## Baseline measurements

Measured on the development machine (2 vCPUs, Intel Xeon @ 2.80 GHz, Linux, GCC 13, Release build), e-commerce dataset, in-memory mode, default 256-page buffer pool, 30 timed iterations after 3 warm-ups. Your numbers will differ; rerun them with the lab.

Point lookup `SELECT * FROM orders WHERE customer_id = 42` with an index on `orders(customer_id)`:

| Orders | Plan | Median | p25–p75 | Rows examined | Page reads |
|---|---|---|---|---|---|
| 1,000 | Index Scan | 0.013 ms | 0.011–0.013 | 13 | 13 |
| 1,000 | Seq Scan (index disabled) | 0.122 ms | 0.119–0.128 | 1,000 | 15 |
| 10,000 | Index Scan | 0.023 ms | 0.021–0.028 | 19 | 19 |
| 10,000 | Seq Scan (index disabled) | 1.41 ms | 1.31–1.56 | 10,000 | 147 |
| 100,000 | Index Scan | 0.039 ms | 0.034–0.045 | 51 | 51 |
| 100,000 | Seq Scan (index disabled) | 16.2 ms | 14.3–23.4 | 100,000 | 1,469 |

The index scan's work grows with the number of matches (plus a three-level tree descent); the sequential scan's grows with the table.

## A measured optimization

Join-and-aggregate query `SELECT c.city, COUNT(*), SUM(o.total) FROM orders o JOIN customers c ON o.customer_id = c.id GROUP BY c.city`, 15 timed iterations after 2 warm-ups.

The first version of the hash join, nested-loop join and hash aggregate allocated a new row vector for every candidate pair and every probe or group key. They now reuse per-operator buffers (`concatInto`, `keyInto`).

| Orders | Join | Before (median, p25–p75) | After (median, p25–p75) | Change |
|---|---|---|---|---|
| 1,000 | Hash Join | 0.506 ms (0.482–0.553) | 0.450 ms (0.416–0.520) | −11%, ranges overlap |
| 10,000 | Hash Join | 8.87 ms (8.74–8.99) | 7.37 ms (7.14–7.48) | −17% |
| 100,000 | Hash Join | 87.7 ms (85.6–104.0) | 67.7 ms (66.5–70.4) | −23% |
| 1,000 | Nested Loop | 15.8 ms (13.8–18.0) | 9.53 ms (9.14–10.07) | −40% |
| 10,000 | Nested Loop | 1,457 ms (1,315–1,575) | 937 ms (909–1,019) | −36% |

Row counts, rows examined and page reads were identical before and after. At 1,000 orders the hash-join difference is within noise.

## Supported scale

Everything runs comfortably up to a few hundred thousand rows per table: the 100,000-order e-commerce dataset (about 360,000 rows in total) loads in under a second in a Release build. Statistics keep exact value counts per column, so memory grows with the number of distinct values; with very large tables (millions of rows) these statistics, the in-memory B+ trees and the per-query memory cap (512 MiB of buffered rows by default) become the limits. QueryLab is a teaching engine: it is single-threaded, has no vectorized execution and makes no claim about production performance.
