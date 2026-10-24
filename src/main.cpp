// GoGoSpot 程序外壳：本地服务、悬浮清单、托盘图标装进同一个 exe。
// 服务跑在后台线程，不弹命令行窗口；托盘右键可以打开控制台或退出。

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>

#include <cstdio>
#include <cstdlib>

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

static void showTrayMenu(HWND h) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"打开控制台");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 2, L"退出");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(h);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                             pt.x, pt.y, 0, h, nullptr);
    DestroyMenu(menu);
    PostMessageW(h, WM_NULL, 0, 0);     // 让菜单正常消失的老规矩

    if (cmd == 1) openFullApp();
    else if (cmd == 2) appQuit();
}

static LRESULT CALLBACK TrayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_TRAY_CB:
            if (l == WM_RBUTTONUP || l == WM_CONTEXTMENU) {
                showTrayMenu(h);
            } else if (l == WM_LBUTTONUP) {
                // 点一下就把清单临时摊开，双击暂时不做别的
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

void appQuit() {
    if (g_trayWnd) PostMessageW(g_trayWnd, WM_CLOSE, 0, 0);
}

// ---- 入口 ----

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    GdiplusStartup(&token, &gsi, nullptr);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // 已有实例：按悬浮清单开关决定要不要开浏览器，然后返回 -1，不再起第二套窗口
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
        // 现画失败就用 exe 里嵌的图标
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
    if (widget) DestroyWindow(widget);          // 顺手把窗口位置存下来
    if (g_trayWnd) DestroyWindow(g_trayWnd);
    if (g_icon && g_icon != LoadIconW(nullptr, MAKEINTRESOURCEW(32512))) DestroyIcon(g_icon);

    serverStop();
    CoUninitialize();
    GdiplusShutdown(token);
    return 0;
}
