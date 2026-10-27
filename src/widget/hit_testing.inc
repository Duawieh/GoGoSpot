// ---- 命中测试 ----

static bool inRect(int x, int y, REAL rx, REAL ry, REAL rw, REAL rh) {
    return x >= rx && x <= rx + rw && y >= ry && y <= ry + rh;
}

// 鼠标窗口坐标 -> 展开态坐标
static void toCard(int mx, int my, int& cx, int& cy) {
    cx = (int)(mx / g_scale) - SHADOW;
    cy = (int)(my / g_scale) - SHADOW;
}

static int hitButton(int cx, int cy) {
    if (cy < 0 || cy > TITLE_H) {
        if (g_editing) return -1;          // 输入中，底栏整块都是输入框
        if (cy > CARD_H - FOOT_H && cy < CARD_H) {
            if (cx > 0 && cx < CARD_W / 2) return 2;
            if (cx >= CARD_W / 2 && cx < CARD_W) return 3;
        }
        return -1;
    }
    if (inRect(cx, cy, CARD_W - 34, 13, 22, 22)) return 1;
    if (inRect(cx, cy, CARD_W - 60, 13, 22, 22)) return 4;
    return -1;
}

// 编辑态下鼠标是不是点在输入框里
static bool hitInput(int cx, int cy) {
    if (!g_editing) return false;
    return inRect(cx, cy, 10, panelInputTop(), CARD_W - 20, 30);
}

// 关联材料那一行：1 选文件 2 粘贴 3 清除
// 关联材料那一行：1 选文件 2 点网址输入框 3 清空
static int hitLinkRow(int cx, int cy) {
    if (!g_editing) return 0;
    RectF r = linkPickRect();
    if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return 1;
    r = linkInputRect();
    if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return 2;
    if (!g_editLinks.empty()) {
        r = linkClearRect();
        if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return 3;
    }
    return 0;
}

static void refreshAsync();   // 定义在后面

// 行尾两个按钮的命中区：绘制为 18px 圆，判定取整格，降低瞄准难度。
// cx/cy 是卡片坐标（toCard 已经减掉 SHADOW），这里不能再加 SHADOW，否则整块往右偏一格。
static bool hitProgress(int cx, int cy, int row) {
    if (row < 0) return false;
    REAL right = (REAL)(6 + CARD_W - 12);
    REAL rowTop = (REAL)(LIST_TOP + row * ROW_H);
    return inRect(cx, cy, right - 32, rowTop + 2, 28, ROW_H - 4);
}

// 行尾的置顶按钮，在“正在进行”左边那一格
static bool hitPin(int cx, int cy, int row) {
    if (row < 0) return false;
    REAL right = (REAL)(6 + CARD_W - 12);
    REAL rowTop = (REAL)(LIST_TOP + row * ROW_H);
    return inRect(cx, cy, right - 58, rowTop + 2, 26, ROW_H - 4);
}

// 单击切换“正在进行”
static void toggleStart(const Task& t) {
    if (g_port == 0) findServerPort();
    if (g_port == 0) return;
    if (t.startedAt == 0) {
        // 开始计时时让进度条从 0 起算
        g_progAll = false;
        g_progId = t.id;
        g_progStart = GetTickCount();
        g_progAnim = 0.0f;
        kickProg();
    }
    wstring path = L"/api/start?id=" + urlEncode(wtou8(t.id)) +
                   (t.startedAt > 0 ? L"&on=0" : L"&on=1");
    string body;
    httpRequest(L"POST", path, body);
    refreshAsync();
}

static bool hitCancel(int cx, int cy) {
    if (!g_editing) return false;
    RectF r = cancelRect();
    return inRect(cx, cy, r.X, r.Y, r.Width, r.Height);
}

// 日历上的点击：1 上一月 2 下一月 3 取消 4 确定 5..8 时间加减，100+d 选中某天
static int hitCalendar(int cx, int cy) {
    RectF r;
    r = calPrev();    if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return 1;
    r = calNext();    if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return 2;
    r = calCancel();  if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return 3;
    r = calOk();      if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return 4;
    r = calHourDec(); if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return 5;
    r = calHourInc(); if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return 6;
    r = calMinDec();  if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return 7;
    r = calMinInc();  if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return 8;

    int first = firstWeekday(g_pickYear, g_pickMonth);
    int dim = daysOfMonth(g_pickYear, g_pickMonth);
    for (int i = 0; i < 42; ++i) {
        int d = i - first + 1;
        if (d < 1 || d > dim) continue;
        r = calCell(i);
        if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return 100 + d;
    }
    return 0;
}

// 打开日历：有选过的就从那个日期起，否则从今天 23:59 起
static void openCalendar() {
    g_editDue = 3;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    g_pickYear = st.wYear;
    g_pickMonth = st.wMonth;
    g_pickDay = st.wDay;
    g_pickHour = 23;
    g_pickMin = 59;

    if (!g_customDue.empty()) {
        int y = 0, mo = 0, d = 0, hh = 23, mm = 59;
        if (swscanf(g_customDue.c_str(), L"%d-%d-%dT%d:%d", &y, &mo, &d, &hh, &mm) == 5) {
            g_pickYear = y; g_pickMonth = mo; g_pickDay = d;
            g_pickHour = hh; g_pickMin = mm;
        }
    }
    g_pickingDate = true;
}

static void applyCalendar() {
    int day = g_pickDay;
    int dim = daysOfMonth(g_pickYear, g_pickMonth);
    if (day > dim) day = dim;                 // 换月之后天数可能不够

    wchar_t buf[32];
    swprintf(buf, 32, L"%04d-%02d-%02dT%02d:%02d",
             g_pickYear, g_pickMonth, day, g_pickHour, g_pickMin);
    g_customDue = buf;
    g_editDue = 3;
    g_pickingDate = false;
}

// 预估弹层上的点击：1 取消 2 确定，10+i 减、20+i 加、30+i 点数字直接输入
static int hitEstPicker(int cx, int cy) {
    RectF r;
    r = estCancel(); if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return 1;
    r = estOk();     if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return 2;
    for (int i = 0; i < 3; ++i) {
        r = estMinus(i); if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return 10 + i;
        r = estPlus(i);  if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return 20 + i;
        r = estValue(i); if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return 30 + i;
    }
    return 0;
}

// 打开时把上次确定的值拆成 日 / 时 / 分，没设过就默认 1 小时
static void openEstPicker() {
    int v = g_editEstCustom > 0 ? g_editEstCustom : 60;
    g_estDay = v / 1440;
    g_estHour = (v % 1440) / 60;
    g_estMinute = v % 60;
    g_estEditCol = -1;
    g_estEditText.clear();
    g_pickingEst = true;
}

static void applyEstPicker() {
    int total = g_estDay * 1440 + g_estHour * 60 + g_estMinute;
    if (total > 99999) total = 99999;         // 和服务端的上限对齐
    g_editEstCustom = total;
    g_editEst = total > 0 ? EST_CUSTOM : -1;
    g_pickingEst = false;
}

// 点在哪个选项上：返回 row*100+i（0=截止 1=预估），没点中返回 -1
static int hitChip(int cx, int cy) {
    if (!g_editing) return -1;
    for (int row = 0; row < 2; ++row) {
        int n = (row == 0) ? 5 : EST_CHIPS;
        for (int i = 0; i < n; ++i) {
            RectF r = chipRect(row, i, n);
            if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return row * 100 + i;
        }
    }
    return -1;
}

static int hitRow(int cx, int cy) {
    (void)cx;
    if (cy < LIST_TOP || cy >= LIST_BOTTOM) return -1;
    int i = (cy - LIST_TOP) / ROW_H;
    int idx = g_scroll + i;
    if (idx < 0 || idx >= (int)g_tasks.size()) return -1;
    return i;
}

static bool hitCheckbox(int cx, int cy, int row) {
    REAL rowY = (REAL)(LIST_TOP + row * ROW_H);
    return inRect(cx, cy, 6 + 10, rowY + (ROW_H - 18) / 2.0f, 22, 22);
}

// 收起时：鼠标是不是落在中央圆点（含外圈那层柔光）上。
// 命中范围保持固定，避免外圈扩张改变命中结果，导致窄条与外圈反复缩放。
static bool hitDot(int mx, int my) {
    REAL dx = (REAL)mx / dotUnit() - DOT_SIZE / 2.0f;
    REAL dy = (REAL)my / dotUnit() - DOT_SIZE / 2.0f;
    const REAL R = 16.0f;                 // 圆点柔光的外沿
    return (dx * dx + dy * dy) <= R * R;
}

// 收起态：光标是否落在窄条上（点击后展开）
static bool hitPill(int mx, int my) {
    if (g_pillW <= 0) return false;
    REAL x0 = (REAL)PILL_X * dotUnit();
    REAL x1 = (REAL)PILL_X * dotUnit() + (REAL)g_pillW;
    REAL y0 = ((DOT_SIZE - PILL_H) / 2.0f) * dotUnit();
    REAL y1 = y0 + (REAL)PILL_H * dotUnit();
    return mx >= x0 && mx <= x1 && my >= y0 && my <= y1;
}

