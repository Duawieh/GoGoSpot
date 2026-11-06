#!/usr/bin/env python3
# 备份策略回归：普通写入不再产生备份，只有定时任务、手动备份和清空已完成才留。
# 打 8770 上的隔离实例，dataFile 必须在 build/test 下。
import http.client
import json
import os
import sys
import time

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


def backups():
    st, body = req("GET", "/api/backups")
    return json.loads(body).get("backups", [])


def task(i):
    return {"id": "b%d" % i, "title": "任务 %d" % i, "done": False, "estimate": 30,
            "due": None, "tags": [], "links": [], "startedAt": 0, "repeat": "",
            "createdAt": "2026-11-06T00:00:00Z", "completedAt": None}


# ---- 先确认是隔离实例 ----
st, body = req("GET", "/api/info")
info = json.loads(body)
if "build/test" not in info.get("dataFile", ""):
    print("ERROR: dataFile 不在 build/test 下:", info.get("dataFile"))
    sys.exit(1)
print("隔离实例确认:", info["dataFile"], "\n")

# ---- 1. 普通写入不产生备份 ----
before = len(backups())
for i in range(1, 6):
    put([task(k) for k in range(i)])
after = len(backups())
t("连续 5 次覆盖写入不新增备份", before == after, "%d -> %d" % (before, after))

# ---- 2. 手动备份仍然 +1 ----
st, _ = req("POST", "/api/backup")
n1 = len(backups())
t("手动备份 +1", st == 200 and n1 == after + 1, "%d -> %d" % (after, n1))

# ---- 3. 备份名仍然带毫秒，同一秒内不会互相覆盖 ----
req("POST", "/api/backup")
req("POST", "/api/backup")
names = [b["name"] for b in backups()[:3]]
t("连按三次拿到三个不同名字", len(set(names)) == 3, names)

# ---- 4. 备份内容就是写入前的数据 ----
put([task(1), task(2)])
req("POST", "/api/backup")
newest = backups()[0]["name"]
path = os.path.join((info.get("backupDir") or "").replace("/", os.sep), newest)
data = json.load(open(path, encoding="utf-8"))
t("备份内容与当前数据一致", len(data.get("tasks", [])) == 2, len(data.get("tasks", [])))

# ---- 5. 连续删除只备份一次 ----
# 冷却是一分钟，必须等过上一次备份才能验；这一段单独占 61 秒
print("\n等 61 秒让备份冷却过期…")
time.sleep(61)
put([task(i) for i in range(1, 5)])
before = len(backups())
for i in (1, 2, 3):
    req("POST", "/api/remove?id=b%d" % i)
after = len(backups())
t("连续三次删除只新增一份备份", after == before + 1, "%d -> %d" % (before, after))

# 删完再点一次普通备份，冷却重计，紧接着的删除不再备
req("POST", "/api/backup")
n2 = len(backups())
req("POST", "/api/remove?id=b4")
t("其他备份动作后紧接的删除不再备份", len(backups()) == n2, "%d -> %d" % (n2, len(backups())))

print("\n" + "=" * 60)
bad = [r for r in results if not r[1]]
print("总计: %d PASS / %d FAIL / %d 项" % (len(results) - len(bad), len(bad), len(results)))
sys.exit(1 if bad else 0)
