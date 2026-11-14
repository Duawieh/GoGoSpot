#!/usr/bin/env python3
# 提醒任务的接口回归：类型字段、due 派生、不能计时、推迟与重复派生都要跟着 Start 走。
# 打 8770 上的隔离实例，dataFile 必须在 build/test 下。
import datetime
import http.client
import json
import sys
import urllib.parse

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


def add(**kw):
    q = urllib.parse.urlencode(kw)
    return req("POST", "/api/add?" + q)


def tasks():
    st, body = req("GET", "/api/tasks")
    return json.loads(body).get("tasks", [])


def by_id(tid):
    for x in tasks():
        if x.get("id") == tid:
            return x
    return None


def rows():
    st, body = req("GET", "/api/widget")
    return [ln.split("\t") for ln in body.splitlines()[1:] if ln.strip()]


def row_of(tid):
    for r in rows():
        if r[0] == tid:
            return r
    return None


def ms(s):
    """把 YYYY-MM-DDTHH:MM 按本地时间转成毫秒，和 dueToUnixMs 一致"""
    d = datetime.datetime.strptime(s[:16], "%Y-%m-%dT%H:%M")
    return int(d.timestamp() * 1000)


def iso(dt):
    return dt.strftime("%Y-%m-%dT%H:%M")


def reset():
    put([])


# ---- 先确认是隔离实例 ----
st, body = req("GET", "/api/info")
info = json.loads(body)
if "build/test" not in info.get("dataFile", ""):
    print("ERROR: dataFile 不在 build/test 下:", info.get("dataFile"))
    sys.exit(1)
print("隔离实例确认:", info["dataFile"], "\n")

now = datetime.datetime.now().replace(second=0, microsecond=0)
start_at = now + datetime.timedelta(hours=3)

# ---- 1. 建提醒任务：due 由开始 + 持续时间推出 ----
reset()
add(title="开会", type="remind", start=iso(start_at), estimate=90, advance=30)
x = by_id(tasks()[0]["id"]) if tasks() else None
t("提醒任务建出来了", x is not None)
if x:
    t("TaskType = remind", x.get("TaskType") == "remind", x.get("TaskType"))
    t("Start 原样保存", x.get("Start") == iso(start_at), x.get("Start"))
    t("Advance 原样保存", x.get("Advance") == 30, x.get("Advance"))
    t("due = 开始 + 90 分钟", x.get("due") == iso(start_at + datetime.timedelta(minutes=90)),
      x.get("due"))
    t("提醒任务默认不在计时", (x.get("startedAt") or 0) == 0, x.get("startedAt"))

# ---- 2. 不填持续时间按 1 小时推 due ----
reset()
add(title="吃药", type="remind", start=iso(start_at))
x = tasks()[0] if tasks() else None
t("不填持续时间按 1 小时", x and x.get("due") == iso(start_at + datetime.timedelta(hours=1)),
  x.get("due") if x else "无任务")

# ---- 3. 过程任务不受影响 ----
reset()
add(title="写报告", due=iso(start_at), estimate=60)
x = tasks()[0] if tasks() else None
t("过程任务 TaskType = process", x and x.get("TaskType") == "process",
  x.get("TaskType") if x else "无任务")
t("过程任务不写 Start", x and not x.get("Start"), x.get("Start") if x else "无任务")
t("过程任务不写 Advance", x and "Advance" not in x, x.get("Advance") if x else "无任务")
t("过程任务 due 原样", x and x.get("due") == iso(start_at), x.get("due") if x else "无任务")

# ---- 4. 不带 type 参数仍按过程任务 ----
reset()
add(title="老客户端", due=iso(start_at), estimate=30)
x = tasks()[0] if tasks() else None
t("缺 type 按过程任务", x and x.get("TaskType") == "process",
  x.get("TaskType") if x else "无任务")

# ---- 5. 提醒任务可以没有开始时间 ----
reset()
add(title="待安排", type="remind")
x = tasks()[0] if tasks() else None
t("没有开始时间也能建", x is not None)
t("没有开始时间就没有 due", x and not x.get("due"), x.get("due") if x else "无任务")
t("没有开始时间 Advance 补 0", x and x.get("Advance") == 0,
  x.get("Advance") if x else "无任务")

# ---- 6. Advance 边界 ----
reset()
add(title="负数", type="remind", start=iso(start_at), advance=-30)
x = tasks()[0] if tasks() else None
t("Advance 负数归 0", x and x.get("Advance") == 0, x.get("Advance") if x else "无任务")

reset()
add(title="超上限", type="remind", start=iso(start_at), advance=200000)
x = tasks()[0] if tasks() else None
t("Advance 超上限夹到 99999", x and x.get("Advance") == 99999, x.get("Advance") if x else "无任务")

# ---- 7. widget 协议 11 列 ----
reset()
add(title="提醒甲", type="remind", start=iso(start_at), estimate=45, advance=15)
add(title="过程乙", due=iso(start_at), estimate=60)
r1 = [r for r in rows() if r[4] == "提醒甲"]
r2 = [r for r in rows() if r[4] == "过程乙"]
t("widget 每行 11 列", r1 and len(r1[0]) == 11, len(r1[0]) if r1 else "缺行")
if r1:
    t("提醒行 type 列 = 1", r1[0][8] == "1", r1[0][8])
    t("提醒行 startMs 正确", r1[0][9] == str(ms(iso(start_at))), r1[0][9])
    t("提醒行 advance 列 = 15", r1[0][10] == "15", r1[0][10])
    t("提醒行 dueMs = 开始 + 45 分",
      r1[0][3] == str(ms(iso(start_at + datetime.timedelta(minutes=45)))), r1[0][3])
if r2:
    t("过程行 type 列 = 0", r2[0][8] == "0", r2[0][8])
    t("过程行 startMs = 0", r2[0][9] == "0", r2[0][9])
    t("过程行 advance 列 = 0", r2[0][10] == "0", r2[0][10])

# ---- 8. 提醒任务不能计时 ----
reset()
add(title="不能计时", type="remind", start=iso(start_at), estimate=30)
tid = tasks()[0]["id"]
st, body = req("POST", "/api/start?id=" + urllib.parse.quote(tid) + "&on=1")
t("提醒任务拒绝开始计时", st == 400, "HTTP %d %s" % (st, body[:60]))
t("拒绝后 startedAt 仍为 0", ((by_id(tid) or {}).get("startedAt") or 0) == 0)

# 过程任务照常能计时
reset()
add(title="能计时", due=iso(start_at), estimate=30)
tid = tasks()[0]["id"]
st, body = req("POST", "/api/start?id=" + urllib.parse.quote(tid) + "&on=1")
t("过程任务能开始计时", st == 200, "HTTP %d" % st)
t("过程任务 startedAt 已写入", (by_id(tid) or {}).get("startedAt", 0) > 0)

# ---- 9. 推迟提醒任务要推 Start ----
reset()
add(title="推提醒", type="remind", start=iso(start_at), estimate=60)
tid = tasks()[0]["id"]
st, body = req("POST", "/api/snooze?id=" + urllib.parse.quote(tid))
x = by_id(tid) or {}
want_start = iso(start_at + datetime.timedelta(days=1))
want_due = iso(start_at + datetime.timedelta(days=1, minutes=60))
t("推迟提醒任务：Start 加一天", x.get("Start") == want_start, x.get("Start"))
t("推迟提醒任务：due 跟着走", x.get("due") == want_due, x.get("due"))

# 已经过点的提醒任务，推迟从当前时间算，不留在过去
reset()
past = now - datetime.timedelta(hours=5)
add(title="过点提醒", type="remind", start=iso(past), estimate=60)
tid = tasks()[0]["id"]
req("POST", "/api/snooze?id=" + urllib.parse.quote(tid))
x = by_id(tid) or {}
new_start = x.get("Start") or ""
t("过点的提醒任务推迟到将来", new_start > iso(now), new_start)

# ---- 10. PUT 时补 due / 补 Advance ----
reset()
st, body = put([{
    "id": "r1", "title": "缺 due", "done": False, "estimate": 90,
    "TaskType": "remind", "Start": iso(start_at),
    "tags": [], "links": [], "startedAt": 0, "repeat": "",
    "createdAt": "2026-11-06T00:00:00Z", "completedAt": None,
}])
x = by_id("r1") or {}
t("PUT 缺 due 时补出来", x.get("due") == iso(start_at + datetime.timedelta(minutes=90)),
  x.get("due"))
t("PUT 缺 Advance 时补 0", x.get("Advance") == 0, x.get("Advance"))

# due 写错了要纠正
reset()
put([{
    "id": "r2", "title": "due 写错", "done": False, "estimate": 60,
    "TaskType": "remind", "Start": iso(start_at), "due": "2020-01-01T00:00",
    "Advance": 10, "tags": [], "links": [], "startedAt": 0, "repeat": "",
    "createdAt": "2026-11-06T00:00:00Z", "completedAt": None,
}])
x = by_id("r2") or {}
t("PUT 纠正写错的 due", x.get("due") == iso(start_at + datetime.timedelta(minutes=60)),
  x.get("due"))

# Advance 负数要归 0
reset()
put([{
    "id": "r3", "title": "负 Advance", "done": False, "estimate": 60,
    "TaskType": "remind", "Start": iso(start_at), "due": iso(start_at),
    "Advance": -99, "tags": [], "links": [], "startedAt": 0, "repeat": "",
    "createdAt": "2026-11-06T00:00:00Z", "completedAt": None,
}])
x = by_id("r3") or {}
t("PUT 把负 Advance 归 0", x.get("Advance") == 0, x.get("Advance"))

# ---- 11. 老数据没有 TaskType 不受影响 ----
reset()
put([{
    "id": "old", "title": "老任务", "done": False, "estimate": 60,
    "due": iso(start_at), "tags": [], "links": [], "startedAt": 0, "repeat": "",
    "createdAt": "2026-11-06T00:00:00Z", "completedAt": None,
}])
x = by_id("old") or {}
t("老数据 due 不被改", x.get("due") == iso(start_at), x.get("due"))
t("老数据不凭空长出 Start", "Start" not in x, x.get("Start"))
t("老数据不凭空长出 Advance", "Advance" not in x, x.get("Advance"))

# ---- 12. 提醒任务的重复派生：勾完成时 Start 也要往后推一期 ----
reset()
put([{
    "id": "rep", "title": "每天开会", "done": False, "estimate": 30,
    "TaskType": "remind", "Start": iso(start_at),
    "due": iso(start_at + datetime.timedelta(minutes=30)),
    "Advance": 10, "repeat": "daily", "route_origin_date": iso(start_at + datetime.timedelta(minutes=30)),
    "tags": [], "links": [], "startedAt": 0,
    "createdAt": "2026-11-06T00:00:00Z", "completedAt": None,
}])
req("POST", "/api/toggle?id=rep")
allt = tasks()
t("重复派生出一条新任务", len(allt) == 2, "%d 条" % len(allt))
t("原任务 repeat 已清空", not (by_id("rep") or {}).get("repeat"),
  (by_id("rep") or {}).get("repeat"))
spawn = [x for x in allt if not x.get("done")]
if spawn:
    s = spawn[0]
    t("派生的一期仍是提醒任务", s.get("TaskType") == "remind", s.get("TaskType"))
    t("派生的 Start 往后推一天", s.get("Start") == iso(start_at + datetime.timedelta(days=1)),
      s.get("Start"))
    t("派生的 due 仍是 Start + 持续时间",
      s.get("due") == iso(start_at + datetime.timedelta(days=1, minutes=30)), s.get("due"))
    t("派生的 Advance 跟着走", s.get("Advance") == 10, s.get("Advance"))

# ---- 13. 提醒任务 + repeat 但缺 due：不能把重复标记当孤立清掉 ----
reset()
put([{
    "id": "rep2", "title": "缺 due 的重复", "done": False, "estimate": 30,
    "TaskType": "remind", "Start": iso(start_at), "Advance": 5,
    "repeat": "weekly", "tags": [], "links": [], "startedAt": 0,
    "createdAt": "2026-11-06T00:00:00Z", "completedAt": None,
}])
x = by_id("rep2") or {}
t("缺 due 的重复提醒任务保住 repeat", x.get("repeat") == "weekly", x.get("repeat"))
t("缺 due 的重复提醒任务补出 due",
  x.get("due") == iso(start_at + datetime.timedelta(minutes=30)), x.get("due"))

# ---- 14. 混合列表：两种任务并存互不干扰 ----
reset()
add(title="提醒丙", type="remind", start=iso(start_at), estimate=45, advance=20)
add(title="过程丁", due=iso(start_at + datetime.timedelta(hours=2)), estimate=120)
allt = tasks()
t("两种任务并存", len(allt) == 2, "%d 条" % len(allt))
remind = [x for x in allt if x.get("TaskType") == "remind"]
proc = [x for x in allt if x.get("TaskType") == "process"]
t("只有一条提醒任务", len(remind) == 1, len(remind))
t("只有一条过程任务", len(proc) == 1, len(proc))
t("提醒任务不影响过程任务的 due",
  proc and proc[0].get("due") == iso(start_at + datetime.timedelta(hours=2)),
  proc[0].get("due") if proc else "无")

print("\n" + "=" * 60)
bad = [r for r in results if not r[1]]
print("总计: %d PASS / %d FAIL / %d 项" % (len(results) - len(bad), len(bad), len(results)))
sys.exit(1 if bad else 0)
