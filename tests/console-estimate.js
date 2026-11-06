// 预估「不设」在控制台的表现：留空能存成 0、列表写「不限时」、负载按 1 小时兜底。
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

(async () => {
  await requireIsolated();
  await put([]);

  const browser = await chromium.launch({ channel: 'chrome', headless: true });
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 940 }, locale: 'zh-CN' });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  await page.goto(BASE, { waitUntil: 'networkidle' });
  await page.waitForTimeout(400);

  check('页面标题是「控制台页面」',
    (await page.title()) === 'GoGoSpot 控制台页面', await page.title());

  // ---- 留空预估新建
  await page.fill('#taskInput', '随手记一下');
  await page.fill('#estInput', '');
  await page.locator('#estInput').blur();
  await page.waitForTimeout(150);
  await page.press('#taskInput', 'Enter');
  await page.waitForTimeout(500);

  const meta = await page.locator('#list .task').first().locator('.task-meta').innerText();
  check('列表显示「不限时」', meta.includes('不限时'), meta.replace(/\n/g, ' '));

  let data = await (await fetch(BASE + 'api/tasks')).json();
  check('落盘 estimate=0', data.tasks[0].estimate === 0, 'estimate=' + data.tasks[0].estimate);

  const foot = await page.textContent('#footerLeft');
  check('底栏负载按 1 小时兜底', /1 小时/.test(foot), foot);

  // ---- 编辑不设的任务：输入框应该是空的
  await page.locator('#list .task .task-rename').first().click();
  await page.waitForTimeout(350);
  const editVal = await page.locator('#editFields #estInput').inputValue();
  check('编辑面板预估框为空', editVal === '', 'value=' + JSON.stringify(editVal));
  await page.keyboard.press('Escape');
  await page.waitForTimeout(250);

  // ---- 填回 90 分钟，显示与落盘都要跟上
  await page.locator('#list .task .task-rename').first().click();
  await page.waitForTimeout(350);
  await page.locator('#editFields #estInput').click();
  await page.fill('#editFields #estInput', '90');
  await page.locator('#editFields #estInput').blur();
  await page.waitForTimeout(150);
  await page.click('#btnEditSave');
  await page.waitForTimeout(700);

  data = await (await fetch(BASE + 'api/tasks')).json();
  check('改回 90 分钟落盘', Math.abs(data.tasks[0].estimate - 90) < 0.01,
    'estimate=' + data.tasks[0].estimate);
  const meta2 = await page.locator('#list .task').first().locator('.task-meta').innerText();
  check('列表显示约 1 小时 30 分', /1 小时 30 分/.test(meta2), meta2.replace(/\n/g, ' '));

  // ---- 一条不设 + 一条 60 分钟 → 负载 2 小时
  await put([
    { id: 'u1', title: '不设', done: false, estimate: 0, due: null, tags: [], links: [],
      startedAt: 0, repeat: '', createdAt: '2026-11-06T00:00:00Z', completedAt: null },
    { id: 'u2', title: '一小时', done: false, estimate: 60, due: null, tags: [], links: [],
      startedAt: 0, repeat: '', createdAt: '2026-11-06T00:00:00Z', completedAt: null },
  ]);
  await page.reload({ waitUntil: 'networkidle' });
  await page.waitForTimeout(400);
  const foot2 = await page.textContent('#footerLeft');
  check('不设 + 60 分钟 = 2 小时', /2 小时/.test(foot2), foot2);

  check('没有页面错误', errors.length === 0, errors.join('; '));

  await browser.close();
  const bad = results.filter((r) => !r.ok);
  console.log(`\n=== 汇总 ===\n${results.length} 项，${bad.length} 项失败`);
  bad.forEach((r) => console.log('  FAIL', r.name, '|', r.detail));
  process.exit(bad.length ? 1 : 0);
})().catch((e) => { console.error('SCRIPT ERROR', e); process.exit(1); });
