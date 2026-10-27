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
    URG_YELLOW = 1,     // (min(预估, 12h), 一天]
    URG_RED    = 2      // <= min(预估, 12h)
};

// 红线跟着预估走，但最多提前 12 小时。
static Urgency urgencyOf(const Task& t) {
    if (t.dueMs <= 0) return URG_NONE;

    long long remain = t.dueMs - nowMs();
    if (remain <= 0) return URG_RED;          // 已经逾期，直接最高级

    long long est = (long long)t.estimate * 60000LL;
    long long redLine = (est < URG_RED_CAP_MS) ? est : URG_RED_CAP_MS;
    if (remain <= redLine) return URG_RED;
    if (remain <= URG_YELLOW_MS) return URG_YELLOW;
    return URG_NONE;
}

// 紧急程度对应的颜色：进度染色、窄条、正在进行按钮都用它，红黄紫三档一一对上
static Color urgencyColor(Urgency u) {
    return (u == URG_RED) ? C_RED : (u == URG_YELLOW ? C_AMBER : C_PURPLE);
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
//   2. 不存在红色任务时，显示进度占比最高的任务
//   3. 一条都没在进行 → 退回最早结束的那条
static const Task* pillTask() {
    const Task* redRunning = nullptr;    // 红色的、且在进行的
    const Task* redAny = nullptr;        // 红色的（按 ddl）
    const Task* mostDone = nullptr;      // 不分颜色，进度最多
    const Task* nearest = nullptr;       // 最早结束
    REAL bestRedRatio = -1.0f;
    REAL bestRatio = -1.0f;

    for (size_t i = 0; i < g_tasks.size(); ++i) {
        const Task& t = g_tasks[i];
        if (!g_justDone.empty() && t.id == g_justDone) continue;   // 刚完成的任务暂不显示

        bool isRed = (urgencyOf(t) == URG_RED);
        if (isRed && (!redAny || t.dueMs < redAny->dueMs)) redAny = &t;

        if (t.startedAt > 0) {
            long long st = 0;
            REAL r = 0;
            progressOf(t, nowMs(), st, r);

            if (isRed && (r > bestRedRatio ||
                          (r == bestRedRatio && redRunning && t.dueMs < redRunning->dueMs))) {
                bestRedRatio = r;
                redRunning = &t;
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

