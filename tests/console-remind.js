// 提醒任务在控制台的表现：类型下拉、开始时间、提前提醒、行内呈现与红色线。
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

  const taskOf = async (title) => {
    const d = await (await fetch(BASE + 'api/tasks')).json();
    return (d.tasks || []).find((t) => t.title === title) || null;
  };

  // 在截止面板里选日期并填时间；quick 是 today / tomorrow
  const setWhen = async (dateObj) => {
    await page.click('#dueBtn');
    await page.waitForTimeout(150);
    const quick = ymd(dateObj) === ymd(new Date()) ? 'today' : 'tomorrow';
    await page.click(`#duePop [data-quick="${quick}"]`);
    await page.waitForTimeout(150);
    await page.click('#dueBtn');
    await page.waitForTimeout(150);
    await page.fill('#dueHH', p2(dateObj.getHours()));
    await page.locator('#dueHH').blur();
    await page.fill('#dueMM', p2(dateObj.getMinutes()));
    await page.locator('#dueMM').blur();
    await page.waitForTimeout(150);
    await page.keyboard.press('Escape');
    await page.waitForTimeout(150);
  };

  // ---- 1. 默认是过程任务 ----
  check('默认类型标签是「过程」', (await page.textContent('#typeLabel')).trim() === '过程',
    await page.textContent('#typeLabel'));
  check('默认时间栏写「截止时间」', (await page.textContent('#dueLabel')).includes('截止时间'),
    await page.textContent('#dueLabel'));
  check('过程任务不显示提前提醒', await page.locator('#advField').isHidden());

  // ---- 2. 切到提醒 ----
  await page.click('#typeBtn');
  await page.waitForTimeout(150);
  check('类型下拉能打开', await page.locator('#typePop').isVisible());
  await page.click('#typePop .type-item[data-type="remind"]');
  await page.waitForTimeout(200);
  check('切换后标签是「提醒」', (await page.textContent('#typeLabel')).trim() === '提醒',
    await page.textContent('#typeLabel'));
  check('提醒任务时间栏写「开始时间」', (await page.textContent('#dueLabel')).includes('开始时间'),
    await page.textContent('#dueLabel'));
  check('提醒任务显示提前提醒', await page.locator('#advField').isVisible());
  check('下拉选完自动收起', await page.locator('#typePop').isHidden());

  // ---- 3. 建一条 2 小时后开始的提醒任务 ----
  const start1 = new Date(Date.now() + 2 * 3600000);
  start1.setSeconds(0, 0);
  await page.fill('#taskInput', '提醒甲');
  await page.fill('#advInput', '15');
  await page.locator('#advInput').blur();
  await setWhen(start1);
  // 估算框默认单位是「小时」（初始 60 分钟整除 60），填 0.5 才是 30 分钟
  await page.fill('#estInput', '0.5');
  await page.locator('#estInput').blur();
  await page.waitForTimeout(150);
  await page.press('#taskInput', 'Enter');
  await page.waitForTimeout(800);

  const t1 = await taskOf('提醒甲');
  check('提醒任务建出来了', !!t1);
  if (t1) {
    check('落盘 TaskType=remind', t1.TaskType === 'remind', t1.TaskType);
    check('落盘 Start 正确', t1.Start === `${ymd(start1)}T${hm(start1)}`, t1.Start);
    check('落盘 Advance=15', t1.Advance === 15, t1.Advance);
    const wantDue = new Date(start1.getTime() + 30 * 60000);
    check('落盘 due = 开始 + 30 分', t1.due === `${ymd(wantDue)}T${hm(wantDue)}`, t1.due);
  }

  // ---- 4. 行内呈现 ----
  const row = page.locator('#list .task', { hasText: '提醒甲' }).first();
  const metaText = (await row.locator('.task-meta').innerText()).replace(/\n/g, ' ');
  check('行内标「提醒」', metaText.includes('提醒'), metaText);
  check('行内显示「后开始」', /后开始/.test(metaText), metaText);
  check('行内显示持续时间', /持续 30 分/.test(metaText), metaText);
  check('提醒任务没有计时按钮', (await row.locator('.task-go').count()) === 0);
  check('提醒任务没有进度条', (await row.locator('.task-progress').count()) === 0);

  // ---- 5. 红色线就是提前提醒 ----
  // start1 在 2 小时后，advance=15 分 → 不该红
  let cls = (await row.getAttribute('class')) || '';
  check('离开始还早时不标红', !cls.includes('urgent'), cls);

  // 改成 20 分钟后开始、提前 30 分提醒 → 应该红
  const start2 = new Date(Date.now() + 20 * 60000);
  start2.setSeconds(0, 0);
  await put([{
    id: 'soon', title: '快开始', done: false, estimate: 30,
    TaskType: 'remind', Start: `${ymd(start2)}T${hm(start2)}`,
    due: `${ymd(new Date(start2.getTime() + 30 * 60000))}T${hm(new Date(start2.getTime() + 30 * 60000))}`,
    Advance: 30, tags: [], links: [], startedAt: 0, repeat: '',
    createdAt: '2026-11-06T00:00:00Z', completedAt: null,
  }]);
  await page.reload({ waitUntil: 'networkidle' });
  await page.waitForTimeout(400);
  const soonRow = page.locator('#list .task', { hasText: '快开始' }).first();
  cls = (await soonRow.getAttribute('class')) || '';
  check('进入提前提醒范围标红', cls.includes('urgent'), cls);

  // 同样剩 20 分钟，advance=0 → 不红
  await put([{
    id: 'soon2', title: '快开始但没提前', done: false, estimate: 30,
    TaskType: 'remind', Start: `${ymd(start2)}T${hm(start2)}`,
    due: `${ymd(new Date(start2.getTime() + 30 * 60000))}T${hm(new Date(start2.getTime() + 30 * 60000))}`,
    Advance: 0, tags: [], links: [], startedAt: 0, repeat: '',
    createdAt: '2026-11-06T00:00:00Z', completedAt: null,
  }]);
  await page.reload({ waitUntil: 'networkidle' });
  await page.waitForTimeout(400);
  const soon2 = page.locator('#list .task', { hasText: '快开始但没提前' }).first();
  cls = (await soon2.getAttribute('class')) || '';
  check('advance=0 时到点才红', !cls.includes('urgent'), cls);

  // 提醒任务永远不亮黄：剩很久也不黄
  await put([{
    id: 'far', title: '很久以后', done: false, estimate: 30,
    TaskType: 'remind', Start: `${ymd(new Date(Date.now() + 30 * 3600000))}T${hm(new Date(Date.now() + 30 * 3600000))}`,
    due: `${ymd(new Date(Date.now() + 31 * 3600000))}T${hm(new Date(Date.now() + 31 * 3600000))}`,
    Advance: 0, tags: [], links: [], startedAt: 0, repeat: '',
    createdAt: '2026-11-06T00:00:00Z', completedAt: null,
  }]);
  await page.reload({ waitUntil: 'networkidle' });
  await page.waitForTimeout(400);
  const farRow = page.locator('#list .task', { hasText: '很久以后' }).first();
  cls = (await farRow.getAttribute('class')) || '';
  check('提醒任务不亮黄', !cls.includes('urgent-y'), cls);

  // ---- 6. 编辑提醒任务：回填正确 ----
  await put([{
    id: 'edit1', title: '要编辑的提醒', done: false, estimate: 45,
    TaskType: 'remind', Start: `${ymd(start1)}T${hm(start1)}`,
    due: `${ymd(new Date(start1.getTime() + 45 * 60000))}T${hm(new Date(start1.getTime() + 45 * 60000))}`,
    Advance: 20, tags: [], links: [], startedAt: 0, repeat: '',
    createdAt: '2026-11-06T00:00:00Z', completedAt: null,
  }]);
  await page.reload({ waitUntil: 'networkidle' });
  await page.waitForTimeout(400);
  await page.locator('#list .task', { hasText: '要编辑的提醒' }).first().locator('.task-rename').click();
  await page.waitForTimeout(400);
  check('编辑弹窗里类型回填「提醒」',
    (await page.textContent('#editFields #typeLabel')).trim() === '提醒',
    await page.textContent('#editFields #typeLabel'));
  check('编辑弹窗里提前提醒回填 20',
    (await page.locator('#editFields #advInput').inputValue()) === '20',
    await page.locator('#editFields #advInput').inputValue());
  // 有日期时 dueLabel 显示具体时刻，靠 title 判断它此刻代表的是开始还是截止
  check('编辑弹窗里时间栏说的是「开始时间」',
    ((await page.locator('#editFields #dueField').getAttribute('title')) || '').includes('开始时间'),
    await page.locator('#editFields #dueField').getAttribute('title'));

  // 改提前提醒为 45 并保存
  await page.fill('#editFields #advInput', '45');
  await page.locator('#editFields #advInput').blur();
  await page.waitForTimeout(150);
  await page.click('#btnEditSave');
  await page.waitForTimeout(800);
  const t2 = await taskOf('要编辑的提醒');
  check('编辑后 Advance 落盘 45', t2 && t2.Advance === 45, t2 && t2.Advance);
  check('编辑后仍是提醒任务', t2 && t2.TaskType === 'remind', t2 && t2.TaskType);
  check('编辑后 Start 没被改', t2 && t2.Start === `${ymd(start1)}T${hm(start1)}`, t2 && t2.Start);

  // ---- 7. 提醒任务改回过程任务 ----
  await page.locator('#list .task', { hasText: '要编辑的提醒' }).first().locator('.task-rename').click();
  await page.waitForTimeout(400);
  await page.click('#editFields #typeBtn');
  await page.waitForTimeout(150);
  await page.click('#editFields #typePop .type-item[data-type="process"]');
  await page.waitForTimeout(200);
  check('改回过程后提前提醒隐藏', await page.locator('#editFields #advField').isHidden());
  await page.click('#btnEditSave');
  await page.waitForTimeout(800);
  const t3 = await taskOf('要编辑的提醒');
  check('改回过程任务落盘', t3 && t3.TaskType === 'process', t3 && t3.TaskType);
  check('改回过程后 Start 清空', t3 && !t3.Start, t3 && t3.Start);
  check('改回过程后 Advance 归 0', t3 && t3.Advance === 0, t3 && t3.Advance);
  check('改回过程后 due 保留', t3 && !!t3.due, t3 && t3.due);

  // ---- 8. 提醒任务不显示计时按钮，过程任务照常 ----
  await put([
    { id: 'r1', title: '提醒类', done: false, estimate: 30, TaskType: 'remind',
      Start: `${ymd(start1)}T${hm(start1)}`, Advance: 5,
      due: `${ymd(new Date(start1.getTime() + 30 * 60000))}T${hm(new Date(start1.getTime() + 30 * 60000))}`,
      tags: [], links: [], startedAt: 0, repeat: '', createdAt: '2026-11-06T00:00:00Z', completedAt: null },
    { id: 'p1', title: '过程类', done: false, estimate: 30, TaskType: 'process',
      due: `${ymd(start1)}T${hm(start1)}`,
      tags: [], links: [], startedAt: 0, repeat: '', createdAt: '2026-11-06T00:00:00Z', completedAt: null },
  ]);
  await page.reload({ waitUntil: 'networkidle' });
  await page.waitForTimeout(400);
  const rRow = page.locator('#list .task', { hasText: '提醒类' }).first();
  const pRow = page.locator('#list .task', { hasText: '过程类' }).first();
  check('提醒任务无计时按钮（混合列表）', (await rRow.locator('.task-go').count()) === 0);
  check('过程任务有计时按钮（混合列表）', (await pRow.locator('.task-go').count()) === 1);

  // ---- 9. Escape 先关类型下拉 ----
  await page.click('#typeBtn');
  await page.waitForTimeout(150);
  check('类型下拉已打开', await page.locator('#typePop').isVisible());
  await page.keyboard.press('Escape');
  await page.waitForTimeout(200);
  check('Escape 关掉类型下拉', await page.locator('#typePop').isHidden());

  // ---- 10. 无脚本错误 ----
  check('页面没有 JS 异常', errors.length === 0, errors.join(' | '));

  await browser.close();

  const bad = results.filter((r) => !r.ok);
  console.log('\n' + '='.repeat(60));
  console.log(`总计: ${results.length - bad.length} PASS / ${bad.length} FAIL / ${results.length} 项`);
  process.exit(bad.length ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(1); });
