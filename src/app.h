// GoGoSpot 内部接口：服务端、悬浮清单、程序外壳编在同一个 exe 里，
// 这里只放三者共享的符号。

#ifndef GOGOSPOT_APP_H
#define GOGOSPOT_APP_H

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include "resource.h"

// ---- 版本 ----

// 版本号只在这里改一处：/api/health、/api/info 和命令行横幅都读它
static const char* const GOGOSPOT_VERSION = "1.0.2";

// ---- 日期 ----

// 某年某月有多少天。服务端校验 due、悬浮清单画月历都走这一份，别再各写一遍
inline int daysOfMonth(int y, int m) {
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m < 1 || m > 12) return 0;
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return d[m - 1];
}

// ---- 端口 ----

// 端口不固定：服务从 8765 往上绑第一个能绑的，悬浮清单按同一区间反查。
static const int PORT_SCAN_START = 8765;
static const int PORT_SCAN_COUNT = 32;                                      // 最多往上试 32 个
static const int PORT_SCAN_END   = PORT_SCAN_START + PORT_SCAN_COUNT - 1;   // 8796

// ---- server.cpp ----

// 启动本地服务：绑端口、写 .port、后台线程跑 accept。
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

// 托盘点一下：临时展开并置顶，2 秒没交互就收回去
void widgetTrayOpen();

// 画 GoGoSpot 标识（3/4 圆弧 + 圆点），以 (cx,cy) 为中心、box 为外框边长。
// brush 为空用品牌渐变，传实色刷子画单色版。
void drawMark(Gdiplus::Graphics& g, float cx, float cy, float box,
              Gdiplus::Brush* brush = nullptr);

// ---- main.cpp ----

// 退出整个程序（含服务线程）
void appQuit();

#endif // GOGOSPOT_APP_H
