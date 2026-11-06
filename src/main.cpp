// GoGoSpot 程序外壳：后台、悬浮清单、托盘图标装进同一个 exe。
// 后台跑在独立线程，不弹命令行窗口；托盘右键可以打开控制台或退出。

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "app.h"

using namespace Gdiplus;

// ---- 状态 ----

static const UINT WM_TRAY_CB = WM_APP + 10;
static const UINT ID_TRAY    = 1;

static HWND   g_trayWnd = nullptr;
static int    g_port    = 0;
static HICON  g_icon    = nullptr;
static NOTIFYICONDATAW g_nid{};

// ---- 托盘图标 ----

static void roundRectPath(GraphicsPath& p, REAL x, REAL y, REAL w, REAL h, REAL r) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    p.AddArc(x, y, r * 2, r * 2, 180, 90);
    p.AddArc(x + w - r * 2, y, r * 2, r * 2, 270, 90);
    p.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90);
    p.AddArc(x, y + h - r * 2, r * 2, r * 2, 90, 90);
    p.CloseFigure();
}

// 圆角方块底 + 标识，和 icons/app 那套一致
static HICON makeTrayIcon(int size) {
    if (size < 8) size = 16;

    Bitmap bmp(size, size, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.Clear(Color(0, 0, 0, 0));

    GraphicsPath p;
    roundRectPath(p, 0, 0, (REAL)size, (REAL)size, size * 0.225f);
    LinearGradientBrush bg(PointF(0, 0), PointF(0, (REAL)size),
                           Color(255, 40, 47, 65), Color(255, 19, 23, 33));
    g.FillPath(&bg, &p);

    REAL box = size * (size <= 20 ? 0.80f : 0.74f);
    drawMark(g, size / 2.0f, size / 2.0f, box);

    HICON icon = nullptr;
    if (bmp.GetHICON(&icon) != Ok || !icon) return nullptr;
    return icon;
}

// ---- 托盘 ----

static void openFullApp() {
    if (g_port <= 0) return;
    wchar_t url[128];
    swprintf(url, 128, L"http://127.0.0.1:%d/", g_port);
    ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

static bool trayAdd(HWND h, HICON icon) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = h;
    g_nid.uID = ID_TRAY;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY_CB;
    g_nid.hIcon = icon;
    lstrcpynW(g_nid.szTip, L"GoGoSpot", 128);
    return Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
}

// 系统通知：复用托盘图标发气泡。Win10 之后它会进通知中心，不是老式气球。
// 后台线程直接调，这里只改一份副本，不动 g_nid 本身。
void appNotify(const wchar_t* text) {
    if (!g_trayWnd) return;
    NOTIFYICONDATAW nid = g_nid;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    nid.uTimeout = 8000;
    lstrcpynW(nid.szInfoTitle, L"GoGoSpot", 64);
    lstrcpynW(nid.szInfo, text, 256);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void showTrayMenu() {
    // 悬浮清单停用后窗口完全隐藏，仅可通过托盘菜单重新启用
    bool on = appWidgetEnabled();
    bool topmost = appTopmostEnabled();

    POINT pt{};
    GetCursorPos(&pt);

    MenuItemDef items[5];
    int n = 0;
    items[n++] = { L"打开控制台页面", 1, true };
    items[n++] = { on ? L"关闭悬浮清单" : L"启用悬浮清单", 3, false };
    items[n++] = { topmost ? L"悬浮清单取消置顶" : L"悬浮清单置顶", 4, true };
    items[n++] = { L"退出", 2, false };

    int cmd = widgetPopupMenu(pt.x, pt.y, items, n);
    if (cmd == 1) openFullApp();
    else if (cmd == 2) appQuit();
    else if (cmd == 3) appSetWidgetEnabled(!appWidgetEnabled());
    else if (cmd == 4) appSetTopmost(!appTopmostEnabled());
}

static LRESULT CALLBACK TrayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_TRAY_CB:
            if (l == WM_RBUTTONUP || l == WM_CONTEXTMENU) {
                showTrayMenu();
            } else if (l == WM_LBUTTONUP) {
                // 单击临时展开清单；双击暂无行为
                widgetTrayOpen();
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(h, m, w, l);
    }
}

// 把窗口提到前台。后台进程直接调 SetForegroundWindow 会被系统拦下（只闪任务栏），
// 标准做法是把自己的输入队列挂到当前前台线程上，借它的权限切过去。
static void bringToFront(HWND hwnd) {
    if (!hwnd) return;
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);

    // AttachThreadInput 要求双方都有消息队列；本线程是临时起的，先建一个
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    HWND fg = GetForegroundWindow();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD myThread = GetCurrentThreadId();
    bool attached = false;

    if (fgThread && fgThread != myThread) attached = AttachThreadInput(myThread, fgThread, TRUE) != FALSE;

    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);

    if (attached) AttachThreadInput(myThread, fgThread, FALSE);
    if (GetForegroundWindow() == hwnd) return;

    // 兜底：这个 API 虽标记为过时，正是为跨进程切前台保留的
    typedef void (WINAPI *SwitchFn)(HWND, BOOL);
    static SwitchFn sw = reinterpret_cast<SwitchFn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SwitchToThisWindow")));
    if (sw) sw(hwnd, TRUE);
}

struct FocusCtx {
    const std::wstring* leaf;
    HWND hit;
};

// 标题里带目录名、且可见的资源管理器窗口。多标签窗口的标题形如
// “目录名 和 1 个其他选项卡 - 文件资源管理器”，用子串匹配即可。
static BOOL CALLBACK focusEnumProc(HWND h, LPARAM lp) {
    FocusCtx* c = reinterpret_cast<FocusCtx*>(lp);

    wchar_t cls[64] = {0};
    GetClassNameW(h, cls, 64);
    if (wcscmp(cls, L"CabinetWClass") != 0) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;

    wchar_t title[512] = {0};
    GetWindowTextW(h, title, 512);
    if (wcsstr(title, c->leaf->c_str())) {
        c->hit = h;
        return FALSE;
    }
    return TRUE;
}

// 资源管理器不一定会自己浮上来：轮询找它的窗口再切前台。
// 单独线程做，调用方（HTTP 请求线程 / 悬浮清单）不用等。
static void focusExplorerWindow(std::wstring leaf) {
    for (int i = 0; i < 12; ++i) {
        Sleep(120);
        FocusCtx ctx{ &leaf, nullptr };
        EnumWindows(focusEnumProc, reinterpret_cast<LPARAM>(&ctx));
        if (ctx.hit) {
            bringToFront(ctx.hit);
            return;
        }
    }
}

// 打开本地路径。文件夹直接进去；文件在父目录里选中。
bool appOpenLocalPath(const std::wstring& path) {
    if (path.empty()) return false;

    AllowSetForegroundWindow(ASFW_ANY);

    DWORD attr = GetFileAttributesW(path.c_str());
    bool isDir = (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));

    // 文件用 explorer.exe /select 而不是 SHOpenFolderAndSelectItems：
    // 实测后者是同步 COM 调用，每次都要 700–800ms；前者在资源管理器已开的情况下
    // 只要约 80ms（窗口出现），只有冷启动才会到 1.1 秒
    bool ok = false;
    if (isDir) {
        HINSTANCE h = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ok = (INT_PTR)h > 32;
    } else {
        std::wstring arg = L"/select,\"" + path + L"\"";
        HINSTANCE h = ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(),
                                    nullptr, SW_SHOWNORMAL);
        ok = (INT_PTR)h > 32;
    }
    if (!ok) return false;

    // 窗口标题用的是目录名：文件夹取自己，文件取所在目录
    std::wstring dir = isDir ? path : path.substr(0, path.find_last_of(L"\\/"));
    size_t slash = dir.find_last_of(L"\\/");
    std::wstring leaf = (slash == std::wstring::npos) ? dir : dir.substr(slash + 1);
    if (!leaf.empty()) std::thread(focusExplorerWindow, leaf).detach();

    return true;
}

void appQuit() {
    if (g_trayWnd) PostMessageW(g_trayWnd, WM_CLOSE, 0, 0);
}

// ---- 入口 ----

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    GdiplusStartup(&token, &gsi, nullptr);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // 已有实例：根据悬浮清单开关决定是否打开浏览器，随后返回 -1，不再创建第二套窗口
    int port = serverStart(__argc, __argv);
    if (port < 0) {
        CoUninitialize();
        GdiplusShutdown(token);
        return 0;
    }
    g_port = port;

    HWND widget = widgetCreate(appWidgetEnabled());

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TrayProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"GoGoSpotTrayWnd";
    RegisterClassExW(&wc);

    g_trayWnd = CreateWindowExW(0, L"GoGoSpotTrayWnd", L"GoGoSpot", WS_OVERLAPPED,
                                0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);

    int small = GetSystemMetrics(SM_CXSMICON);
    g_icon = makeTrayIcon(small > 0 ? small : 16);
    if (!g_icon) {
        // 动态绘制失败时使用程序资源中的图标
        g_icon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                   small, small, LR_DEFAULTCOLOR);
    }
    if (!g_icon) g_icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));   // IDI_APPLICATION

    if (g_trayWnd) trayAdd(g_trayWnd, g_icon);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (widget) DestroyWindow(widget);          // 销毁窗口前保存其位置
    if (g_trayWnd) DestroyWindow(g_trayWnd);
    if (g_icon && g_icon != LoadIconW(nullptr, MAKEINTRESOURCEW(32512))) DestroyIcon(g_icon);

    serverBackupOnExit();                       // 关之前留一份，这次会话改过才备
    serverStop();
    CoUninitialize();
    GdiplusShutdown(token);

    // 不用 return 走静态析构：GDI+ 已经关掉，字体等静态对象再析构会访问已释放的资源，
    // 进程会以段错误退出。界面已经清理干净，剩下交给系统回收
    fflush(stdout);
    ExitProcess(0);
}
