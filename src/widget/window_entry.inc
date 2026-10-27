// ---- 入口 ----

// MinGW 默认 WINVER 版本较低，需在此声明 user32 导出函数。
extern "C" __declspec(dllimport) BOOL WINAPI SetProcessDPIAware(VOID);

void widgetNotifyEnabled(bool on) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_WIDGET_ENABLED, on ? 1 : 0, 0);
}

void widgetTrayOpen() {
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_TRAY_OPEN, 0, 0);
}

void widgetNotifyTopmost(bool on) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_TOPMOST, on ? 1 : 0, 0);
}

// 圆点再按屏幕大小放大一档。用逻辑高度算，4K 开 200% 缩放和 1080p 一样大，
// 只有真的多了屏幕面积才长个儿。卡片不跟着放大，它在小屏上已经占一半高度了。
static void updateDotScale() {
    int sh = GetSystemMetrics(SM_CYSCREEN);
    float logicalH = (g_scale > 0.1f) ? (float)sh / g_scale : (float)sh;
    // 1080p 打 1.1，4K@100% 打 2.0，中间线性；下限 1.0，上限 2.0
    g_dotK = 1.1f + (logicalH - 1080.0f) * 0.9f / 1080.0f;
    if (g_dotK < 1.0f) g_dotK = 1.0f;
    if (g_dotK > 2.0f) g_dotK = 2.0f;
}

// 缩放变了就按新比例重算尺寸、重建离屏位图
static void applyScale(float scale) {
    g_scale = scale;
    updateDotScale();
    g_winW = (int)(WIN_W * g_scale + 0.5f);
    g_winH = (int)(WIN_H * g_scale + 0.5f);
    computeSurface();
    destroySurface();
    createSurface();
}

HWND widgetCreate(bool enabled) {
    // 让系统按真实 DPI 布局，而不是把窗口当成物理像素再虚拟化坐标
    {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        typedef BOOL (WINAPI *PFN_SetCtx)(HANDLE);
        typedef UINT (WINAPI *PFN_GetDpi)(void);
        bool aware = false;
        if (user32) {
            PFN_SetCtx setCtx = reinterpret_cast<PFN_SetCtx>(
                reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
            if (setCtx) {
                aware = setCtx((HANDLE)-4) != 0 || setCtx((HANDLE)-3) != 0 || setCtx((HANDLE)-2) != 0;
            }
        }
        if (!aware) SetProcessDPIAware();

        UINT dpi = 96;
        if (user32) {
            PFN_GetDpi getDpi = reinterpret_cast<PFN_GetDpi>(
                    reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForSystem")));
            if (getDpi) dpi = getDpi();
        }
        if (dpi < 72 || dpi > 480) dpi = 96;
        g_scale = dpi / 96.0f;

        updateDotScale();

        g_winW = (int)(WIN_W * g_scale + 0.5f);
        g_winH = (int)(WIN_H * g_scale + 0.5f);
    }

    g_root = exeDir();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"GoGoSpotWidgetWnd";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    POINT pos{};
    bool havePos = loadSettings(pos);

    // 校验用整个虚拟桌面：副屏在主屏左边时坐标是负的，按主屏量会把位置打回主屏
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    bool onScreen = havePos &&
                    pos.x > vx - 100 && pos.y > vy - 100 &&
                    pos.x < vx + vw - 100 && pos.y < vy + vh - 100;
    if (!onScreen) {
        pos.x = GetSystemMetrics(SM_CXSCREEN) - g_winW - 40;
        pos.y = 80;
    }

    computeSurface();

    int dotPx = (int)(DOT_SIZE * dotUnit() + 0.5f);
    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        L"GoGoSpotWidgetWnd", L"GoGoSpot 悬浮清单", WS_POPUP,
        pos.x, pos.y, dotPx, dotPx, nullptr, nullptr, wc.hInstance, nullptr);

    if (!g_hwnd) return nullptr;

    createSurface();

    g_topmost = appTopmostEnabled();

    // 桌面窗口作 owner：owned 窗口紧贴 owner 的 Z 序，Win+D 时跟着浮上来，
    // 但不是 topmost，普通窗口照样能盖住。置顶模式下不能挂 owner，否则设不上 topmost。
    if (!g_topmost) {
        HWND progman = FindWindowW(L"Progman", nullptr);
        if (progman) SetWindowLongPtrW(g_hwnd, GWLP_HWNDPARENT, (LONG_PTR)progman);
    }

    g_widgetOn = enabled;
    if (enabled) {
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        if (g_topmost) applyTopmost();
        present();

        // 首次取数放到窗口显示之后，避免启动时一片空白
        findServerPort();
        if (g_port != 0 && fetchTasks()) {
            syncPillWidth();
            present();
        }
    } else {
        ShowWindow(g_hwnd, SW_HIDE);
    }

    return g_hwnd;
}
