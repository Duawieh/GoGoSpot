// GoGoSpot 内部接口：后台、悬浮清单、程序外壳编在同一个 exe 里，
// 这里只放三者共享的符号。

#ifndef GOGOSPOT_APP_H
#define GOGOSPOT_APP_H

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include "resource.h"

// ---- 版本 ----

// 版本号只在这里改一处：/api/health、/api/info 和命令行横幅都读它
static const char* const GOGOSPOT_VERSION = "1.0.6";

// ---- 日期 ----

// 指定年月的天数。后台校验 due 与悬浮清单绘制月历共用此实现，不再各自维护。
inline int daysOfMonth(int y, int m) {
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m < 1 || m > 12) return 0;
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return d[m - 1];
}

// ---- 端口 ----

// 端口不固定：后台从 8765 往上绑第一个能绑的，悬浮清单按同一区间反查。
static const int PORT_SCAN_START = 8765;
static const int PORT_SCAN_COUNT = 32;                                      // 最多扫描后续 32 个端口
static const int PORT_SCAN_END   = PORT_SCAN_START + PORT_SCAN_COUNT - 1;   // 8796

// ---- server.cpp ----

// 启动后台：绑端口、写 .port、起线程跑 accept。
// 返回监听端口；<0 表示不该继续（已有实例或启动失败）。
int  serverStart(int argc, char** argv);
void serverStop();

// 悬浮清单开关，存在 data\settings.json
bool appWidgetEnabled();
void appSetWidgetEnabled(bool on);

// 悬浮清单置顶：开=压在普通窗口上面，关=只贴在桌面上
bool appTopmostEnabled();
void appSetTopmost(bool on);

// 开机自动启动：写 HKCU\...\Run，跟着 exe 路径走
bool appAutoStartEnabled();
void appSetAutoStart(bool on);

// ---- widget.cpp ----

// 创建悬浮清单窗口；enabled=false 时只创建不显示。
HWND widgetCreate(bool enabled);

// 跨线程安全：只投消息，处理在 UI 线程
void widgetNotifyEnabled(bool on);
void widgetNotifyTopmost(bool on);

// 单击托盘图标：临时展开并置顶，2 秒无交互后收起
void widgetTrayOpen();

// 右键菜单：单独一个置顶分层窗口，Z 序独立于悬浮清单。
// 返回选中项的 cmd；点空白处或按 Esc 关闭时返回 0。使用屏幕坐标，并按屏幕边界调整菜单位置。
struct MenuItemDef {
    const wchar_t* label;
    int cmd;
    bool gap;         // 这一项下面画一条分隔线
};
int widgetPopupMenu(int screenX, int screenY, const MenuItemDef* items, int count);

// 画 GoGoSpot 标识（3/4 圆弧 + 圆点），以 (cx,cy) 为中心、box 为外框边长。
// brush 为空用品牌渐变，传实色刷子画单色版。
void drawMark(Gdiplus::Graphics& g, float cx, float cy, float box,
              Gdiplus::Brush* brush = nullptr);

// ---- main.cpp ----

// 弹一条系统通知。走托盘图标的气泡，Win10 之后会进通知中心；
// 任何线程都能调，内部只做一次 Shell_NotifyIcon。
void appNotify(const wchar_t* text);

// 退出整个程序（含后台线程）
void appQuit();

#endif // GOGOSPOT_APP_H
