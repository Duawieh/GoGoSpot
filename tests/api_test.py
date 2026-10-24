# GoGoSpot 接口极限测试：正常路径 + 畸形输入 + 并发 + 重复任务派生
# 只打 127.0.0.1:8765 的隔离实例，不碰真实 data/。

import http.client, json, time, threading, urllib.parse, os, sys

HOST, PORT = '127.0.0.1', 8765
RESULTS = []


def req(method, path, body=None, headers=None, timeout=15):
    c = http.client.HTTPConnection(HOST, PORT, timeout=timeout)
    h = dict(headers or {})
    if isinstance(body, str):
        body = body.encode('utf-8')
    c.request(method, path, body=body, headers=h)
    r = c.getresponse()
    data = r.read()
    hd = dict((k.lower(), v) for k, v in r.getheaders())
    c.close()
    return r.status, hd, data


def check(name, ok, detail=''):
    RESULTS.append((name, bool(ok), detail))
    print(('PASS ' if ok else 'FAIL ') + name + (('  | ' + str(detail)) if detail else ''))


def text(data):
    return data.decode('utf-8', 'replace')


def jbody(data):
    try:
        return json.loads(data.decode('utf-8'))
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


print('=== A. 基本接口 ===')
put_tasks([])
s, h, d = req('GET', '/api/health')
check('health 200 + 应用名', s == 200 and jbody(d).get('app') == 'GoGoSpot', '%s %s' % (s, d[:80]))

s, h, d = req('GET', '/api/info')
info = jbody(d)
check('info 返回版本 1.0.3', info.get('version') == '1.0.3', str(info)[:120])
check('info 路径可被 JSON 解析', '__parse_error__' not in info)

s, h, d = req('GET', '/api/widget')
check('widget 空库返回 #0 0', text(d).startswith('#0\t0'), repr(text(d)[:40]))

# 建一条完整任务
q = urllib.parse.urlencode({'title': '写报告', 'due': '2026-10-25T18:30',
                            'estimate': '60', 'links': 'https://example.com|D:\\a b\\c.txt'})
s, h, d = req('POST', '/api/add?' + q)
check('add 200', s == 200 and jbody(d).get('ok') is True, str(d[:120]))
new_id = jbody(d).get('id', '')

s, h, d = req('GET', '/api/tasks')
data = jbody(d)
t0 = data['tasks'][0]
check('add 后字段齐全', t0['title'] == '写报告' and t0['estimate'] == 60 and
      t0['due'] == '2026-10-25T18:30' and len(t0['links']) == 2, json.dumps(t0, ensure_ascii=False)[:200])

s, h, d = req('GET', '/api/widget')
line = text(d).splitlines()[1].split('\t')
check('widget 行 8 列（末列是置顶）', len(line) == 8, repr(line))
check('widget 链接用竖线拼接', line[5].count('|') == 1, line[5])
check('widget 标题正确', line[4] == '写报告', line[4])

print('\n=== B. 输入边界 ===')
s, h, d = req('POST', '/api/add?title=')
check('空标题 400', s == 400, '%s %s' % (s, d[:60]))
s, h, d = req('POST', '/api/add?title=%20%20%20')
check('纯空白标题 400', s == 400, '%s %s' % (s, d[:60]))

# 2000 字节 ASCII
s, h, d = req('POST', '/api/add?' + urllib.parse.urlencode({'title': 'A' * 2000}))
s2, h2, d2 = req('GET', '/api/tasks')
check('2000 字节标题接受', s == 200 and len(jbody(d2)['tasks'][-1]['title']) == 2000, str(s))

# 2100 字节中文（700 字）→ 服务端按字节截断，可能切在多字节中间
s, h, d = req('POST', '/api/add?' + urllib.parse.urlencode({'title': '汉' * 700}))
s2, h2, d2 = req('GET', '/api/tasks')
raw_ok = True
try:
    json.loads(d2.decode('utf-8'))
except Exception as e:
    raw_ok = False
check('超长中文标题后 /api/tasks 仍是合法 UTF-8', raw_ok, '解析失败说明截断切坏了多字节字符' if not raw_ok else '')
if not raw_ok:
    # 找出坏在哪
    try:
        d2.decode('utf-8')
    except UnicodeDecodeError as e:
        print('     UnicodeDecodeError:', e)

put_tasks([task(0)])   # 上面故意写坏了，重置回可用状态

# 控制字符 / 制表符
s, h, d = req('POST', '/api/add?' + urllib.parse.urlencode({'title': 'a\tb\nc\rd'}))
s2, h2, d2 = req('GET', '/api/widget')
last = text(d2).splitlines()[-1].split('\t')
check('标题里的制表/换行不破坏 widget 协议', len(last) == 8, '列数=%d' % len(last))

# estimate 边界
for est, want in [(-5, 0), (0, 0), (99999, 1440), ('abc', 0)]:
    req('POST', '/api/add?' + urllib.parse.urlencode({'title': 'est ' + str(est), 'estimate': str(est)}))
    s, h, d = req('GET', '/api/tasks')
    got = jbody(d)['tasks'][-1]['estimate']
    check('estimate=%s 夹到 %s' % (est, want), got == want, 'got=%s' % got)

# due 边界
for due, want in [('2026-13-01T00:00', None), ('2026-02-30T10:00', None), ('1969-01-01T00:00', None),
                  ('2201-01-01T00:00', None), ('2026-10-24', None), ('2026-10-24T25:61', None)]:
    req('POST', '/api/add?' + urllib.parse.urlencode({'title': 'due ' + due, 'due': due}))
    s, h, d = req('GET', '/api/tasks')
    got = jbody(d)['tasks'][-1]['due']
    check('due=%s → %s' % (due, want), got == want, 'got=%r' % got)

# links 上限
links = '|'.join('u%d' % i for i in range(10))
req('POST', '/api/add?' + urllib.parse.urlencode({'title': 'links', 'links': links}))
s, h, d = req('GET', '/api/tasks')
check('links 最多 8 条', len(jbody(d)['tasks'][-1]['links']) == 8, str(len(jbody(d)['tasks'][-1]['links'])))

# 不存在的 id
for path in ['/api/toggle?id=nope', '/api/snooze?id=nope', '/api/remove?id=nope']:
    s, h, d = req('POST', path)
    check('%s → 404' % path, s == 404, '%s %s' % (s, d[:60]))
s, h, d = req('POST', '/api/toggle')
check('toggle 缺 id → 400', s == 400, str(s))
s, h, d = req('GET', '/api/toggle?id=x')
check('GET toggle → 405', s == 405, str(s))

print('\n=== C. 数据文件被写坏后的表现 ===')
put_tasks([task(1)])
s, h, d = req('PUT', '/api/tasks', '[1,2]', {'Content-Type': 'application/json'})
check('PUT 顶层数组被拒（要求 tasks 数组）', s == 400, str(s))
s, h, d = req('GET', '/api/widget')
check('被拒之后原数据没被写坏', s == 200, '%s %s' % (s, d[:60]))

s, h, d = req('PUT', '/api/tasks', '{"tasks":[]}garbage', {'Content-Type': 'application/json'})
check('尾部带垃圾的 JSON 被拒', s == 400, str(s))
s, h, d = req('GET', '/api/tasks')
check('之后 GET 仍是合法 JSON', jbody(d).get('__parse_error__') is None, d[:60])

put_tasks([task(1), task(2)])
s, h, d = req('GET', '/api/export')
check('export 带下载头', 'content-disposition' in h and 'gogospot-' in h['content-disposition'], h.get('content-disposition', ''))

print('\n=== D. 静态文件与路径穿越 ===')
for p, want in [('/../data/tasks.json', 400), ('/..%2f..%2fdata%2ftasks.json', 400),
                ('/%2e%2e/data/tasks.json', 404), ('/C:/Windows/win.ini', 404),
                ('/web/app.js', 404), ('/', 200), ('/index.html', 200), ('/app.js', 200),
                ('/nope.js', 404), ('/app.js?v=1', 200)]:
    s, h, d = req('GET', p)
    check('GET %s → %s' % (p, want), s == want, 'got %s' % s)

# HEAD 的实际行为在 api_test2.py 用裸 socket 验证（http.client 会按规范丢掉体）

# 静态文件方法限制
s, h, d = req('POST', '/index.html')
check('POST 静态文件 → 405', s == 405, str(s))

print('\n=== E. 请求体与协议边界 ===')
big = '{"tasks":[' + ','.join(['{"id":"x%d","title":"%s"}' % (i, 'a' * 100) for i in range(200000)]) + ']}'
try:
    s, h, d = req('PUT', '/api/tasks', big.encode(), {'Content-Type': 'application/json'}, timeout=30)
    check('超过 16MB 的请求体 → 413', s == 413, 'size=%.1fMB got %s' % (len(big) / 1048576.0, s))
except Exception as e:
    check('超过 16MB 的请求体 → 413', False,
          'size=%.1fMB，客户端收到 %s（服务端先关连接）' % (len(big) / 1048576.0, type(e).__name__))

# chunked 编码：服务端不解析 Transfer-Encoding
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
    s, h, d = req('GET', '/?' + 'a' * 70000)
    check('70KB 请求行被拒（当前只是 404/400）', s in (400, 414, 404), 'got %s' % s)
except Exception as e:
    check('70KB 请求行', True, '连接被断开：%s' % type(e).__name__)

print('\n=== F. 备份 ===')
put_tasks([task(1)])
before = len(jbody(req('GET', '/api/backups')[2]).get('backups', []))
req('POST', '/api/backup')
req('POST', '/api/backup')
s, h, d = req('GET', '/api/backups')
bl = jbody(d).get('backups', [])
names = [b['name'] for b in bl]
# 备份目录有份数上限，满了以后总数不再增长，只能比最新的两个名字
check('同一秒内两次备份都留下（文件名带毫秒）',
      len(names) >= 2 and names[0] != names[1] and names[1] != names[2] if len(names) >= 3 else False,
      '%d -> %d %s' % (before, len(bl), names[:2]))
print('     备份名:', names[:4])

# 备份名穿越
for n in ['../tasks.json', '..\\tasks.json', 'a/b.json', '', 'x.txt']:
    s, h, d = req('POST', '/api/deletebackup?name=' + urllib.parse.quote(n))
    check('deletebackup name=%r → 400' % n, s == 400, 'got %s' % s)

# 恢复一份被手工写坏的备份
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

# 跨站请求：带恶意 Origin 的写操作
put_tasks([task(1)])   # 恢复测试把文件写坏了，先重置再测 CSRF
s, h, d = req('POST', '/api/add?title=csrf', None, {'Origin': 'http://evil.example'})
check('跨站 POST /api/add 被 403 拦下', s == 403, 'got %s' % s)
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
n = len(jbody(d)['tasks'])
check('40 并发 add 全部落盘', n == 40 and not errs, 'count=%d errs=%s' % (n, errs[:3]))

# 并发读写同一份数据
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
ok = jbody(d).get('tasks') is not None and len(jbody(d)['tasks']) == 50
check('并发读 + 连续整份写后数据完好', ok, 'len=%s' % len(jbody(d).get('tasks', [])))

print('\n=== I. 重复任务派生 ===')
# 8 条（2 的幂，push_back 很可能触发扩容）带 repeat 的任务
base = [task(i, repeat='weekly', due='2026-10-20T09:00') for i in range(8)]
put_tasks(base)
s, h, d = req('POST', '/api/toggle?id=id-000')
s2, h2, d2 = req('GET', '/api/tasks')
ts = jbody(d2)['tasks']
orig = [t for t in ts if t['id'] == 'id-000'][0]
new = [t for t in ts if not t['id'].startswith('id-')]
check('toggle 后派生了一条', len(ts) == 9, 'len=%d' % len(ts))
check('原任务 repeat 被清空（悬垂引用会写丢）', orig['repeat'] == '', 'repeat=%r' % orig['repeat'])
check('新任务 repeat 继承', len(new) == 1 and new[0]['repeat'] == 'weekly', str(new[:1])[:120])
check('新任务 due +7 天', len(new) == 1 and new[0]['due'] == '2026-10-27T09:00', new[0]['due'] if new else '')

# 逾期扫描：8 条一起派生
put_tasks([task(i, repeat='daily', due='2026-10-01T09:00') for i in range(8)])
print('     等 22 秒让后台扫描跑一轮…')
time.sleep(22)
s, h, d = req('GET', '/api/health')
alive = s == 200
check('逾期扫描后进程仍存活', alive, 'health=%s' % s)
if alive:
    s2, h2, d2 = req('GET', '/api/tasks')
    ts = jbody(d2)['tasks']
    check('8 条逾期任务都派生了下一期', len(ts) == 16, 'len=%d' % len(ts))
    cleared = sum(1 for t in ts if t['id'].startswith('id-') and t['repeat'] == '')
    check('原任务 repeat 都被清空', cleared == 8, 'cleared=%d' % cleared)

print('\n=== 汇总 ===')
bad = [r for r in RESULTS if not r[1]]
print('%d 项检查，%d 项失败' % (len(RESULTS), len(bad)))
for n, ok, det in bad:
    print('  FAIL', n, '|', det)
