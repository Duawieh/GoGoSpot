/* GoGoSpot 控制台前端：状态存在内存，改动 debounce 后整份 PUT 给后端落盘。 */

'use strict';

const $ = (id) => document.getElementById(id);

const el = {
  todayLabel:   $('todayLabel'),
  statDone:     $('statDone'),
  statTotal:    $('statTotal'),
  statPending:  $('statPending'),
  progressBar:  $('progressBar'),
  progressPct:  $('progressPct'),
  taskInput:    $('taskInput'),
  btnAdd:       $('btnAdd'),
  estField:     $('estField'),
  estBtn:       $('estBtn'),
  estLabel:     $('estLabel'),
  estMenu:      $('estMenu'),
  dueField:     $('dueField'),
  dueBtn:       $('dueBtn'),
  dueLabel:     $('dueLabel'),
  duePop:       $('duePop'),
  dueMonth:     $('dueMonth'),
  dueGrid:      $('dueGrid'),
  duePrev:      $('duePrev'),
  dueNext:      $('dueNext'),
  dueHH:        $('dueHH'),
  dueMM:        $('dueMM'),
  dueClear:     $('dueClear'),
  tagInput:     $('tagInput'),
  viewGroup:    $('viewGroup'),
  searchInput:  $('searchInput'),
  btnClearSearch: $('btnClearSearch'),
  tagRow:       $('tagRow'),
  list:         $('list'),
  empty:        $('empty'),
  emptyTitle:   $('emptyTitle'),
  emptySub:     $('emptySub'),
  footerLeft:   $('footerLeft'),
  btnClearDone: $('btnClearDone'),
  saveState:    $('saveState'),
  saveText:     $('saveText'),
  btnTheme:     $('btnTheme'),
  btnData:      $('btnData'),
  swWidget:     $('swWidget'),
  widgetNote:   $('widgetNote'),
  linkField:    $('linkField'),
  linkInput:    $('linkInput'),
  btnPasteLink: $('btnPasteLink'),
  linkChips:    $('linkChips'),
  btnStart:     $('btnStart'),
  swTopmost:    $('swTopmost'),
  swAutostart:  $('swAutostart'),
  sheetMask:    $('sheetMask'),
  btnCloseSheet:$('btnCloseSheet'),
  infoPath:     $('infoPath'),
  btnManualBackup: $('btnManualBackup'),
  btnExport:    $('btnExport'),
  btnImport:    $('btnImport'),
  fileInput:    $('fileInput'),
  backupList:   $('backupList'),
  backupCount:  $('backupCount'),
  toastWrap:    $('toastWrap'),
};

const state = {
  tasks: [],
  view: 'all',
  estimate: 0,
  dueDate: '',
  dueTime: '23:59',
  query: '',
  tag: null,
  editingId: null,
  cancelEdit: false,
  saveTimer: null,
  prevIds: null,
  loaded: false,
  loadError: false,          // 读不出任务文件时，禁止回写，免得把原数据覆盖成空表
  links: [],                 // 正在添加的这条任务的「从这里开始」
  repeat: '',                // 重复规则：'' / daily / weekly / biweekly / monthly
};

// 同一时刻只允许一份保存在途，否则后发的旧数据可能盖掉新数据
const saveGate = { inflight: false, again: false };

/* ---------------------------------------------------------------- 工具 */

const esc = (s) => String(s).replace(/[&<>"']/g, (c) => ({
  '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
}[c]));

function uid() {
  if (window.crypto && crypto.randomUUID) return crypto.randomUUID();
  return 't' + Date.now().toString(36) + Math.random().toString(36).slice(2, 9);
}

function todayStr() {
  const d = new Date();
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
}

// 截止时间统一存成 "YYYY-MM-DDTHH:mm"（本地时间）。
// 老数据只有日期，按当天 23:59 补齐。
function normalizeDue(v) {
  if (typeof v !== 'string') return null;
  const s = v.trim();
  if (/^\d{4}-\d{2}-\d{2}$/.test(s)) return s + 'T23:59';
  if (/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}$/.test(s)) return s;
  if (/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}/.test(s)) return s.slice(0, 16);
  return null;
}

function parseDue(due) {
  if (!due) return null;
  const dt = new Date(due);
  return isNaN(dt.getTime()) ? null : dt;
}

// 紧急分级，和悬浮清单用同一套线：
// 剩余 <= min(预估时长, 12 小时) 是红色，一天以内是黄色，再远就不标。
function urgencyOf(t) {
  if (!t.due || t.done) return 0;
  const dt = parseDue(t.due);
  if (!dt) return 0;
  const remain = dt.getTime() - Date.now();
  if (remain <= 0) return 2;
  const redLine = Math.min((t.estimate || 0) * 60000, 12 * 3600000);
  if (remain <= redLine) return 2;
  if (remain <= 24 * 3600000) return 1;
  return 0;
}

function dueInfo(due) {
  const dt = parseDue(due);
  if (!dt) return null;

  const diffMs = dt.getTime() - Date.now();
  const hh = String(dt.getHours()).padStart(2, '0');
  const mm = String(dt.getMinutes()).padStart(2, '0');
  const clock = `${hh}:${mm}`;

  if (diffMs < 0) {
    const overMin = Math.round(-diffMs / 60000);
    if (overMin < 60) return { text: `逾期 ${overMin} 分钟`, cls: 'overdue' };
    const overH = overMin / 60;
    if (overH < 24) return { text: `逾期 ${Math.round(overH)} 小时`, cls: 'overdue' };
    return { text: `逾期 ${Math.round(overH / 24)} 天`, cls: 'overdue' };
  }

  const remainMin = Math.round(diffMs / 60000);
  if (remainMin < 60) return { text: `还剩 ${remainMin} 分钟`, cls: 'soon' };
  const remainH = remainMin / 60;
  if (remainH < 12) return { text: `还剩 ${Math.round(remainH)} 小时`, cls: 'soon' };

  const today = new Date(); today.setHours(0, 0, 0, 0);
  const day = new Date(dt); day.setHours(0, 0, 0, 0);
  const dayDiff = Math.round((day - today) / 86400000);

  if (dayDiff === 0) return { text: `今天 ${clock}`, cls: 'today' };
  if (dayDiff === 1) return { text: `明天 ${clock}`, cls: '' };
  if (dayDiff === 2) return { text: `后天 ${clock}`, cls: '' };
  if (dayDiff < 7) return { text: `${dayDiff} 天后 ${clock}`, cls: '' };
  return { text: `${dt.getMonth() + 1}月${dt.getDate()}日 ${clock}`, cls: '' };
}

function parseTags(raw) {
  return String(raw || '')
    .split(/[,，、\s]+/)
    .map((s) => s.trim().replace(/^#/, ''))
    .filter(Boolean)
    .filter((v, i, a) => a.indexOf(v) === i)
    .slice(0, 6);
}

function fmtDuration(min) {
  if (!min) return '';
  if (min < 60) return min + ' 分钟';
  if (min >= 1440) {
    const d = min / 1440;
    return (Number.isInteger(d) ? d : d.toFixed(1)) + ' 天';
  }
  const h = min / 60;
  return (Number.isInteger(h) ? h : h.toFixed(1)) + ' 小时';
}

function fmtSize(n) {
  if (n < 1024) return n + ' B';
  if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
  return (n / 1024 / 1024).toFixed(2) + ' MB';
}

function normalize(t) {
  const src = (t && typeof t === 'object' && !Array.isArray(t)) ? t : {};
  const due = normalizeDue(src.due);
  return {
    id: src.id || uid(),
    title: String(src.title || '').slice(0, 500),
    done: !!src.done,
    estimate: Number.isFinite(Number(src.estimate)) && Number(src.estimate) > 0
      ? Math.min(Math.round(Number(src.estimate)), 24 * 60)
      : 0,
    due,
    tags: Array.isArray(src.tags) ? src.tags.filter((x) => typeof x === 'string').slice(0, 6) : [],
    links: Array.isArray(src.links)
      ? src.links.filter((x) => typeof x === 'string' && x.trim()).slice(0, 8)
      : [],
    startedAt: Number.isFinite(Number(src.startedAt)) && Number(src.startedAt) > 0
      ? Number(src.startedAt)
      : 0,
    pinned: !!src.pinned,
    // 没有截止时间的重复永远等不到下一期，导入的旧数据也按这条规矩清掉
    repeat: due && REPEATS.some((r) => r.key === src.repeat) ? src.repeat : '',
    createdAt: src.createdAt || new Date().toISOString(),
    completedAt: src.completedAt || null,
  };
}

// 只有对象才算一条任务：老数据或手改过的文件里可能混进数字，
// 拿它当任务会画出一行空白，看着像丢了内容
const isTaskObject = (t) => !!t && typeof t === 'object' && !Array.isArray(t);

// id 会直接进 data-* 属性。导入的文件可能带引号、尖括号，先只留安全字符；
// 空的和重名的重新发一个，避免两条任务共用一个 id。
function sanitizeIds(list) {
  const used = new Set();
  for (const t of list) {
    let id = String(t.id || '').replace(/[^A-Za-z0-9_-]/g, '').slice(0, 64);
    if (!id || used.has(id)) id = uid();
    t.id = id;
    used.add(id);
  }
  return list;
}

/* ---------------------------------------------------------------- 存储 */

function setSaveState(kind, text) {
  el.saveState.classList.remove('is-saving', 'is-error');
  if (kind === 'saving') el.saveState.classList.add('is-saving');
  if (kind === 'error') el.saveState.classList.add('is-error');
  el.saveText.textContent = text;
}

function scheduleSave() {
  if (state.loadError) {          // 读不出来的状态下回写，会把原数据整份覆盖掉
    setSaveState('error', '读取失败，改动暂不保存');
    return;
  }
  setSaveState('saving', '保存中…');
  clearTimeout(state.saveTimer);
  state.saveTimer = setTimeout(saveNow, 420);
}

function currentPayload() {
  return {
    schema: 1,
    updatedAt: new Date().toISOString(),
    tasks: state.tasks,
  };
}

async function saveNow(retry) {
  clearTimeout(state.saveTimer);
  state.saveTimer = null;

  if (state.loadError) return;
  if (saveGate.inflight) { saveGate.again = true; return; }
  saveGate.inflight = true;
  saveGate.again = false;

  const attempt = retry || 0;
  try {
    const res = await fetch('/api/tasks', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(currentPayload()),
    });
    if (!res.ok) {
      let msg = 'HTTP ' + res.status;
      try { const d = await res.json(); if (d && d.error) msg = d.error; } catch (e) { /* 保持默认 */ }
      throw new Error(msg);
    }
    const d = new Date();
    const hh = String(d.getHours()).padStart(2, '0');
    const mm = String(d.getMinutes()).padStart(2, '0');
    const ss = String(d.getSeconds()).padStart(2, '0');
    setSaveState('ok', `已保存 ${hh}:${mm}:${ss}`);
  } catch (e) {
    saveGate.inflight = false;
    if (attempt < 2) {
      setSaveState('saving', '连接中断，正在重试…');
      state.saveTimer = setTimeout(() => saveNow(attempt + 1), 1800);
      return;
    }
    setSaveState('error', '保存失败：' + e.message);
    return;
  }

  saveGate.inflight = false;
  if (saveGate.again) saveNow();
}

async function loadTasks() {
  try {
    const res = await fetch('/api/tasks', { cache: 'no-store' });
    if (!res.ok) {
      let msg = 'HTTP ' + res.status;
      try { const d = await res.json(); if (d && d.error) msg = d.error; } catch (e) { /* 保持默认 */ }
      throw new Error(msg);
    }
    const data = await res.json();
    if (!data || !Array.isArray(data.tasks)) throw new Error('数据格式不对');
    state.tasks = sanitizeIds(data.tasks.filter(isTaskObject).map(normalize));
    state.loadError = false;
    setSaveState('ok', '已同步');
  } catch (e) {
    // 保留内存里已有的列表，同时锁住保存：坏文件不该被一份空表覆盖
    state.loadError = true;
    setSaveState('error', '读取失败');
    toast('读取任务失败：' + e.message);
  }
  state.loaded = true;
}

/* ---------------------------------------------------------------- 渲染 */

// 和悬浮清单共用一套排序：先看置顶，再看截止时间，没截止的垫底；
// 完全相同的保持原来的先后（sort 是稳定的）
function byPinThenDue(a, b) {
  if (!!a.pinned !== !!b.pinned) return a.pinned ? -1 : 1;
  const da = parseDue(a.due);
  const db = parseDue(b.due);
  const ta = da ? da.getTime() : Infinity;
  const tb = db ? db.getTime() : Infinity;
  if (ta === tb) return 0;
  return ta - tb;
}

function visibleTasks() {
  let arr = state.tasks.slice();
  const today = todayStr();

  if (state.view === 'pending') arr = arr.filter((t) => !t.done);
  else if (state.view === 'done') arr = arr.filter((t) => t.done);
  else if (state.view === 'today') {
    const todayEnd = today + 'T23:59';
    arr = arr.filter((t) => !t.done && t.due && t.due <= todayEnd);
  }

  if (state.tag) arr = arr.filter((t) => t.tags.includes(state.tag));

  const q = state.query.trim().toLowerCase();
  if (q) {
    arr = arr.filter((t) =>
      t.title.toLowerCase().includes(q) ||
      t.tags.join(' ').toLowerCase().includes(q));
  }

  if (state.view === 'done') {
    arr.sort((a, b) => String(b.completedAt || '').localeCompare(String(a.completedAt || '')));
  } else {
    const pending = arr.filter((t) => !t.done).sort(byPinThenDue);
    const done = arr.filter((t) => t.done)
      .sort((a, b) => String(b.completedAt || '').localeCompare(String(a.completedAt || '')));
    arr = pending.concat(done);
  }
  return arr;
}

// 正在进行：预计最晚开始 = 截止 - 预计用时；起始 = min(点下去的时刻, 最晚开始)
// 没填预估的任务算不出占比，界面用固定 60% 的紫色表示「在做，但不知道还剩多少」
function hasEstimate(t) {
  return t.estimate > 0;
}

function progressRatio(t) {
  if (!t.startedAt || !hasEstimate(t)) return 0;
  const estMs = t.estimate * 60000;
  const dueMs = t.due ? new Date(t.due).getTime() : 0;
  const latest = dueMs ? dueMs - estMs : t.startedAt;
  const start = Math.min(t.startedAt, latest);
  return Math.max(0, Math.min(1, (Date.now() - start) / estMs));
}

const RING_C = 2 * Math.PI * 9;      // 半径 9 的周长

// 进行中 → 常显「停止」按钮；没进行 → 悬停才出现的「正在进行」按钮
function startButton(t) {
  const urg = urgencyOf(t);
  const id = esc(t.id);
  if (t.startedAt) {
    const cls = urg === 2 ? 'is-red' : (urg === 1 ? 'is-yellow' : 'is-purple');
    return `<button class="task-go is-stop ${cls}" data-start="${id}" data-on="0"
                    title="停止进行" aria-label="停止进行">
        <svg viewBox="0 0 24 24" width="11" height="11"><rect x="6" y="6" width="12" height="12" rx="1.6" fill="currentColor"/></svg>
      </button>`;
  }
  const bcls = urg === 2 ? 'is-red' : (urg === 1 ? 'is-yellow' : 'is-purple');
  return `<button class="task-go ${bcls}" data-start="${id}" data-on="1"
                  title="正在进行" aria-label="正在进行">
      <svg viewBox="0 0 24 24" width="12" height="12"><path d="M8 5.5v13l11-6.5z" fill="currentColor"/></svg>
    </button>`;
}

// 行顶那条直进度条。颜色按「用时占比」分档：<=60% 绿，<=80% 黄，再往上红
function progressBar(t) {
  if (!t.startedAt) return '';
  const id = esc(t.id);
  if (!hasEstimate(t)) {
    return `<div class="task-progress is-unknown" data-bar="${id}">
      <i style="width:60%"></i>
    </div>`;
  }
  const r = progressRatio(t);
  const cls = r <= 0.6 ? 'is-green' : (r <= 0.8 ? 'is-yellow' : 'is-red');
  const pct = Math.round(r * 1000) / 10;
  return `<div class="task-progress ${cls}" data-bar="${id}">
      <i style="width:${pct}%"></i>
    </div>`;
}

// 重复规则：值存进任务里，标签用于展示
const REPEATS = [
  { key: '', label: '' },
  { key: 'daily', label: '每天' },
  { key: 'weekly', label: '每星期' },
  { key: 'biweekly', label: '每两星期' },
  { key: 'monthly', label: '每月' },
];

function repeatLabel(key) {
  const r = REPEATS.find((x) => x.key === key);
  return r ? r.label : '';
}

function taskHTML(t) {
  const di = dueInfo(t.due);
  const urg = urgencyOf(t);
  const urgCls = urg === 2 ? ' urgent' : (urg === 1 ? ' urgent-y' : '');
  const id = esc(t.id);
  const meta = [];
  if (t.estimate) meta.push(`<span class="m-est">约 ${esc(fmtDuration(t.estimate))}</span>`);
  if (di) meta.push(`<span class="m-due ${di.cls}">${esc(di.text)}</span>`);
  if (t.repeat) meta.push(`<span class="m-rep">${esc(repeatLabel(t.repeat))}</span>`);
  for (const tag of t.tags) meta.push(`<span class="m-tag">${esc(tag)}</span>`);

  const title = state.editingId === t.id
    ? `<input class="task-edit" type="text" value="${esc(t.title)}" data-edit="${id}">`
    : `<div class="task-title" data-title="${id}">${esc(t.title)}</div>`;

  // 挂了材料的任务，标题旁边给一个「从这里开始」的小图标
  const linkBtn = (t.links && t.links.length)
    ? `<button class="task-start" data-open="${id}" title="从这里开始：${t.links.length} 项材料">
         <svg viewBox="0 0 24 24" width="14" height="14" fill="none">
           <path d="M10.6 13.4a3.3 3.3 0 004.7 0l2.5-2.5a3.3 3.3 0 00-4.7-4.7l-1.2 1.2"
                 stroke="currentColor" stroke-width="1.7" stroke-linecap="round"/>
           <path d="M13.4 10.6a3.3 3.3 0 00-4.7 0l-2.5 2.5a3.3 3.3 0 004.7 4.7l1.2-1.2"
                 stroke="currentColor" stroke-width="1.7" stroke-linecap="round"/>
         </svg>
       </button>`
    : '';

  return `
    <li class="task${t.done ? ' done' : ''}${urgCls}${t.startedAt ? ' is-running' : ''}" data-id="${id}">
      ${progressBar(t)}
      <button class="check" data-check="${id}" aria-label="标记完成">
        <svg viewBox="0 0 24 24"><path d="M4.5 12.5l5 5L19.5 7"/></svg>
      </button>
      <div class="task-body">
        <div class="task-head">${title}${linkBtn}</div>
        <div class="task-meta">${meta.join('')}</div>
      </div>
      <button class="task-pin${t.pinned ? ' is-on' : ''}" data-pin="${id}"
              aria-pressed="${t.pinned ? 'true' : 'false'}"
              aria-label="${t.pinned ? '取消置顶' : '置顶'}"
              title="${t.pinned ? '取消置顶' : '置顶'}">
        <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor"
             stroke-width="1.7" stroke-linejoin="round" stroke-linecap="round">
          <path d="M12 17v5"/>
          <path d="M9 10.76a2 2 0 0 1-1.11 1.79l-1.78.9A2 2 0 0 0 5 15.24V16a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-.76a2 2 0 0 0-1.11-1.79l-1.78-.9A2 2 0 0 1 15 10.76V7a1 1 0 0 1 1-1 2 2 0 0 0 0-4H8a2 2 0 0 0 0 4 1 1 0 0 1 1 1z"/>
        </svg>
      </button>
      ${startButton(t)}
      <button class="task-rename" data-edit="${id}" aria-label="改名" title="改名">
        <svg viewBox="0 0 24 24" width="14" height="14" fill="none">
          <path d="M4 20h4L19 9a2.5 2.5 0 00-3.5-3.5L4.5 16.5 4 20z" stroke="currentColor"
                stroke-width="1.7" stroke-linejoin="round"/>
        </svg>
      </button>
      <button class="task-del" data-del="${id}" aria-label="删除">
        <svg viewBox="0 0 24 24" width="15" height="15" fill="none">
          <path d="M6 6l12 12M18 6L6 18" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"/>
        </svg>
      </button>
    </li>`;
}

// 重排动画：先记下每一行原来的纵向位置，重排后让它从旧位置滑到新位置。
// 勾选任务时，被勾的那条会滑到列表底部，下面的任务跟着补位。
function captureRows() {
  const m = new Map();
  el.list.querySelectorAll('.task').forEach((n) => {
    const id = n.dataset.id;
    if (id) m.set(id, n.getBoundingClientRect().top);
  });
  return m;
}

function playRowMove(before) {
  if (!before.size) return;
  el.list.querySelectorAll('.task').forEach((n) => {
    const id = n.dataset.id;
    if (!id || !before.has(id)) return;
    const dy = before.get(id) - n.getBoundingClientRect().top;
    if (Math.abs(dy) < 1) return;
    n.style.transition = 'none';
    n.style.transform = `translateY(${dy}px)`;
    void n.offsetHeight;              // 先把起点落定，否则会和后面的过渡合并成一帧
    n.style.transition = '';
    n.style.transform = '';
  });
}

function render() {
  const all = state.tasks;
  const total = all.length;
  const done = all.filter((t) => t.done).length;
  const pending = total - done;
  const pct = total ? Math.round((done / total) * 100) : 0;

  el.statDone.textContent = done;
  el.statTotal.textContent = total;
  el.statPending.textContent = pending;
  el.progressBar.style.setProperty('--p', pct + '%');
  el.progressPct.textContent = pct + '%';

  const items = visibleTasks();
  const ids = new Set(items.map((t) => t.id));

  // 行数一多，captureRows 的 getBoundingClientRect 会强制整页重排，
  // 5000 条时光这一项就接近一秒。超过这个量级直接不播重排动画。
  const beforeRows = items.length > 300 ? new Map() : captureRows();
  el.list.innerHTML = items.map((t) => {
    let html = taskHTML(t);
    if (state.prevIds && !state.prevIds.has(t.id)) {
      html = html.replace('class="task', 'class="task animate');
    }
    return html;
  }).join('');
  playRowMove(beforeRows);

  state.prevIds = ids;

  // 空状态
  if (items.length === 0) {
    el.empty.hidden = false;
    const msg = emptyMessage();
    el.emptyTitle.textContent = msg.title;
    el.emptySub.textContent = msg.sub;
  } else {
    el.empty.hidden = true;
  }

  // 标签行
  const counts = new Map();
  for (const t of all) for (const g of t.tags) counts.set(g, (counts.get(g) || 0) + 1);
  const tags = [...counts.entries()].sort((a, b) => b[1] - a[1] || a[0].localeCompare(b[0]));
  el.tagRow.innerHTML = tags.map(([g]) =>
    `<button class="tag-chip${state.tag === g ? ' is-on' : ''}" data-tag="${esc(g)}">${esc(g)}</button>`
  ).join('');

  // 底栏
  const loadMin = all.filter((t) => !t.done && t.estimate).reduce((s, t) => s + t.estimate, 0);
  const loadText = loadMin ? `（预计 ${fmtDuration(loadMin)}）` : '';
  el.footerLeft.textContent = total === 0
    ? '共 0 项'
    : `共 ${total} 项 · ${pending} 项待办${loadText}${done ? ` · ${done} 项已完成` : ''}`;
  el.btnClearDone.disabled = done === 0;

  // 编辑框聚焦
  const editInput = el.list.querySelector('.task-edit');
  if (editInput) {
    editInput.focus();
    const v = editInput.value;
    editInput.setSelectionRange(v.length, v.length);
  }
}

function emptyMessage() {
  if (state.loadError) {
    return { title: '任务文件读取失败', sub: '改动不会写回磁盘。可以到设置里从历史备份恢复' };
  }
  if (state.query.trim()) return { title: '没有匹配的任务', sub: '换个关键词试试，或清空搜索框' };
  if (state.tag) return { title: `标签 ${state.tag} 下没有任务`, sub: '点击标签可以取消筛选' };
  if (state.view === 'done') return { title: '还没有完成的任务', sub: '完成一项后，它会归档到这里' };
  if (state.view === 'today') return { title: '今天没有到期任务', sub: '给任务设置截止日期，就会出现在这里' };
  if (state.view === 'pending') return { title: '待办清空了', sub: '所有任务都已完成，休息一下' };
  return { title: '还没有任务', sub: '在上面输入框写下第一件事，按 Enter 添加' };
}

/* ---------------------------------------------------------------- 操作 */

function setEstimate(minutes) {
  state.estimate = minutes || 0;
  el.estLabel.textContent = state.estimate ? fmtDuration(state.estimate) : '预估时长';
  el.estBtn.classList.toggle('is-empty', !state.estimate);
  el.estMenu.querySelectorAll('[data-est]').forEach((b) => {
    b.classList.toggle('is-on', Number(b.dataset.est) === state.estimate);
  });
}

function openEstMenu(open) {
  el.estMenu.hidden = !open;
  el.estField.classList.toggle('is-open', open);
  el.estBtn.setAttribute('aria-expanded', open ? 'true' : 'false');
}

/* ---------------------------------------------------------------- 截止时间 */

const p2 = (n) => String(n).padStart(2, '0');

function ymd(d) {
  return `${d.getFullYear()}-${p2(d.getMonth() + 1)}-${p2(d.getDate())}`;
}

function parseYmd(s) {
  const [y, m, d] = s.split('-').map(Number);
  return new Date(y, m - 1, d);
}

// 和列表里那套口径一致：今天 / 明天 / 后天 / N 天后 / 月日
function dueLabelText() {
  if (!state.dueDate) return '截止时间';

  const d = parseYmd(state.dueDate);
  const clock = state.dueTime || '23:59';
  const today = new Date();
  today.setHours(0, 0, 0, 0);
  const day = new Date(d);
  day.setHours(0, 0, 0, 0);
  const diff = Math.round((day - today) / 86400000);

  if (diff === 0) return `今天 ${clock}`;
  if (diff === 1) return `明天 ${clock}`;
  if (diff === 2) return `后天 ${clock}`;
  if (diff > 2 && diff < 7) return `${diff} 天后 ${clock}`;

  const year = d.getFullYear() === today.getFullYear() ? '' : `${d.getFullYear()}年`;
  return `${year}${d.getMonth() + 1}月${d.getDate()}日 ${clock}`;
}

function syncDue() {
  el.dueLabel.textContent = dueLabelText();
  el.dueBtn.classList.toggle('is-empty', !state.dueDate);
}

let dueView = { y: 0, m: 0 };        // 日历当前翻到哪个月

function renderDueGrid() {
  const { y, m } = dueView;
  el.dueMonth.textContent = `${y} 年 ${m + 1} 月`;

  const lead = (new Date(y, m, 1).getDay() + 6) % 7;   // 周一打头
  const todayIso = todayStr();

  let html = '';
  for (let i = 0; i < 42; ++i) {
    const d = new Date(y, m, 1 - lead + i);
    const ds = ymd(d);
    const cls = ['due-day'];
    if (d.getMonth() !== m) cls.push('is-out');
    if (ds === todayIso) cls.push('is-today');
    if (ds === state.dueDate) cls.push('is-on');
    html += `<button type="button" class="${cls.join(' ')}" data-date="${ds}">${d.getDate()}</button>`;
  }
  el.dueGrid.innerHTML = html;
}

function openDuePop(open) {
  el.duePop.hidden = !open;
  el.dueField.classList.toggle('is-open', open);
  el.dueBtn.setAttribute('aria-expanded', open ? 'true' : 'false');
  if (!open) return;

  const base = state.dueDate ? parseYmd(state.dueDate) : new Date();
  dueView = { y: base.getFullYear(), m: base.getMonth() };
  el.dueHH.value = (state.dueTime || '23:59').slice(0, 2);
  el.dueMM.value = (state.dueTime || '23:59').slice(3, 5);
  renderDueGrid();

  // 窄屏时按视口宽度夹住 left，避免面板从右边溢出
  const fr = el.dueField.getBoundingClientRect();
  const pw = el.duePop.offsetWidth || 252;
  const left = Math.max(10, Math.min(fr.left - 4, window.innerWidth - 10 - pw));
  el.duePop.style.left = `${left - fr.left}px`;
}

function pickDueDate(ds) {
  state.dueDate = ds;
  if (!state.dueTime) state.dueTime = '23:59';
  syncDue();
  openDuePop(false);
}

function readClock() {
  let h = parseInt(el.dueHH.value, 10);
  let m = parseInt(el.dueMM.value, 10);
  if (!Number.isFinite(h)) h = 23;
  if (!Number.isFinite(m)) m = 59;
  h = Math.min(23, Math.max(0, h));
  m = Math.min(59, Math.max(0, m));
  el.dueHH.value = p2(h);
  el.dueMM.value = p2(m);
  state.dueTime = `${p2(h)}:${p2(m)}`;
  syncDue();
}

function clearDue() {
  state.dueDate = '';
  state.dueTime = '23:59';
  // 没有截止时间的重复任务永远等不到下一期，清截止就一并清掉重复
  if (state.repeat) {
    state.repeat = '';
    paintRepeatChips();
  }
  syncDue();
  openDuePop(false);
}

/* ---- 从这里开始：一条任务可以挂多个链接或本地文件 ---- */

// 只显示最后一段，长路径不至于把 chip 撑爆
function shortLink(u) {
  let s = String(u).replace(/^file:[/][/][/]/i, '').replace(/\\/g, '/');
  const parts = s.split('/').filter(Boolean);
  let name = parts.length ? parts[parts.length - 1] : s;
  try { name = decodeURIComponent(name); } catch (e) { /* 保持原样 */ }
  return name.length > 26 ? name.slice(0, 24) + '…' : name;
}

function paintRepeatChips() {
  const box = $('repeatChips');
  if (!box) return;
  box.querySelectorAll('[data-rep]').forEach((b) => {
    b.classList.toggle('is-on', (b.dataset.rep || '') === (state.repeat || ''));
  });
}

function renderLinkChips() {
  if (el.btnStart) {
    el.btnStart.textContent = state.links.length
      ? '从这里开始 · ' + state.links.length
      : '从这里开始';
  }
  const box = el.linkChips;
  if (!box) return;
  box.hidden = state.links.length === 0;
  box.innerHTML = state.links.map((u, i) =>
    `<span class="link-chip" title="${esc(u)}">` +
      `<span class="link-chip-name">${esc(shortLink(u))}</span>` +
      `<button type="button" data-unlink="${i}" aria-label="移除">×</button>` +
    `</span>`).join('');
}

function addLink(raw) {
  const u = String(raw || '').trim();
  if (!u) return false;
  if (state.links.includes(u)) return false;
  if (state.links.length >= 8) { toast('最多挂 8 个'); return false; }
  state.links.push(u);
  renderLinkChips();
  return true;
}

// 系统给的可能是 file:///、也可能是裸路径，两种都留着，打开时交给系统判断
function linksFromDrop(dt) {
  const out = [];
  try {
    const uri = dt.getData('text/uri-list');
    if (uri) {
      uri.split(/\r?\n/).forEach((line) => {
        const t = line.trim();
        if (t && !t.startsWith('#')) out.push(t);
      });
    }
  } catch (e) { /* 有的浏览器不给读 */ }
  if (!out.length) {
    try {
      const plain = dt.getData('text/plain');
      if (plain && /^(https?:|file:|[a-zA-Z]:\\|\\\\)/.test(plain.trim())) out.push(plain.trim());
    } catch (e) { /* 同上 */ }
  }
  return out;
}

// 依次交给系统打开，中间隔一点，别一下把桌面刷爆
// 切换「正在进行」：按钮是开始，环是取消
async function toggleStart(id, on) {
  const t = state.tasks.find((x) => x.id === id);
  if (!t) return;
  try {
    const res = await fetch('/api/start?id=' + encodeURIComponent(id) + '&on=' + (on ? 1 : 0), {
      method: 'POST',
    });
    const data = await res.json();
    if (!data.ok) throw new Error(data.error || '保存失败');
    t.startedAt = on ? Date.now() : 0;
    render();
    toast(on ? '开始计时' : '已取消');
  } catch (e) {
    toast('没能保存，再试一次');
  }
}

async function openTaskLinks(id) {
  const t = state.tasks.find((x) => x.id === id);
  if (!t || !t.links || !t.links.length) return;
  let failed = 0;
  for (const u of t.links) {
    try {
      const res = await fetch('/api/open?url=' + encodeURIComponent(u), {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: '{}',
      });
      const data = await res.json();
      if (!data.ok) failed++;
    } catch (e) {
      failed++;
    }
    await new Promise((r) => setTimeout(r, 120));
  }
  if (failed) toast(failed + ' 项没能打开');
}

function addTask() {
  const title = el.taskInput.value.trim();
  if (!title) {
    el.taskInput.focus();
    return;
  }
  const t = normalize({
    id: uid(),
    title,
    estimate: state.estimate,
    due: state.dueDate ? `${state.dueDate}T${state.dueTime || '23:59'}` : null,
    tags: parseTags(el.tagInput.value),
    links: state.links.slice(),
    repeat: state.repeat,
    createdAt: new Date().toISOString(),
  });
  state.tasks.unshift(t);

  el.taskInput.value = '';
  setEstimate(0);
  state.dueDate = '';
  state.dueTime = '23:59';
  syncDue();
  el.tagInput.value = '';
  state.links = [];
  renderLinkChips();
  state.repeat = '';
  paintRepeatChips();
  if (el.linkField) {
    el.linkField.classList.remove('is-open');
    el.btnStart.setAttribute('aria-expanded', 'false');
  }
  el.taskInput.focus();

  render();
  scheduleSave();
}

// 勾完成走服务端的 /api/toggle，不再本地改完整份 PUT。
// 重复任务的下一期只在服务端派生，本地改法会把那条链静默断掉。
async function toggleDone(id) {
  const t = state.tasks.find((x) => x.id === id);
  if (!t) return;

  if (state.saveTimer) await saveNow();     // 先把手头没落盘的改动写掉，免得被重读冲掉
  if (state.loadError) return;

  try {
    const res = await fetch('/api/toggle?id=' + encodeURIComponent(id), { method: 'POST' });
    const data = await res.json();
    if (!res.ok || !data.ok) throw new Error(data.error || ('HTTP ' + res.status));
  } catch (e) {
    toast('没能保存：' + e.message);
    return;
  }

  await loadTasks();                        // 派生出来的下一期只有服务端知道
  render();
}

// 置顶只改一个字段，跟着整份保存走，和改名、改标签是同一条路
function togglePin(id) {
  const t = state.tasks.find((x) => x.id === id);
  if (!t) return;
  t.pinned = !t.pinned;
  render();
  scheduleSave();
}

function deleteTask(id) {
  const idx = state.tasks.findIndex((x) => x.id === id);
  if (idx < 0) return;
  const removed = state.tasks[idx];

  const node = el.list.querySelector(`.task[data-id="${CSS.escape(id)}"]`);
  const finish = () => {
    state.tasks.splice(idx, 1);
    render();
    scheduleSave();
  };

  if (node) {
    node.classList.add('removing');
    setTimeout(finish, 190);
  } else {
    finish();
  }

  toast('已删除一项任务', '撤销', () => {
    state.tasks.splice(Math.min(idx, state.tasks.length), 0, removed);
    render();
    scheduleSave();
  });
}

function clearDone() {
  const snapshot = state.tasks.slice();
  const count = state.tasks.filter((t) => t.done).length;
  if (!count) return;
  state.tasks = state.tasks.filter((t) => !t.done);
  render();
  scheduleSave();
  toast(`已清除 ${count} 项已完成任务`, '撤销', () => {
    state.tasks = snapshot;
    render();
    scheduleSave();
  });
}

/* ---------------------------------------------------------------- 编辑标题 */

function startEdit(id) {
  state.editingId = id;
  state.cancelEdit = false;
  render();
}

function commitEdit(input) {
  const id = input.dataset.edit;
  const t = state.tasks.find((x) => x.id === id);
  state.editingId = null;
  if (!t) { render(); return; }

  if (state.cancelEdit) { state.cancelEdit = false; render(); return; }

  const val = input.value.trim();
  if (val && val !== t.title) {
    t.title = val.slice(0, 500);
    scheduleSave();
  }
  render();
}

/* ---------------------------------------------------------------- 主题 */

function applyTheme(name) {
  document.documentElement.setAttribute('data-theme', name);
  try { localStorage.setItem('gogospot-theme', name); } catch (e) {}
}

function initTheme() {
  let saved = null;
  try { saved = localStorage.getItem('gogospot-theme'); } catch (e) {}
  if (saved === 'dark' || saved === 'light') { applyTheme(saved); return; }
  const prefersLight = window.matchMedia && window.matchMedia('(prefers-color-scheme: light)').matches;
  applyTheme(prefersLight ? 'light' : 'dark');
}

/* ---------------------------------------------------------------- Toast */

function toast(msg, actionLabel, onAction) {
  const node = document.createElement('div');
  node.className = 'toast';
  node.innerHTML = `<span>${esc(msg)}</span>`;
  if (actionLabel) {
    const btn = document.createElement('button');
    btn.className = 'toast-action';
    btn.textContent = actionLabel;
    btn.onclick = () => { onAction && onAction(); dismiss(); };
    node.appendChild(btn);
  }
  el.toastWrap.appendChild(node);

  let closed = false;
  function dismiss() {
    if (closed) return;
    closed = true;
    node.classList.add('out');
    setTimeout(() => node.remove(), 220);
  }
  setTimeout(dismiss, actionLabel ? 5200 : 2600);
  return dismiss;
}

/* ---------------------------------------------------------------- 数据面板 */

async function openSheet() {
  el.sheetMask.hidden = false;
  loadWidgetSetting();
  try {
    const res = await fetch('/api/info');
    const info = await res.json();
    el.infoPath.textContent = info.dataFile || '未知';
  } catch (e) {
    el.infoPath.textContent = '读取失败';
  }
  refreshBackups();
}

/* ---------------------------------------------------------------- 悬浮清单开关 */

function paintSwitch(node, on) {
  node.setAttribute('aria-checked', on ? 'true' : 'false');
}

// 服务端一次返回三项，统一刷一遍，省得各自记状态
function paintAllSettings(data) {
  paintSwitch(el.swWidget, !!data.widget);
  paintSwitch(el.swTopmost, !!data.topmost);
  paintSwitch(el.swAutostart, !!data.autostart);
  el.widgetNote.hidden = !!data.widget;
}

async function loadWidgetSetting() {
  try {
    const res = await fetch('/api/settings');
    paintAllSettings(await res.json());
  } catch (e) {
    /* 读不到就保持原样 */
  }
}

async function toggleSetting(key, node, onMsg, offMsg) {
  const next = node.getAttribute('aria-checked') !== 'true';
  node.setAttribute('aria-checked', next ? 'true' : 'false');   // 先给反馈，失败再回滚
  if (key === 'widget') el.widgetNote.hidden = next;
  node.disabled = true;
  try {
    const res = await fetch('/api/settings', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ [key]: next }),
    });
    const data = await res.json();
    if (!data.ok) throw new Error(data.error || '保存失败');
    paintAllSettings(data);
    toast(next ? onMsg : offMsg);
  } catch (e) {
    node.setAttribute('aria-checked', next ? 'false' : 'true');
    if (key === 'widget') el.widgetNote.hidden = !next;
    toast('设置没有保存成功');
  } finally {
    node.disabled = false;
  }
}

// 进行中的环每秒推一点，不用整表重绘
setInterval(() => {
  document.querySelectorAll('[data-bar]').forEach((node) => {
    const t = state.tasks.find((x) => x.id === node.dataset.bar);
    if (!t || !t.startedAt) return;
    const fill = node.querySelector('i');
    if (!fill) return;
    if (!hasEstimate(t)) {           // 没填预估：固定 60% 紫色，不跟时间走
      fill.style.width = '60%';
      node.className = 'task-progress is-unknown';
      return;
    }
    const r = progressRatio(t);
    fill.style.width = (r * 100).toFixed(1) + '%';
    node.classList.toggle('is-green', r <= 0.6);
    node.classList.toggle('is-yellow', r > 0.6 && r <= 0.8);
    node.classList.toggle('is-red', r > 0.8);
  });
}, 1000);

function closeSheet() {
  el.sheetMask.hidden = true;
}

async function refreshBackups() {
  el.backupList.innerHTML = '<div class="backup-empty">加载中…</div>';
  try {
    const res = await fetch('/api/backups');
    const data = await res.json();
    const list = data.backups || [];
    el.backupCount.textContent = list.length ? `${list.length} 份` : '';
    if (!list.length) {
      el.backupList.innerHTML = '<div class="backup-empty">还没有备份，修改任务后会自动生成</div>';
      return;
    }
    el.backupList.innerHTML = list.map((b) => `
      <div class="backup-item">
        <span class="backup-time">${esc(b.time)}</span>
        <span class="backup-size">${fmtSize(b.size)}</span>
        <button class="backup-restore" data-restore="${esc(b.name)}">恢复</button>
        <button class="backup-del" data-delbackup="${esc(b.name)}" title="删除这份备份" aria-label="删除备份">
          <svg viewBox="0 0 24 24" width="15" height="15" fill="none">
            <path d="M5.5 7h13M10 7V5.3c0-.7.5-1.3 1.3-1.3h1.4c.8 0 1.3.6 1.3 1.3V7"
                  stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>
            <path d="M6.8 7l.7 11.1c.06 1 .9 1.9 1.9 1.9h5.2c1 0 1.84-.9 1.9-1.9L17.2 7"
                  stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>
          </svg>
        </button>
      </div>`).join('');
  } catch (e) {
    el.backupList.innerHTML = '<div class="backup-empty">读取备份列表失败</div>';
  }
}

async function deleteBackup(name) {
  if (!confirm(`删除这份备份吗？\n\n${name}\n\n删除后无法找回。`)) return;
  try {
    const res = await fetch('/api/deletebackup?name=' + encodeURIComponent(name), { method: 'POST' });
    const data = await res.json();
    if (!res.ok || !data.ok) throw new Error(data.error || '删除失败');
    await refreshBackups();
    toast('已删除这份备份');
  } catch (e) {
    toast('删除失败：' + e.message);
  }
}

async function restoreBackup(name) {
  if (!confirm(`确定用这份备份覆盖当前任务列表吗？\n\n${name}\n\n当前数据会先自动备份一份。`)) return;
  try {
    const res = await fetch('/api/restore?name=' + encodeURIComponent(name), { method: 'POST' });
    const data = await res.json();
    if (!res.ok || !data.ok) throw new Error(data.error || '恢复失败');
    await loadTasks();
    render();
    closeSheet();
    toast('已从备份恢复');
  } catch (e) {
    toast('恢复失败：' + e.message);
  }
}

async function manualBackup() {
  try {
    const res = await fetch('/api/backup', { method: 'POST' });
    const data = await res.json();
    if (!res.ok || !data.ok) throw new Error(data.error || '备份失败');
    await refreshBackups();
    toast('已创建备份');
  } catch (e) {
    toast('备份失败：' + e.message);
  }
}

function exportJSON() {
  const a = document.createElement('a');
  a.href = '/api/export';
  a.download = '';
  document.body.appendChild(a);
  a.click();
  a.remove();
  toast('已导出为 JSON 文件');
}

function importJSON(file) {
  const reader = new FileReader();
  reader.onload = async () => {
    let parsed;
    try {
      parsed = JSON.parse(String(reader.result));
    } catch (e) {
      toast('文件不是合法的 JSON');
      return;
    }
    const tasks = Array.isArray(parsed) ? parsed : parsed.tasks;
    if (!Array.isArray(tasks)) {
      toast('文件里没有 tasks 数组');
      return;
    }
    if (!confirm(`导入会覆盖当前 ${state.tasks.length} 项任务，确定继续吗？\n（当前数据会先自动备份）`)) return;

    state.tasks = sanitizeIds(tasks.filter(isTaskObject).map(normalize));
    render();
    await saveNow();
    closeSheet();
    toast(`已导入 ${state.tasks.length} 项任务`);
  };
  reader.readAsText(file, 'utf-8');
}

/* ---------------------------------------------------------------- 事件绑定 */

function bind() {
  // 添加
  el.btnAdd.addEventListener('click', addTask);
  el.taskInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') { e.preventDefault(); addTask(); }
    if (e.key === 'Escape') { el.taskInput.value = ''; }
  });

  // 预估时长下拉
  el.estBtn.addEventListener('click', (e) => {
    e.stopPropagation();
    openDuePop(false);                 // 两个面板不叠着开
    openEstMenu(el.estMenu.hidden);
  });

  el.estMenu.addEventListener('click', (e) => {
    const btn = e.target.closest('[data-est]');
    if (!btn) return;
    setEstimate(Number(btn.dataset.est) || 0);
    openEstMenu(false);
  });

  // 截止时间面板
  el.dueBtn.addEventListener('click', (e) => {
    e.stopPropagation();
    openEstMenu(false);
    openDuePop(el.duePop.hidden);
  });

  el.duePop.addEventListener('click', (e) => {
    e.stopPropagation();               // 面板里点哪儿都别触发“点外面就关”

    const day = e.target.closest('.due-day');
    if (day) { pickDueDate(day.dataset.date); return; }

    const quick = e.target.closest('[data-quick]');
    if (quick) {
      const d = new Date();
      if (quick.dataset.quick === 'tomorrow') d.setDate(d.getDate() + 1);
      if (quick.dataset.quick === 'monday') d.setDate(d.getDate() + (((8 - d.getDay()) % 7) || 7));
      pickDueDate(ymd(d));
      return;
    }

    if (e.target.closest('#dueClear')) clearDue();
  });

  el.duePrev.addEventListener('click', () => {
    if (--dueView.m < 0) { dueView.m = 11; --dueView.y; }
    renderDueGrid();
  });

  el.dueNext.addEventListener('click', () => {
    if (++dueView.m > 11) { dueView.m = 0; ++dueView.y; }
    renderDueGrid();
  });

  [el.dueHH, el.dueMM].forEach((inp) => {
    inp.addEventListener('input', () => {
      inp.value = inp.value.replace(/\D/g, '').slice(0, 2);
    });
    inp.addEventListener('blur', readClock);
    inp.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') { e.preventDefault(); readClock(); inp.blur(); return; }
      if (e.key !== 'ArrowUp' && e.key !== 'ArrowDown') return;
      e.preventDefault();
      const span = inp === el.dueHH ? 24 : 60;
      const cur = parseInt(inp.value, 10) || 0;
      inp.value = p2((cur + (e.key === 'ArrowUp' ? 1 : -1) + span) % span);
      readClock();
    });
  });

  document.addEventListener('click', (e) => {
    if (!el.estField.contains(e.target)) openEstMenu(false);
    if (!el.dueField.contains(e.target)) openDuePop(false);
  });

  // 视图切换
  el.viewGroup.addEventListener('click', (e) => {
    const btn = e.target.closest('.seg-btn');
    if (!btn) return;
    state.view = btn.dataset.view;
    el.viewGroup.querySelectorAll('.seg-btn').forEach((b) => b.classList.toggle('is-on', b === btn));
    render();
  });

  // 搜索
  el.searchInput.addEventListener('input', () => {
    state.query = el.searchInput.value;
    el.btnClearSearch.hidden = !state.query;
    render();
  });
  el.btnClearSearch.addEventListener('click', () => {
    el.searchInput.value = '';
    state.query = '';
    el.btnClearSearch.hidden = true;
    el.searchInput.focus();
    render();
  });

  // 标签筛选
  el.tagRow.addEventListener('click', (e) => {
    const chip = e.target.closest('.tag-chip');
    if (!chip) return;
    const g = chip.dataset.tag;
    state.tag = state.tag === g ? null : g;
    render();
  });

  // 列表交互
  el.list.addEventListener('click', (e) => {
    const pin = e.target.closest('[data-pin]');
    if (pin) { togglePin(pin.dataset.pin); return; }
    const startBtn = e.target.closest('[data-start]');
    if (startBtn) { toggleStart(startBtn.dataset.start, startBtn.dataset.on === '1'); return; }
    const edit = e.target.closest('[data-edit]');
    if (edit) { startEdit(edit.dataset.edit); return; }
    const open = e.target.closest('[data-open]');
    if (open) { openTaskLinks(open.dataset.open); return; }
    const check = e.target.closest('[data-check]');
    if (check) { toggleDone(check.dataset.check); return; }
    const del = e.target.closest('[data-del]');
    if (del) { deleteTask(del.dataset.del); return; }
  });



  el.list.addEventListener('keydown', (e) => {
    if (!e.target.classList.contains('task-edit')) return;
    if (e.key === 'Enter') { e.preventDefault(); e.target.blur(); }
    if (e.key === 'Escape') { state.cancelEdit = true; e.target.blur(); }
  });

  el.list.addEventListener('focusout', (e) => {
    if (e.target.classList.contains('task-edit')) commitEdit(e.target);
  });

  // 底栏
  el.btnClearDone.addEventListener('click', clearDone);

  // 主题
  el.btnTheme.addEventListener('click', () => {
    const cur = document.documentElement.getAttribute('data-theme');
    applyTheme(cur === 'dark' ? 'light' : 'dark');
  });

  // 从这里开始：粘贴 / 手填 / 拖文件
  if (el.btnStart) {
    // 平时收成一个按钮，点一下才摊开输入框
    el.btnStart.addEventListener('click', () => {
      const open = el.linkField.classList.toggle('is-open');
      el.btnStart.setAttribute('aria-expanded', open ? 'true' : 'false');
      if (open) el.linkInput.focus();
    });
  }
  if (el.btnPasteLink) {
    el.btnPasteLink.addEventListener('click', async () => {
      try {
        const text = await navigator.clipboard.readText();
        const items = String(text || '').split(/\r?\n/).map((x) => x.trim()).filter(Boolean);
        if (!items.length) { el.linkInput.focus(); return; }
        items.slice(0, 8).forEach(addLink);
      } catch (e) {
        el.linkInput.focus();          // 没给剪贴板权限就退回手填
      }
    });
  }
  {
    const repBox = $('repeatChips');
    if (repBox) {
      repBox.addEventListener('click', (e) => {
        const b = e.target.closest('[data-rep]');
        if (!b) return;
        const key = b.dataset.rep || '';
        // 没有截止时间的重复任务永远不会触发派生，选重复时顺手把截止定在今天
        if (key && !state.dueDate) {
          state.dueDate = todayStr();
          state.dueTime = '23:59';
          syncDue();
          toast('重复任务需要一个截止时间，已设为今天 23:59');
        }
        state.repeat = key;
        paintRepeatChips();
      });
    }
  }
  if (el.linkInput) {
    el.linkInput.addEventListener('blur', () => {
      // 没填东西就收回去，别占着地方
      if (!el.linkInput.value.trim() && el.linkField) {
        el.linkField.classList.remove('is-open');
        el.btnStart.setAttribute('aria-expanded', 'false');
      }
    });
  }
  if (el.linkInput) {
    el.linkInput.addEventListener('keydown', (e) => {
      if (e.key !== 'Enter') return;
      e.preventDefault();
      e.stopPropagation();
      if (addLink(el.linkInput.value)) el.linkInput.value = '';
    });
  }
  if (el.linkChips) {
    el.linkChips.addEventListener('click', (e) => {
      const btn = e.target.closest('[data-unlink]');
      if (!btn) return;
      state.links.splice(Number(btn.dataset.unlink), 1);
      renderLinkChips();
    });
  }
  {
    const composer = el.taskInput.closest('.composer');
    if (composer) {
      composer.addEventListener('dragover', (e) => {
        const types = e.dataTransfer ? Array.from(e.dataTransfer.types) : [];
        if (types.includes('Files')) e.preventDefault();
      });
      composer.addEventListener('drop', (e) => {
        const links = linksFromDrop(e.dataTransfer);
        if (!links.length) return;
        e.preventDefault();
        links.forEach(addLink);
      });
    }
  }

  // 数据面板
  el.btnData.addEventListener('click', openSheet);
  el.swWidget.addEventListener('click', () =>
    toggleSetting('widget', el.swWidget, '悬浮清单已打开', '悬浮清单已关闭'));
  el.swTopmost.addEventListener('click', () =>
    toggleSetting('topmost', el.swTopmost, '悬浮清单已置顶', '悬浮清单回到桌面层'));
  el.swAutostart.addEventListener('click', () =>
    toggleSetting('autostart', el.swAutostart, '已加入开机启动', '已取消开机启动'));
  el.btnCloseSheet.addEventListener('click', closeSheet);
  el.sheetMask.addEventListener('click', (e) => {
    if (e.target === el.sheetMask) closeSheet();
  });
  el.btnManualBackup.addEventListener('click', manualBackup);
  el.btnExport.addEventListener('click', exportJSON);
  el.btnImport.addEventListener('click', () => el.fileInput.click());
  el.fileInput.addEventListener('change', () => {
    const f = el.fileInput.files && el.fileInput.files[0];
    if (f) importJSON(f);
    el.fileInput.value = '';
  });
  el.backupList.addEventListener('click', (e) => {
    const del = e.target.closest('[data-delbackup]');
    if (del) { deleteBackup(del.dataset.delbackup); return; }
    const btn = e.target.closest('[data-restore]');
    if (btn) restoreBackup(btn.dataset.restore);
  });

  // 全局快捷键
  document.addEventListener('keydown', (e) => {
    const tag = (e.target.tagName || '').toLowerCase();
    const typing = tag === 'input' || tag === 'textarea';

    if (e.key === '/' && !typing) {
      e.preventDefault();
      el.searchInput.focus();
    }
    if (e.key === 'Escape') {
      if (!el.duePop.hidden) { openDuePop(false); return; }
      if (!el.estMenu.hidden) { openEstMenu(false); return; }
      if (!el.sheetMask.hidden) { closeSheet(); return; }
      if (state.query) {
        el.searchInput.value = '';
        state.query = '';
        el.btnClearSearch.hidden = true;
        render();
      }
    }
    if (e.key.toLowerCase() === 'n' && !typing && !e.ctrlKey && !e.metaKey && !e.altKey) {
      e.preventDefault();
      el.taskInput.focus();
    }
  });

  // 关页面前把未落盘的改动用 sendBeacon 送走。
  // 送出去了就不拦：拦一次会弹「离开此网站？」，而改动其实已经写进本地服务了。
  window.addEventListener('beforeunload', () => {
    if (!state.saveTimer && !saveGate.again) return;
    clearTimeout(state.saveTimer);
    state.saveTimer = null;
    try {
      const blob = new Blob([JSON.stringify(currentPayload())], { type: 'application/json' });
      navigator.sendBeacon('/api/tasks', blob);
    } catch (err) {
      /* 送不出去也没别的办法，下次打开会重新读盘 */
    }
  });
}

/* ---------------------------------------------------------------- 启动 */

async function init() {
  initTheme();

  const d = new Date();
  const week = ['星期日', '星期一', '星期二', '星期三', '星期四', '星期五', '星期六'][d.getDay()];
  el.todayLabel.textContent = `${d.getFullYear()} 年 ${d.getMonth() + 1} 月 ${d.getDate()} 日 · ${week}`;

  bind();
  setEstimate(0);
  syncDue();
  await loadTasks();
  render();
  el.taskInput.focus();
}

init();
