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
  estInput:     $('estInput'),
  estUnitBtn:   $('estUnitBtn'),
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
  btnAddLink:   $('btnAddLink'),
  btnPick:      $('btnPick'),
  pickPop:      $('pickPop'),
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
  composer:     $('composer'),
  composerMeta: $('composerMeta'),
  tagsField:    $('tagsField'),
  editMask:     $('editMask'),
  editTitle:    $('editTitle'),
  editFields:   $('editFields'),
  editChips:    $('editChips'),
  btnEditClose: $('btnEditClose'),
  btnEditCancel:$('btnEditCancel'),
  btnEditSave:  $('btnEditSave'),
};

const state = {
  tasks: [],
  view: 'all',
  estimate: 0,
  dueDate: '',
  dueTime: '23:59',
  query: '',
  tag: null,
  saveTimer: null,
  prevIds: null,
  loaded: false,
  loadError: false,          // 读不出任务文件时，禁止回写，避免原数据被空列表覆盖
  links: [],                 // 正在编辑任务的关联材料
  repeat: '',                // 重复规则：'' / daily / weekly / biweekly / monthly
};

// 同一时刻只允许一份保存在途，避免较晚完成的旧请求覆盖新数据
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
// 剩余时间不超过 min(预估时长, 12 小时) 显示红色，一天以内显示黄色，更远不标记。
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

// 显示只到分钟：拆成 天 / 小时 / 分 三段整数，不出现 1.5 天，也不出现秒。
// 存储值精确到秒，此处四舍五入到分钟；不足一分钟按 1 分显示，避免出现“0 分”
function fmtDuration(min) {
  if (!min) return '';
  let total = Math.round(min);
  if (total < 1) total = 1;
  const d = Math.floor(total / 1440);
  const h = Math.floor((total % 1440) / 60);
  const m = total % 60;
  const out = [];
  if (d) out.push(d + ' 天');
  if (h) out.push(h + ' 小时');
  if (m) out.push(m + ' 分');
  return out.join(' ');
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
      ? Math.min(Math.round(Number(src.estimate) * 60) / 60, EST_MAX)
      : 0,
    due,
    tags: Array.isArray(src.tags) ? src.tags.filter((x) => typeof x === 'string').slice(0, 6) : [],
    links: Array.isArray(src.links)
      ? src.links.map(normalizeLink).filter(Boolean).slice(0, 8)
      : [],
    startedAt: Number.isFinite(Number(src.startedAt)) && Number(src.startedAt) > 0
      ? Number(src.startedAt)
      : 0,
    pinned: !!src.pinned,
    // 无截止时间的重复任务无法生成下一期；导入数据也按同一规则清除重复设置
    repeat: due && REPEATS.some((r) => r.key === src.repeat) ? src.repeat : '',
    createdAt: src.createdAt || new Date().toISOString(),
    completedAt: src.completedAt || null,
  };
}

// 只有对象才算一条任务：历史数据或人工编辑的文件中可能包含数字；
// 将其作为任务渲染会生成空白行，造成任务缺失的误判
const isTaskObject = (t) => !!t && typeof t === 'object' && !Array.isArray(t);

// id 会直接进 data-* 属性。导入的文件可能带引号、尖括号，仅保留安全字符；
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
  if (state.loadError) {          // 读取失败时禁止回写，避免覆盖原数据
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
    // 保留内存里已有的列表，同时锁住保存：避免损坏文件被空列表覆盖
    state.loadError = true;
    setSaveState('error', '读取失败');
    toast('读取任务失败：' + e.message);
  }
  state.loaded = true;
}

/* ---------------------------------------------------------------- 渲染 */

// 和悬浮清单共用一套排序：依次按置顶状态和截止时间排序，无截止时间的任务排在末尾；
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

// 正在进行：预计最晚开始 = 截止 - 预计用时；起始 = min(计时开始时刻, 最晚开始)
// 未填写预估时长的任务无法计算进度比例，界面以固定 60% 紫色填充表示运行状态
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

// 进行中：常显“停止”按钮；未进行：悬停时显示“正在进行”按钮
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

// 紧急程度对应的进度条 class，和悬浮清单的红 / 黄 / 紫三档对齐
function urgencyCls(t) {
  const u = urgencyOf(t);
  return u === 2 ? 'is-red' : (u === 1 ? 'is-yellow' : 'is-purple');
}

// 行顶那条直进度条。颜色按“用时占比”分档：<=60% 绿，<=80% 黄，再往上红。
// 未填写预估时长时固定填充 80%，颜色按紧急程度确定。
function progressBar(t) {
  if (!t.startedAt) return '';
  const id = esc(t.id);
  if (!hasEstimate(t)) {
    return `<div class="task-progress ${urgencyCls(t)}" data-bar="${id}">
      <i style="width:80%"></i>
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

  const title = `<div class="task-title" data-title="${id}">${esc(t.title)}</div>`;

  // 挂了材料的任务，标题旁边给一个“关联材料”的小图标
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
             stroke-width="1.8" stroke-linejoin="round" stroke-linecap="round">
          <path d="M5 4.5h14"/>
          <path d="M12 20V9.5"/>
          <path d="M7.8 13.2 12 9.5l4.2 3.7"/>
        </svg>
      </button>
      ${startButton(t)}
      <button class="task-rename" data-edit="${id}" aria-label="编辑任务" title="编辑任务">
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
    void n.offsetHeight;              // 先完成起始布局计算，避免与后续过渡合并为同一帧
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
  // 5000 条时该项耗时接近一秒；超过该量级不播放重排动画。
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

}

function emptyMessage() {
  if (state.loadError) {
    return { title: '任务文件读取失败', sub: '改动不会写回磁盘。可在设置中从历史备份恢复' };
  }
  if (state.query.trim()) return { title: '无匹配任务', sub: '调整搜索关键词或清空搜索框' };
  if (state.tag) return { title: `标签 ${state.tag} 下暂无任务`, sub: '选择标签以取消筛选' };
  if (state.view === 'done') return { title: '暂无已完成任务', sub: '已完成任务将在此显示' };
  if (state.view === 'today') return { title: '今日无到期任务', sub: '设置任务截止时间后将在此显示' };
  if (state.view === 'pending') return { title: '暂无待办任务', sub: '所有任务均已完成' };
  return { title: '暂无任务', sub: '输入任务标题，按 Enter 添加' };
}

/* ---------------------------------------------------------------- 操作 */

// 预估时长：一个数字加一个单位（日 / 时 / 分），滚轮或点击换单位。
// 允许小数（1.5 日等于 36 时）；存储按秒四舍五入，显示统一拆分为整数。
const EST_MAX = 99999;               // 分钟，约 69 天
const EST_UNITS = [
  { label: '日', min: 1440 },
  { label: '时', min: 60 },
  { label: '分', min: 1 },
];
let estUnit = 2;                     // 当前单位下标，默认“分”
let estRaw = '';                     // 编辑中的原始数字
// 数字是用户自己打的，还是从已有任务回填的。
// 自己打的：换单位只换乘数，2 分 → 2 时（用户的意思就是“2 时”）；
// 回填的：换单位做换算，90 分 → 1.5 时。
let estTyped = false;

// 单位换算保留四位小数，避免往返转换导致精度损失：90 分 = 0.0625 日
const trimNum = (n) => Math.round(n * 10000) / 10000;

function paintEst() {
  const typing = document.activeElement === el.estInput;
  el.estInput.value = typing
    ? estRaw
    : (state.estimate ? fmtDuration(state.estimate) : '');
  el.estUnitBtn.hidden = !typing;    // 单位只在编辑时露出来
  el.estUnitBtn.textContent = EST_UNITS[estUnit].label;
}

function setEstimate(minutes) {
  const n = Number(minutes) || 0;
  // 按秒存储，以保留 0.5 分钟（30 秒）等精度
  state.estimate = Math.min(Math.max(Math.round(n * 60) / 60, 0), EST_MAX);
  paintEst();
}

// 可整除时选用较大单位：2160 分 → 1.5 日，45 分 → 45 分
function beginEstEdit() {
  const v = state.estimate;
  estUnit = v && v % 1440 === 0 ? 0 : (v && v % 60 === 0 ? 1 : 2);
  estRaw = v ? String(trimNum(v / EST_UNITS[estUnit].min)) : '';
  estTyped = false;                  // 回填的数字，换单位要换算
  paintEst();
  el.estInput.select();
}

// 换单位：回填来的数字跟着换算（1.5 日 -> 36 时），
// 用户自己打的数字原样留着，只换乘数（打 2 再点“时”就是 2 时）
function shiftEstUnit(step) {
  const next = (estUnit + step + EST_UNITS.length) % EST_UNITS.length;
  const n = parseFloat(estRaw);
  if (!estTyped && Number.isFinite(n)) {
    const mins = n * EST_UNITS[estUnit].min;
    estRaw = String(trimNum(mins / EST_UNITS[next].min));
  }
  estUnit = next;
  paintEst();
}

// 按 Enter 提交后会触发失焦事件并产生第二次提交；将原始值更新为提交结果，
// 确保重复计算一致并保留输入值
function commitEstimate() {
  const n = parseFloat(estRaw);
  setEstimate(Number.isFinite(n) ? n * EST_UNITS[estUnit].min : 0);
  estRaw = state.estimate
    ? String(trimNum(state.estimate / EST_UNITS[estUnit].min))
    : '';
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

// 与任务列表使用相同日期显示格式：今天 / 明天 / 后天 / N 天后 / 月日
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
  // 无截止时间的重复任务不会派生下一期，清除截止时间时同时清除重复设置
  if (state.repeat) {
    state.repeat = '';
    paintRepeatChips();
  }
  syncDue();
  openDuePop(false);
}

/* ---- 关联材料：一条任务可以挂多个链接或本地文件 ---- */

// 链接统一成 {url, protocol, call}：网址交给浏览器，本地路径在文件夹中显示。
// 老数据只有一串字符串，这里按前缀补出 protocol / call
function normalizeLink(x) {
  const given = (x && typeof x === 'object' && !Array.isArray(x)) ? x : {};
  const url = String(typeof x === 'string' ? x : (given.url || '')).trim();
  if (!url) return null;
  const web = /^https?:\/\//i.test(url);
  const protocol = typeof given.protocol === 'string' && given.protocol
    ? given.protocol
    : (web ? (/^https:/i.test(url) ? 'https' : 'http') : 'file');
  const call = typeof given.call === 'string' && given.call
    ? given.call
    : (protocol === 'file' ? 'explorer' : 'browser');
  return { url, protocol, call };
}

// 将 file:///C:/a/b.docx 转为 C:\a\b.docx，其余原样返回
function fromFileUri(u) {
  const s = String(u || '');
  if (!/^file:/i.test(s)) return s;
  let p = s.replace(/^file:[/]{2,3}/i, '');
  try { p = decodeURIComponent(p); } catch (e) { /* 保持原样 */ }
  return p.replace(/\//g, '\\');
}

// 只显示最后一段，避免长路径超出 chip 宽度
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
    const on = (b.dataset.rep || '') === (state.repeat || '');
    b.classList.toggle('is-on', on);
    b.setAttribute('aria-pressed', on ? 'true' : 'false');
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
  box.innerHTML = state.links.map((l, i) =>
    `<span class="link-chip" title="${esc(l.url)}">` +
      `<span class="link-chip-name">${esc(shortLink(l.url))}</span>` +
      `<button type="button" data-unlink="${i}" aria-label="移除">×</button>` +
    `</span>`).join('');
}

// 仅接受网址与绝对路径；其余视为无效输入，避免被当作 file 链接写入
function looksLikeLink(u) {
  return /^(https?|ftp|file):\/\//i.test(u)
      || /^[a-zA-Z]:[\\/]/.test(u)
      || /^\\\\/.test(u);
}

function addLink(raw) {
  const text = String(raw || '').trim();
  if (!text) return false;                       // 空字符串：什么都不做
  if (!looksLikeLink(text)) {
    toast('无法识别的网址或文件路径');
    return false;
  }
  const l = normalizeLink(text);
  if (!l) return false;
  if (state.links.some((x) => x.url === l.url)) return false;
  if (state.links.length >= 8) { toast('最多添加 8 项'); return false; }
  state.links.push(l);
  renderLinkChips();
  return true;
}

// 拖放数据可能为 file:/// URI 或裸路径，统一转换为链接
function linksFromDrop(dt) {
  const out = [];
  try {
    const uri = dt.getData('text/uri-list');
    if (uri) {
      uri.split(/\r?\n/).forEach((line) => {
        const t = line.trim();
        if (t && !t.startsWith('#')) out.push(fromFileUri(t));
      });
    }
  } catch (e) { /* 有的浏览器不给读 */ }
  if (!out.length) {
    try {
      const plain = dt.getData('text/plain');
      if (plain && /^(https?:|file:|[a-zA-Z]:\\|\\\\)/.test(plain.trim())) {
        out.push(fromFileUri(plain.trim()));
      }
    } catch (e) { /* 同上 */ }
  }
  return out;
}

// 依次交由系统打开，间隔少量时间，避免同时弹出过多窗口
// 切换“正在进行”：按钮用于开始计时，进度环用于停止计时
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
    toast(on ? '开始计时' : '已停止计时');
  } catch (e) {
    toast('保存失败');
  }
}

async function openTaskLinks(id) {
  const t = state.tasks.find((x) => x.id === id);
  if (!t || !t.links || !t.links.length) return;
  let failed = 0;
  for (const l of t.links) {
    try {
      const res = await fetch('/api/open?url=' + encodeURIComponent(l.url)
        + '&call=' + encodeURIComponent(l.call || ''), {
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
  if (failed) toast(failed + ' 项无法打开');
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

// 完成任务通过服务端 /api/toggle 修改，避免本地重写完整任务文件。
// 重复任务的下一期仅由服务端派生；本地修改会导致后续任务链中断。
async function toggleDone(id) {
  const t = state.tasks.find((x) => x.id === id);
  if (!t) return;

  if (state.saveTimer) await saveNow();     // 先将待保存改动写入磁盘，避免后续重读覆盖
  if (state.loadError) return;

  try {
    const res = await fetch('/api/toggle?id=' + encodeURIComponent(id), { method: 'POST' });
    const data = await res.json();
    if (!res.ok || !data.ok) throw new Error(data.error || ('HTTP ' + res.status));
  } catch (e) {
    toast('保存失败：' + e.message);
    return;
  }

  await loadTasks();                        // 派生出来的下一期只有服务端知道
  render();
}

// 置顶仅修改一个字段，并随任务列表一并保存，与标题和标签使用相同保存流程
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

/* ---------------------------------------------------------------- 编辑任务 */

// 编辑窗口不另造控件：打开时把输入区的预估、截止、材料三块整个挪进弹窗，
// 关闭时放回原位，并把控制台自己的草稿换回来。
const editor = { id: null, draft: null };

function openTaskEditor(id) {
  const t = state.tasks.find((x) => x.id === id);
  if (!t || !el.editMask) return;

  editor.id = id;
  editor.draft = {
    dueDate: state.dueDate,
    dueTime: state.dueTime,
    links: state.links.slice(),
    estimate: state.estimate,
    estRaw,
    estUnit,
  };

  // 任务的值灌进共用控件
  const due = typeof t.due === 'string' ? t.due : '';
  state.dueDate = due ? due.slice(0, 10) : '';
  state.dueTime = due.length >= 16 ? due.slice(11, 16) : '23:59';
  state.links = (t.links || []).map(normalizeLink).filter(Boolean);
  state.estimate = Number(t.estimate) > 0 ? Number(t.estimate) : 0;
  estUnit = state.estimate && state.estimate % 1440 === 0 ? 0
          : (state.estimate && state.estimate % 60 === 0 ? 1 : 2);
  estRaw = state.estimate ? String(trimNum(state.estimate / EST_UNITS[estUnit].min)) : '';
  estTyped = false;                  // 编辑面板里是回填值，换单位要换算

  paintEst();
  syncDue();
  renderLinkChips();

  el.editFields.appendChild(el.estField);
  el.editFields.appendChild(el.dueField);
  el.editFields.appendChild(el.linkField);
  el.editChips.appendChild(el.linkChips);

  el.editTitle.value = t.title;
  el.editMask.hidden = false;
  el.editTitle.focus();
  el.editTitle.select();
}

function closeTaskEditor() {
  if (!el.editMask || el.editMask.hidden) return;
  el.editMask.hidden = true;
  editor.id = null;

  // 控件放回输入区，顺序与原布局一致
  el.composerMeta.insertBefore(el.estField, el.tagsField);
  el.composerMeta.insertBefore(el.dueField, el.tagsField);
  el.composerMeta.insertBefore(el.linkField, el.tagsField);
  el.composer.appendChild(el.linkChips);

  const d = editor.draft;
  if (d) {
    state.dueDate = d.dueDate;
    state.dueTime = d.dueTime;
    state.links = d.links;
    state.estimate = d.estimate;
    estRaw = d.estRaw;
    estUnit = d.estUnit;
  }
  editor.draft = null;

  paintEst();
  syncDue();
  renderLinkChips();
}

function saveTaskEditor() {
  const t = state.tasks.find((x) => x.id === editor.id);
  if (!t) { closeTaskEditor(); return; }

  commitEstimate();                       // 输入框里的数字先落成 state.estimate
  const title = el.editTitle.value.trim();
  if (!title) {
    toast('标题不能为空');
    el.editTitle.focus();
    return;
  }

  t.title = title.slice(0, 500);
  t.due = state.dueDate ? `${state.dueDate}T${state.dueTime || '23:59'}` : null;
  t.estimate = state.estimate || 0;
  t.links = state.links.slice();

  closeTaskEditor();
  render();
  scheduleSave();
  toast('已保存');
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

// 服务端一次返回三项设置，统一刷新，避免各自维护状态
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
    /* 设置读取失败时保留现有状态 */
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
    toast('设置保存失败');
  } finally {
    node.disabled = false;
  }
}

// 每秒更新进行中任务的进度环，无需重绘任务列表
setInterval(() => {
  document.querySelectorAll('[data-bar]').forEach((node) => {
    const t = state.tasks.find((x) => x.id === node.dataset.bar);
    if (!t || !t.startedAt) return;
    const fill = node.querySelector('i');
    if (!fill) return;
    if (!hasEstimate(t)) {           // 没填预估：固定八成，颜色跟紧急程度走
      fill.style.width = '80%';
      node.className = 'task-progress ' + urgencyCls(t);
      return;
    }
    const r = progressRatio(t);
    fill.style.width = (r * 100).toFixed(1) + '%';
    node.className = 'task-progress ' +
      (r <= 0.6 ? 'is-green' : (r <= 0.8 ? 'is-yellow' : 'is-red'));
  });
}, 1000);

// 设置面板打开时与后台保持同步：托盘菜单可切换悬浮清单，无需刷新页面
setInterval(() => {
  if (el.sheetMask.hidden) return;
  if ([el.swWidget, el.swTopmost, el.swAutostart].some((n) => n && n.disabled)) return;
  loadWidgetSetting();
}, 2000);

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
      el.backupList.innerHTML = '<div class="backup-empty">暂无备份；任务变更后将自动创建</div>';
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
      toast('文件中没有 tasks 数组');
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
  // 编辑任务弹窗：点小窗外面或按 Esc 都等于取消
  if (el.editMask) {
    el.editMask.addEventListener('click', (e) => {
      if (e.target === el.editMask) closeTaskEditor();
    });
    el.btnEditClose.addEventListener('click', closeTaskEditor);
    el.btnEditCancel.addEventListener('click', closeTaskEditor);
    el.btnEditSave.addEventListener('click', saveTaskEditor);
    el.editTitle.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') { e.preventDefault(); saveTaskEditor(); }
    });
    document.addEventListener('keydown', (e) => {
      if (e.key === 'Escape' && !el.editMask.hidden) {
        e.stopPropagation();
        closeTaskEditor();
      }
    }, true);
  }

  // 添加
  el.btnAdd.addEventListener('click', addTask);
  el.taskInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') { e.preventDefault(); addTask(); }
    if (e.key === 'Escape') { el.taskInput.value = ''; }
  });

  // 预估时长：聚焦后输入数字，滚轮或点击换单位
  el.estInput.addEventListener('focus', beginEstEdit);

  el.estInput.addEventListener('input', () => {
    // 只留数字和一个小数点
    let v = el.estInput.value.replace(/[^\d.]/g, '');
    const dot = v.indexOf('.');
    if (dot >= 0) v = v.slice(0, dot + 1) + v.slice(dot + 1).replace(/\./g, '');
    estRaw = v.slice(0, 7);
    estTyped = true;                 // 从现在起这个数字是用户的意思
    el.estInput.value = estRaw;
  });

  el.estInput.addEventListener('blur', commitEstimate);

  el.estInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
      e.preventDefault();
      commitEstimate();
      el.estInput.blur();
    }
    if (e.key === 'Escape') {
      e.stopPropagation();
      beginEstEdit();                  // 还原成当前值，不提交改了一半的数字
      el.estInput.blur();
    }
  });

  // 单位：滚轮、点击、上下键都能换。阻止 mousedown 默认失焦行为，以保持输入框焦点
  el.estUnitBtn.addEventListener('mousedown', (e) => e.preventDefault());
  el.estUnitBtn.addEventListener('click', () => shiftEstUnit(1));
  el.estUnitBtn.addEventListener('wheel', (e) => {
    e.preventDefault();
    shiftEstUnit(e.deltaY > 0 ? 1 : -1);
  }, { passive: false });
  el.estUnitBtn.addEventListener('keydown', (e) => {
    if (e.key === 'ArrowUp') { e.preventDefault(); shiftEstUnit(-1); }
    if (e.key === 'ArrowDown') { e.preventDefault(); shiftEstUnit(1); }
  });

  // 截止时间面板
  el.dueBtn.addEventListener('click', (e) => {
    e.stopPropagation();
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
    if (!el.dueField.contains(e.target)) openDuePop(false);
    // 选择来源的菜单：单击其他区域时收起
    if (el.pickPop && !el.pickPop.hidden
        && !el.pickPop.contains(e.target) && !el.btnPick.contains(e.target)) {
      el.pickPop.hidden = true;
    }
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
    if (edit) { openTaskEditor(edit.dataset.edit); return; }
    const open = e.target.closest('[data-open]');
    if (open) { openTaskLinks(open.dataset.open); return; }
    const check = e.target.closest('[data-check]');
    if (check) { toggleDone(check.dataset.check); return; }
    const del = e.target.closest('[data-del]');
    if (del) { deleteTask(del.dataset.del); return; }
  });




  // 底栏
  el.btnClearDone.addEventListener('click', clearDone);

  // 主题
  el.btnTheme.addEventListener('click', () => {
    const cur = document.documentElement.getAttribute('data-theme');
    applyTheme(cur === 'dark' ? 'light' : 'dark');
  });

  // 关联材料：粘贴 / 手填 / 拖文件
  if (el.btnStart) {
    // 默认收起为按钮，点击后展开输入框
    el.btnStart.addEventListener('click', () => {
      const open = el.linkField.classList.toggle('is-open');
      el.btnStart.setAttribute('aria-expanded', open ? 'true' : 'false');
      if (open) el.linkInput.focus();
    });
  }
  // “+”按钮与 Enter 执行相同操作：提交当前输入并清空输入框。
  if (el.btnAddLink) {
    // 未拦截 mousedown 时，点击 + 会使输入框失焦，内容为空则自动收起
    el.btnAddLink.addEventListener('mousedown', (e) => e.preventDefault());
    el.btnAddLink.addEventListener('click', () => {
      if (addLink(el.linkInput.value)) el.linkInput.value = '';
      el.linkInput.focus();
    });
  }

  // 浏览器拿不到本地绝对路径，只能让本机进程弹原生选择框
  async function pickFromDisk(mode) {
    try {
      const res = await fetch('/api/pick?mode=' + mode, { method: 'POST' });
      const data = await res.json();
      if (!data.ok) { toast('无法打开选择框'); return; }
      const paths = Array.isArray(data.paths) ? data.paths : [];
      if (!paths.length) return;                 // 取消了，什么都不做
      let added = 0;
      paths.forEach((p) => { if (addLink(p)) added++; });
      if (added) toast('已添加 ' + added + ' 项');
    } catch (e) {
      toast('无法打开选择框');
    }
  }

  // 选择按钮：一个按钮 + 两项菜单。Windows 的选择框不能文件和文件夹混选
  if (el.btnPick) {
    el.btnPick.addEventListener('mousedown', (e) => e.preventDefault());   // 别把输入框顶失焦
    el.btnPick.addEventListener('click', (e) => {
      e.stopPropagation();
      if (el.pickPop) el.pickPop.hidden = !el.pickPop.hidden;
    });
  }
  if (el.pickPop) {
    // 未拦截 mousedown 时，单击菜单项会使输入框失焦，菜单随之收起，点击落不到项上
    el.pickPop.addEventListener('mousedown', (e) => e.preventDefault());
    el.pickPop.addEventListener('click', (e) => {
      const item = e.target.closest('.pick-item');
      if (!item) return;
      el.pickPop.hidden = true;
      pickFromDisk(item.dataset.mode);
    });
  }
  {
    const repBox = $('repeatChips');
    if (repBox) {
      repBox.addEventListener('click', (e) => {
        const b = e.target.closest('[data-rep]');
        if (!b) return;
        const key = b.dataset.rep || '';
        // 无截止时间的重复任务不会触发派生，选择重复时同时将截止时间设为今天
        if (key && !state.dueDate) {
          state.dueDate = todayStr();
          state.dueTime = '23:59';
          syncDue();
          toast('重复任务需设置截止时间，已设为今天 23:59');
        }
        state.repeat = key;
        paintRepeatChips();
      });
    }
  }
  if (el.linkInput) {
    el.linkInput.addEventListener('blur', () => {
      // 内容为空时收起，避免占用空间
      if (!el.linkInput.value.trim() && el.linkField) {
        el.linkField.classList.remove('is-open');
        el.btnStart.setAttribute('aria-expanded', 'false');
        if (el.pickPop) el.pickPop.hidden = true;
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

  // 页面关闭前通过 sendBeacon 提交尚未持久化的更改。
  // 已持久化的更改无需拦截页面关闭，避免浏览器显示离开确认提示。
  window.addEventListener('beforeunload', () => {
    if (!state.saveTimer && !saveGate.again) return;
    clearTimeout(state.saveTimer);
    state.saveTimer = null;
    try {
      const blob = new Blob([JSON.stringify(currentPayload())], { type: 'application/json' });
      navigator.sendBeacon('/api/tasks', blob);
    } catch (err) {
      /* sendBeacon 失败后无法在页面关闭时重试；下次加载将从本地数据文件重新读取 */
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
  paintRepeatChips();      // HTML 里写死的 is-on 也要补上 aria-pressed
  await loadTasks();
  render();
  el.taskInput.focus();
}

init();
