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
        SolidBrush bg(pal().menuBg);
        g.FillPath(&bg, &p);
        Pen pen(pal().menuBorder, 1.0f);
        g.DrawPath(&pen, &p);

        // 菜单可能从托盘线程弹出，字体在本地建，不共用悬浮清单的那几个
        Font font(L"Microsoft YaHei UI", 11.0f, FontStyleRegular, UnitPixel);

        REAL y = MN_PAD;
        for (int i = 0; i < g_menuCount; ++i) {
            RectF r(6, y, g_menuLogicalW - 12, MN_ROW);
            if (i == g_menuHover) {
                GraphicsPath hp;
                roundRect(hp, r, 7.0f);
                SolidBrush hb(Color(40, pal().accent.GetR(), pal().accent.GetG(), pal().accent.GetB()));
                g.FillPath(&hb, &hp);
            }
            drawText(g, g_menuItems[i].label, &font,
                     RectF(r.X + 10, r.Y, r.Width - 16, r.Height),
                     i == g_menuHover ? pal().text : pal().textDim, true, 0);

            y += MN_ROW;
            if (g_menuItems[i].gap) {
                Pen sp(pal().menuSep, 1.0f);
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

// 前台是不是全屏程序：只看焦点窗口的矩形——尺寸等于屏幕，或四边盖住屏幕，就算全屏。
// 不用 SHQueryUserNotificationState：它要问 shell，实测约 196µs，是这里几个窗口 API
// 加起来的近百倍；而且它把「最大化窗口」也报成忙，正是之前误藏悬浮清单的来源。
static bool fullscreenAppInFront() {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == g_hwnd) return false;

    // 桌面和任务栏本身就铺满屏幕（Progman 正好是 0,0 屏幕宽高），但它们不是「全屏程序」。
    // 点一下桌面前台就变成它，不排除的话悬浮清单会立刻被藏起来；
    // 启动时前台往往也是桌面，于是「刚启动就不见」。
    wchar_t cls[64] = {0};
    GetClassNameW(fg, cls, 64);
    if (wcscmp(cls, L"Progman") == 0 ||
        wcscmp(cls, L"WorkerW") == 0 ||
        wcscmp(cls, L"SHELLDLL_DefView") == 0 ||
        wcscmp(cls, L"SysListView32") == 0 ||
        wcscmp(cls, L"Shell_TrayWnd") == 0) {
        return false;
    }

    // 最大化窗口即使盖住整个屏幕（任务栏自动隐藏时就是这样）也不算全屏。
    // 只看矩形会把 VS Code 这类最大化窗口误判成全屏，启动时把悬浮清单藏掉。
    if (IsZoomed(fg)) return false;

    RECT r{};
    if (!GetWindowRect(fg, &r)) return false;

    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return false;

    // 任务栏可见时最大化窗口只盖住工作区，高度差一个任务栏，两种判定都不会命中
    int mw = mi.rcMonitor.right - mi.rcMonitor.left;
    int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    if (r.right - r.left == mw && r.bottom - r.top == mh) return true;

    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
           r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}

// 切置顶。owned 窗口设 HWND_TOPMOST 不生效，所以置顶时先把 owner 摘掉；
// 关掉时再把 owner 挂回桌面，Win+D 才会跟着浮上来。
static void applyTopmost() {
    if (!g_hwnd) return;

    if (g_topmost) {
        SetWindowLongPtrW(g_hwnd, GWLP_HWNDPARENT, 0);
        // SetWindowPos(HWND_TOPMOST) 在有些情况下不生效（实测新起的实例样式位就是没加上），
        // 这里直接把 WS_EX_TOPMOST 写进扩展样式，再让 SetWindowPos 去调整 Z 序
        LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
        if (!(ex & WS_EX_TOPMOST)) {
            SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex | WS_EX_TOPMOST);
        }
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
// 状态以后台为准：只发请求，等它回 WM_APP_TOPMOST 再切。
// 本地先改的话，请求失败就会变成「窗口没置顶、设置里却写着置顶」，
// 这种不一致会一直持续到下次重启。
static void toggleTopmost() {
    bool on = !g_topmost;

    if (g_port == 0) findServerPort();
    if (g_port == 0) {                  // 连不上后台就本地生效，至少按钮有反应
        g_topmost = on;
        applyTopmost();
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    string body;
    httpRequest(L"POST", wstring(L"/api/topmost?on=") + (on ? L"1" : L"0"), body);
}

static void keepAboveDesktop() {
    // 右键菜单展开时不动 Z 序：菜单是独立置顶窗口，重新抢置顶会把它压下去
    if (g_menuOpen) return;

    // 设置里的置顶是唯一事实来源。窗口本地可能在启动阶段读到旧值，
    // 或者错过一次通知；每轮守护都对齐，避免「设置里开着、窗口却没置顶」。
    bool wantTopmost = appTopmostEnabled();
    if (g_topmost != wantTopmost) {
        g_topmost = wantTopmost;
        applyTopmost();
        return;
    }

    // 置顶模式：保持在普通窗口之上；前台为全屏程序时暂时隐藏，避免干扰
    if (g_topmost) {
        // 第一轮守护把「启动时已经在全屏」也当作刚切进全屏；退出全屏后恢复。
        bool fs = fullscreenAppInFront();
        bool entered = fs && !g_fsPrev;
        g_fsPrev = fs;

        if (entered && !g_hiddenForFullscreen) {
            g_hiddenForFullscreen = true;
            ShowWindow(g_hwnd, SW_HIDE);
        } else if (!fs && g_hiddenForFullscreen) {
            g_hiddenForFullscreen = false;
            if (g_widgetOn) ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        }
        // 置顶状态有丢失的情况（窗口会永久沉到普通层，用户看到的就是“悬浮清单不见了”）。
        // 每 600ms 校验一次样式位，缺了就整段重设。
        // 不看 fs：前台是不是全屏跟置顶样式无关，之前放在 !fs 里面，
        // 全屏时这条就永远不跑，启动阶段没加上就一直沉在下面。
        LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
        if (ex & WS_EX_TOPMOST) {
            SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        } else {
            applyTopmost();
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
// 切换日间 / 夜间：配色整组换，毛玻璃跟着开关，旧背景作废。
// 主题来自设置文件（控制台那个开关），不再跟随系统的应用模式。
// 系统「应用模式」的当前值：跟随系统时用它决定深浅
static bool systemPrefersLight() {
    HKEY k = nullptr;
    DWORD light = 1, sz = sizeof(light), type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        RegQueryValueExW(k, L"AppsUseLightTheme", nullptr, &type, (LPBYTE)&light, &sz);
        RegCloseKey(k);
    }
    return light != 0;
}

// 三档模式换算成当前该用哪套配色
static bool themeModeIsLight(int mode) {
    if (mode == 0) return true;
    if (mode == 1) return false;
    return systemPrefersLight();
}

static void setDayMode(bool light) {
    g_dayMode = light;
    g_pal = light ? static_cast<const Palette*>(&g_dayPalette)
                  : static_cast<const Palette*>(&g_nightPalette);
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

// 算窄条完全展开的宽度，右边放不下就缩，缩到读不下标题就不显示。
static void syncPillWidth() {

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
    refreshAfterInteraction();      // 被唤起时立刻取一次最新数据

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
    g_bits = nullptr;
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

}

