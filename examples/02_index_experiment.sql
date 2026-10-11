-- Index experiment. Load the e-commerce dataset first (sidebar, or
--   build/engine/querylab_cli --dataset ecommerce --scale 10000 --rules)
-- then run these one at a time and compare the plans and page reads.

SELECT * FROM orders WHERE customer_id = 42;          -- Seq Scan: reads every page

CREATE INDEX idx_orders_customer ON orders(customer_id);

SELECT * FROM orders WHERE customer_id = 42;          -- Index Scan: a handful of pages

SELECT COUNT(*) FROM orders WHERE customer_id > 5;    -- unselective: the optimizer keeps the Seq Scan

-- Composite index: equality on the first column, range on the second.
CREATE INDEX idx_orders_customer_total ON orders(customer_id, total);
SELECT id, total FROM orders WHERE customer_id = 7 AND total > 500 ORDER BY total DESC;
