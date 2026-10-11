# QueryLab SQL dialect

Everything listed here is implemented and tested. Anything not listed is not supported and produces a clear error rather than being silently ignored.

## Lexical rules

- Keywords and unquoted identifiers are case-insensitive; identifiers are folded to lower case. Double-quoted identifiers (`"Order Date"`) keep their case and may be reserved words. `""` inside a quoted identifier is a literal quote.
- String literals use single quotes; `''` is an escaped quote: `'it''s'`.
- Numbers: `42` is INTEGER (64-bit; out-of-range literals are an error), `4.2`, `.5`, `1e3`, `2.5E-3` are FLOAT.
- Comments: `-- to end of line` and `/* block */`.
- Statements are separated by `;`. A script stops at the first failing statement.

## Types

| Type | Accepted names | Notes |
|---|---|---|
| INTEGER | `INTEGER`, `INT`, `BIGINT` | 64-bit signed; arithmetic overflow is an error |
| FLOAT | `FLOAT`, `REAL`, `DOUBLE` | IEEE double |
| TEXT | `TEXT`, `VARCHAR`, `VARCHAR(n)` | UTF-8; the length in `VARCHAR(n)` is accepted but not enforced |
| BOOLEAN | `BOOLEAN`, `BOOL` | `TRUE`, `FALSE` |
| NULL | | any column may hold NULL |

INTEGER and FLOAT compare and combine with each other. Storing an INTEGER into a FLOAT column converts it; storing a FLOAT into an INTEGER column is allowed only when it is integral (`2.0`). Other cross-type comparisons (`TEXT = INTEGER`) are type errors at bind time.

## Statements

```sql
CREATE TABLE [IF NOT EXISTS] name (col type, ...)
DROP TABLE [IF EXISTS] name
CREATE [UNIQUE] INDEX [IF NOT EXISTS] name ON table (col [, col ...])
DROP INDEX [IF EXISTS] name
INSERT INTO table [(col, ...)] VALUES (expr, ...) [, (...)]
UPDATE table SET col = expr [, ...] [WHERE condition]
DELETE FROM table [WHERE condition]
EXPLAIN statement            -- plan only (SELECT, UPDATE, DELETE)
SELECT ...
```

- Column constraints (`PRIMARY KEY`, `NOT NULL`, `DEFAULT`, ...) are not supported; use a `UNIQUE INDEX` for uniqueness. Unique indexes allow any number of NULLs.
- `INSERT` values must be constant expressions. Omitted columns are NULL. `INSERT ... SELECT` is not supported.
- `INSERT`, `UPDATE` and `DELETE` are atomic with respect to errors during the statement (for example a unique-key violation on the fifth row undoes the first four). There are no multi-statement transactions.

## SELECT

```sql
SELECT [DISTINCT] item [, item ...]
[FROM table [AS] alias
   [ [INNER] JOIN table [AS] alias ON condition
   | LEFT [OUTER] JOIN table [AS] alias ON condition
   | , table [AS] alias ] ...]
[WHERE condition]
[GROUP BY expr [, ...]]
[HAVING condition]
[ORDER BY expr [ASC | DESC] [, ...]]
[LIMIT n] [OFFSET n]
```

- Select items: expressions with optional `AS alias` (or a bare alias), `*` and `t.*`.
- `SELECT` without `FROM` evaluates expressions once (`SELECT 1 + 1`).
- `GROUP BY` accepts expressions, output aliases and positions (`GROUP BY 1`). With aggregation, every selected column must be grouped or inside an aggregate.
- `ORDER BY` accepts output aliases, positions and arbitrary expressions (including ones not selected, except with `DISTINCT`). NULL sorts as the smallest value: first in ascending order, last in descending order (as in SQLite).
- `LIMIT` and `OFFSET` take non-negative integer literals.
- A comma join without a WHERE condition is a cross join; an equality in WHERE between the two tables becomes the join condition during optimization.
- `RIGHT JOIN`, `FULL JOIN`, `USING`, `NATURAL`, subqueries, `UNION`, window functions and CTEs are not supported.

## Expressions and operators

Precedence, lowest to highest:

| Level | Operators |
|---|---|
| 1 | `OR` |
| 2 | `AND` |
| 3 | `NOT` |
| 4 | `=  <>  !=  <  <=  >  >=`, `IS [NOT] NULL`, `[NOT] BETWEEN a AND b`, `[NOT] IN (list)`, `[NOT] LIKE pattern` |
| 5 | `+  -  \|\|` |
| 6 | `*  /  %` |
| 7 | unary `-`, `+` |

Comparisons do not chain (`1 < 2 < 3` is a syntax error).

- Integer `/` truncates toward zero; `%` is the remainder. Division or modulo by zero is a runtime error.
- `||` concatenates the text form of both operands; NULL if either is NULL.
- `LIKE` supports `%` and `_` and is case-sensitive (SQLite's is case-insensitive for ASCII).
- `IN` takes a list of expressions, not a subquery.

### NULL semantics (three-valued logic)

- Comparisons and arithmetic with NULL yield NULL.
- `AND`: FALSE if either side is FALSE, else NULL if either is NULL, else TRUE. `OR` is the dual.
- `NOT NULL` is NULL.
- `x IN (a, b)` is TRUE on a match, otherwise NULL if any list item (or x) is NULL, otherwise FALSE. `NOT IN` negates that, so `3 NOT IN (1, NULL)` is NULL.
- `WHERE`, `HAVING` and `ON` keep a row only when the condition is TRUE.
- Aggregates ignore NULL inputs; `COUNT(*)` counts rows. Over zero rows, `COUNT` returns 0 and `SUM/AVG/MIN/MAX` return NULL. GROUP BY puts all NULLs in one group; DISTINCT treats NULLs as equal.

## Functions

| Function | Result |
|---|---|
| `COUNT(*)`, `COUNT(x)`, `COUNT(DISTINCT x)` | INTEGER |
| `SUM(x)`, `SUM(DISTINCT x)` | INTEGER for integer input (overflow is an error), else FLOAT |
| `AVG(x)`, `AVG(DISTINCT x)` | FLOAT |
| `MIN(x)`, `MAX(x)` | type of x; works on any type |
| `LOWER(t)`, `UPPER(t)` | TEXT (ASCII case mapping) |
| `LENGTH(t)` | INTEGER, bytes |
| `ABS(n)` | type of n |
| `ROUND(n [, digits])` | FLOAT, half away from zero |
| `COALESCE(a, b, ...)` | first non-NULL argument |

## Differences from SQLite seen in differential tests

The SQLite differential suite avoids constructs where the dialects intentionally differ: division by zero (SQLite returns NULL, QueryLab raises an error), `LIKE` case sensitivity, and integer overflow (SQLite converts to floating point). Everything else in the shared subset produced identical results across roughly 1,900 random queries.
