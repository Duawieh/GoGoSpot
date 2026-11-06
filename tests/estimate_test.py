#!/usr/bin/env python3
# 预估「不设」的接口回归：0 要原样传到悬浮清单，不能再被兜底成 60。
# 打 8770 上的隔离实例，dataFile 必须在 build/test 下。
import http.client
import json
import sys

HOST, PORT = "127.0.0.1", 8770
results = []


def req(method, path, body=None):
    c = http.client.HTTPConnection(HOST, PORT, timeout=8)
    headers = {"Content-Type": "application/json"}
    if isinstance(body, str):
        body = body.encode("utf-8")
    c.request(method, path, body=body, headers=headers)
    r = c.getresponse()
    data = r.read().decode("utf-8", "replace")
    c.close()
    return r.status, data


def t(name, cond, detail=""):
    results.append((name, bool(cond), detail))
    print(("[PASS] " if cond else "[FAIL] ") + name + (" -- " + str(detail) if detail else ""))
    return cond


def put(tasks):
    return req("PUT", "/api/tasks", json.dumps({"schema": 1, "tasks": tasks}))


def task(i, **kw):
    d = {"id": "e%d" % i, "title": "任务 %d" % i, "done": False, "estimate": 0,
         "due": None, "tags": [], "links": [], "startedAt": 0, "repeat": "",
         "createdAt": "2026-11-06T00:00:00Z", "completedAt": None}
    d.update(kw)
    return d


# 每行格式：id \t 0 \t estimate \t dueMs \t title \t links \t startedAt \t pinned
def widget_rows():
    st, body = req("GET", "/api/widget")
    lines = body.splitlines()[1:]
    return [ln.split("\t") for ln in lines if ln.strip()]


# ---- 先确认是隔离实例 ----
st, body = req("GET", "/api/info")
info = json.loads(body)
if "build/test" not in info.get("dataFile", ""):
    print("ERROR: dataFile 不在 build/test 下:", info.get("dataFile"))
    sys.exit(1)
print("隔离实例确认:", info["dataFile"], "\n")


# ---- 1. estimate 各种取值经过 /api/widget 的样子 ----
cases = [
    ("estimate=0 → 原样 0", 0, "0"),
    ("estimate=0.5 → ceil 1", 0.5, "1"),
    ("estimate=0.2 → ceil 1", 0.2, "1"),
    ("estimate=1.9 → ceil 2", 1.9, "2"),
    ("estimate=99999 → 99999", 99999, "99999"),
]
for name, val, want in cases:
    put([task(1, estimate=val)])
    rows = widget_rows()
    got = rows[0][2] if rows else "(无行)"
    t(name, got == want, "got=%s" % got)

# 缺字段
put([{k: v for k, v in task(1).items() if k != "estimate"}])
rows = widget_rows()
got = rows[0][2] if rows else "(无行)"
t("estimate 字段缺失 → 0", got == "0", "got=%s" % got)

# 字符串类型的 estimate 不算数
put([task(1, estimate="60")])
rows = widget_rows()
got = rows[0][2] if rows else "(无行)"
t("estimate 是字符串 → 0", got == "0", "got=%s" % got)

# 负数：不该被当成「设了预估」，至少要非负
put([task(1, estimate=-5)])
rows = widget_rows()
got = rows[0][2] if rows else "(无行)"
t("estimate=-5 → 非负", got.isdigit() and int(got) >= 0, "got=%s" % got)


# ---- 2. 存读往返：0 不能被悄悄改成 60 ----
put([task(1, estimate=0)])
st, body = req("GET", "/api/tasks")
stored = json.loads(body)["tasks"][0].get("estimate")
t("存 0 读回 0", stored == 0, "stored=%r" % stored)

put([task(1, estimate=0), task(2, estimate=60)])
st, body = req("GET", "/api/tasks")
got = sorted(x.get("estimate") for x in json.loads(body)["tasks"])
t("0 和 60 混存各自保留", got == [0, 60], "got=%s" % got)


# ---- 3. 全都不设：每行都是 0，顺序不乱 ----
put([task(i) for i in range(1, 6)])
rows = widget_rows()
ests = [r[2] for r in rows]
t("五条不设预估全是 0", len(rows) == 5 and set(ests) == {"0"}, "ests=%s" % ests)


# ---- 4. 不设预估 + 逾期：仍算红、仍能派生 ----
put([task(1, estimate=0, due="2020-01-01T09:00", repeat="daily")])
st, body = req("GET", "/api/widget")
t("不设预估的逾期任务仍在列表", "任务 1" in body, body.splitlines()[:2])
st, body = req("POST", "/api/toggle?id=e1")
t("不设预估也能 toggle", st == 200, st)
st, body = req("GET", "/api/tasks")
tasks = json.loads(body)["tasks"]
t("重复任务派生了一期", len(tasks) == 2, "len=%d" % len(tasks))
t("派生的一期也不设预估",
  all(x.get("estimate") == 0 for x in tasks),
  [x.get("estimate") for x in tasks])


# ---- 5. 上限仍然生效 ----
put([task(1, estimate=100000)])
st, body = req("GET", "/api/tasks")
stored = json.loads(body)["tasks"][0].get("estimate")
t("超过 99999 由前端截断，服务端原样存", stored == 100000, "stored=%r" % stored)


print("\n" + "=" * 60)
bad = [r for r in results if not r[1]]
print("总计: %d PASS / %d FAIL / %d 项" % (len(results) - len(bad), len(bad), len(results)))
sys.exit(1 if bad else 0)
