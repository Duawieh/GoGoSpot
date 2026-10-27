// 控制台第二批：重复任务、beforeunload、导入注入、多分辨率、主题、性能
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
async function api(pathname, opts) {
  const r = await fetch(BASE + pathname, opts);
  return r.json();
}
async function put(tasks) {
  await api('api/tasks', { method: 'PUT', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ schema: 1, tasks }) });
}
const tomorrow = (h) => {
  const d = new Date(Date.now() + 86400000);
  d.setHours(h, 0, 0, 0);
  const p = (n) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}T${p(d.getHours())}:${p(d.getMinutes())}`;
};


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

  // ---------- 1. 控制台勾完成重复任务 ----------
  await put([{ id: 'rep-1', title: '每日站会', done: false, estimate: 0, due: tomorrow(9),
    tags: [], links: [], startedAt: 0, repeat: 'daily', createdAt: new Date().toISOString(),
    completedAt: null }]);
  let ctx = await browser.newContext({ viewport: { width: 1200, height: 800 }, locale: 'zh-CN' });
  let page = await ctx.newPage();
  await page.goto(BASE, { waitUntil: 'networkidle' });
  await page.locator('#list .task .check').first().click();
  await page.waitForTimeout(1500);
  let data = await api('api/tasks');
  check('控制台勾完成重复任务会派生下一期', data.tasks.length === 2,
    '任务数=' + data.tasks.length);
  const orig = data.tasks.find(t => t.id === 'rep-1');
  check('控制台勾完成后原任务 repeat 被清空', orig && orig.repeat === '',
    'repeat=' + JSON.stringify(orig && orig.repeat));
  await ctx.close();

  // ---------- 2. beforeunload ----------
  await put([{ id: 't1', title: '原始标题', done: false, estimate: 0, due: null, tags: [],
    links: [], startedAt: 0, repeat: '', createdAt: new Date().toISOString(), completedAt: null }]);
  ctx = await browser.newContext({ viewport: { width: 1200, height: 800 } });
  page = await ctx.newPage();
  await page.goto(BASE, { waitUntil: 'networkidle' });
  const fireUnload = () => page.evaluate(() => {
    const e = new Event('beforeunload', { cancelable: true });
    window.dispatchEvent(e);
    return e.defaultPrevented;
  });
  const preventedClean = await fireUnload();
  check('刚打开、无改动时关页不拦', !preventedClean, 'defaultPrevented=' + preventedClean);
  await page.locator('#list .task .task-rename').click();
  await page.waitForTimeout(250);
  await page.fill('#editTitle', '改过的标题');
  await page.click('#btnEditSave');
  await page.waitForTimeout(1200);
  const saveText = await page.textContent('#saveText');
  check('改动已落盘', /已保存/.test(saveText), saveText);
  const preventedAfter = await fireUnload();
  check('已保存后关页不再弹“离开”确认', !preventedAfter, 'defaultPrevented=' + preventedAfter);
  await ctx.close();

  // ---------- 3. 导入带引号的 id（XSS） ----------
  const evil = {
    schema: 1,
    tasks: [{
      id: 'x"><img src=x onerror="window.__xss=1">',
      title: '导入的任务', done: false, estimate: 0, due: null, tags: [], links: [],
      startedAt: 0, repeat: '', createdAt: new Date().toISOString(), completedAt: null,
    }],
  };
  const evilPath = path.join(__dirname, 'evil.json');
  fs.writeFileSync(evilPath, JSON.stringify(evil), 'utf-8');
  ctx = await browser.newContext({ viewport: { width: 1200, height: 800 } });
  page = await ctx.newPage();
  page.on('dialog', (d) => d.accept());
  await page.goto(BASE, { waitUntil: 'networkidle' });
  await page.click('#btnData');
  await page.waitForTimeout(300);
  await page.setInputFiles('#fileInput', evilPath);
  await page.waitForTimeout(800);
  const xss = await page.evaluate(() => window.__xss || 0);
  check('导入的 id 不能注入脚本', !xss, 'window.__xss=' + xss);
  await page.screenshot({ path: path.join(SHOTS, 'c5-import-id.png') });
  await ctx.close();

  // ---------- 4. 多分辨率 ----------
  await put([1, 2, 3].map((i) => ({ id: 'w' + i, title: '布局任务 ' + i, done: false,
    estimate: 30, due: null, tags: [], links: [], startedAt: 0, repeat: '',
    createdAt: new Date().toISOString(), completedAt: null })));
  const widths = [1440, 1024, 800, 620, 480, 375];
  for (const w of widths) {
    ctx = await browser.newContext({ viewport: { width: w, height: 800 }, locale: 'zh-CN' });
    page = await ctx.newPage();
    await page.goto(BASE, { waitUntil: 'networkidle' });
    await page.waitForTimeout(300);
    const m = await page.evaluate(() => ({
      sw: document.documentElement.scrollWidth,
      cw: document.documentElement.clientWidth,
      overflow: [...document.querySelectorAll('body *')]
        .filter(n => n.getBoundingClientRect().right > window.innerWidth + 1)
        .slice(0, 5).map(n => n.className || n.tagName),
    }));
    check(`宽度 ${w} 无横向溢出`, m.sw <= m.cw + 1, JSON.stringify(m));
    await page.screenshot({ path: path.join(SHOTS, `c6-w${w}.png`), fullPage: true });

    // 窄屏下截止面板是否超出视口
    if (w <= 480) {
      await page.click('#dueBtn');
      await page.waitForTimeout(200);
      const box = await page.locator('#duePop').boundingBox();
      check(`宽度 ${w} 截止面板在视口内`, box && box.x >= -1 && box.x + box.width <= w + 1,
        JSON.stringify(box));
      await page.screenshot({ path: path.join(SHOTS, `c7-due-w${w}.png`) });
      await page.keyboard.press('Escape');
      await page.click('#btnData');
      await page.waitForTimeout(300);
      const sheet = await page.locator('#sheet').boundingBox();
      check(`宽度 ${w} 设置面板不超宽`, sheet && sheet.x >= -1 && sheet.width <= w + 1,
        JSON.stringify(sheet));
      await page.screenshot({ path: path.join(SHOTS, `c8-sheet-w${w}.png`) });
    }
    await ctx.close();
  }

  // ---------- 5. 主题 ----------
  ctx = await browser.newContext({ viewport: { width: 1200, height: 800 } });
  page = await ctx.newPage();
  await page.goto(BASE, { waitUntil: 'networkidle' });
  const before = await page.getAttribute('html', 'data-theme');
  await page.click('#btnTheme');
  await page.waitForTimeout(200);
  const after = await page.getAttribute('html', 'data-theme');
  check('主题可切换', before !== after, before + ' → ' + after);
  await page.screenshot({ path: path.join(SHOTS, `c9-theme-${after}.png`) });
  await ctx.close();

  // ---------- 6. 性能：500 条 ----------
  const many = [];
  for (let i = 0; i < 500; ++i) {
    many.push({ id: 'p' + i, title: '压测任务 ' + i + ' 号', done: i % 3 === 0, estimate: 30,
      due: tomorrow(9), tags: ['批' + (i % 5)], links: [], startedAt: 0, repeat: '',
      createdAt: new Date().toISOString(), completedAt: null });
  }
  await put(many);
  ctx = await browser.newContext({ viewport: { width: 1200, height: 800 } });
  page = await ctx.newPage();
  const t0 = Date.now();
  await page.goto(BASE, { waitUntil: 'networkidle' });
  const loadMs = Date.now() - t0;
  await page.waitForTimeout(300);
  const t1 = Date.now();
  await page.fill('#searchInput', '压测任务 499');
  await page.waitForTimeout(50);
  const searchMs = Date.now() - t1;
  check('500 条加载 < 3s', loadMs < 3000, loadMs + 'ms');
  check('500 条搜索一次 < 300ms', searchMs < 300, searchMs + 'ms');
  check('搜索命中 1 条', (await page.locator('#list .task').count()) === 1);
  await page.screenshot({ path: path.join(SHOTS, 'c10-500.png') });
  await ctx.close();

  // ---------- 7. 100% 进度标签是否溢出 ----------
  const doneTask = (i) => ({ id: 'd' + i, title: '完成 ' + i, done: true, estimate: 0, due: null,
    tags: [], links: [], startedAt: 0, repeat: '', createdAt: new Date().toISOString(),
    completedAt: new Date().toISOString() });
  await put([doneTask(1), doneTask(2)]);
  ctx = await browser.newContext({ viewport: { width: 1200, height: 800 } });
  page = await ctx.newPage();
  await page.goto(BASE, { waitUntil: 'networkidle' });
  await page.waitForTimeout(300);
  const pctBox = await page.locator('#progressPct').boundingBox();
  const trackBox = await page.locator('.ov-track').boundingBox();
  check('100% 时百分比标签不溢出轨道', pctBox.x + pctBox.width <= trackBox.x + trackBox.width + 2,
    JSON.stringify({ pctBox, trackBox }));
  await page.screenshot({ path: path.join(SHOTS, 'c11-100pct.png') });
  await ctx.close();

  await browser.close();
  console.log('\n=== 汇总 ===');
  const bad = results.filter(r => !r.ok);
  console.log(`${results.length} 项，${bad.length} 项失败`);
  bad.forEach(r => console.log('  FAIL', r.name, '|', r.detail));
  fs.writeFileSync(path.join(__dirname, 'console-log2.json'), JSON.stringify(results, null, 2));
})().catch((e) => { console.error('SCRIPT ERROR', e); process.exit(1); });
