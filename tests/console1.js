// 控制台端到端测试（第一批）：添加 / 勾选 / 改名 / 删除 / 搜索 / 标签 / 快捷键
const { chromium } = require('playwright');
const fs = require('fs');
const path = require('path');

const BASE = process.env.GOGOSPOT_BASE || 'http://127.0.0.1:8765/';
const SHOTS = path.join(__dirname, 'shots');
fs.mkdirSync(SHOTS, { recursive: true });

const results = [];
function check(name, ok, detail = '') {
  results.push({ name, ok: !!ok, detail });
  console.log((ok ? 'PASS ' : 'FAIL ') + name + (detail ? '  | ' + detail : ''));
}


// 只准打隔离副本：8765 上要是正式实例，跑测试会把真实任务清空
async function requireIsolated() {
  const info = await fetch(BASE + 'api/info').then((r) => r.json());
  const f = String(info.dataFile || '').replace(/\\/g, '/');
  if (!/\/build\/test\//.test(f)) {
    console.error('拒绝运行：数据目录是 ' + f + '，不是隔离副本。'
      + '请用 --data build/test/... 另起一个实例再测。');
    process.exit(2);
  }
}

(async () => {
  await requireIsolated();
  const browser = await chromium.launch({ channel: 'chrome', headless: true });
  const ctx = await browser.newContext({ viewport: { width: 1440, height: 900 }, locale: 'zh-CN' });
  const page = await ctx.newPage();
  const errors = [];
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });
  page.on('pageerror', (e) => errors.push('pageerror: ' + e.message));

  await fetch(BASE + 'api/tasks', { method: 'PUT', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ schema: 1, tasks: [] }) });
  await page.goto(BASE, { waitUntil: 'networkidle' });
  await page.waitForTimeout(400);

  check('页面标题', (await page.title()) === 'GoGoSpot 控制台', await page.title());
  check('初始空状态可见', await page.locator('#empty').isVisible());
  await page.screenshot({ path: path.join(SHOTS, 'c1-empty.png') });

  // ---- 添加：纯标题
  await page.fill('#taskInput', '买牛奶');
  await page.press('#taskInput', 'Enter');
  await page.waitForTimeout(250);
  check('回车添加一条', (await page.locator('#list .task').count()) === 1);

  // ---- 添加：带截止 / 预估 / 标签 / 链接
  await page.fill('#taskInput', '写发布说明');
  await page.fill('#estInput', '60');
  await page.locator('#estInput').blur();
  await page.click('#dueBtn');
  await page.waitForTimeout(120);
  await page.fill('#dueHH', '18');
  await page.fill('#dueMM', '30');
  await page.locator('#dueHH').blur();
  const d3 = new Date(Date.now() + 3 * 86400000);
  const ds = d3.getFullYear() + '-' + String(d3.getMonth() + 1).padStart(2, '0') + '-' + String(d3.getDate()).padStart(2, '0');
  await page.click(`#dueGrid .due-day[data-date="${ds}"]`);
  await page.waitForTimeout(150);
  await page.fill('#tagInput', '发布, 紧急');
  await page.click('#btnStart');
  await page.fill('#linkInput', 'https://example.com/spec');
  await page.press('#linkInput', 'Enter');
  await page.click('#btnAdd');
  await page.waitForTimeout(300);
  check('按钮添加一条', (await page.locator('#list .task').count()) === 2);
  const second = page.locator('#list .task').nth(0);
  const meta = await second.locator('.task-meta').innerText();
  check('元信息含预估/截止/标签', meta.includes('约 1 小时') && meta.includes('18:30') && meta.includes('发布'), meta.replace(/\n/g, ' '));
  check('链接图标出现', await second.locator('.task-start').count() === 1);
  await page.screenshot({ path: path.join(SHOTS, 'c2-two-tasks.png') });

  // ---- 勾选完成
  await page.locator('#list .task').nth(1).locator('.check').click();
  await page.waitForTimeout(350);
  check('勾选后进入 done 态', await page.locator('#list .task').nth(1).evaluate(n => n.classList.contains('done')));
  check('概览已完成数 +1', (await page.textContent('#statDone')) === '1');
  await page.screenshot({ path: path.join(SHOTS, 'c3-done.png') });

  // ---- 改名
  await page.locator('#list .task').nth(0).locator('.task-rename').click();
  await page.waitForTimeout(250);
  const edit = page.locator('#editTitle');
  check('编辑弹窗出现并聚焦标题',
    (await page.isVisible('#editMask')) && await edit.evaluate(n => document.activeElement === n));
  await edit.fill('写 1.0.0 发布说明');
  await page.click('#btnEditSave');
  await page.waitForTimeout(400);
  check('改名生效', (await page.locator('#list .task').nth(0).innerText()).includes('1.0.0'));

  // ---- 删除 + 撤销
  await page.locator('#list .task').nth(0).locator('.task-del').click();
  await page.waitForTimeout(300);
  check('删除后少一条', (await page.locator('#list .task').count()) === 1);
  check('撤销提示出现', await page.locator('.toast-action').count() === 1);
  await page.locator('.toast-action').click();
  await page.waitForTimeout(300);
  check('撤销恢复', (await page.locator('#list .task').count()) === 2);

  // ---- 搜索
  await page.fill('#searchInput', '牛奶');
  await page.waitForTimeout(200);
  check('搜索命中 1 条', (await page.locator('#list .task').count()) === 1);
  check('搜索空态文案', (await page.locator('#empty').isVisible()) === false);
  await page.fill('#searchInput', 'zzz');
  await page.waitForTimeout(200);
  check('无结果空态', (await page.textContent('#emptyTitle')) === '无匹配任务');
  await page.click('#btnClearSearch');
  await page.waitForTimeout(150);

  // ---- 标签筛选
  await page.locator('.tag-chip', { hasText: '发布' }).click();
  await page.waitForTimeout(200);
  check('标签筛选 1 条', (await page.locator('#list .task').count()) === 1);
  await page.locator('.tag-chip', { hasText: '发布' }).click();
  await page.waitForTimeout(150);

  // ---- 视图
  await page.click('.seg-btn[data-view="done"]');
  await page.waitForTimeout(200);
  check('已完成视图 1 条', (await page.locator('#list .task').count()) === 1);
  await page.click('.seg-btn[data-view="all"]');
  await page.waitForTimeout(150);

  // ---- 快捷键
  await page.locator('body').click({ position: { x: 10, y: 10 } });
  await page.keyboard.press('/');
  check('/ 聚焦搜索', await page.evaluate(() => document.activeElement.id === 'searchInput'));
  await page.keyboard.press('Escape');
  await page.locator('body').click({ position: { x: 10, y: 10 } });
  await page.keyboard.press('n');
  check('N 聚焦输入框', await page.evaluate(() => document.activeElement.id === 'taskInput'));

  // ---- 长标题 / 长标签 / 长链接
  const longTitle = '很长的标题'.repeat(40);
  await page.fill('#taskInput', longTitle);
  await page.press('#taskInput', 'Enter');
  await page.waitForTimeout(300);
  const overflow = await page.evaluate(() => {
    const n = document.querySelector('#list .task .task-title');
    return { scroll: n.scrollWidth, client: n.clientWidth };
  });
  check('超长标题不撑破布局（有省略号）', overflow.scroll <= overflow.client + 2, JSON.stringify(overflow));
  await page.screenshot({ path: path.join(SHOTS, 'c4-long-title.png') });

  // ---- 页脚与保存状态
  await page.waitForTimeout(900);
  const saveText = await page.textContent('#saveText');
  check('保存状态已更新', /已保存|已同步/.test(saveText), saveText);

  check('无控制台错误', errors.length === 0, errors.slice(0, 3).join(' | '));

  console.log('\n=== 汇总 ===');
  const bad = results.filter(r => !r.ok);
  console.log(`${results.length} 项，${bad.length} 项失败`);
  bad.forEach(r => console.log('  FAIL', r.name, '|', r.detail));

  fs.writeFileSync(path.join(__dirname, 'console-log1.json'), JSON.stringify({ results, errors }, null, 2));
  await browser.close();
})().catch((e) => { console.error('SCRIPT ERROR', e); process.exit(1); });
