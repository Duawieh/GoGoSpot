// 预估单位切换：用户自己打的数字只换乘数，回填的数字才做换算
const { chromium } = require('playwright');

const BASE = process.env.GOGOSPOT_BASE || 'http://127.0.0.1:8770/';
const results = [];
const check = (name, ok, detail = '') => {
  results.push({ name, ok: !!ok, detail });
  console.log((ok ? 'PASS ' : 'FAIL ') + name + (detail ? '  | ' + detail : ''));
};

(async () => {
  await fetch(BASE + 'api/tasks', {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      schema: 1,
      tasks: [
        { id: 'e1', title: '已有 90 分钟的任务', done: false, pinned: false,
          estimate: 90, due: null, startedAt: null, repeat: '', links: [], tags: [] },
      ],
    }),
  });

  const browser = await chromium.launch({ channel: 'chrome', headless: true });
  const ctx = await browser.newContext({ viewport: { width: 1200, height: 820 }, locale: 'zh-CN' });
  const page = await ctx.newPage();
  await page.goto(BASE, { waitUntil: 'networkidle' });
  await page.waitForTimeout(400);

  const unit = () => page.locator('#estUnitBtn').textContent();
  const val = () => page.locator('#estInput').inputValue();

  // ---- 用户自己打数字
  await page.click('#estInput');
  await page.waitForTimeout(120);
  await page.fill('#estInput', '2');
  await page.waitForTimeout(120);
  const u0 = await unit();
  await page.click('#estUnitBtn');                 // 分 -> 日
  await page.waitForTimeout(150);
  const u1 = await unit();
  const v1 = await val();
  check('手打数字换单位不改数字', v1 === '2', `${u0} -> ${u1}, 值=${v1}`);
  check('单位确实换了', u0 !== u1, `${u0} -> ${u1}`);

  await page.click('#estUnitBtn');                 // 日 -> 时
  await page.waitForTimeout(150);
  check('再换一次数字还是 2', (await val()) === '2', `值=${await val()}`);
  await page.keyboard.press('Escape');
  await page.waitForTimeout(150);

  // ---- 回填的数字
  await page.locator('#list .task').first().locator('[data-edit]').click();
  await page.waitForTimeout(400);
  check('编辑面板打开', await page.locator('#editMask').isVisible(), '');

  // 直接走新建流程回填不现实，改为：在编辑面板里看 90 分钟怎么显示
  const editEst = page.locator('#editFields #estInput');
  if (await editEst.count()) {
    await editEst.click();
    await page.waitForTimeout(150);
    const before = await page.locator('#editFields #estUnitBtn').textContent();
    const beforeVal = await editEst.inputValue();
    await page.locator('#editFields #estUnitBtn').click();
    await page.waitForTimeout(150);
    const after = await page.locator('#editFields #estUnitBtn').textContent();
    const afterVal = await editEst.inputValue();
    check('回填的数字换单位做换算', beforeVal !== afterVal,
      `${beforeVal}${before} -> ${afterVal}${after}`);
  } else {
    check('回填的数字换单位做换算', false, '找不到编辑面板的预估输入框');
  }

  await browser.close();
  const bad = results.filter((r) => !r.ok);
  console.log(`\n=== 汇总 ===\n${results.length} 项，${bad.length} 项失败`);
  bad.forEach((r) => console.log('  FAIL', r.name, '|', r.detail));
  process.exit(bad.length ? 1 : 0);
})().catch((e) => { console.error('SCRIPT ERROR', e); process.exit(1); });
