// ---- 绘制辅助 ----

static void roundRect(GraphicsPath& path, const RectF& r, REAL rad) {
    REAL d = rad * 2;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

static Font* titleFont() {
    static Font f(L"Microsoft YaHei UI", 13.5f, FontStyleBold, UnitPixel);
    return &f;
}
static Font* bodyFont() {
    static Font f(L"Microsoft YaHei UI", 13.0f, FontStyleRegular, UnitPixel);
    return &f;
}
static Font* smallFont() {
    static Font f(L"Microsoft YaHei UI", 11.0f, FontStyleRegular, UnitPixel);
    return &f;
}

// align: 0=左 1=居中 2=右
// outline 不透明时给文字描一圈边：用于确保进度色覆盖整行时辅助文字仍具备足够对比度
static void drawText(Graphics& g, const wstring& text, Font* font, const RectF& r,
                     const Color& color, bool centerV, int align,
                     Color outline = Color(0, 0, 0, 0)) {
    StringFormat fmt;
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    fmt.SetLineAlignment(centerV ? StringAlignmentCenter : StringAlignmentNear);
    fmt.SetAlignment(align == 1 ? StringAlignmentCenter
                   : align == 2 ? StringAlignmentFar : StringAlignmentNear);

    if (outline.GetA() == 0) {
        SolidBrush br(color);
        g.DrawString(text.c_str(), -1, font, r, &fmt, &br);
        return;
    }

    // 描边通过路径绘制：先将文字转换为图形，再描边并填充字形。
    // 重复调用 DrawString 无法生成独立的外描边轮廓。
    FontFamily fam;
    font->GetFamily(&fam);
    GraphicsPath path;
    if (path.AddString(text.c_str(), -1, &fam, font->GetStyle(),
                       font->GetSize(), r, &fmt) != Ok) {
        SolidBrush br(color);
        g.DrawString(text.c_str(), -1, font, r, &fmt, &br);
        return;
    }
    Pen pen(outline, 1.0f);
    pen.SetLineJoin(LineJoinRound);
    g.DrawPath(&pen, &path);
    SolidBrush br(color);
    g.FillPath(&br, &path);
}

static long long nowMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    const unsigned long long EPOCH = 116444736000000000ULL;
    return (long long)((u.QuadPart - EPOCH) / 10000ULL);
}

// 紧急程度分为两档，枚举值随紧急程度递增，可直接比较并取最大值
enum Urgency {
    URG_NONE   = 0,
    URG_YELLOW = 1,     // 剩余时间进入负载黄线
    URG_RED    = 2      // <= min(预估, 12h)
};

// 红线跟着预估走，但最多提前 12 小时。
// ---- 负载模型 ----
//
// 单看一条任务算不出「今天做不做得完」：100 条各 1 小时、明天一起到期，
// 每条单独看都不急，合起来要 100 小时。
//
// 所以先按截止时刻把预估聚成一档（一次遍历建 map），再按时间升序求前缀和
// （第二遍走 map）。前缀和就是「到这一档为止总共要花多少时间」，
// 黄线由它乘系数得到，红线和原来一样按单条任务算。
//
// 两个单位约定：
//   · 预估 ≥ 24h 的任务，认为用户已经把睡觉吃饭算进去了，累计时按一半计
//   · 前缀 < 12h 说明一口气能做完，系数 1.0；否则按 2.0（日历时间大约是工作量的两倍）
//   两者相乘，长任务的 0.5 会被系数 2.0 还原，正好对上用户填的原始时长。
//   黄线最低 12h：短任务也要提前半天亮，不能拖到最后一小时。

struct DueBucket {
    long long due;        // 截止时刻
    long long prefix;     // 到这一档为止的累计（长任务已减半）
};

static std::vector<DueBucket> g_buckets;

static const long long EST_LONG_MS = 24LL * 3600000LL;   // 到这个时长视为已含休息
static const long long DAY_CAP_MS  = 12LL * 3600000LL;   // 一口气能做完的上限

// 任务数据变了就重算一次，绘制时只查表
static void rebuildWorkload() {
    g_buckets.clear();

    std::map<long long, long long> byDue;   // 截止时刻 → 预估和（长任务已减半）
    for (size_t i = 0; i < g_tasks.size(); ++i) {
        const Task& t = g_tasks[i];
        if (t.dueMs <= 0) continue;                          // 没截止时间排不进去

        long long est = (long long)t.estimate * 60000LL;
        if (est <= 0) est = 3600000LL;                       // 没填预估按 1 小时
        if (est >= EST_LONG_MS) est /= 2;
        byDue[t.dueMs] += est;
    }

    long long cum = 0;
    g_buckets.reserve(byDue.size());
    for (std::map<long long, long long>::const_iterator it = byDue.begin();
         it != byDue.end(); ++it) {
        cum += it->second;
        DueBucket b;
        b.due = it->first;
        b.prefix = cum;
        g_buckets.push_back(b);
    }
}

// 这条任务的黄线：它所在那一档的前缀 × 系数，最低 12 小时
static long long yellowLineOf(const Task& t) {
    size_t lo = 0, hi = g_buckets.size();
    while (lo < hi) {                                        // 按截止时刻二分
        size_t mid = lo + (hi - lo) / 2;
        if (g_buckets[mid].due < t.dueMs) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= g_buckets.size() || g_buckets[lo].due != t.dueMs) return 0;

    const DueBucket& b = g_buckets[lo];
    // 前缀够满 12 小时说明一天做不完，按日历时间算；否则一口气能做完
    long long line = b.prefix * (b.prefix >= DAY_CAP_MS ? 2 : 1);
    return (line < DAY_CAP_MS) ? DAY_CAP_MS : line;
}

static Urgency urgencyOf(const Task& t) {
    if (t.dueMs <= 0) return URG_NONE;

    long long remain = t.dueMs - nowMs();
    if (remain <= 0) return URG_RED;          // 已经逾期，直接最高级

    // 红色只跟这条任务自己有关：预估多久、还剩多久。没填预估按 1 小时
    long long est = (long long)t.estimate * 60000LL;
    if (est <= 0) est = 3600000LL;
    long long redLine = (est < URG_RED_CAP_MS) ? est : URG_RED_CAP_MS;
    if (remain <= redLine) return URG_RED;

    // 黄色看整条时间线的负载：前面的任务也会挤占这一档的时间
    if (remain <= yellowLineOf(t)) return URG_YELLOW;
    return URG_NONE;
}

// 列表顺序：置顶最前，其次按紧急程度（红 → 黄 → 普通），再按到期时间，没截止的垫底。
// 用的是和圆点外圈、窄条同一个 urgencyOf，最急的一条一定排在展开列表的第一行。
// 必须在 rebuildWorkload 之后调用：黄线依赖整份任务的累计负载。
static void sortByUrgency() {
    std::stable_sort(g_tasks.begin(), g_tasks.end(), [](const Task& a, const Task& b) {
        if (a.pinned != b.pinned) return a.pinned;

        Urgency ua = urgencyOf(a);
        Urgency ub = urgencyOf(b);
        if (ua != ub) return ua > ub;

        if (a.dueMs == 0 && b.dueMs == 0) return false;
        if (a.dueMs == 0) return false;
        if (b.dueMs == 0) return true;
        return a.dueMs < b.dueMs;
    });
}

// 紧急程度对应的颜色：进度染色、窄条、正在进行按钮都用它，红黄紫三档一一对上
static Color urgencyColor(Urgency u) {
    return (u == URG_RED) ? pal().red : (u == URG_YELLOW ? pal().amber : pal().purple);
}

// 全局最急的那一档，决定圆点外面套什么颜色的圈
static Urgency worstUrgency() {
    Urgency worst = URG_NONE;
    for (size_t i = 0; i < g_tasks.size(); ++i) {
        const Task& t = g_tasks[i];
        if (!g_justDone.empty() && t.id == g_justDone) continue;
        Urgency u = urgencyOf(t);
        if (u > worst) worst = u;
    }
    return worst;
}

// 进行中的任务：起始时间和用时占比，pillTask 和 paintRow 都要用
static void progressOf(const Task& t, long long nowMs, long long& startMs, REAL& ratio);

// 窄条和圆点环上显示哪条：
//   1. 红色任务中优先“已经在做”的一条（多条时比较进度）；均未进行时按截止时间最近
//   2. 没有红色时显示黄色任务，同样优先在做的
//   3. 都不紧急 → 显示进度占比最高的进行中任务
//   4. 一条都没在进行 → 退回最早结束的那条
//
// 第 2 步不能省：圆点外圈画的是全局最急的那一档（worstUrgency），
// 窄条若按“进度最高”挑，就会出现圈是黄的、窄条里却是一条紫任务。
static const Task* pillTask() {
    const Task* redRunning = nullptr;    // 红色的、且在进行的
    const Task* redAny = nullptr;        // 红色的（按 ddl）
    const Task* amberRunning = nullptr;  // 黄色的、且在进行的
    const Task* amberAny = nullptr;      // 黄色的（按 ddl）
    const Task* mostDone = nullptr;      // 都不紧急时，进度最多
    const Task* nearest = nullptr;       // 最早结束
    REAL bestRedRatio = -1.0f;
    REAL bestAmberRatio = -1.0f;
    REAL bestRatio = -1.0f;

    for (size_t i = 0; i < g_tasks.size(); ++i) {
        const Task& t = g_tasks[i];
        if (!g_justDone.empty() && t.id == g_justDone) continue;   // 刚完成的任务暂不显示

        const Urgency u = urgencyOf(t);
        const bool isRed = (u == URG_RED);
        const bool isAmber = (u == URG_YELLOW);
        if (isRed && (!redAny || t.dueMs < redAny->dueMs)) redAny = &t;
        if (isAmber && (!amberAny || t.dueMs < amberAny->dueMs)) amberAny = &t;

        if (t.startedAt > 0) {
            long long st = 0;
            REAL r = 0;
            progressOf(t, nowMs(), st, r);

            if (isRed && (r > bestRedRatio ||
                          (r == bestRedRatio && redRunning && t.dueMs < redRunning->dueMs))) {
                bestRedRatio = r;
                redRunning = &t;
            }
            if (isAmber && (r > bestAmberRatio ||
                            (r == bestAmberRatio && amberRunning && t.dueMs < amberRunning->dueMs))) {
                bestAmberRatio = r;
                amberRunning = &t;
            }
            if (r > bestRatio || (r == bestRatio && mostDone && t.dueMs < mostDone->dueMs)) {
                bestRatio = r;
                mostDone = &t;
            }
        }

        if (t.dueMs > 0) {                                        // 没截止时间的不算“快结束”
            if (!nearest || t.dueMs < nearest->dueMs) nearest = &t;
        }
    }

    if (redRunning) return redRunning;
    if (redAny) return redAny;
    if (amberRunning) return amberRunning;
    if (amberAny) return amberAny;
    if (mostDone) return mostDone;
    return nearest;
}

// 把毫秒转成本地时间
static void msToLocal(long long ms, SYSTEMTIME& st) {
    ULARGE_INTEGER u;
    u.QuadPart = (unsigned long long)ms * 10000ULL + 116444736000000000ULL;
    FILETIME ft{};
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    FILETIME local{};
    FileTimeToLocalFileTime(&ft, &local);
    FileTimeToSystemTime(&local, &st);
}

// 任务行右侧那一小段时间文字
static wstring dueText(const Task& t) {
    if (t.dueMs <= 0) return L"";
    long long remain = t.dueMs - nowMs();

    if (remain <= 0) {
        long long overMin = (-remain) / 60000LL;
        if (overMin < 60) return L"逾期";
        long long overH = overMin / 60;
        if (overH < 24) return L"逾期 " + std::to_wstring(overH) + L"h";
        return L"逾期 " + std::to_wstring(overH / 24) + L"d";
    }

    long long min = remain / 60000LL;
    if (min < 60) return std::to_wstring(min) + L"min";
    long long h = min / 60;
    if (h < 24) return std::to_wstring(h) + L"h";

    SYSTEMTIME st{};
    msToLocal(t.dueMs, st);
    long long d = h / 24;
    if (d < 7) return std::to_wstring(d) + L"d";
    return std::to_wstring(st.wMonth) + L"/" + std::to_wstring(st.wDay);
}

// 量一段文字在 96 DPI 逻辑坐标下的宽度，用来决定横条该多宽
static REAL textWidth(const wstring& s, Font* font) {
    if (s.empty()) return 0.0f;
    HDC dc = GetDC(nullptr);
    if (!dc) return 0.0f;
    Graphics g(dc);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    RectF box;
    g.MeasureString(s.c_str(), -1, font, PointF(0, 0), &box);
    ReleaseDC(nullptr, dc);
    return box.Width;
}

static wstring shortDuration(int minutes) {
    if (minutes <= 0) return L"";
    wchar_t buf[32];
    if (minutes < 60) {
        swprintf(buf, 32, L"%dm", minutes);
    } else {
        double h = minutes / 60.0;
        if (h == (int)h) swprintf(buf, 32, L"%dh", (int)h);
        else swprintf(buf, 32, L"%.1fh", h);
    }
    return buf;
}

// 计时器文本 HH:MM:SS。负数照写，前面带个减号——倒计时过了 100% 继续往下走
static wstring clockText(long long ms) {
    bool neg = ms < 0;
    if (neg) ms = -ms;
    long long total = ms / 1000;
    wchar_t buf[32];
    swprintf(buf, 32, neg ? L"-%02d:%02d:%02d" : L"%02d:%02d:%02d",
             (int)(total / 3600), (int)(total / 60 % 60), (int)(total % 60));
    return buf;
}

