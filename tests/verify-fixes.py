# 1.0.1 修复回归：逐条对应复测报告里的 P0 / P1 编号
# 只打 127.0.0.1:8765 的隔离副本（build/test/run2），不碰真实 data/。

import http.client
import json
import os
import socket
import time
import urllib.parse

HOST, PORT = '127.0.0.1', int(os.environ.get('GOGOSPOT_PORT', '8765'))
RESULTS = []


def req(method, path, body=None, headers=None, timeout=30):
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


# 数据文件与备份目录从 /api/info 取，不写死某个测试目录
_info = json.loads(req('GET', '/api/info')[2].decode('utf-8'))
DATA = (_info.get('dataFile') or '').replace('/', os.sep)
BACKUPS = (_info.get('backupDir') or '').replace('/', os.sep)


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


def reset(tasks=None):
    put_tasks(tasks or [])


def iso(days_ago=0, hour=9):
    t = time.time() - days_ago * 86400
    lt = time.localtime(t)
    return '%04d-%02d-%02dT%02d:%02d' % (lt.tm_year, lt.tm_mon, lt.tm_mday, hour, 0)


print('=== 前置：清空任务 ===')
reset()

# ---------------------------------------------------------------- P0-1
print('\n=== P0-1 任务 id 不再重复 ===')
ids = []
for i in range(6):
    st, _, body = req('POST', '/api/add?title=' + urllib.parse.quote('并发任务%d' % i))
    ids.append(jbody(body).get('id'))
check('连续 6 次 /api/add 拿到 6 个不同 id', len(set(ids)) == 6, ids)
check('id 形状是 8-4-4-4-12', all(len(x) == 36 and x.count('-') == 4 for x in ids if x), ids)

# 并发
import threading
conc = []
lock = threading.Lock()


def add_one(n):
    st, _, body = req('POST', '/api/add?title=' + urllib.parse.quote('并发%d' % n))
    with lock:
        conc.append(jbody(body).get('id'))


ts = [threading.Thread(target=add_one, args=(i,)) for i in range(20)]
[t.start() for t in ts]
[t.join() for t in ts]
check('20 并发 /api/add 拿到 20 个不同 id', len(set(conc)) == 20, len(set(conc)))

# 勾第二条不会勾错第一条
reset([{'id': 'aaa', 'title': '甲任务', 'done': False},
       {'id': 'bbb', 'title': '乙任务', 'done': False}])
req('POST', '/api/toggle?id=bbb')
st, _, body = req('GET', '/api/tasks')
d = jbody(body)
by = {t['id']: t for t in d.get('tasks', [])}
check('勾乙任务只改乙', by['bbb']['done'] is True and by['aaa']['done'] is False,
      {'甲': by['aaa']['done'], '乙': by['bbb']['done']})

# ---------------------------------------------------------------- P0-4
print('\n=== P0-4 超长中文标题不再写坏文件 ===')
long_title = '汉' * 700
st, _, body = req('POST', '/api/add?title=' + urllib.parse.quote(long_title))
check('700 个汉字标题返回 200', st == 200, st)
st, _, body = req('GET', '/api/tasks')
ok = False
try:
    d = json.loads(body.decode('utf-8'))
    ok = isinstance(d.get('tasks'), list) and any(t.get('title', '').startswith('汉') for t in d['tasks'])
except Exception as e:
    ok = False
check('/api/tasks 仍是合法 JSON', ok)
with open(DATA, 'rb') as f:
    raw = f.read()
try:
    raw.decode('utf-8')
    good = True
except UnicodeDecodeError as e:
    good = False
    print('  decode error:', e)
check('tasks.json 整体是合法 UTF-8', good)

# 非法 UTF-8 字节也不该写进去
st, _, _ = req('POST', '/api/add?title=%FF%FEabc')
st, _, body = req('GET', '/api/tasks')
try:
    json.loads(body.decode('utf-8'))
    good = True
except Exception:
    good = False
check('非法字节标题不污染文件', good)

# ---------------------------------------------------------------- P1-5
print('\n=== P1-5 非法日期被拒 ===')
bad = ['2026-13-01T10:00', '2026-02-30T10:00', '1969-01-01T10:00',
       '2201-01-01T10:00', '2026-05-05T25:61', '2026-04-31T09:00']
for b in bad:
    reset()
    req('POST', '/api/add?title=x&due=' + urllib.parse.quote(b))
    st, _, body = req('GET', '/api/tasks')
    t = jbody(body)['tasks'][0]
    check('拒绝 ' + b, t.get('due') is None, t.get('due'))
for b in ['2026-02-28T10:00', '2024-02-29T23:59', '2026-12-31T00:00']:
    reset()
    req('POST', '/api/add?title=x&due=' + urllib.parse.quote(b))
    st, _, body = req('GET', '/api/tasks')
    t = jbody(body)['tasks'][0]
    check('接受 ' + b, t.get('due') == b, t.get('due'))

# ---------------------------------------------------------------- P1-2
print('\n=== P1-2 写接口校验来源 ===')
reset()
st, _, _ = req('POST', '/api/add?title=x', headers={'Origin': 'http://evil.example'})
check('恶意 Origin 被拒 403', st == 403, st)
st, _, _ = req('POST', '/api/add?title=x', headers={'Origin': 'null'})
check('Origin: null 被拒', st == 403, st)
st, _, _ = req('POST', '/api/add?title=x', headers={'Origin': 'http://127.0.0.1:8765'})
check('本机 Origin 放行', st == 200, st)
st, _, _ = req('POST', '/api/add?title=x')
check('无 Origin（命令行）放行', st == 200, st)
st, _, _ = req('POST', '/api/add?title=x', headers={'Sec-Fetch-Site': 'cross-site'})
check('Sec-Fetch-Site: cross-site 被拒', st == 403, st)
st, _, _ = req('PUT', '/api/tasks', json.dumps({'schema': 1, 'tasks': []}),
               {'Content-Type': 'text/plain', 'Origin': 'http://evil.example'})
check('跨站 PUT /api/tasks 被拒', st == 403, st)
st, _, _ = req('GET', '/api/tasks', headers={'Origin': 'http://evil.example'})
check('GET 不受影响', st == 200, st)

# Host 头（DNS rebinding）：读接口也要挡，且必须整段匹配
st, _, _ = req('POST', '/api/add?title=x', headers={'Host': 'evil.example'})
check('陌生 Host 被拒', st == 403, st)
st, _, _ = req('GET', '/api/tasks', headers={'Host': 'evil.com:8765'})
check('GET 也查 Host（挡 rebinding）', st == 403, st)
st, _, _ = req('GET', '/api/info', headers={'Host': 'evil.com'})
check('GET /api/info 同样挡', st == 403, st)
st, _, _ = req('GET', '/api/tasks', headers={'Host': '127.0.0.1.evil.com'})
check('Host 子串不算本机', st == 403, st)
st, _, _ = req('GET', '/api/tasks', headers={'Host': 'localhost.evil.com'})
check('Host localhost.evil.com 被拒', st == 403, st)
st, _, _ = req('GET', '/api/tasks', headers={'Host': '127.0.0.1:8765'})
check('本机 Host 放行', st == 200, st)
st, _, _ = req('GET', '/api/tasks', headers={'Host': 'localhost:8765'})
check('localhost 放行', st == 200, st)

st, _, _ = req('POST', '/api/add?title=x', headers={'Origin': 'http://127.0.0.1.evil.com'})
check('Origin 子串不算本机', st == 403, st)
st, _, _ = req('POST', '/api/add?title=x', headers={'Origin': 'http://localhost.evil.com'})
check('Origin localhost.evil.com 被拒', st == 403, st)
st, _, _ = req('POST', '/api/add?title=x', headers={'Origin': 'http://evil.com/?x=127.0.0.1'})
check('Origin 路径里带 127.0.0.1 也不算', st == 403, st)
st, _, _ = req('POST', '/api/add?title=x', headers={'Origin': 'http://localhost:8765'})
check('Origin localhost 放行', st == 200, st)

# ---------------------------------------------------------------- P0-3 / P0-2
print('\n=== P0-2 逾期重复任务收敛 ===')
reset()
for i in range(8):
    st, _, _ = req('POST', '/api/add?title=' + urllib.parse.quote('逾期%d' % i) +
                   '&due=' + urllib.parse.quote('2026-10-01T09:00'))
# 直接给它们打上 repeat=daily
st, _, body = req('GET', '/api/tasks')
d = jbody(body)
for t in d['tasks']:
    t['repeat'] = 'daily'
put_tasks(d['tasks'])

print('  等一轮后台扫描（约 22 秒）…')
time.sleep(22)
st, _, body = req('GET', '/api/tasks')
d1 = jbody(body)['tasks']
n1 = len(d1)
cleared1 = sum(1 for t in d1 if not t.get('repeat'))
check('8 条 → 16 条', n1 == 16, n1)
check('8 条原任务的 repeat 全部清空', cleared1 == 8, cleared1)
future = [t for t in d1 if t.get('repeat')]
check('派生的 8 条 due 都在将来', all(t['due'] > '2026-10-24' for t in future),
      [t['due'] for t in future][:3])

print('  再等一轮，确认不再增长…')
time.sleep(22)
st, _, body = req('GET', '/api/tasks')
n2 = len(jbody(body)['tasks'])
check('第二轮不再派生', n2 == n1, '%d → %d' % (n1, n2))

print('\n=== P0-3 勾完成派生下一期 ===')
reset()
req('POST', '/api/add?title=' + urllib.parse.quote('每周例会') +
    '&due=' + urllib.parse.quote('2026-12-01T09:00'))
st, _, body = req('GET', '/api/tasks')
d = jbody(body)
d['tasks'][0]['repeat'] = 'weekly'
put_tasks(d['tasks'])
tid = d['tasks'][0]['id']
st, _, body = req('POST', '/api/toggle?id=' + tid)
st, _, body = req('GET', '/api/tasks')
d = jbody(body)['tasks']
check('勾完成后多出一条', len(d) == 2, len(d))
if len(d) == 2:
    new = [t for t in d if t['id'] != tid][0]
    old = [t for t in d if t['id'] == tid][0]
    check('下一期 due = 2026-12-08', new['due'] == '2026-12-08T09:00', new['due'])
    check('下一期继承 repeat', new.get('repeat') == 'weekly', new.get('repeat'))
    check('原任务 repeat 清空', not old.get('repeat'), old.get('repeat'))
    check('下一期 done=false', new['done'] is False)

# ---------------------------------------------------------------- P1-6 / P1-7
print('\n=== P1-6 同秒备份不互相覆盖 ===')
for f in os.listdir(BACKUPS):
    os.remove(os.path.join(BACKUPS, f))
for i in range(3):
    req('POST', '/api/backup')
st, _, body = req('GET', '/api/backups')
bl = jbody(body).get('backups', [])
check('连按三次得到三份备份', len(bl) == 3, len(bl))
names = [b['name'] for b in bl]
check('备份文件名带毫秒', all(len(n) > len('tasks-20261024-100000.json') for n in names), names)

print('\n=== P1-7 恢复前校验内容 ===')
reset([{'id': 'keep', 'title': '现有数据', 'done': False}])
badname = 'tasks-20260101-000000-000.json'
with open(os.path.join(BACKUPS, badname), 'w', encoding='utf-8') as f:
    f.write('{"schema":1,"tasks":[{"id":')
st, _, body = req('POST', '/api/restore?name=' + urllib.parse.quote(badname))
check('坏备份被拒', st == 400, (st, text(body)[:80]))
st, _, body = req('GET', '/api/tasks')
check('坏备份没有覆盖现有数据', jbody(body)['tasks'][0]['id'] == 'keep')

# ---------------------------------------------------------------- 写入校验
print('\n=== 写入校验：元素类型与孤立 repeat ===')
st, _, body = req('PUT', '/api/tasks', '{"schema":1,"tasks":[1,2]}',
                  {'Content-Type': 'application/json'})
check('tasks 里放数字被拒 400', st == 400, st)

st, _, body = put_tasks([{'id': 'r1', 'title': '无截止重复', 'due': None, 'repeat': 'daily'}])
check('孤立 repeat 不报错（顺手清掉）', st == 200, st)
st, _, body = req('GET', '/api/tasks')
t = jbody(body)['tasks'][0]
check('写盘后 repeat 被清空', t.get('repeat') == '', repr(t.get('repeat')))
check('due 仍是 null', t.get('due') is None, t.get('due'))

put_tasks([{'id': 'r2', 'title': '正常重复', 'due': '2026-12-01T09:00', 'repeat': 'weekly'}])
st, _, body = req('GET', '/api/tasks')
t = jbody(body)['tasks'][0]
check('有截止的 repeat 保留', t.get('repeat') == 'weekly', t.get('repeat'))

# ---------------------------------------------------------------- 协议
print('\n=== 协议层 ===')
st, hd, body = req('HEAD', '/api/health')
check('HEAD 不发响应体', len(body) == 0, len(body))
check('HEAD 仍带 Content-Length', 'content-length' in hd, hd.get('content-length'))

# chunked
s = socket.create_connection((HOST, PORT), timeout=10)
s.sendall(b'POST /api/add?title=x HTTP/1.1\r\nHost: 127.0.0.1\r\n'
          b'Transfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n')
raw = b''
try:
    while True:
        b = s.recv(4096)
        if not b:
            break
        raw += b
except Exception:
    pass
s.close()
check('chunked 返回 411', raw.startswith(b'HTTP/1.1 411'), raw[:40])

# 24MB
try:
    big = b'{"schema":1,"tasks":[],"pad":"' + b'a' * (24 * 1024 * 1024) + b'"}'
    st, _, body = req('PUT', '/api/tasks', big, {'Content-Type': 'application/json'}, timeout=60)
    check('24MB 请求体收到 413', st == 413, st)
except Exception as e:
    check('24MB 请求体收到 413', False, repr(e))

# /api/widget 的 id 清洗
reset([{'id': 'a\tb\nc', 'title': '带控制符的 id', 'done': False}])
st, _, body = req('GET', '/api/widget')
lines = [l for l in text(body).split('\n') if l.strip()]
check('/api/widget 行数正确', len(lines) == 2, lines)
check('id 里的制表符/换行被洗掉', lines[1].split('\t')[0] == 'a b c', lines[1][:20])

# 坏数据文件
with open(DATA, 'w', encoding='utf-8') as f:
    f.write('{"schema":1,"tasks":[{"id":')
st, _, body = req('GET', '/api/tasks')
check('坏文件 GET /api/tasks 返回 500', st == 500, st)
st, _, body = req('GET', '/api/widget')
check('坏文件 GET /api/widget 返回 500', st == 500, st)
with open(DATA, 'w', encoding='utf-8') as f:
    f.write('{"schema":1,"tasks":[]}')

# PUT 必须带 tasks 数组
st, _, body = req('PUT', '/api/tasks', '{"schema":1}', {'Content-Type': 'application/json'})
check('缺 tasks 数组的 PUT 返回 400', st == 400, st)

# ---------------------------------------------------------------- 汇总
print('\n=== 汇总 ===')
total = len(RESULTS)
fail = [r for r in RESULTS if not r[1]]
print('%d 项断言，%d 项失败' % (total, len(fail)))
for name, _, detail in fail:
    print('  FAIL', name, detail)
raise SystemExit(1 if fail else 0)
