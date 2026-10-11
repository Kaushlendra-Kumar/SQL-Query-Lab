-- The statements from the project brief. Run in the editor or with:
--   build/engine/querylab_cli < examples/01_basics.sql
CREATE TABLE users (
    id INTEGER,
    name TEXT,
    age INTEGER,
    city TEXT
);

INSERT INTO users VALUES
    (1, 'Aman', 24, 'Bengaluru'),
    (2, 'Priya', 29, 'Delhi'),
    (3, 'Ravi', NULL, 'Pune'),
    (4, 'Meera', 35, 'Delhi');

SELECT name, age
FROM users
WHERE age > 25
ORDER BY age DESC
LIMIT 10;

SELECT city, COUNT(*) AS people, AVG(age) AS avg_age
FROM users
GROUP BY city
HAVING COUNT(*) >= 1
ORDER BY people DESC, city;
