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
    URG_RED    = 2      // <= min(预估 + 30min, 12h)
};

// ---- 负载模型 ----
//
// 单看一条任务算不出「今天做不做得完」：100 条各 1 小时、明天一起到期，
// 每条单独看都不急，合起来要 100 小时。
//
// 按截止时刻聚成若干档，三遍走：
//   一、建 map：同一截止时刻的有效预估求和，同时记下这一档最长的红色线
//   二、按时间升序求前缀和，得到每档的黄线
//   三、按时间倒序把更晚那一档的等级往回带：后面的活都急了，前面更早到期的只会更急
//
// 两个单位约定：
//   · 预估 ≥ 24h 的任务，认为用户已经把睡觉吃饭算进去了，累计时按一半计
//   · 前缀 < 12h 说明一口气能做完，系数 1.0；否则按 2.0（日历时间大约是工作量的两倍）
//   两者相乘，长任务的 0.5 会被系数 2.0 还原，正好对上用户填的原始时长。
//   黄线最低 16h：前一天 16:00 就能看到次日 08:00 到期的任务。

struct DueBucket {
    long long due;        // 截止时刻
    long long prefix;     // 到这一档为止的累计（长任务已减半）
    long long redLine;    // 这一档里最长的红色线：max(min(预估, 12h))
    int       tier;       // 第三次遍历定下的等级：0 普通 / 1 黄 / 2 红
};

static std::vector<DueBucket> g_buckets;

static const long long EST_LONG_MS = 24LL * 3600000LL;   // 到这个时长视为已含休息
static const long long DAY_CAP_MS  = 12LL * 3600000LL;   // 一口气能做完的上限
// 黄线的最低提前量。取 16 小时：前一天 16:00 下班前就能看到次日 08:00 到期的任务
static const long long URG_YELLOW_FLOOR_MS = 16LL * 3600000LL;
// 红线在预估之外多给半小时：卡着点才开始，一点缓冲都没有
static const long long URG_RED_GRACE_MS = 30LL * 60000LL;

// 任务数据变了就重算一次，绘制时只查表
static void rebuildWorkload() {
    g_buckets.clear();

    std::map<long long, long long> byDue;     // 截止时刻 → 有效预估和（长任务已减半）
    std::map<long long, long long> redByDue;  // 截止时刻 → 最长的红色线
    for (size_t i = 0; i < g_tasks.size(); ++i) {
        const Task& t = g_tasks[i];
        if (t.dueMs <= 0) continue;                          // 没截止时间排不进去

        long long est = (long long)t.estimate * 60000LL;
        if (est <= 0) est = 3600000LL;                       // 没填预估按 1 小时
        long long red = est + URG_RED_GRACE_MS;
        if (red > URG_RED_CAP_MS) red = URG_RED_CAP_MS;
        if (red > redByDue[t.dueMs]) redByDue[t.dueMs] = red;
        if (est >= EST_LONG_MS) est /= 2;
        byDue[t.dueMs] += est;
    }

    // 第二遍：升序求前缀和，黄线由前缀乘系数得到
    long long cum = 0;
    g_buckets.reserve(byDue.size());
    for (std::map<long long, long long>::const_iterator it = byDue.begin();
         it != byDue.end(); ++it) {
        cum += it->second;
        DueBucket b;
        b.due = it->first;
        b.prefix = cum;
        b.redLine = redByDue[it->first];
        b.tier = 0;
        g_buckets.push_back(b);
    }

    // 第三遍：倒序把更晚那一档的等级往回带。
    // 不这么传会出现「两天的活已经标红，明天到期的五分钟任务还是普通」这种反直觉结果：
    // 更早到期的任务不可能比更晚到期的更闲。
    long long now = nowMs();
    int carry = 0;
    for (int i = (int)g_buckets.size() - 1; i >= 0; --i) {
        DueBucket& b = g_buckets[i];
        long long line = b.prefix * (b.prefix >= DAY_CAP_MS ? 2 : 1);
        if (line < URG_YELLOW_FLOOR_MS) line = URG_YELLOW_FLOOR_MS;

        long long remain = b.due - now;
        int tier = 0;
        if (remain <= 0 || remain <= b.redLine) tier = 2;
        else if (remain <= line) tier = 1;

        if (carry > tier) tier = carry;      // 更晚的那档更急，这一档跟着急
        b.tier = tier;
        carry = tier;
    }
}

// 这条任务的等级：第三次遍历已经按截止时刻定好，这里只查表
static Urgency urgencyOf(const Task& t) {
    if (t.dueMs <= 0) return URG_NONE;

    size_t lo = 0, hi = g_buckets.size();
    while (lo < hi) {                                        // 按截止时刻二分
        size_t mid = lo + (hi - lo) / 2;
        if (g_buckets[mid].due < t.dueMs) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= g_buckets.size() || g_buckets[lo].due != t.dueMs) return URG_NONE;
    return (Urgency)g_buckets[lo].tier;
}

// 列表顺序：进行中的任务最前，进度高的在前（不限时的按 0 计），
// 其余按置顶 → 紧急程度（红 → 黄 → 普通）→ 到期时间，没截止的垫底。
// 进行中压过置顶是有意的：那是用户此刻正在做的事，比任何静态优先级都该先看到。
// 必须在 rebuildWorkload 之后调用：黄线依赖整份任务的累计负载。
static REAL runningProgress(const Task& t);   // 定义在下面「进行中的任务」那节

static void sortByUrgency() {
    std::stable_sort(g_tasks.begin(), g_tasks.end(), [](const Task& a, const Task& b) {
        REAL pa = runningProgress(a);
        REAL pb = runningProgress(b);
        bool ra = (pa >= 0.0f);
        bool rb = (pb >= 0.0f);
        if (ra != rb) return ra;                  // 进行中的一律排前面
        if (ra && pa != pb) return pa > pb;       // 都在进行中：进度高的在上

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

// 进行中的任务返回 0..1；不限时的没有比例可算，按 0；
// 没在进行返回 -1，调用方靠它区分「进度 0」和「根本不在进行中」。
static REAL runningProgress(const Task& t) {
    if (t.startedAt <= 0) return -1.0f;
    if (!hasEstimate(t)) return 0.0f;
    long long st = 0;
    REAL r = 0;
    progressOf(t, nowMs(), st, r);
    return r;
}

// 窄条和圆点环上显示哪条：
//   1. 进行中的任务，进度最高的一条（不限时按 0，排在其他进行中之后）
//   2. 没有进行中的 → 红色里截止最近的一条
//   3. 再没有 → 黄色里截止最近的一条
//   4. 都不紧急 → 最早结束的那条
//
// 进行中排第一：圆点外圈和窄条上的进度环只有它画得出来。
// 2、3 步不能省：圆点外圈画的是全局最急的那一档（worstUrgency），
// 窄条若只按进度挑，就会出现圈是黄的、窄条里却是一条紫任务。
static const Task* pillTask() {
    const Task* running = nullptr;       // 进行中，进度最高
    const Task* redAny = nullptr;        // 红色的（按 ddl）
    const Task* amberAny = nullptr;      // 黄色的（按 ddl）
    const Task* nearest = nullptr;       // 最早结束
    REAL bestRatio = -1.0f;

    for (size_t i = 0; i < g_tasks.size(); ++i) {
        const Task& t = g_tasks[i];
        if (!g_justDone.empty() && t.id == g_justDone) continue;   // 刚完成的任务暂不显示

        const Urgency u = urgencyOf(t);
        if (u == URG_RED && (!redAny || t.dueMs < redAny->dueMs)) redAny = &t;
        if (u == URG_YELLOW && (!amberAny || t.dueMs < amberAny->dueMs)) amberAny = &t;

        if (t.startedAt > 0) {
            REAL r = runningProgress(t);
            if (r > bestRatio) {
                bestRatio = r;
                running = &t;
            }
        }

        if (t.dueMs > 0) {                                        // 没截止时间的不算“快结束”
            if (!nearest || t.dueMs < nearest->dueMs) nearest = &t;
        }
    }

    if (running) return running;
    if (redAny) return redAny;
    if (amberAny) return amberAny;
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

