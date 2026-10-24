# 接口测试第二批：协议边界、备份、CSRF、并发、重复任务派生
import http.client, json, socket, time, threading, urllib.parse, os

HOST, PORT = '127.0.0.1', 8765
RESULTS = []


def req(method, path, body=None, headers=None, timeout=15):
    c = http.client.HTTPConnection(HOST, PORT, timeout=timeout)
    if isinstance(body, str):
        body = body.encode('utf-8')
    c.request(method, path, body=body, headers=headers or {})
    r = c.getresponse()
    data = r.read()
    hd = dict((k.lower(), v) for k, v in r.getheaders())
    c.close()
    return r.status, hd, data


def check(name, ok, detail=''):
    RESULTS.append((name, bool(ok), detail))
    print(('PASS ' if ok else 'FAIL ') + name + (('  | ' + str(detail)) if detail else ''))


def jbody(data):
    try:
        return json.loads(data.decode('utf-8', 'replace'))
    except Exception as e:
        return {'__parse_error__': str(e)}


def put_tasks(tasks, extra=None):
    payload = {'schema': 1, 'tasks': tasks}
    if extra:
        payload.update(extra)
    return req('PUT', '/api/tasks', json.dumps(payload, ensure_ascii=False),
               {'Content-Type': 'application/json'})


def task(i, **kw):
    t = {'id': 'id-%03d' % i, 'title': '任务 %d' % i, 'done': False,
         'estimate': 30, 'due': None, 'tags': [], 'links': [], 'startedAt': 0,
         'repeat': '', 'createdAt': '2026-10-24T00:00:00.000Z', 'completedAt': None}
    t.update(kw)
    return t


print('=== E. 协议边界 ===')
s, h, d = req('GET', '/api/health')
check('进程存活', s == 200, str(s))

# HEAD：服务端把 HEAD 当 GET，但客户端按规范不读体。用裸 socket 看实际字节
sk = socket.create_connection((HOST, PORT), timeout=5)
sk.sendall(b'HEAD /api/health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n')
raw = b''
while True:
    b = sk.recv(4096)
    if not b:
        break
    raw += b
sk.close()
head, _, body = raw.partition(b'\r\n\r\n')
check('HEAD 只发响应头', len(body) == 0, 'body=%d 字节' % len(body))

# 413：只发头不发体，看服务端是否立刻回 413
sk = socket.create_connection((HOST, PORT), timeout=5)
sk.sendall(b'PUT /api/tasks HTTP/1.1\r\nHost: x\r\nContent-Length: 20000000\r\n\r\n')
try:
    raw = sk.recv(4096)
except Exception as e:
    raw = b''
sk.close()
check('超大 Content-Length 立刻回 413', raw.startswith(b'HTTP/1.1 413'), raw[:60])

# chunked
c = http.client.HTTPConnection(HOST, PORT, timeout=10)
c.putrequest('PUT', '/api/tasks')
c.putheader('Transfer-Encoding', 'chunked')
c.putheader('Content-Type', 'application/json')
c.endheaders()
c.send(b'10\r\n{"tasks":[]}\r\n0\r\n\r\n')
r = c.getresponse()
d = r.read()
c.close()
check('chunked 请求被拒绝（411 并说明原因）', r.status == 411, 'got %s %s' % (r.status, d[:60]))

# 超长请求行
try:
    s, h, d = req('GET', '/?' + 'a' * 70000, timeout=10)
    check('70KB 请求行 → 4xx', s in (400, 404, 414), 'got %s' % s)
except Exception as e:
    check('70KB 请求行 → 4xx', True, '连接被断开：%s' % type(e).__name__)

print('\n=== F. 备份 ===')
put_tasks([task(1)])
req('POST', '/api/backup')
req('POST', '/api/backup')
s, h, d = req('GET', '/api/backups')
bl = jbody(d).get('backups', [])
names = [b['name'] for b in bl]
check('连续两次备份都留下（文件名带毫秒）', len(bl) >= 2 and names[0] != names[1],
      '当前 %d 份，最新 %s' % (len(bl), names[:2]))

for n in ['../tasks.json', '..\\tasks.json', 'a/b.json', '', 'x.txt']:
    s, h, d = req('POST', '/api/deletebackup?name=' + urllib.parse.quote(n))
    check('deletebackup name=%r → 400' % n, s == 400, 'got %s' % s)

bad = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'test', 'run2',
                   'data', 'backups', 'tasks-19990101-000000.json')
os.makedirs(os.path.dirname(bad), exist_ok=True)
open(bad, 'w', encoding='utf-8').write('{"tasks": [ broken')
s, h, d = req('POST', '/api/restore?name=tasks-19990101-000000.json')
check('损坏的备份被拒（恢复前先校验）', s == 400, 'got %s %s' % (s, d[:60]))
s, h, d = req('GET', '/api/tasks')
check('恢复失败后数据完好', jbody(d).get('__parse_error__') is None, d[:40])

print('\n=== G. 设置与 CSRF ===')
s, h, d = req('POST', '/api/settings', '{"widget":', {'Content-Type': 'application/json'})
check('设置缺值 → 400', s == 400, str(s))
s, h, d = req('POST', '/api/settings', '{"widget":false}', {'Content-Type': 'application/json'})
check('widget=false 生效', jbody(d).get('widget') is False, str(d[:80]))
s, h, d = req('POST', '/api/settings', '{"widget":true}', {'Content-Type': 'application/json'})
check('widget=true 还原', jbody(d).get('widget') is True, str(d[:80]))

put_tasks([task(1)])
s, h, d = req('POST', '/api/add?title=csrf', None, {'Origin': 'http://evil.example'})
check('跨站 POST /api/add 被 403 拦下', s == 403, 'got %s' % s)
s, h, d = req('PUT', '/api/tasks', '{"tasks":[]}', {'Origin': 'http://evil.example',
                                                   'Content-Type': 'text/plain'})
check('跨站 PUT /api/tasks 被 403 拦下', s == 403, 'got %s' % s)
s, h, d = req('POST', '/api/open?url=http://example.com', '{}',
              {'Origin': 'http://evil.example', 'Content-Type': 'application/json'})
check('跨站 /api/open 被 403 拦下', s == 403, 'got %s' % s)
s, h, d = req('POST', '/api/open?url=', '{}', {'Content-Type': 'application/json'})
check('空 url → 400', s == 400, str(s))

print('\n=== H. 并发 ===')
put_tasks([])
errs = []
lock = threading.Lock()


def add_one(i):
    try:
        st, hh, dd = req('POST', '/api/add?' + urllib.parse.urlencode({'title': 'c%d' % i}), timeout=20)
        if st != 200:
            with lock:
                errs.append((i, st, dd[:60]))
    except Exception as e:
        with lock:
            errs.append((i, 'exc', str(e)))


ths = [threading.Thread(target=add_one, args=(i,)) for i in range(40)]
[t.start() for t in ths]
[t.join() for t in ths]
s, h, d = req('GET', '/api/tasks')
n = len(jbody(d).get('tasks', []))
check('40 并发 add 全部落盘', n == 40 and not errs, 'count=%d errs=%s' % (n, errs[:3]))

stop = False


def reader():
    while not stop:
        try:
            req('GET', '/api/tasks', timeout=5)
        except Exception:
            pass


rt = threading.Thread(target=reader)
rt.start()
for i in range(20):
    put_tasks([task(j) for j in range(50)])
stop = True
rt.join()
s, h, d = req('GET', '/api/tasks')
ok = len(jbody(d).get('tasks', [])) == 50
check('并发读 + 连续整份写后数据完好', ok, 'len=%s' % len(jbody(d).get('tasks', [])))

print('\n=== I. 重复任务派生 ===')
put_tasks([task(i, repeat='weekly', due='2026-10-20T09:00') for i in range(8)])
s, h, d = req('POST', '/api/toggle?id=id-000')
s2, h2, d2 = req('GET', '/api/tasks')
ts = jbody(d2).get('tasks', [])
orig = [t for t in ts if t['id'] == 'id-000'][0]
new = [t for t in ts if not t['id'].startswith('id-')]
check('toggle 后派生了一条', len(ts) == 9, 'len=%d' % len(ts))
check('原任务 repeat 被清空（悬垂引用会写丢）', orig['repeat'] == '', 'repeat=%r' % orig['repeat'])
check('新任务 repeat 继承', len(new) == 1 and new[0]['repeat'] == 'weekly', str(new[:1])[:120])
check('新任务 due +7 天', len(new) == 1 and new[0]['due'] == '2026-10-27T09:00', new[0]['due'] if new else '')

put_tasks([task(i, repeat='daily', due='2026-10-01T09:00') for i in range(8)])
print('     等 22 秒让后台扫描跑一轮…')
time.sleep(22)
s, h, d = req('GET', '/api/health')
alive = s == 200
check('逾期扫描后进程仍存活', alive, 'health=%s' % s)
if alive:
    s2, h2, d2 = req('GET', '/api/tasks')
    ts = jbody(d2).get('tasks', [])
    check('8 条逾期任务都派生了下一期', len(ts) == 16, 'len=%d' % len(ts))
    cleared = sum(1 for t in ts if t['id'].startswith('id-') and t['repeat'] == '')
    check('原任务 repeat 都被清空', cleared == 8, 'cleared=%d' % cleared)

print('\n=== 汇总 ===')
bad = [r for r in RESULTS if not r[1]]
print('%d 项检查，%d 项失败' % (len(RESULTS), len(bad)))
for n, ok, det in bad:
    print('  FAIL', n, '|', det)
