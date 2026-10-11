#include "querylab/api/datasets.h"

#include <cmath>
#include <cstdio>

#include "querylab/common/error.h"

namespace querylab::api {

std::uint64_t Rng::next() {
    std::uint64_t z = (s_ += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

std::int64_t Rng::range(std::int64_t lo, std::int64_t hi) {
    const auto span = static_cast<std::uint64_t>(hi - lo) + 1;
    return lo + static_cast<std::int64_t>(next() % span);
}

double Rng::unit() { return static_cast<double>(next() >> 11) / 9007199254740992.0; }

std::int64_t Rng::skewed(std::int64_t lo, std::int64_t hi, double skew) {
    const double u = unit();
    const double x = std::pow(u, skew);
    return lo + static_cast<std::int64_t>(x * static_cast<double>(hi - lo + 1)) % (hi - lo + 1);
}

namespace {

using catalog::ColumnInfo;

const std::vector<std::string> kFirst = {"Aman",  "Priya", "Rahul", "Ananya", "Vikram", "Sneha", "Arjun", "Kavya", "Rohan", "Isha",
                                         "Karan", "Meera", "Aditya", "Diya",  "Nikhil", "Pooja", "Sanjay", "Riya", "Varun", "Tara",
                                         "Liam",  "Emma",  "Noah",  "Olivia", "Mateo", "Sofia", "Yuki",  "Hana",  "Omar",  "Leila"};
const std::vector<std::string> kLast = {"Sharma", "Iyer", "Patel", "Reddy", "Khan", "Gupta", "Nair", "Singh", "Das", "Mehta",
                                        "Rao",    "Bose", "Kapoor", "Joshi", "Menon", "Smith", "Garcia", "Tanaka", "Haddad", "Silva"};
const std::vector<std::string> kCities = {"Bengaluru", "Delhi", "Mumbai", "Chennai", "Hyderabad", "Pune", "Kolkata", "Jaipur",
                                          "Ahmedabad", "Kochi", "Lucknow", "Indore", "Chandigarh", "Bhopal", "Mysuru", "Goa"};

catalog::TableInfo& fresh(catalog::Catalog& cat, const std::string& name, std::vector<ColumnInfo> cols) {
    if (cat.findTable(name)) cat.dropTable(name);
    return cat.createTable(name, std::move(cols));
}

std::string date(Rng& r, int yearLo, int yearHi) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", static_cast<int>(r.range(yearLo, yearHi)), static_cast<int>(r.range(1, 12)),
                  static_cast<int>(r.range(1, 28)));
    return buf;
}

std::string personName(Rng& r) { return r.pick(kFirst) + " " + r.pick(kLast); }

double money(double v) { return std::round(v * 100.0) / 100.0; }

nlohmann::json ecommerce(catalog::Catalog& cat, std::size_t scale, Rng& r) {
    const auto nOrders = static_cast<std::int64_t>(scale);
    const auto nCustomers = std::max<std::int64_t>(10, nOrders / 10);
    const std::int64_t nProducts = std::max<std::int64_t>(20, std::min<std::int64_t>(500, nOrders / 20));
    const std::vector<std::string> tiers = {"bronze", "silver", "gold", "platinum"};
    const std::vector<std::string> categories = {"books", "electronics", "kitchen", "toys", "garden", "sports", "beauty", "grocery"};
    const std::vector<std::string> statuses = {"delivered", "delivered", "delivered", "shipped", "processing", "cancelled", "returned"};
    const std::vector<std::string> adjectives = {"Classic", "Smart", "Compact", "Deluxe", "Eco", "Pro", "Mini", "Ultra"};
    const std::vector<std::string> nouns = {"Lamp", "Kettle", "Novel", "Headphones", "Backpack", "Blender", "Puzzle", "Bottle", "Mat", "Watch"};

    auto& customers = fresh(cat, "customers", {{"id", DataType::Integer}, {"name", DataType::Text}, {"city", DataType::Text},
                                                {"signup_year", DataType::Integer}, {"tier", DataType::Text}});
    for (std::int64_t i = 1; i <= nCustomers; ++i) {
        // Tier is skewed: most customers are bronze.
        const auto tier = tiers[static_cast<std::size_t>(r.skewed(0, 3, 2.5))];
        cat.insertRow(customers, {Value::integer(i), Value::text(personName(r)), Value::text(r.pick(kCities)),
                                  Value::integer(r.range(2016, 2025)), Value::text(tier)});
    }
    auto& products = fresh(cat, "products", {{"id", DataType::Integer}, {"name", DataType::Text}, {"category", DataType::Text},
                                              {"price", DataType::Float}});
    std::vector<double> prices(static_cast<std::size_t>(nProducts) + 1);
    for (std::int64_t i = 1; i <= nProducts; ++i) {
        prices[static_cast<std::size_t>(i)] = money(5 + r.unit() * 495);
        cat.insertRow(products, {Value::integer(i), Value::text(r.pick(adjectives) + " " + r.pick(nouns) + " " + std::to_string(i)),
                                 Value::text(r.pick(categories)), Value::floating(prices[static_cast<std::size_t>(i)])});
    }
    auto& orders = fresh(cat, "orders", {{"id", DataType::Integer}, {"customer_id", DataType::Integer}, {"order_date", DataType::Text},
                                          {"status", DataType::Text}, {"total", DataType::Float}});
    auto& items = fresh(cat, "order_items", {{"order_id", DataType::Integer}, {"product_id", DataType::Integer},
                                              {"quantity", DataType::Integer}, {"unit_price", DataType::Float}});
    std::size_t itemCount = 0;
    for (std::int64_t o = 1; o <= nOrders; ++o) {
        const auto cust = r.skewed(1, nCustomers, 1.6);  // some customers order much more often
        const auto lines = r.range(1, 4);
        double total = 0;
        for (std::int64_t l = 0; l < lines; ++l) {
            const auto pid = r.range(1, nProducts);
            const auto qty = r.range(1, 5);
            total += prices[static_cast<std::size_t>(pid)] * static_cast<double>(qty);
            cat.insertRow(items, {Value::integer(o), Value::integer(pid), Value::integer(qty), Value::floating(prices[static_cast<std::size_t>(pid)])});
            ++itemCount;
        }
        // A few orders have no recorded status (NULL) to exercise NULL semantics.
        Value status = r.range(0, 99) < 2 ? Value::null() : Value::text(r.pick(statuses));
        cat.insertRow(orders, {Value::integer(o), Value::integer(cust), Value::text(date(r, 2021, 2025)), status, Value::floating(money(total))});
    }
    return {{"customers", nCustomers}, {"products", nProducts}, {"orders", nOrders}, {"order_items", itemCount}};
}

nlohmann::json social(catalog::Catalog& cat, std::size_t scale, Rng& r) {
    const auto nPosts = static_cast<std::int64_t>(scale);
    const auto nUsers = std::max<std::int64_t>(10, nPosts / 8);
    const std::vector<std::string> topics = {"tech", "music", "travel", "food", "sports", "art", "science", "movies", "books", "fitness"};
    auto& users = fresh(cat, "users", {{"id", DataType::Integer}, {"name", DataType::Text}, {"city", DataType::Text},
                                        {"joined_year", DataType::Integer}, {"verified", DataType::Boolean}});
    for (std::int64_t i = 1; i <= nUsers; ++i) {
        cat.insertRow(users, {Value::integer(i), Value::text(personName(r)), Value::text(r.pick(kCities)), Value::integer(r.range(2012, 2025)),
                              Value::boolean(r.range(0, 99) < 7)});
    }
    auto& posts = fresh(cat, "posts", {{"id", DataType::Integer}, {"user_id", DataType::Integer}, {"topic", DataType::Text},
                                        {"created_day", DataType::Integer}, {"likes", DataType::Integer}});
    for (std::int64_t p = 1; p <= nPosts; ++p) {
        const auto author = r.skewed(1, nUsers, 2.0);  // a few prolific posters
        const auto likes = static_cast<std::int64_t>(std::floor(std::pow(r.unit(), 4.0) * 5000));  // heavy-tailed
        cat.insertRow(posts, {Value::integer(p), Value::integer(author), Value::text(r.pick(topics)), Value::integer(r.range(1, 365)),
                              Value::integer(likes)});
    }
    auto& follows = fresh(cat, "follows", {{"follower_id", DataType::Integer}, {"followee_id", DataType::Integer}});
    const auto nFollows = nUsers * 6;
    for (std::int64_t f = 0; f < nFollows; ++f) {
        const auto a = r.range(1, nUsers);
        auto b = r.skewed(1, nUsers, 2.5);  // popular accounts get followed more
        if (b == a) b = (b % nUsers) + 1;
        cat.insertRow(follows, {Value::integer(a), Value::integer(b)});
    }
    auto& comments = fresh(cat, "comments", {{"id", DataType::Integer}, {"post_id", DataType::Integer}, {"user_id", DataType::Integer},
                                              {"body_length", DataType::Integer}});
    const auto nComments = nPosts * 2;
    for (std::int64_t c = 1; c <= nComments; ++c) {
        cat.insertRow(comments, {Value::integer(c), Value::integer(r.skewed(1, nPosts, 1.5)), Value::integer(r.range(1, nUsers)),
                                 Value::integer(r.range(1, 280))});
    }
    return {{"users", nUsers}, {"posts", nPosts}, {"follows", nFollows}, {"comments", nComments}};
}

nlohmann::json employees(catalog::Catalog& cat, std::size_t scale, Rng& r) {
    const auto nEmp = static_cast<std::int64_t>(scale);
    const std::vector<std::string> deptNames = {"Engineering", "Sales",   "Marketing", "Finance", "Support",  "Operations",
                                                "Research",    "Legal",   "Design",    "People",  "Security", "Data"};
    const std::vector<std::string> titles = {"Associate", "Engineer", "Senior Engineer", "Analyst", "Manager", "Director", "Specialist"};
    auto& depts = fresh(cat, "departments", {{"id", DataType::Integer}, {"name", DataType::Text}, {"location", DataType::Text},
                                              {"budget", DataType::Float}});
    const auto nDept = static_cast<std::int64_t>(deptNames.size());
    for (std::int64_t d = 1; d <= nDept; ++d) {
        cat.insertRow(depts, {Value::integer(d), Value::text(deptNames[static_cast<std::size_t>(d - 1)]), Value::text(r.pick(kCities)),
                              Value::floating(money(500000 + r.unit() * 4500000))});
    }
    auto& emps = fresh(cat, "employees", {{"id", DataType::Integer}, {"name", DataType::Text}, {"department_id", DataType::Integer},
                                           {"manager_id", DataType::Integer}, {"hire_year", DataType::Integer}, {"title", DataType::Text}});
    for (std::int64_t e = 1; e <= nEmp; ++e) {
        Value manager = e <= nDept ? Value::null() : Value::integer(r.range(1, std::max<std::int64_t>(1, std::min(e - 1, nEmp / 10 + nDept))));
        cat.insertRow(emps, {Value::integer(e), Value::text(personName(r)), Value::integer(r.skewed(1, nDept, 1.8)), manager,
                             Value::integer(r.range(2005, 2025)), Value::text(r.pick(titles))});
    }
    auto& sal = fresh(cat, "salaries", {{"employee_id", DataType::Integer}, {"year", DataType::Integer}, {"amount", DataType::Float}});
    std::size_t nSal = 0;
    for (std::int64_t e = 1; e <= nEmp; ++e) {
        double base = 400000 + r.unit() * 2600000;
        for (int y = 2022; y <= 2025; ++y) {
            cat.insertRow(sal, {Value::integer(e), Value::integer(y), Value::floating(money(base))});
            base *= 1.0 + r.unit() * 0.12;
            ++nSal;
        }
    }
    return {{"departments", nDept}, {"employees", nEmp}, {"salaries", nSal}};
}

}  // namespace

std::vector<DatasetInfo> availableDatasets() {
    return {
        {"ecommerce",
         "E-commerce",
         "Customers, products, orders and order lines. Order counts per customer are skewed, which makes index and estimate comparisons interesting.",
         {"customers", "products", "orders", "order_items"},
         {{"Orders of one customer", "SELECT * FROM orders WHERE customer_id = 42;"},
          {"Revenue by city", "SELECT c.city, COUNT(*) AS orders, SUM(o.total) AS revenue\nFROM orders o JOIN customers c ON o.customer_id = c.id\nWHERE o.status = 'delivered'\nGROUP BY c.city\nORDER BY revenue DESC\nLIMIT 5;"},
          {"Top products", "SELECT p.name, SUM(i.quantity) AS units\nFROM order_items i\nJOIN products p ON p.id = i.product_id\nGROUP BY p.name\nORDER BY units DESC\nLIMIT 10;"},
          {"Three-way join", "SELECT c.name, o.id, p.category, i.quantity\nFROM customers c\nJOIN orders o ON o.customer_id = c.id\nJOIN order_items i ON i.order_id = o.id\nJOIN products p ON p.id = i.product_id\nWHERE c.tier = 'platinum' AND p.price > 400;"},
          {"Missing status (NULL)", "SELECT COUNT(*) AS total, COUNT(status) AS with_status\nFROM orders;"},
          {"Index experiment", "CREATE INDEX idx_orders_customer ON orders(customer_id);"}}},
        {"social",
         "Social network",
         "Users, posts, follows and comments with heavy-tailed likes and a few very prolific users.",
         {"users", "posts", "follows", "comments"},
         {{"Popular posts", "SELECT topic, id, likes FROM posts WHERE likes > 3000 ORDER BY likes DESC LIMIT 20;"},
          {"Posts per topic", "SELECT topic, COUNT(*) AS posts, AVG(likes) AS avg_likes\nFROM posts\nGROUP BY topic\nHAVING COUNT(*) > 10\nORDER BY avg_likes DESC;"},
          {"Most followed users", "SELECT u.name, COUNT(*) AS followers\nFROM follows f JOIN users u ON u.id = f.followee_id\nGROUP BY u.name\nORDER BY followers DESC\nLIMIT 10;"},
          {"Users without posts (LEFT JOIN)", "SELECT u.id, u.name\nFROM users u LEFT JOIN posts p ON p.user_id = u.id\nWHERE p.id IS NULL\nLIMIT 20;"},
          {"Verified users in a city", "SELECT name, joined_year FROM users WHERE verified = TRUE AND city = 'Bengaluru';"}}},
        {"employees",
         "Employee analytics",
         "Departments, employees (with managers) and yearly salaries.",
         {"departments", "employees", "salaries"},
         {{"Headcount by department", "SELECT d.name, COUNT(*) AS headcount\nFROM employees e JOIN departments d ON d.id = e.department_id\nGROUP BY d.name\nORDER BY headcount DESC;"},
          {"Average 2025 salary by title", "SELECT e.title, AVG(s.amount) AS avg_salary, MAX(s.amount) AS top\nFROM employees e JOIN salaries s ON s.employee_id = e.id\nWHERE s.year = 2025\nGROUP BY e.title\nORDER BY avg_salary DESC;"},
          {"Recent hires", "SELECT id, name, title FROM employees WHERE hire_year BETWEEN 2023 AND 2025 ORDER BY hire_year DESC, id LIMIT 25;"},
          {"Employees and managers", "SELECT e.name AS employee, m.name AS manager\nFROM employees e LEFT JOIN employees m ON m.id = e.manager_id\nLIMIT 20;"}}},
    };
}

nlohmann::json loadDataset(catalog::Catalog& catalog, const std::string& name, std::size_t scale, std::uint64_t seed) {
    if (scale < 10 || scale > 1000000) throw QueryError(ErrorStage::Api, "scale must be between 10 and 1,000,000 rows");
    Rng r(seed);
    if (name == "ecommerce") return ecommerce(catalog, scale, r);
    if (name == "social") return social(catalog, scale, r);
    if (name == "employees") return employees(catalog, scale, r);
    throw QueryError(ErrorStage::Api, "unknown dataset '" + name + "' (available: ecommerce, social, employees)");
}

}  // namespace querylab::api
