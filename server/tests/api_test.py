#!/usr/bin/env python3
"""Integration tests for the QueryLab HTTP API (standard library only).

Usage: python3 server/tests/api_test.py path/to/querylab_server
Starts the server on a free port with a small dataset and checks every
endpoint, including malformed requests, invalid queries and size limits.
"""
import json
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)
        print("FAIL:", msg)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def call(base, method, path, body=None, raw=None, headers=None):
    data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
    req = urllib.request.Request(base + path, data=data, method=method, headers=headers or {"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, json.loads(r.read() or b"null"), dict(r.headers)
    except urllib.error.HTTPError as e:
        payload = e.read()
        try:
            return e.code, json.loads(payload), dict(e.headers)
        except ValueError:
            return e.code, payload, dict(e.headers)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    port = free_port()
    proc = subprocess.Popen([sys.argv[1], "--port", str(port), "--dataset", "ecommerce", "--scale", "2000"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    base = f"http://127.0.0.1:{port}/api/v1"
    try:
        for _ in range(100):
            try:
                if call(base, "GET", "/health")[0] == 200:
                    break
            except OSError:
                time.sleep(0.1)
        status, body, _ = call(base, "GET", "/health")
        check(status == 200 and body["status"] == "ok", "health")

        status, body, _ = call(base, "POST", "/query", {"sql": "SELECT * FROM orders WHERE customer_id = 3"})
        r = body["results"][0]
        check(status == 200 and body["success"], "query success")
        for key in ("ast", "tokens", "logicalPlan", "optimizedPlan", "physicalPlan", "optimizer", "metrics", "events", "explanation", "advisor"):
            check(key in r, f"query response has {key}")
        check(r["rowCount"] == len(r["rows"]), "row count matches rows")

        status, body, _ = call(base, "POST", "/explain", {"sql": "SELECT * FROM orders WHERE id = 1"})
        check(status == 200 and "actual" not in body["results"][0]["physicalPlan"], "explain has no actuals")

        status, body, _ = call(base, "POST", "/parse", {"sql": "SELECT a FROM b WHERE"})
        check(status == 200 and body["success"] is False and body["error"]["stage"] == "parser", "parse error reported")
        check(len(body["tokens"]) > 0, "tokens returned even when parsing fails")

        # Malformed requests.
        status, body, _ = call(base, "POST", "/query", raw=b"{nope")
        check(status == 400 and body["error"]["stage"] == "api", "malformed JSON -> 400")
        status, body, _ = call(base, "POST", "/query", raw=b"[1,2]")
        check(status == 400, "non-object body -> 400")
        status, body, _ = call(base, "POST", "/query", {"query": "SELECT 1"})
        check(status == 400, "missing sql -> 400")
        status, body, _ = call(base, "POST", "/query", {"sql": "SELECT 1", "options": {"maxRows": -5}})
        check(status == 400, "invalid maxRows -> 400")
        status, body, _ = call(base, "POST", "/query", {"sql": "SELECT 1", "options": {"timing": "yes"}})
        check(status == 400, "invalid option type -> 400")
        status, body, _ = call(base, "POST", "/query", {"sql": "SELECT " + "1+" * 120000 + "1"})
        check(status == 413, "oversized SQL -> 413")
        status, body, _ = call(base, "GET", "/nope")
        check(status == 404, "unknown endpoint -> 404")

        # Invalid queries are reported, not crashes.
        status, body, _ = call(base, "POST", "/query", {"sql": "SELECT nope FROM orders"})
        check(status == 200 and not body["success"] and body["error"]["stage"] == "binder", "binder error")
        check(body["error"]["span"]["column"] == 8, "binder error location")
        status, body, _ = call(base, "POST", "/query", {"sql": "SELECT 'unterminated"})
        check(not body["success"] and body["error"]["stage"] == "lexer", "lexer error")
        status, body, _ = call(base, "POST", "/query", {"sql": "SELECT 1/0"})
        check(not body["success"] and body["error"]["stage"] == "executor", "runtime error")

        # Excessive results are truncated but counted.
        status, body, _ = call(base, "POST", "/query", {"sql": "SELECT * FROM order_items", "options": {"maxRows": 100}})
        r = body["results"][0]
        check(r["truncated"] and len(r["rows"]) == 100 and r["rowCount"] > 100, "result truncation")

        # Catalog / tables / storage.
        status, body, _ = call(base, "GET", "/catalog")
        check(status == 200 and len(body["tables"]) == 4, "catalog lists tables")
        status, body, _ = call(base, "GET", "/tables/orders/schema")
        check(status == 200 and body["columns"][0]["name"] == "id", "schema")
        status, body, _ = call(base, "GET", "/tables/orders/rows?offset=5&limit=3")
        check(status == 200 and len(body["rows"]) == 3, "rows paging")
        status, body, _ = call(base, "GET", "/tables/missing/schema")
        check(status == 404, "missing table -> 404")
        status, body, _ = call(base, "GET", "/tables/orders/pages/0")
        check(status == 200 and body["liveRows"] > 0, "page detail")
        status, body, _ = call(base, "GET", "/storage")
        check(status == 200 and body["pageSize"] == 4096, "storage")

        # Indexes: structured creation is validated.
        status, body, _ = call(base, "POST", "/indexes", {"name": "bad name", "table": "orders", "columns": ["id"]})
        check(status == 400, "invalid index name rejected")
        status, body, _ = call(base, "POST", "/indexes", {"name": "idx_c", "table": "orders", "columns": ["customer_id"]})
        check(status == 200 and body["success"], "create index")
        status, body, _ = call(base, "GET", "/indexes/idx_c")
        check(status == 200 and body["valid"] is True, "index detail")
        status, body, _ = call(base, "POST", "/query", {"sql": "SELECT * FROM orders WHERE customer_id = 3"})
        check("Index Scan" in json.dumps(body["results"][0]["physicalPlan"]), "index used after creation")
        status, body, _ = call(base, "DELETE", "/indexes/idx_c")
        check(status == 200 and body["success"], "drop index")

        # Benchmarks.
        status, body, _ = call(base, "POST", "/benchmarks", {"sql": "SELECT COUNT(*) FROM orders", "iterations": 3, "warmup": 0})
        check(status == 200 and body["success"] and len(body["variants"][0]["samplesMs"]) == 3, "benchmark")
        status, body, _ = call(base, "POST", "/benchmarks", {"sql": "DELETE FROM orders"})
        check(status == 400, "benchmark rejects non-SELECT")

        # Settings and datasets.
        status, body, _ = call(base, "PUT", "/settings", {"bufferPoolPages": 64})
        check(status == 200 and body["bufferPoolPages"] == 64, "settings update")
        status, body, _ = call(base, "PUT", "/settings", {"timeoutMs": 5})
        check(status == 400, "settings validation")
        status, body, _ = call(base, "GET", "/datasets")
        check(status == 200 and len(body) == 3, "datasets listed")
        status, body, _ = call(base, "POST", "/datasets/social/load", {"scale": 300})
        check(status == 200 and body["tables"]["posts"] == 300, "dataset load")
        status, body, _ = call(base, "POST", "/datasets/nope/load", {"scale": 300})
        check(status == 400, "unknown dataset")

        # CORS is limited to local origins.
        _, _, headers = call(base, "GET", "/health", headers={"Origin": "http://localhost:5173"})
        check(headers.get("Access-Control-Allow-Origin") == "http://localhost:5173", "CORS for local dev server")
        _, _, headers = call(base, "GET", "/health", headers={"Origin": "http://evil.example"})
        check("Access-Control-Allow-Origin" not in headers, "no CORS for foreign origins")

        status, body, _ = call(base, "GET", "/history")
        check(status == 200 and isinstance(body, list), "history")
    finally:
        proc.terminate()
        proc.wait(timeout=10)
    print(f"{'OK' if not FAILURES else 'FAILED'}: {len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
