#!/usr/bin/env python3
# 边界情况测试：打 8770 端口的隔离实例（dataFile 在 build/test/boundary 下）
import http.client
import json
import os
import socket
import subprocess
import sys
import time
import threading

PORT = 8770
HOST = "127.0.0.1"

def req(method, path, body=None, headers=None, port=PORT):
    """发送请求并返回 (status, body_text)。"""
    c = http.client.HTTPConnection(HOST, port, timeout=5)
    h = {"Content-Type": "application/json"}
    if headers:
        h.update(headers)
    c.request(method, path, body=body, headers=h)
    r = c.getresponse()
    status = r.status
    data = r.read().decode("utf-8", errors="replace")
    c.close()
    return status, data

def post_tasks(tasks_list):
    """以数组格式 POST 任务列表。"""
    body = json.dumps({"tasks": tasks_list})
    return req("POST", "/api/tasks", body)

results = []
def t(name, cond, detail=""):
    results.append((name, cond, detail))
    mark = "PASS" if cond else "FAIL"
    print(f"[{mark}] {name}" + (f" -- {detail}" if detail else ""))
    return cond

# ---- 先确认是隔离实例 ----
status, body = req("GET", "/api/info")
info = json.loads(body)
if "build/test/boundary" not in info.get("dataFile", ""):
    print(f"ERROR: dataFile 不在 build/test/boundary 下: {info.get('dataFile')}")
    sys.exit(1)
print(f"隔离实例确认: port={PORT}, dataFile={info['dataFile']}")

# 清掉上一轮残留，保证用例从空列表开始
post_tasks([])

# ---- 1. 空任务列表 ----
print("\n== 1. 空任务列表 ==")
status, body = req("GET", "/api/tasks")
t("空列表 GET 200", status == 200, f"got {status}")
tasks = json.loads(body)
t("空列表 tasks 是数组", isinstance(tasks.get("tasks"), list), str(tasks)[:200])
t("空列表 tasks 长度 0", len(tasks.get("tasks", [])) == 0)

# ---- 2. 超长标题 ----
print("\n== 2. 超长标题 ==")
long_title = "A" * 10000
task = {"id": "test-long", "title": long_title, "estimate": 60, "due": None, "tags": [], "links": [], "done": False, "pinned": False, "repeat": "", "createdAt": "2026-01-01T00:00:00Z", "completedAt": None}
status, body = post_tasks([task])
t("超长标题 POST 200", status == 200, f"got {status} {body[:200]}")
status, body = req("GET", "/api/tasks")
tasks_obj = json.loads(body)
found = [x for x in tasks_obj.get("tasks", []) if x.get("id") == "test-long"]
t("超长标题存储成功", len(found) == 1)
if found:
    stored_title = found[0].get("title", "")
    t("超长标题完整保留", len(stored_title) == 10000, f"stored length={len(stored_title)}")

# ---- 3. 特殊字符 ----
print("\n== 3. 特殊字符 ==")
special_title = "测试<b>bold</b>\n\t\"quote\"'apos'\\backslash"
task = {"id": "test-special", "title": special_title, "estimate": 60, "due": None, "tags": [], "links": [], "done": False, "pinned": False, "repeat": "", "createdAt": "2026-01-01T00:00:00Z", "completedAt": None}
status, body = post_tasks([task])
t("特殊字符 POST 200", status == 200, f"got {status} {body[:200]}")
status, body = req("GET", "/api/tasks")
tasks_obj = json.loads(body)
found = [x for x in tasks_obj.get("tasks", []) if x.get("id") == "test-special"]
t("特殊字符存储成功", len(found) == 1)
if found:
    stored = found[0].get("title", "")
    t("特殊字符完整保留", stored == special_title, f"stored={repr(stored[:80])}")

# ---- 4. 极端 estimate 值 ----
print("\n== 4. 极端 estimate 值 ==")
for est_val in [0, -1, 99999, 100000, 2147483647]:
    task = {"id": f"test-est-{est_val}", "title": "est test", "estimate": est_val, "due": None, "tags": [], "links": [], "done": False, "pinned": False, "repeat": "", "createdAt": "2026-01-01T00:00:00Z", "completedAt": None}
    status, body = post_tasks([task])
    t(f"estimate={est_val} POST", status == 200, f"got {status} {body[:100]}")

status, body = req("GET", "/api/tasks")
tasks_obj = json.loads(body)
for est_val in [0, -1, 99999]:
    found = [x for x in tasks_obj.get("tasks", []) if x.get("id") == f"test-est-{est_val}"]
    if found:
        stored_est = found[0].get("estimate", 0)
        t(f"estimate={est_val} 存储", isinstance(stored_est, (int, float)), f"stored={stored_est}")

# ---- 5. 重复任务锚点 ----
print("\n== 5. 重复任务锚点 ==")
# 先清空
status, body = post_tasks([])
due_future = "2099-12-31T23:59"
task = {"id": "test-repeat", "title": "repeat test", "estimate": 60, "due": due_future, "tags": [], "links": [], "done": False, "pinned": False, "repeat": "daily", "route_origin_date": due_future, "createdAt": "2026-01-01T00:00:00Z", "completedAt": None}
status, body = post_tasks([task])
t("重复任务 POST 200", status == 200, f"got {status}")

# 推迟
status, body = req("POST", "/api/snooze?id=test-repeat")
t("推迟一天 200", status == 200, f"got {status} {body[:200]}")

status, body = req("GET", "/api/tasks")
tasks_obj = json.loads(body)
found = [x for x in tasks_obj.get("tasks", []) if x.get("id") == "test-repeat"]
if found:
    rod = found[0].get("route_origin_date", "")
    due = found[0].get("due", "")
    t("锚点保留原值", rod == due_future, f"rod={rod} due={due}")
    t("due 已推迟", due != due_future, f"due={due}")

# ---- 6. 没有截止时间的推迟 ----
print("\n== 6. 没有截止时间的推迟 ==")
task = {"id": "test-no-due", "title": "no due", "estimate": 60, "due": None, "tags": [], "links": [], "done": False, "pinned": False, "repeat": "", "createdAt": "2026-01-01T00:00:00Z", "completedAt": None}
status, body = post_tasks([task])
t("无截止任务 POST", status == 200, f"got {status}")
status, body = req("POST", "/api/snooze?id=test-no-due")
t("无截止推迟返回 400", status == 400, f"got {status} {body[:200]}")

# ---- 7. 主题切换 ----
print("\n== 7. 主题切换 ==")
for mode_str, mode_num in [("light", 0), ("dark", 1), ("system", 2)]:
    status, body = req("POST", f"/api/theme?mode={mode_str}")
    t(f"theme={mode_str} POST 200", status == 200, f"got {status} {body[:100]}")
    resp = json.loads(body)
    t(f"theme={mode_str} mode={mode_num}", resp.get("themeMode") == mode_num, str(resp))

# ---- 8. 无效主题值 ----
print("\n== 8. 无效主题值 ==")
status, body = req("POST", "/api/theme?mode=invalid")
t("无效主题 200", status == 200, f"got {status} {body[:100]}")
resp = json.loads(body)
t("无效主题回退 system", resp.get("themeMode") == 2, str(resp))

# ---- 9. 非法 JSON ----
print("\n== 9. 非法 JSON ==")
status, body = req("POST", "/api/tasks", "{invalid json")
t("非法 JSON 返回 400", status == 400, f"got {status} {body[:200]}")

# ---- 10. tasks 不是数组 ----
print("\n== 10. tasks 不是数组 ==")
status, body = req("POST", "/api/tasks", json.dumps({"tasks": "not an array"}))
t("tasks 非数组返回 400", status == 400, f"got {status} {body[:200]}")

# ---- 11. 空数组提交 ----
print("\n== 11. 空数组提交 ==")
status, body = post_tasks([])
t("空数组提交 200", status == 200, f"got {status}")

# ---- 12. 设置接口 ----
print("\n== 12. 设置接口 ==")
status, body = req("GET", "/api/settings")
t("GET settings 200", status == 200, f"got {status}")
settings = json.loads(body)
t("settings 有 themeMode", "themeMode" in settings, str(settings)[:200])
t("settings 有 systemLight", "systemLight" in settings, str(settings)[:200])
t("settings 有 topmost", "topmost" in settings, str(settings)[:200])

# ---- 13. 不存在的 API 路径 ----
print("\n== 13. 不存在的 API 路径 ==")
status, body = req("GET", "/api/nonexistent")
t("不存在路径 404", status == 404, f"got {status}")

# ---- 14. 不存在的任务 id ----
print("\n== 14. 不存在的任务 id ==")
status, body = req("POST", "/api/toggle?id=nonexistent-id-xxx")
t("不存在 toggle", status != 200, f"got {status} {body[:200]}")

status, body = req("POST", "/api/snooze?id=nonexistent-id-xxx")
t("不存在 snooze", status == 404, f"got {status} {body[:200]}")

# ---- 15. 重复任务无截止时间 ----
print("\n== 15. 重复任务无截止时间 ==")
task = {"id": "test-repeat-nodue", "title": "repeat no due", "estimate": 60, "due": None, "tags": [], "links": [], "done": False, "pinned": False, "repeat": "daily", "createdAt": "2026-01-01T00:00:00Z", "completedAt": None}
status, body = post_tasks([task])
t("重复无截止 POST", status == 200, f"got {status}")
status, body = req("GET", "/api/tasks")
tasks_obj = json.loads(body)
found = [x for x in tasks_obj.get("tasks", []) if x.get("id") == "test-repeat-nodue"]
if found:
    t("重复无截止 repeat 被清空", not found[0].get("repeat"), f"repeat={found[0].get('repeat')}")

# ---- 16. 超大任务数据 ----
print("\n== 16. 超大任务数据（100 条）==")
big_tasks = []
for i in range(100):
    big_tasks.append({"id": f"flood-{i}", "title": f"Task {i}", "estimate": 60, "due": "2099-12-31T23:59", "tags": [], "links": [], "done": False, "pinned": i == 0, "repeat": "", "createdAt": "2026-01-01T00:00:00Z", "completedAt": None})
status, body = post_tasks(big_tasks)
t("100 条 POST 200", status == 200, f"got {status}")
status, body = req("GET", "/api/tasks")
tasks_obj = json.loads(body)
count = len(tasks_obj.get("tasks", []))
t("100 条存储", count >= 100, f"count={count}")

# ---- 17. 打开路径（POST）----
print("\n== 17. 打开不存在路径 ==")
status, body = req("POST", "/api/open?url=C:/nonexistent/path/file.txt")
t("不存在路径 open", status in [200, 500], f"got {status} {body[:200]}")

# ---- 18. 打开 web URL ----
print("\n== 18. 打开 web URL ==")
status, body = req("POST", "/api/open?url=https://example.com")
t("web URL open 200", status == 200, f"got {status} {body[:200]}")

# ---- 19. widget 数据接口 ----
print("\n== 19. widget 数据接口 ==")
status, body = req("GET", "/api/widget")
t("GET widget 200", status == 200, f"got {status}")
t("widget 有任务行", "flood-0" in body, "flood-0 not in widget data")

# ---- 20. 健康检查 ----
print("\n== 20. 健康检查 ==")
status, body = req("GET", "/api/health")
t("health 200", status == 200, f"got {status}")
health = json.loads(body)
t("health version=1.0.8", health.get("version") == "1.0.8", str(health)[:200])

# ---- 21. 置顶切换 ----
print("\n== 21. 置顶切换 ==")
status, body = req("POST", "/api/topmost?on=1")
t("topmost on 200", status == 200, f"got {status} {body[:100]}")
status, body = req("POST", "/api/topmost?on=0")
t("topmost off 200", status == 200, f"got {status} {body[:100]}")

# ---- 22. 并发请求 ----
print("\n== 22. 并发请求 ==")
errors = []
def concurrent_get():
    try:
        s, b = req("GET", "/api/tasks")
        if s != 200:
            errors.append(f"status={s}")
    except Exception as e:
        errors.append(str(e))

threads = [threading.Thread(target=concurrent_get) for _ in range(20)]
for th in threads:
    th.start()
for th in threads:
    th.join(timeout=5)
t("20 并发 GET 无错误", len(errors) == 0, f"errors={errors[:5]}")

# ---- 23. route_origin_date 保留 ----
print("\n== 23. route_origin_date 推迟保留 ==")
# 清空再放一条
post_tasks([])
origin = "2099-06-15T12:00"
due_val = "2099-06-15T12:00"
task = {"id": "test-origin", "title": "origin test", "estimate": 60, "due": due_val, "tags": [], "links": [], "done": False, "pinned": False, "repeat": "weekly", "route_origin_date": origin, "createdAt": "2026-01-01T00:00:00Z", "completedAt": None}
status, body = post_tasks([task])
t("带锚点 POST 200", status == 200, f"got {status}")
# 推迟后锚点不动
status, body = req("POST", "/api/snooze?id=test-origin")
t("推迟带锚点 200", status == 200, f"got {status}")
status, body = req("GET", "/api/tasks")
tasks_obj = json.loads(body)
found = [x for x in tasks_obj.get("tasks", []) if x.get("id") == "test-origin"]
if found:
    rod = found[0].get("route_origin_date", "")
    t("锚点保留原值", rod == origin, f"rod={rod} expected={origin}")

# ---- 24. 改期后锚点跟随 ----
print("\n== 24. 改期后锚点跟随 ==")
new_due = "2099-08-20T09:00"
task["due"] = new_due
status, body = post_tasks([task])
t("改期 POST 200", status == 200, f"got {status}")
status, body = req("GET", "/api/tasks")
tasks_obj = json.loads(body)
found = [x for x in tasks_obj.get("tasks", []) if x.get("id") == "test-origin"]
if found:
    rod = found[0].get("route_origin_date", "")
    t("改期后锚点更新", rod == new_due, f"rod={rod} expected={new_due}")

# ---- 25. 只改标题不改期，锚点不动 ----
print("\n== 25. 只改标题锚点不动 ==")
# 模拟控制台：先取服务端当前值，再只改标题回传
status, body = req("GET", "/api/tasks")
tasks_obj = json.loads(body)
found = [x for x in tasks_obj.get("tasks", []) if x.get("id") == "test-origin"]
current = dict(found[0]) if found else dict(task)
current["title"] = "changed title"
status, body = post_tasks([current])
t("改标题 POST 200", status == 200, f"got {status}")
status, body = req("GET", "/api/tasks")
tasks_obj = json.loads(body)
found = [x for x in tasks_obj.get("tasks", []) if x.get("id") == "test-origin"]
if found:
    rod = found[0].get("route_origin_date", "")
    t("改标题锚点不变", rod == new_due, f"rod={rod} expected={new_due}")

# 客户端拿着推迟前的旧锚点回传，也不能把节奏改回去
stale = dict(current)
stale["route_origin_date"] = origin
status, body = post_tasks([stale])
t("旧锚点回传 POST 200", status == 200, f"got {status}")
status, body = req("GET", "/api/tasks")
tasks_obj = json.loads(body)
found = [x for x in tasks_obj.get("tasks", []) if x.get("id") == "test-origin"]
if found:
    rod = found[0].get("route_origin_date", "")
    t("旧锚点不覆盖磁盘值", rod == new_due, f"rod={rod} expected={new_due}")

# ---- 26. 极端 themeMode ----
print("\n== 26. 极端 themeMode ==")
status, body = req("POST", "/api/settings", json.dumps({"themeMode": -1}))
t("themeMode=-1", status == 200, f"got {status} {body[:100]}")
resp = json.loads(body)
# -1 不在 0-2 范围内，appSetThemeMode 应忽略
t("themeMode=-1 忽略", "themeMode" not in resp or resp.get("themeMode") in [0, 1, 2], str(resp))

status, body = req("POST", "/api/settings", json.dumps({"themeMode": 99}))
t("themeMode=99", status == 200, f"got {status} {body[:100]}")
resp = json.loads(body)
t("themeMode=99 忽略", "themeMode" not in resp or resp.get("themeMode") in [0, 1, 2], str(resp))

# ---- 27. chunked 编码被拒 ----
print("\n== 27. chunked 编码 ==")
c = http.client.HTTPConnection(HOST, PORT, timeout=5)
c.request("POST", "/api/tasks", body='{"tasks":[]}', headers={"Content-Type": "application/json", "Transfer-Encoding": "chunked"})
r = c.getresponse()
chunked_status = r.status
chunked_body = r.read().decode("utf-8", errors="replace")
c.close()
t("chunked 被拒 (411)", chunked_status == 411, f"got {chunked_status} {chunked_body[:100]}")

# ---- 28. 来源于校验 ----
print("\n== 28. 来源校验 ==")
# 正常带 Origin 的请求
status, body = req("POST", "/api/tasks", '{"tasks":[]}', {"Origin": "http://127.0.0.1:8770"})
t("带 Origin 请求", status == 200, f"got {status}")

# ---- 29. toggle 重复任务 ----
print("\n== 29. toggle 重复任务完成 ==")
post_tasks([])
task = {"id": "test-toggle-repeat", "title": "toggle repeat", "estimate": 60, "due": "2099-12-31T23:59", "tags": [], "links": [], "done": False, "pinned": False, "repeat": "daily", "route_origin_date": "2099-12-31T23:59", "createdAt": "2026-01-01T00:00:00Z", "completedAt": None}
post_tasks([task])
status, body = req("POST", "/api/toggle?id=test-toggle-repeat")
t("toggle 重复任务", status == 200, f"got {status} {body[:200]}")
status, body = req("GET", "/api/tasks")
tasks_obj = json.loads(body)
# 完成后应该派生出下一期
has_done = any(x.get("id") == "test-toggle-repeat" and x.get("done") for x in tasks_obj.get("tasks", []))
has_next = any(x.get("title") == "toggle repeat" and not x.get("done") for x in tasks_obj.get("tasks", []))
t("原任务标记完成", has_done, f"tasks={tasks_obj.get('tasks', [])[:3]}")
t("派生下一期", has_next, "")

# ---- 30. settings.json 持久化 ----
print("\n== 30. settings.json 持久化 ==")
# 改一次主题
req("POST", "/api/theme?mode=dark")
# 读文件
settings_path = "build/test/boundary/data/settings.json"
try:
    with open(settings_path, "r") as f:
        raw = f.read()
    t("settings.json 有 themeMode", "themeMode" in raw, raw[:200])
    t("settings.json themeMode=1", '"themeMode": 1' in raw, raw[:200])
except Exception as e:
    t("settings.json 读取", False, str(e))

# ---- 31. 旧格式 settings.json 兼容 ----
print("\n== 31. 旧格式 settings.json 兼容 ==")
# 兼容换算只在 loadSettings()（启动）时做，所以要另起一个干净实例
def free_port():
    s = socket.socket()
    s.bind((HOST, 0))
    p = s.getsockname()[1]
    s.close()
    return p

legacy_root = os.path.abspath("build/test/boundary-legacy")
legacy_data = os.path.join(legacy_root, "data")
os.makedirs(legacy_data, exist_ok=True)
with open(os.path.join(legacy_data, "settings.json"), "w") as f:
    f.write('{\n  "widget": true,\n  "topmost": true,\n  "autostart": false,\n  "themeLight": true\n}\n')

legacy_port = free_port()
exe = os.path.abspath("GoGoSpot.exe")
proc = subprocess.Popen(
    [exe, "--no-browser", "--port", str(legacy_port),
     "--data", os.path.join(legacy_data, "tasks.json")],
    cwd=os.getcwd(), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    legacy_info = None
    for _ in range(60):
        try:
            s, b = req("GET", "/api/info", port=legacy_port)
            if s == 200:
                legacy_info = json.loads(b)
                break
        except Exception:
            pass
        time.sleep(0.25)

    if not legacy_info or "boundary-legacy" not in legacy_info.get("dataFile", ""):
        t("旧格式实例启动", False, f"info={legacy_info}")
    else:
        s, b = req("GET", "/api/settings", port=legacy_port)
        settings = json.loads(b)
        t("旧 themeLight=true → themeMode=0", settings.get("themeMode") == 0, str(settings)[:200])
finally:
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()

# ---- 汇总 ----
print("\n" + "=" * 60)
passed = sum(1 for _, c, _ in results if c)
failed = sum(1 for _, c, _ in results if not c)
total = len(results)
print(f"总计: {passed} PASS / {failed} FAIL / {total} 项")
if failed:
    print("\n失败项:")
    for name, cond, detail in results:
        if not cond:
            print(f"  FAIL: {name} -- {detail}")
