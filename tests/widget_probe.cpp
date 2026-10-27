// 悬浮清单探针：按进程号找窗口，读矩形、投点击、发关闭。
// 只发消息，不搬鼠标不截屏。
//
// 用法：widget_probe.exe <pid> <命令> [参数]
//   windows                 列出该进程所有顶层窗口（含类名）
//   rect                    打印悬浮清单窗口矩形（屏幕坐标）
//   click <x> <y>           在客户区坐标处左键单击
//   rclick <x> <y>          在客户区坐标处右键单击
//   menu                    打印右键菜单窗口的状态（找不到时退出码 5）
//   clickabs <x> <y>        在屏幕坐标处点右键菜单（选菜单项）
//   close                   给悬浮清单窗口发 WM_CLOSE
//   quit                    给托盘窗口发 WM_CLOSE

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <vector>

struct Win {
    HWND h;
    wchar_t cls[128];
    wchar_t title[256];
    RECT rc;
    bool visible;
};

static std::vector<Win> g_wins;

static BOOL CALLBACK enumProc(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != (DWORD)(INT_PTR)lp) return TRUE;

    Win w{};
    w.h = h;
    GetClassNameW(h, w.cls, 128);
    GetWindowTextW(h, w.title, 256);
    GetWindowRect(h, &w.rc);
    w.visible = IsWindowVisible(h) != 0;
    g_wins.push_back(w);
    return TRUE;
}

static HWND findWidget() {
    for (const Win& w : g_wins) {
        if (wcsstr(w.title, L"悬浮清单")) return w.h;
    }
    return nullptr;
}

static HWND findTray() {
    for (const Win& w : g_wins) {
        if (wcscmp(w.title, L"GoGoSpot") == 0) return w.h;
    }
    return nullptr;
}

static HWND findMenu() {
    for (const Win& w : g_wins) {
        if (wcscmp(w.cls, L"GoGoSpotMenuWnd") == 0) return w.h;
    }
    return nullptr;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        printf("usage: widget_probe <pid> windows|rect|click <x> <y>|rclick <x> <y>|menu|clickabs <x> <y>|close|quit\n");
        return 2;
    }
    DWORD pid = (DWORD)_wtoi(argv[1]);
    EnumWindows(enumProc, (LPARAM)(INT_PTR)pid);

    const wchar_t* cmd = argv[2];

    if (wcscmp(cmd, L"windows") == 0) {
        for (const Win& w : g_wins) {
            printf("%p  [%ls]  %ls  %ld,%ld %ldx%ld%s\n", (void*)w.h, w.cls, w.title,
                   (long)w.rc.left, (long)w.rc.top,
                   (long)(w.rc.right - w.rc.left), (long)(w.rc.bottom - w.rc.top),
                   w.visible ? "" : "  (隐藏)");
        }
        return 0;
    }

    if (wcscmp(cmd, L"menu") == 0) {
        HWND m = findMenu();
        if (!m) {
            printf("没有菜单窗口\n");
            return 5;
        }
        RECT r{};
        GetWindowRect(m, &r);
        printf("%ld %ld %ld %ld 可见=%d\n", (long)r.left, (long)r.top,
               (long)(r.right - r.left), (long)(r.bottom - r.top),
               (int)(IsWindowVisible(m) != 0));
        return 0;
    }

    if (wcscmp(cmd, L"clickabs") == 0 && argc >= 5) {
        HWND m = findMenu();
        if (!m) {
            printf("没有菜单窗口\n");
            return 5;
        }
        int sx = _wtoi(argv[3]), sy = _wtoi(argv[4]);
        POINT pt{sx, sy};
        ScreenToClient(m, &pt);
        LPARAM lp = MAKELPARAM(pt.x, pt.y);
        SendMessageW(m, WM_LBUTTONDOWN, MK_LBUTTON, lp);
        SendMessageW(m, WM_LBUTTONUP, 0, lp);
        printf("menu clicked %d %d\n", sx, sy);
        return 0;
    }

    HWND h = findWidget();
    if (!h) {
        printf("找不到悬浮清单窗口\n");
        return 3;
    }

    if (wcscmp(cmd, L"rect") == 0) {
        RECT r{};
        GetWindowRect(h, &r);
        printf("%ld %ld %ld %ld\n", (long)r.left, (long)r.top,
               (long)(r.right - r.left), (long)(r.bottom - r.top));
        return 0;
    }

    if ((wcscmp(cmd, L"click") == 0 || wcscmp(cmd, L"rclick") == 0) && argc >= 5) {
        int x = _wtoi(argv[3]), y = _wtoi(argv[4]);
        LPARAM lp = MAKELPARAM(x, y);
        if (cmd[0] == L'r') {
            // 右键会进模态菜单循环，SendMessage 要等菜单关掉才返回，这里只能投递
            PostMessageW(h, WM_RBUTTONDOWN, MK_RBUTTON, lp);
            PostMessageW(h, WM_RBUTTONUP, 0, lp);
        } else {
            SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, lp);
            SendMessageW(h, WM_LBUTTONUP, 0, lp);
        }
        printf("%ls %d %d\n", cmd, x, y);
        return 0;
    }

    // 采样窗口宽度：用来量动画（窄条滑出、卡片展开）的时长和曲线
    if (wcscmp(cmd, L"watch") == 0 && argc >= 4) {
        int ms = _wtoi(argv[3]);
        DWORD t0 = GetTickCount();
        for (;;) {
            RECT r{};
            GetWindowRect(h, &r);
            DWORD t = GetTickCount() - t0;
            printf("%lu %ld %ld\n", (unsigned long)t, (long)(r.right - r.left),
                   (long)(r.bottom - r.top));
            if ((int)t >= ms) break;
            Sleep(10);
        }
        return 0;
    }

    if (wcscmp(cmd, L"close") == 0) {
        SendMessageW(h, WM_CLOSE, 0, 0);
        printf("close sent\n");
        return 0;
    }

    if (wcscmp(cmd, L"quit") == 0) {
        HWND tray = findTray();
        if (!tray) {
            printf("找不到托盘窗口\n");
            return 4;
        }
        SendMessageW(tray, WM_CLOSE, 0, 0);
        printf("quit sent\n");
        return 0;
    }

    printf("未知命令\n");
    return 2;
}
