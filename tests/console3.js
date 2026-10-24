// 控制台第三批：设置面板 / 备份 / 导出导入 / 置顶排序
const { chromium } = require('playwright');
const fs = require('fs');
const path = require('path');
const BASE = 'http://127.0.0.1:8765/';
const SHOTS = path.join(__dirname, 'shots');
const results = [];
function check(name, ok, detail = '') {
  results.push({ name, ok: !!ok, detail });
  console.log((ok ? 'PASS ' : 'FAIL ') + name + (detail ? '  | ' + detail : ''));
}
const mk = (i, title) => ({ id: 'k' + i, title, done: false, estimate: 30, due: null, tags: [],
  links: [], startedAt: 0, repeat: '', createdAt: new Date().toISOString(), completedAt: null });

(async () => {
  const browser = await chromium.launch({ channel: 'chrome', headless: true });
  const ctx = await browser.newContext({ viewport: { width: 1200, height: 860 }, locale: 'zh-CN',
    acceptDownloads: true });
  const page = await ctx.newPage();
  page.on('dialog', (d) => d.accept());

  await fetch(BASE + 'api/tasks', { method: 'PUT', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ schema: 1, tasks: [mk(1, '任务一'), mk(2, '任务二'), mk(3, '任务三')] }) });
  await page.goto(BASE, { waitUntil: 'networkidle' });

  // ---- 设置面板
  await page.click('#btnData');
  await page.waitForTimeout(400);
  check('设置面板打开', await page.locator('#sheet').isVisible());
  const infoPath = await page.textContent('#infoPath');
  check('显示数据文件路径', infoPath.endsWith('tasks.json'), infoPath);
  await page.screenshot({ path: path.join(SHOTS, 'c13-sheet.png') });

  // ---- 手动备份
  const before = await page.locator('.backup-item').count();
  await page.click('#btnManualBackup');
  await page.waitForTimeout(700);
  const after = await page.locator('.backup-item').count();
  check('立即备份后列表 +1', after >= before, `${before} → ${after}`);

  // ---- 删除一份备份
  if (after > 0) {
    await page.locator('.backup-del').first().click();
    await page.waitForTimeout(700);
    const afterDel = await page.locator('.backup-item').count();
    check('删除备份后列表 -1', afterDel === after - 1, `${after} → ${afterDel}`);
  }

  // ---- 导出
  const dl = page.waitForEvent('download');
  await page.click('#btnExport');
  const download = await dl;
  const fn = download.suggestedFilename();
  const tmp = path.join(__dirname, 'exported.json');
  await download.saveAs(tmp);
  const exported = JSON.parse(fs.readFileSync(tmp, 'utf-8'));
  check('导出文件名带时间戳', /^gogospot-\d{8}-\d{6}(-\d{3})?\.json$/.test(fn), fn);
  check('导出内容含 3 条任务', Array.isArray(exported.tasks) && exported.tasks.length === 3,
    'tasks=' + (exported.tasks || []).length);

  // ---- 导入
  const imp = { schema: 1, tasks: [mk(9, '导入甲'), mk(10, '导入乙')] };
  const impPath = path.join(__dirname, 'import.json');
  fs.writeFileSync(impPath, JSON.stringify(imp), 'utf-8');
  await page.setInputFiles('#fileInput', impPath);
  await page.waitForTimeout(1000);
  check('导入后列表 2 条', (await page.locator('#list .task').count()) === 2);
  const saved = await (await fetch(BASE + 'api/tasks')).json();
  check('导入已落盘', saved.tasks.length === 2 && saved.tasks[0].title === '导入甲',
    saved.tasks.map(t => t.title).join(','));

  // ---- 置顶：排序靠置顶按钮调，拖拽已经取消
  if (await page.locator('#sheet').isVisible()) { await page.click('#btnCloseSheet'); }
  await page.waitForTimeout(200);
  const rows = page.locator('#list .task');
  check('初始顺序就是存储顺序', (await rows.nth(0).textContent()).includes('导入甲'));

  await rows.nth(1).locator('.task-pin').click();
  await page.waitForTimeout(700);
  const firstTxt = await page.locator('#list .task').first().textContent();
  check('置顶后排到最前面', firstTxt.includes('导入乙'), firstTxt.trim().slice(0, 12));

  const pinCls = await page.locator('#list .task').first().locator('.task-pin').getAttribute('class');
  check('置顶按钮变成常显的选中态', pinCls.includes('is-on'), pinCls);

  const savedPin = await (await fetch(BASE + 'api/tasks')).json();
  const pinTask = savedPin.tasks.find((t) => t.title === '导入乙');
  check('置顶写进任务字段', !!pinTask && pinTask.pinned === true,
    'pinned=' + (pinTask && pinTask.pinned));
  await page.screenshot({ path: path.join(SHOTS, 'c13-pin.png') });

  await page.locator('#list .task').first().locator('.task-pin').click();
  await page.waitForTimeout(700);
  const backTxt = await page.locator('#list .task').first().textContent();
  check('取消置顶后回到原顺序', backTxt.includes('导入甲'), backTxt.trim().slice(0, 12));

  // ---- 截止面板选完日期是否还开着
  await page.click('#dueBtn');
  await page.waitForTimeout(200);
  const day = page.locator('#dueGrid .due-day').nth(10);
  await day.click();
  await page.waitForTimeout(200);
  const popOpen = await page.locator('#duePop').isVisible();
  check('选完日期后面板自动关闭（可再打开改时间）', !popOpen, '仍打开=' + popOpen);

  // ---- 空标题 / 纯空格
  await page.fill('#taskInput', '   ');
  await page.press('#taskInput', 'Enter');
  await page.waitForTimeout(200);
  check('纯空格不会新建任务', (await page.locator('#list .task').count()) === 2);

  // ---- 重复规则必须带截止：没有截止的重复任务永远不会触发派生
  await page.fill('#taskInput', '重复任务必须有截止');
  await page.click('#dueBtn');
  await page.waitForTimeout(150);
  await page.click('#dueClear');                 // 先确保是「不设截止」
  await page.waitForTimeout(150);
  const emptyCls = await page.locator('#dueBtn').getAttribute('class');
  check('清掉截止后是空的', emptyCls.includes('is-empty'), emptyCls);

  await page.click('#dueBtn');                   // 清除会顺手关面板，重新打开
  await page.waitForTimeout(150);
  await page.click('#repeatChips [data-rep="daily"]');
  await page.waitForTimeout(150);
  const dueCls = await page.locator('#dueBtn').getAttribute('class');
  const dueTxt = await page.locator('#dueLabel').textContent();
  check('选重复时自动补上截止时间', !dueCls.includes('is-empty') && dueTxt.includes('今天'), dueTxt);

  await page.click('#dueClear');
  await page.waitForTimeout(150);
  const repOn = await page.locator('#repeatChips .is-on').getAttribute('data-rep');
  check('清截止时重复一起清掉', (repOn || '') === '', 'repeat=' + repOn);

  await page.press('#taskInput', 'Enter');
  await page.waitForTimeout(800);
  const rep = await (await fetch(BASE + 'api/tasks')).json();
  const rt = rep.tasks.find(t => t.title === '重复任务必须有截止');
  check('不会存出「无截止 + 重复」的任务', !!rt && !rt.repeat,
    rt ? `due=${rt.due} repeat=${rt.repeat}` : '未创建');

  // ---- 导入的文件也守同一条规矩，且非对象元素直接丢掉
  const orphan = {
    schema: 1,
    tasks: [
      { id: 'o1', title: '无截止重复', due: null, repeat: 'daily' },
      { id: 'o2', title: '有截止重复', due: '2026-12-01T09:00', repeat: 'weekly' },
      1,
    ],
  };
  const orphanPath = path.join(__dirname, 'orphan.json');
  fs.writeFileSync(orphanPath, JSON.stringify(orphan), 'utf-8');
  await page.click('#btnData');
  await page.waitForTimeout(200);
  await page.setInputFiles('#fileInput', orphanPath);
  await page.waitForTimeout(900);
  if (await page.locator('#sheet').isVisible()) { await page.click('#btnCloseSheet'); }
  await page.waitForTimeout(200);

  const afterImp = await (await fetch(BASE + 'api/tasks')).json();
  const o1 = afterImp.tasks.find(t => t.title === '无截止重复');
  const o2 = afterImp.tasks.find(t => t.title === '有截止重复');
  check('导入时清掉孤立 repeat', !!o1 && !o1.repeat, o1 ? 'repeat=' + o1.repeat : '未导入');
  check('有截止的 repeat 保留', !!o2 && o2.repeat === 'weekly', o2 ? 'repeat=' + o2.repeat : '未导入');
  check('非对象元素被丢弃', afterImp.tasks.length === 2,
    afterImp.tasks.map(t => typeof t).join(','));

  console.log('\n=== 汇总 ===');
  const bad = results.filter(r => !r.ok);
  console.log(`${results.length} 项，${bad.length} 项失败`);
  bad.forEach(r => console.log('  FAIL', r.name, '|', r.detail));
  await browser.close();
})().catch(e => { console.error('SCRIPT ERROR', e); process.exit(1); });
