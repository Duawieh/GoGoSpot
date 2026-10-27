// ---- 右键菜单窗口 ----

// 菜单使用独立分层窗口，其 Z 序与悬浮清单分离，可在置顶状态及收起状态下正常显示。
// 菜单显示期间暂停悬浮清单的置顶调整，由菜单窗口的 HWND_TOPMOST 维持层级。
static HWND g_menuWnd = nullptr;
static bool g_menuDone = false;
static int  g_menuResult = 0;
static int  g_menuHover = -1;
static int  g_menuX = 0, g_menuY = 0;      // 屏幕坐标
static int  g_menuW = 0, g_menuH = 0;      // 设备像素
static const MenuItemDef* g_menuItems = nullptr;
static int  g_menuCount = 0;

static REAL g_menuLogicalW = 118;          // 按最长的一项量出来的宽度
static const REAL MN_ROW = 28;
static const REAL MN_GAP = 9;
static const REAL MN_PAD = 6;

static REAL menuHeight(const MenuItemDef* items, int n) {
    REAL h = MN_PAD * 2;
    for (int i = 0; i < n; ++i) h += MN_ROW + (items[i].gap ? MN_GAP : 0);
    return h;
}

// 按最长的一项算菜单宽度，避免长标签被截断
static REAL menuWidthFor(const MenuItemDef* items, int n) {
    REAL maxW = 0;
    HDC screen = GetDC(nullptr);
    {
        Graphics g(screen);
        Font font(L"Microsoft YaHei UI", 11.0f, FontStyleRegular, UnitPixel);
        for (int i = 0; i < n; ++i) {
            RectF m;
            g.MeasureString(items[i].label, -1, &font, PointF(0, 0), &m);
            if (m.Width > maxW) maxW = m.Width;
        }
    }
    ReleaseDC(nullptr, screen);

    REAL w = maxW + 36;          // 左侧 10 + 右侧 16，再留一点余量防测量误差
    if (w < 96) w = 96;
    if (w > 240) w = 240;
    return w;
}

// 设备像素的鼠标位置落在第几项，-1 表示不在任何一项上
static int menuHit(int px, int py) {
    REAL x = px / g_scale;
    REAL y = py / g_scale;
    REAL top = MN_PAD;
    for (int i = 0; i < g_menuCount; ++i) {
        if (x >= 6 && x <= g_menuLogicalW - 6 && y >= top && y <= top + MN_ROW) return i;
        top += MN_ROW + (g_menuItems[i].gap ? MN_GAP : 0);
    }
    return -1;
}

// 返回 false 表示没能把位图贴上去（多半是窗口少了 WS_EX_LAYERED）
static bool menuPaint(HWND h) {
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_menuW;
    bi.bmiHeader.biHeight = -g_menuH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);

    {
        Graphics g(mem);
        g.ScaleTransform(g_scale, g_scale);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.Clear(Color(0, 0, 0, 0));

        RectF box(0.5f, 0.5f, g_menuLogicalW - 1.0f, menuHeight(g_menuItems, g_menuCount) - 1.0f);
        GraphicsPath p;
        roundRect(p, box, 11.0f);
        SolidBrush bg(Color(250, 25, 29, 38));
        g.FillPath(&bg, &p);
        Pen pen(Color(46, 255, 255, 255), 1.0f);
        g.DrawPath(&pen, &p);

        // 菜单可能从托盘线程弹出，字体在本地建，不共用悬浮清单的那几个
        Font font(L"Microsoft YaHei UI", 11.0f, FontStyleRegular, UnitPixel);

        REAL y = MN_PAD;
        for (int i = 0; i < g_menuCount; ++i) {
            RectF r(6, y, g_menuLogicalW - 12, MN_ROW);
            if (i == g_menuHover) {
                GraphicsPath hp;
                roundRect(hp, r, 7.0f);
                SolidBrush hb(Color(40, C_ACCENT.GetR(), C_ACCENT.GetG(), C_ACCENT.GetB()));
                g.FillPath(&hb, &hp);
            }
            drawText(g, g_menuItems[i].label, &font,
                     RectF(r.X + 10, r.Y, r.Width - 16, r.Height),
                     i == g_menuHover ? C_TEXT : C_TEXT_DIM, true, 0);

            y += MN_ROW;
            if (g_menuItems[i].gap) {
                Pen sp(Color(26, 255, 255, 255), 1.0f);
                g.DrawLine(&sp, 12.0f, y + 4, g_menuLogicalW - 12.0f, y + 4);
                y += MN_GAP;
            }
        }
    }

    premultiplyAlpha((BYTE*)bits, g_menuW, g_menuH);

    POINT dst{ g_menuX, g_menuY };
    SIZE size{ g_menuW, g_menuH };
    POINT src{ 0, 0 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    BOOL ok = UpdateLayeredWindow(h, screen, &dst, &size, mem, &src, 0, &bf, ULW_ALPHA);

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok != FALSE;
}

static LRESULT CALLBACK MenuProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_MOUSEMOVE: {
            int hv = menuHit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            if (hv != g_menuHover) {
                g_menuHover = hv;
                menuPaint(h);
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
            // 捕获到窗口外单击时关闭菜单并拦截该消息
            if (menuHit(GET_X_LPARAM(l), GET_Y_LPARAM(l)) < 0) g_menuDone = true;
            return 0;
        case WM_LBUTTONUP: {
            int hv = menuHit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            if (hv >= 0) g_menuResult = g_menuItems[hv].cmd;
            g_menuDone = true;
            return 0;
        }
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            g_menuDone = true;
            return 0;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) g_menuDone = true;
            return 0;
        case WM_CAPTURECHANGED:
            if (!g_menuDone) g_menuDone = true;
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;
        default:
            return DefWindowProcW(h, m, w, l);
    }
}

int widgetPopupMenu(int screenX, int screenY, const MenuItemDef* items, int count) {
    if (!items || count <= 0 || count > 8) return 0;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = MenuProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"GoGoSpotMenuWnd";
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW
        if (!RegisterClassExW(&wc)) return 0;
        registered = true;
    }

    g_menuItems = items;
    g_menuCount = count;
    g_menuHover = -1;
    g_menuResult = 0;
    g_menuDone = false;

    g_menuLogicalW = menuWidthFor(items, count);
    g_menuW = (int)(g_menuLogicalW * g_scale + 0.5f);
    g_menuH = (int)(menuHeight(items, count) * g_scale + 0.5f);

    // 贴边时翻到光标另一侧，别让菜单出屏幕
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int x = screenX, y = screenY;
    if (x + g_menuW > work.right)  x = screenX - g_menuW;
    if (y + g_menuH > work.bottom) y = screenY - g_menuH;
    if (x < work.left) x = work.left;
    if (y < work.top)  y = work.top;
    g_menuX = x;
    g_menuY = y;

    if (!g_menuWnd || !IsWindow(g_menuWnd)) {
        // 必须是分层窗口，否则 UpdateLayeredWindow 会失败，窗口存在但什么都不显示
        g_menuWnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
                                    L"GoGoSpotMenuWnd", L"", WS_POPUP,
                                    x, y, g_menuW, g_menuH,
                                    nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!g_menuWnd) return 0;
    }

    g_menuOpen = true;
    SetWindowPos(g_menuWnd, HWND_TOPMOST, x, y, g_menuW, g_menuH,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (!menuPaint(g_menuWnd)) {
        // 画不出来就别弹：否则窗口挡住鼠标却看不见内容
        ShowWindow(g_menuWnd, SW_HIDE);
        g_menuOpen = false;
        return 0;
    }
    // 不抢前台（窗口是 WS_EX_NOACTIVATE）：托盘那条路径抢不到，抢到了也容易被抢回去。
    // 捕获能拿到就拿来吃掉那一次点击，拿不到由下面的轮询兜底。
    SetCapture(g_menuWnd);

    // 模态循环：等消息，顺便每 50ms 看一眼鼠标有没有按在菜单外面。
    // 托盘线程不是前台线程，捕获拿不到，单靠消息收不到那次点击。
    MSG msg;
    while (!g_menuDone) {
        DWORD wr = MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
        if (wr == WAIT_OBJECT_0) {
            while (!g_menuDone && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    PostQuitMessage((int)msg.wParam);
                    g_menuDone = true;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (g_menuDone) break;

        bool pressed = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ||
                       (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
        if (pressed) {
            POINT cur{};
            GetCursorPos(&cur);
            RECT box{};
            GetWindowRect(g_menuWnd, &box);
            if (!PtInRect(&box, cur)) g_menuDone = true;
        }
    }

    if (GetCapture() == g_menuWnd) ReleaseCapture();
    ShowWindow(g_menuWnd, SW_HIDE);
    g_menuOpen = false;
    g_menuItems = nullptr;
    g_menuCount = 0;
    return g_menuResult;
}

// 窗口挂接桌面时，SetWindowPos 使用父窗口坐标，调用前需转换坐标系
static void placeWindow(int screenX, int screenY, int w, int h) {
    POINT pt{screenX, screenY};
    if (g_desktopParent) ScreenToClient(g_desktopParent, &pt);
    SetWindowPos(g_hwnd, nullptr, pt.x, pt.y, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void present();

// Win+D 会将桌面提升至前景并遮挡悬浮清单；检测窗口中心所属窗口，
// 若命中桌面图标层则提升悬浮清单的 Z 序；被普通窗口遮挡时不调整。
// 前台是不是全屏程序（全屏视频、游戏、演示）。用系统的通知状态判断，
// 该状态比窗口矩形检测更准确：最大化窗口覆盖工作区但不属于全屏。
static bool fullscreenAppInFront() {
    // 这些符号需 NTDDI_VISTA 才在头文件中暴露，改为运行时获取函数地址。
    // 返回值：1 不在 / 2 忙（全屏、演示）/ 3 D3D 全屏 / 4 演示模式 / 5 正常 / 6 静默 / 7 应用
    typedef HRESULT (WINAPI *QueryFn)(int*);
    static QueryFn fn = reinterpret_cast<QueryFn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"shell32.dll"), "SHQueryUserNotificationState")));
    if (fn) {
        int st = 5;
        if (SUCCEEDED(fn(&st))) return st == 2 || st == 3 || st == 4;
    }

    // 旧版系统不支持该 API，改用窗口矩形检测；窗口需覆盖工作区和任务栏才视为全屏
    HWND fg = GetForegroundWindow();
    if (!fg || fg == g_hwnd) return false;

    RECT r{};
    if (!GetWindowRect(fg, &r)) return false;

    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;

    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
           r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}

// 切置顶。owned 窗口设 HWND_TOPMOST 不生效，所以置顶时先把 owner 摘掉；
// 关掉时再把 owner 挂回桌面，Win+D 才会跟着浮上来。
static void applyTopmost() {
    if (!g_hwnd) return;

    if (g_topmost) {
        SetWindowLongPtrW(g_hwnd, GWLP_HWNDPARENT, 0);
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        // 同时清除扩展样式并设置 NOTOPMOST，确保托盘临时置顶状态一并解除
        LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
        if (ex & WS_EX_TOPMOST) SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex & ~WS_EX_TOPMOST);
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        HWND progman = FindWindowW(L"Progman", nullptr);
        if (progman) SetWindowLongPtrW(g_hwnd, GWLP_HWNDPARENT, (LONG_PTR)progman);
    }

    if (g_hiddenForFullscreen) {
        g_hiddenForFullscreen = false;
        if (g_widgetOn) ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    }
    present();
}

// 标题栏那个置顶按钮：改的就是“悬浮清单置顶”这个设置，和控制台同一个值。
// 先更新本地状态以提供即时反馈，再发送请求并持久化设置；
// 后台存完会回一条 WM_APP_TOPMOST，值一样，等于确认一次。
static void toggleTopmost() {
    bool on = !g_topmost;
    g_topmost = on;
    applyTopmost();
    InvalidateRect(g_hwnd, nullptr, FALSE);

    if (g_port == 0) findServerPort();
    if (g_port == 0) return;
    string body;
    httpRequest(L"POST", wstring(L"/api/topmost?on=") + (on ? L"1" : L"0"), body);
}

static void keepAboveDesktop() {
    // 右键菜单展开时不动 Z 序：菜单是独立置顶窗口，重新抢置顶会把它压下去
    if (g_menuOpen) return;

    // 置顶模式：保持在普通窗口之上；前台为全屏程序时暂时隐藏，避免干扰
    if (g_topmost) {
        bool fs = fullscreenAppInFront();
        if (fs && !g_hiddenForFullscreen) {
            g_hiddenForFullscreen = true;
            ShowWindow(g_hwnd, SW_HIDE);
        } else if (!fs && g_hiddenForFullscreen) {
            g_hiddenForFullscreen = false;
            if (g_widgetOn) ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        }
        if (!fs) {
            SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return;
    }

    RECT wr{};
    GetWindowRect(g_hwnd, &wr);
    POINT pt{ (wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2 };

    HWND under = WindowFromPoint(pt);
    if (!under || under == g_hwnd) return;

    wchar_t cls[64] = {0};
    GetClassNameW(under, cls, 64);
    if (wcscmp(cls, L"SysListView32") == 0 ||
        wcscmp(cls, L"SHELLDLL_DefView") == 0 ||
        wcscmp(cls, L"Progman") == 0 ||
        wcscmp(cls, L"WorkerW") == 0) {
        SetWindowPos(g_hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        present();
    }
}

static void present() {
    if (!g_memDC || !g_screenDC) return;

    paint(g_memDC);
    if (g_bits) premultiplyAlpha(g_bits, g_surfW, g_surfH);

    RECT wr{};
    GetWindowRect(g_hwnd, &wr);

    int curW = g_expanded ? g_winW : dotPixels() + g_pillW;
    int curH = g_expanded ? g_winH : dotPixels();
    if (curW > g_surfW) curW = g_surfW;      // 位图就那么大，多出来的画不出来
    if (curH > g_surfH) curH = g_surfH;

    POINT src{0, 0};
    SIZE size{curW, curH};
    POINT dst{wr.left, wr.top};

    float f = g_expanded ? g_fade : 1.0f;
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    BLENDFUNCTION bf{AC_SRC_OVER, 0, (BYTE)(255 * f), AC_SRC_ALPHA};

    UpdateLayeredWindow(g_hwnd, g_screenDC, &dst, &size, g_memDC, &src, 0, &bf, ULW_ALPHA);
}

// 窄条何时露：红色常驻，黄色只在鼠标停在圆点上时拉出；滑动交给 TIMER_SLIDE。
static void updatePillTarget() {
    if (!g_hwnd) return;

    bool show = (g_pillFull > 0) && (worstUrgency() == URG_RED || g_pillWant);
    int  target     = show ? g_pillFull : 0;
    REAL ringTarget = g_pillWant ? 1.0f : 0.0f;

    bool pillMoved = (target != g_pillTarget);
    bool ringMoved = (ringTarget != g_ringTarget);
    if (!pillMoved && !ringMoved) return;

    g_pillTarget = target;
    g_ringTarget = ringTarget;
    kickProg();   // 圆点那圈环跟着淡入淡出

    if (g_expanded) {
        // 展开时看不见圆点和窄条，直接落位。
        g_pillW = target;
        g_ringT = ringTarget;
    } else {
        SetTimer(g_hwnd, TIMER_SLIDE, 16, nullptr);
    }
}

// 读 Windows 的“应用模式”：AppsUseLightTheme = 1 是浅色，走日间配色。
// 每三秒跟着刷新一次，用户在设置里切主题不用重启悬浮清单。
static void refreshDayMode() {
    HKEY k = nullptr;
    DWORD light = 1, sz = sizeof(light), type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        RegQueryValueExW(k, L"AppsUseLightTheme", nullptr, &type, (LPBYTE)&light, &sz);
        RegCloseKey(k);
    }
    g_dayMode = (light != 0);
}

// 算窄条完全展开的宽度，右边放不下就缩，缩到读不下标题就不显示。
static void syncPillWidth() {
    refreshDayMode();

    RECT wr{};
    GetWindowRect(g_hwnd, &wr);
    const int dotPx = dotPixels();

    int full = 0;
    const Task* t = pillTask();
    if (t) {
        // 22 左内边距 + 标题 + 15 右内边距 + 8 间隔 + 10 余量（DrawString 自带内边距）
        wstring due = dueText(*t);
        REAL colW = due.empty() ? 0.0f : textWidth(due, smallFont());
        if (t->startedAt > 0) {          // 进行中右边那列还要塞下倒计时
            long long st = 0;
            REAL rr = 0;
            progressOf(*t, nowMs(), st, rr);
            wstring tail = hasEstimate(*t)
                         ? clockText((long long)t->estimate * 60000LL - (nowMs() - st))
                         : wstring(L"正在进行");
            REAL w2 = textWidth(tail, smallFont());
            if (w2 > colW) colW = w2;
        }
        REAL logical = 22.0f + textWidth(t->title, titleFont()) + 15.0f + 10.0f;
        if (colW > 0) logical += colW + 8.0f;
        if (logical < (REAL)PILL_MIN) logical = (REAL)PILL_MIN;
        if (logical > (REAL)PILL_W)   logical = (REAL)PILL_W;

        full = (int)(logical * dotUnit() + 0.5f);

        HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (mon && GetMonitorInfoW(mon, &mi)) {
            int room = mi.rcWork.right - (wr.left + dotPx) - (int)(10 * g_scale);
            if (room < full) full = room;
        }
        if (full < (int)(PILL_MIN * dotUnit())) full = 0;
    }

    g_pillFull = full;
    updatePillTarget();
}

static void captureBlurred(int x, int y) {
    if (!g_bgDC || !g_rawDC || !g_smallDC) return;

    HDC screen = GetDC(nullptr);
    if (!screen) return;
    // 抓的是窗口左上角这一块。收起态窗口比卡片小，多抓的部分正好是展开后会盖住的地方
    BitBlt(g_rawDC, 0, 0, g_surfW, g_surfH, screen, x, y, SRCCOPY);
    ReleaseDC(nullptr, screen);

    int sw = g_surfW / 6; if (sw < 4) sw = 4;
    int sh = g_surfH / 6; if (sh < 4) sh = 4;

    SetStretchBltMode(g_smallDC, HALFTONE);
    SetBrushOrgEx(g_smallDC, 0, 0, nullptr);
    StretchBlt(g_smallDC, 0, 0, sw, sh, g_rawDC, 0, 0, g_surfW, g_surfH, SRCCOPY);

    SetStretchBltMode(g_bgDC, HALFTONE);
    SetBrushOrgEx(g_bgDC, 0, 0, nullptr);
    StretchBlt(g_bgDC, 0, 0, g_surfW, g_surfH, g_smallDC, 0, 0, sw, sh, SRCCOPY);

    // BitBlt 不写 alpha，同时压暗，保证浅色壁纸上的文字对比度。
    if (g_bgBits) {
        int n = g_surfW * g_surfH;
        for (int i = 0; i < n; ++i) {
            BYTE* p = g_bgBits + (size_t)i * 4;
            p[0] = (BYTE)(p[0] * 45 / 100);
            p[1] = (BYTE)(p[1] * 45 / 100);
            p[2] = (BYTE)(p[2] * 45 / 100);
            p[3] = 255;
        }
    }
    g_bgReady = true;
}

// 隐藏窗口后等待合成器提交新帧，再执行屏幕捕获；
// 等待合成器更新画面后再调用 BitBlt，避免模糊背景缓存包含上一帧任务列表。
static void flushComposition() {
    typedef HRESULT (WINAPI *PFN_Flush)(void);
    static PFN_Flush fn = reinterpret_cast<PFN_Flush>(reinterpret_cast<void*>(
        GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmFlush")));
    if (fn) fn();
}

// 抓取模糊背景前隐藏窗口，避免画面包含窗口自身及上一帧任务列表。
// 隐藏和抓取在同一消息中完成，以避免产生可见闪烁。
static void recaptureBackground() {
    if (!g_hwnd) return;
    RECT wr{};
    GetWindowRect(g_hwnd, &wr);
    ShowWindow(g_hwnd, SW_HIDE);
    flushComposition();
    captureBlurred(wr.left, wr.top);
    if (g_widgetOn) ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
}

// 窗口一藏一显，系统的鼠标离开追踪会失效；重新注册一次，
// 否则窗口无法接收 WM_MOUSELEAVE，鼠标移出后将无法触发收起。
// 光标本来就在窗口外面时不要注册：TME_LEAVE 会立刻回一条 WM_MOUSELEAVE，
// 托盘临时会话将在半秒内自动收起。
static void trackLeave() {
    POINT pt{};
    GetCursorPos(&pt);
    RECT wr{};
    GetWindowRect(g_hwnd, &wr);
    if (!PtInRect(&wr, pt)) return;

    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, g_hwnd, 0};
    TrackMouseEvent(&tme);
}

// 卡片按圆点位置摊开，但必须落在显示器工作区里：
// 圆点贴在右下角时直接摊开会有一大半在屏幕外，底栏的“添加任务 / 打开控制台”就点不到了。
// 放不下先翻到圆点的另一侧，还放不下再夹进工作区。
static void resolveCardPos(int& x, int& y, int w, int h) {
    POINT anchor{x + dotPixels() / 2, y + dotPixels() / 2};
    HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return;

    const RECT& work = mi.rcWork;
    if (x + w > work.right) {
        int flipped = x + dotPixels() - w;
        x = (flipped >= work.left) ? flipped : (work.right - w);
    }
    if (y + h > work.bottom) {
        int flipped = y + dotPixels() - h;
        y = (flipped >= work.top) ? flipped : (work.bottom - h);
    }
    if (x < work.left) x = work.left;
    if (y < work.top)  y = work.top;
}

// bringCursor：从圆点点开时为真。空间不够时卡片会翻到圆点另一侧，或者被夹进工作区，
// 此时将指针移至卡片区域，避免失焦逻辑立即触发收起。
static void expandWidget(bool bringCursor = false) {
    if (g_expanded) return;

    // 窄条可能还在滑动，先让它落位。
    KillTimer(g_hwnd, TIMER_SLIDE);
    g_pillW = g_pillTarget;
    g_ringT = g_ringTarget;

    RECT wr{};
    GetWindowRect(g_hwnd, &wr);
    g_dotX = wr.left;
    g_dotY = wr.top;

    int cardX = wr.left, cardY = wr.top;
    resolveCardPos(cardX, cardY, g_winW, g_winH);

    // 先隐藏窗口再捕获背景。此时圆点仍显示在屏幕上，且位于卡片左上角，
    // 同时抓取圆点会在卡片圆角外形成模糊残影；抓取区域应与卡片最终位置一致。
    ShowWindow(g_hwnd, SW_HIDE);
    flushComposition();
    captureBlurred(cardX, cardY);

    g_expanded = true;
    g_fade = 0.0f;

    // 清单展开时所有进度条从 0 开始动画，不等收起时再补
    g_progAll = true;
    g_progStart = GetTickCount();
    g_progAnim = 0.0f;
    kickProg();

    placeWindow(cardX, cardY, g_winW, g_winH);
    if (g_widgetOn) ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);

    if (bringCursor && (cardX != wr.left || cardY != wr.top)) {
        // 卡片没开在圆点那儿（翻了边或者被夹进工作区）：鼠标留在原处就丢了目标，
        // 还会立刻撞上“移开就收起”。落点是左上角那个徽标——它和圆点原本是同一个
        // 位置；将指针移至该区域后可避免失焦触发收起，且该区域不响应点击或悬停。
        SetCursorPos(cardX + (int)((SHADOW + 25) * g_scale),
                     cardY + (int)((SHADOW + 23) * g_scale));
    }

    trackLeave();

    SetTimer(g_hwnd, TIMER_FADE, 16, nullptr);
    present();
}

static void finishEdit(bool commit, bool keepDraft = false);   // 定义在后面
static void restoreEdit();             // 定义在后面，打开编辑时恢复草稿

static void endTraySession();      // 定义在后面，收起时要用

static void collapseWidget() {
    if (!g_expanded) return;

    // 收起不取消编辑；未提交输入保存为草稿，并在再次打开时恢复
    if (g_editing) finishEdit(false, true);

    g_expanded = false;
    g_fade = 1.0f;
    KillTimer(g_hwnd, TIMER_FADE);

    int d = dotPixels();

    // 回到圆点位置。展开时卡片可能翻到另一侧，所以不能直接按卡片当前位置收；
    // 但用户拖动卡片时 g_dotX/g_dotY 是跟着走的，收起后圆点应位于卡片当前左上角
    placeWindow(g_dotX, g_dotY, d + g_pillW, d);
    present();

    // 由托盘叫出来的，收起就算会话结束：恢复窗口层级和显示状态
    if (g_trayOpen) endTraySession();
}

// 托盘会话收尾：收起、把打开前的状态放回去（置顶层、显示与否）
static void endTraySession() {
    if (!g_trayOpen) return;
    g_trayOpen = false;
    KillTimer(g_hwnd, TIMER_TRAY);

    if (g_expanded) collapseWidget();      // 此时 g_trayOpen 已清除，避免重复执行托盘会话收尾逻辑

    if (!g_trayWasEnabled) {
        g_widgetOn = false;                // 打开前是关着的，再关回去；设置文件不动
        ShowWindow(g_hwnd, SW_HIDE);
    }
    applyTopmost();                        // 回到设置里那一层
}

// 位图按“卡片”和“圆点 + 窄条”里大的那个开：高 DPI 下窄条比卡片还宽
static void computeSurface() {
    int collapsedW = dotPixels() + (int)((REAL)PILL_W * dotUnit() + 0.5f) + 8;
    g_surfW = (g_winW > collapsedW) ? g_winW : collapsedW;
    g_surfH = (g_winH > dotPixels()) ? g_winH : dotPixels();
}

static void destroySurface() {
    if (g_bmp)   { DeleteObject(g_bmp);   g_bmp = nullptr; }
    if (g_memDC) { DeleteDC(g_memDC);     g_memDC = nullptr; }
    if (g_bgDC)  { DeleteDC(g_bgDC);      g_bgDC = nullptr; }
    if (g_rawDC) { DeleteDC(g_rawDC);     g_rawDC = nullptr; }
    if (g_smallDC) { DeleteDC(g_smallDC); g_smallDC = nullptr; }
    if (g_bgBmp)   { DeleteObject(g_bgBmp);   g_bgBmp = nullptr; }
    if (g_rawBmp)  { DeleteObject(g_rawBmp);  g_rawBmp = nullptr; }
    if (g_smallBmp){ DeleteObject(g_smallBmp); g_smallBmp = nullptr; }
    g_bits = nullptr;
    g_bgBits = nullptr;
    g_bgReady = false;
}

static void createSurface() {
    if (!g_screenDC) g_screenDC = GetDC(nullptr);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_surfW;
    bi.bmiHeader.biHeight = -g_surfH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    g_bmp = CreateDIBSection(g_screenDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    g_bits = (BYTE*)bits;
    g_memDC = CreateCompatibleDC(g_screenDC);
    if (g_bmp) SelectObject(g_memDC, g_bmp);

    // 抓屏模糊用的三张离屏位图
    auto makeDib = [](int w, int h, HDC ref, BYTE** bitsOut, HBITMAP* bmpOut) -> HDC {
        BITMAPINFO b{};
        b.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        b.bmiHeader.biWidth = w;
        b.bmiHeader.biHeight = -h;
        b.bmiHeader.biPlanes = 1;
        b.bmiHeader.biBitCount = 32;
        b.bmiHeader.biCompression = BI_RGB;
        void* p = nullptr;
        HBITMAP bm = CreateDIBSection(ref, &b, DIB_RGB_COLORS, &p, nullptr, 0);
        HDC dc = CreateCompatibleDC(ref);
        if (bm) SelectObject(dc, bm);
        if (bitsOut) *bitsOut = (BYTE*)p;
        if (bmpOut) *bmpOut = bm;
        return dc;
    };

    g_bgDC    = makeDib(g_surfW, g_surfH, g_screenDC, &g_bgBits, &g_bgBmp);
    g_rawDC   = makeDib(g_surfW, g_surfH, g_screenDC, nullptr, &g_rawBmp);
    g_smallDC = makeDib(g_surfW / 6 + 2, g_surfH / 6 + 2, g_screenDC, nullptr, &g_smallBmp);
}

