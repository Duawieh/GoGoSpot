// 紧急度定级：只有提醒任务的截止档不定级，也不把等级传给更早的档。
// 只打隔离副本，需要 --data build/test/... 起的实例。
const { chromium } = require('playwright');

const BASE = process.env.GOGOSPOT_BASE || 'http://127.0.0.1:8770/';
const results = [];
function check(name, ok, detail = '') {
  results.push({ name, ok: !!ok, detail });
  console.log((ok ? 'PASS ' : 'FAIL ') + name + (detail ? '  | ' + detail : ''));
}

async function requireIsolated() {
  const info = await fetch(BASE + 'api/info').then((r) => r.json());
  const f = String(info.dataFile || '').replace(/\\/g, '/');
  if (!/(^|\/)build\/test\//.test(f)) {
    console.error('拒绝运行：数据目录是 ' + f + '，不是隔离副本。');
    process.exit(2);
  }
}

const put = (tasks) => fetch(BASE + 'api/tasks', {
  method: 'PUT', headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify({ schema: 1, tasks }),
});

const p2 = (n) => String(n).padStart(2, '0');
const ymd = (d) => `${d.getFullYear()}-${p2(d.getMonth() + 1)}-${p2(d.getDate())}`;
const hm = (d) => `${p2(d.getHours())}:${p2(d.getMinutes())}`;
const at = (ms) => { const d = new Date(ms); return `${ymd(d)}T${hm(d)}`; };

// 一条过程任务：给截止和预估（分钟）
const proc = (id, title, dueMs, estMin) => ({
  id, title, done: false, estimate: estMin, TaskType: 'process', due: at(dueMs),
  tags: [], links: [], startedAt: 0, repeat: '',
  createdAt: '2026-11-06T00:00:00Z', completedAt: null,
});

// 一条提醒任务：给开始时刻和持续时间，due 由服务端推
const remind = (id, title, startMs, estMin, advance = 0) => ({
  id, title, done: false, estimate: estMin, TaskType: 'remind',
  Start: at(startMs), Advance: advance, due: at(startMs + estMin * 60000),
  tags: [], links: [], startedAt: 0, repeat: '',
  createdAt: '2026-11-06T00:00:00Z', completedAt: null,
});

(async () => {
  await requireIsolated();
  await put([]);

  const browser = await chromium.launch({ channel: 'chrome', headless: true });
  const page = await browser.newPage({ viewport: { width: 1280, height: 940 }, locale: 'zh-CN' });
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));

  const clsOf = async (title) => {
    const el = page.locator('#list .task', { hasText: title }).first();
    return (await el.getAttribute('class')) || '';
  };
  const isRed = (c) => c.includes('urgent') && !c.includes('urgent-y');
  const isYellow = (c) => c.includes('urgent-y');
  const isPlain = (c) => !c.includes('urgent');

  const load = async (tasks) => {
    await put(tasks);
    await page.goto(BASE, { waitUntil: 'networkidle' });
    await page.waitForTimeout(450);
  };

  const H = 3600000, M = 60000;
  const now = Date.now();

  // ---- 1. 已过点的纯提醒档，不能把红传给更早的过程任务 ----
  // 过程任务还剩 20 小时、预估 30 分，红线只有 1 小时，本来不该红
  await load([
    proc('p1', '过程早', now + 20 * H, 30),
    remind('r1', '提醒已过', now - 60 * M, 30),
  ]);
  let c = await clsOf('过程早');
  check('纯提醒档不把红传给更早的过程任务', !isRed(c), c);
  check('该过程任务也不该黄（剩 20h > 16h）', isPlain(c), c);
  c = await clsOf('提醒已过');
  check('已过点的提醒任务自己仍然红', isRed(c), c);

  // ---- 2. 纯提醒档自己不该亮黄 ----
  // 提醒任务 1.5 小时后开始、持续 30 分，提前提醒 0：到点才红，中间不黄
  await load([
    remind('r2', '提醒待开始', now + 1.5 * H, 30, 0),
  ]);
  c = await clsOf('提醒待开始');
  check('纯提醒档不亮黄', !isYellow(c), c);
  check('没到提前量就不红', !isRed(c), c);

  // ---- 3. 混合档（同截止时刻有过程任务）照常定级 ----
  // 同一时刻：过程任务剩 2 小时 → 黄线 16h → 黄
  const dueMix = now + 2 * H;
  await load([
    proc('p3', '混合过程', dueMix, 30),
    remind('r3', '混合提醒', dueMix - 30 * M, 30, 0),
  ]);
  c = await clsOf('混合过程');
  check('混合档里的过程任务照常标黄', isYellow(c), c);
  c = await clsOf('混合提醒');
  check('混合档里的提醒任务不被负载黄线染色', !isYellow(c), c);

  // ---- 4. 提醒任务的预估仍然计入前缀和 ----
  // 同一截止时刻放一条过程任务 + 一条大预估提醒任务，前缀和跨过 12h 门槛
  const dueBig = now + 24 * H;
  await load([
    proc('p4a', '前缀小', dueBig, 30),
    remind('r4a', '前缀提醒小', dueBig - 10 * H, 600, 0),   // 10 小时
  ]);
  c = await clsOf('前缀小');
  check('前缀 10.5h 时黄线仍 16h，剩 24h 不黄', isPlain(c), c);

  await load([
    proc('p4b', '前缀大', dueBig, 30),
    remind('r4b', '前缀提醒大', dueBig - 15 * H, 900, 0),   // 15 小时
  ]);
  c = await clsOf('前缀大');
  check('提醒任务预估把前缀顶过 12h 后，过程任务变黄', isYellow(c), c);

  // ---- 5. carry 机制没被改坏：更晚的过程档红，要传给更早的档 ----
  await load([
    proc('p5a', '更早的档', now + 2 * H, 30),
    proc('p5b', '更晚且红', now + 11 * H, 720),   // 预估 12h，红线封顶 12h，剩 11h → 红
  ]);
  c = await clsOf('更晚且红');
  check('更晚的过程档自己红', isRed(c), c);
  c = await clsOf('更早的档');
  check('更早的档继承红色', isRed(c), c);

  // ---- 6. 纯提醒档夹在两个过程档之间，不阻断 carry ----
  await load([
    proc('p6a', '最早', now + 2 * H, 30),
    remind('p6r', '中间的提醒', now + 5 * H, 30, 0),
    proc('p6b', '最晚且红', now + 11 * H, 720),
  ]);
  c = await clsOf('最晚且红');
  check('最晚档自己红', isRed(c), c);
  c = await clsOf('最早');
  check('纯提醒档夹在中间时，carry 仍能穿过去', isRed(c), c);

  // ---- 7. 提醒任务的红色线不受影响 ----
  await load([
    remind('r7', '提前量内', now + 20 * M, 30, 30),
    remind('r8', '提前量外', now + 90 * M, 30, 30),
  ]);
  c = await clsOf('提前量内');
  check('进入提前提醒范围仍然红', isRed(c), c);
  c = await clsOf('提前量外');
  check('提前量外不红', !isRed(c), c);

  check('页面没有 JS 异常', errors.length === 0, errors.join(' | '));

  await browser.close();

  const bad = results.filter((r) => !r.ok);
  console.log('\n' + '='.repeat(60));
  console.log(`总计: ${results.length - bad.length} PASS / ${bad.length} FAIL / ${results.length} 项`);
  process.exit(bad.length ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(1); });
