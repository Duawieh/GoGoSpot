// README 截图：控制台的日间 / 夜间两版，写到 docs/。
// 只打隔离副本，需要先起一个 --data build/test/shots/... 的实例。
const { chromium } = require('playwright');
const fs = require('fs');
const path = require('path');

const BASE = process.env.GOGOSPOT_BASE || 'http://127.0.0.1:8770/';
const DOCS = path.join(__dirname, '..', 'docs');
fs.mkdirSync(DOCS, { recursive: true });


// 只准打隔离副本：8765 上要是正式实例，跑这个会把真实任务清空
async function requireIsolated() {
  const info = await fetch(BASE + 'api/info').then((r) => r.json());
  const f = String(info.dataFile || '').replace(/\\/g, '/');
  if (!/(^|\/)build\/test\//.test(f)) {
    console.error('拒绝运行：数据目录是 ' + f + '，不是隔离副本。');
    process.exit(2);
  }
}

const iso = (d) => {
  const p = (n) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}T${p(d.getHours())}:${p(d.getMinutes())}`;
};

// 造一组看得出层次的任务：一条已逾期（红）、一条正在进行、一条明天、一条没截止
function demo() {
  const now = Date.now();
  const at = (h, m, dayOffset) => {
    const d = new Date(now + dayOffset * 86400000);
    d.setHours(h, m, 0, 0);
    return iso(d);
  };
  const T = new Date(now).toISOString();
  return [
    { id: 'demo-1', title: '整理季度汇报材料', done: false, pinned: true, estimate: 120,
      due: at(18, 0, 0), tags: ['工作'], links: [], startedAt: 0,
      repeat: '', createdAt: T, completedAt: null },
    { id: 'demo-2', title: '回复客户邮件', done: false, pinned: false, estimate: 30,
      due: at(20, 30, 0), tags: ['工作', '沟通'], links: [], startedAt: now - 10 * 60000,
      repeat: '', createdAt: T, completedAt: null },
    { id: 'demo-3', title: '读《设计心理学》第 3 章', done: false, pinned: false, estimate: 60,
      due: at(21, 0, 1), tags: ['学习'], links: [], startedAt: 0,
      repeat: '', createdAt: T, completedAt: null },
    { id: 'demo-4', title: '预约体检', done: false, pinned: false, estimate: 15,
      due: null, tags: ['生活'], links: [], startedAt: 0,
      repeat: '', createdAt: T, completedAt: null },
    { id: 'demo-5', title: '整理书桌', done: true, pinned: false, estimate: 20,
      due: null, tags: ['生活'], links: [], startedAt: 0,
      repeat: '', createdAt: T, completedAt: T },
  ];
}

async function shoot(page, name, theme) {
  await fetch(BASE + 'api/theme?mode=' + theme, { method: 'POST' });
  await page.reload({ waitUntil: 'networkidle' });
  await page.waitForTimeout(500);
  await page.screenshot({ path: path.join(DOCS, name), fullPage: false });
  console.log('wrote docs/' + name);
}

(async () => {
  await requireIsolated();

  await fetch(BASE + 'api/tasks', {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ schema: 1, tasks: demo() }),
  });

  const browser = await chromium.launch({ channel: 'chrome', headless: true });
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 940 },
    deviceScaleFactor: 2, locale: 'zh-CN' });
  const page = await ctx.newPage();
  await page.goto(BASE, { waitUntil: 'networkidle' });
  await page.waitForTimeout(600);

  await shoot(page, 'console-light.png', 'light');
  await shoot(page, 'console-dark.png', 'dark');

  // 收尾：主题回到跟随系统
  await fetch(BASE + 'api/theme?mode=system', { method: 'POST' });
  await browser.close();
})().catch((e) => { console.error('SCRIPT ERROR', e); process.exit(1); });
