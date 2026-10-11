-- Three-valued logic: WHERE keeps a row only when the condition is TRUE.
CREATE TABLE t (x INTEGER, label TEXT);
INSERT INTO t VALUES (1, 'one'), (NULL, 'unknown'), (3, 'three');

SELECT label FROM t WHERE x > 1;          -- three
SELECT label FROM t WHERE NOT (x > 1);    -- one      (NULL is neither)
SELECT label FROM t WHERE x IS NULL;      -- unknown
SELECT NULL = NULL, NULL AND FALSE, NULL OR TRUE, 1 IN (2, NULL);
SELECT COUNT(*), COUNT(x), SUM(x), AVG(x) FROM t;
