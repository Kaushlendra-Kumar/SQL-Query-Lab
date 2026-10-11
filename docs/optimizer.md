# Optimizer and cost model

The optimizer is rule-based with a transparent cost model. Rules rewrite the logical plan only when the rewrite cannot change the query's result; physical planning then chooses implementations by estimated cost. Every rule and decision is recorded in the trace returned with each query (`optimizer` field): rule name, whether it applied, the reason, details, the estimated plan cost before and after, before/after plan snapshots for logical rules, and the costed alternatives for physical decisions.

## Canonical logical plan

The binder produces the textbook translation, which the rules then improve:

```
Limit → Sort → Distinct → Project → Filter(HAVING) → Aggregate → Filter(WHERE) → Join tree → Scan …
```

## Logical rules (applied in this order)

### 1. ConstantFolding
Evaluates constant sub-expressions once at plan time: `age > 20 + 5` becomes `age > 25`. If evaluation raises an error (division by zero, overflow), the expression is left alone, so the error only happens if the expression is actually evaluated at run time.

### 2. FilterSimplification
Three-valued-logic identities that hold even when operands are NULL: `x AND TRUE = x`, `x AND FALSE = FALSE`, `x OR FALSE = x`, `x OR TRUE = TRUE`, `NOT NOT x = x`. Filters that are always TRUE are removed. A filter that can never be TRUE (`FALSE` or `NULL`) replaces its subtree with an `Empty` operator, and emptiness propagates upward through filters, projections, sorts, limits, DISTINCT, inner joins and grouped aggregates. A global aggregate over an empty input is kept (it must still return one row: `COUNT(*) = 0`).

### 3. PredicatePushdown
Splits filters into AND-ed conjuncts and moves each as far down as it can go:

- into a scan, where it is evaluated while pages are decoded (columns used by the filter are decoded first; other columns only for matching rows);
- into the matching side of an inner join;
- a WHERE conjunct that references both sides of an inner join is merged into the join condition (this turns `FROM a, b WHERE a.x = b.y` into a hash-joinable equi-join);
- ON-clause conjuncts that reference only the right side of a LEFT JOIN are pushed into the right input;
- WHERE conjuncts on the left side of a LEFT JOIN are pushed into the left input, but conjuncts on the right (null-extended) side stay above the join, because filtering below the join would change which rows are NULL-padded.

### 4. JoinReordering
For a tree of three or more inner-joined inputs, rebuilds a left-deep join order greedily: start with the input with the smallest estimated row count, then repeatedly add the input that keeps the intermediate result smallest, preferring inputs connected by a join condition to avoid cross products. The new order is kept only if its estimated cost is lower than the written order's; otherwise the trace says the original was kept. LEFT JOIN order is never changed. Two-input joins are not reordered logically; the hash-join build side is chosen during physical planning.

### 5. ProjectionPushdown
Computes the columns each operator actually needs and narrows every scan to them, so unused columns are never decoded (`COUNT(*)` reads no columns at all). DISTINCT keeps all of its input columns because it compares whole rows.

## Physical decisions

### AccessPathSelection
For every scan, the planner costs a sequential scan and each index on the table. An index is usable when the scan's conjuncts give equality on a prefix of its columns, optionally followed by one range condition (`<`, `<=`, `>`, `>=`, `BETWEEN` with constants) on the next column. Conjuncts answered by the index become the index condition; the rest stay as a residual filter. Index scans are not used for `IN`, `OR`, `LIKE`, `<>`, function calls or column-to-column comparisons, and never to satisfy ORDER BY or GROUP BY. When an index exists but is not used, the trace explains why (no usable predicate, or the predicate is not selective enough).

### JoinStrategy
For each join the planner costs a nested loop and, when the condition contains at least one `column = column` equality between the two inputs, a hash join with either input as the build side (only the right input for LEFT JOIN, which must probe with the left). Non-equality parts of the condition become a residual filter. The cheapest alternative wins; for tiny inputs that can legitimately be the nested loop.

## Statistics

Each column keeps, updated on every insert, update and delete:

- the number of distinct non-NULL values and the NULL count,
- minimum and maximum,
- an equi-depth histogram of up to 32 buckets (rebuilt lazily).

The exact value frequencies exist internally only to maintain these summaries. The estimator deliberately uses only the summaries, as a real system would:

| Predicate | Estimated selectivity |
|---|---|
| `col = const` | `1 / distinct` × non-NULL fraction; 0 if the constant is outside [min, max] |
| `col <> const` | `1 − 1/distinct` × non-NULL fraction |
| `col < const` etc., `BETWEEN` | histogram fraction (linear interpolation inside a numeric bucket, half a bucket otherwise) × non-NULL fraction |
| `col IS NULL` | NULL fraction |
| `col IN (c1, ..., cn)` | sum of the equality estimates, capped at 1 |
| `col LIKE 'abc'` (no wildcard) | as equality |
| `col LIKE 'abc%'` | 0.05 |
| other `LIKE` | 0.1 |
| `a.x = b.y` | `1 / max(distinct(a.x), distinct(b.y))` |
| `p AND q` | `s(p) × s(q)` (independence) |
| `p OR q` | `s(p) + s(q) − s(p)·s(q)` |
| `NOT p` | `1 − s(p)` |
| anything else | 1/3 |

Join output: `|L| × |R| × selectivity(condition)`, at least `|L|` for LEFT JOIN. Grouped aggregate output: product of the grouping columns' distinct counts, capped by input rows.

Because of the uniformity and independence assumptions, estimates can be far off on skewed or correlated data (the sample datasets are skewed on purpose). The UI flags operators whose estimate and actual row count differ by 2× or more, only for operators that ran to completion (an operator stopped early by LIMIT is not compared).

## Cost formulas

Costs are abstract units where reading one page sequentially costs 1.0. The constants (`CostParams` in `cost_model.h`) are modelled on PostgreSQL's planner constants. They rank alternatives; they do not predict milliseconds, and they were not calibrated against this engine's measured times.

| Operator | Cost |
|---|---|
| Seq Scan | `pages × 1.0 + rows × 0.01 × (0.5 + 0.5 × fraction of columns read) + rows × 0.0025 × predicate terms` |
| Index Scan | `(height + ⌈matches/32⌉) × 0.25 + matches × 0.005 + min(matches, pages) × 1.5 + matches × (0.02 + 0.01 × (0.5 + 0.5 × columns fraction)) + matches × 0.0025 × residual terms` |
| Hash Join | `build × 0.02 + probe × 0.01 + output × 0.01` |
| Nested Loop | `left × right × 0.0025 + output × 0.01` |
| Sort | `n log2 n × 0.005 × keys + n × 0.01` |
| Hash Aggregate | `n × (0.01 + 0.0025 × (group-by expressions + aggregates))` |
| Filter | `n × 0.0025 × terms` |
| Distinct | `n × 0.02` |

The index-scan formula assumes each matching row may sit on a different page (pessimistic for clustered data, realistic for the generated data), which is why a range that matches a large share of the table falls back to a sequential scan.

## Known limitations

- Single-column histograms only; no multi-column statistics, so correlated predicates are mis-estimated.
- No cost-based join enumeration beyond the greedy heuristic; no bushy trees.
- No index-only scans, index nested-loop joins, sort avoidance via index order, merge joins, or Top-N sort.
- HAVING conjuncts on grouping columns are not pushed below the aggregate.
- Constant folding does not simplify partially constant expressions such as `x * 1`.
