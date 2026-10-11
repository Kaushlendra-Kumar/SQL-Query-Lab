-- Requires the e-commerce dataset.
-- Three inner joins: the optimizer reorders them to start from the smallest
-- filtered input and picks hash-join build sides.
SELECT c.name, o.id, p.category, i.quantity
FROM customers c
JOIN orders o ON o.customer_id = c.id
JOIN order_items i ON i.order_id = o.id
JOIN products p ON p.id = i.product_id
WHERE c.tier = 'platinum' AND p.price > 400;

-- Aggregation with HAVING and ORDER BY on an aggregate alias.
SELECT c.city, COUNT(*) AS orders, SUM(o.total) AS revenue
FROM orders o JOIN customers c ON o.customer_id = c.id
WHERE o.status = 'delivered'
GROUP BY c.city
HAVING COUNT(*) > 100
ORDER BY revenue DESC;

-- LEFT JOIN: customers who never ordered.
SELECT c.id, c.name
FROM customers c LEFT JOIN orders o ON o.customer_id = c.id
WHERE o.id IS NULL;
