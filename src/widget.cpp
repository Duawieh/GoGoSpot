// GoGoSpot 悬浮清单：常驻桌面的圆角面板，显示未完成任务，可以直接勾选。
// 纯 Win32 + GDI+ 分层窗口，圆角与投影由 GDI+ 绘制；任务数据通过 /api/widget 和 /api/toggle 读取。

#define WIN32_LEAN_AND_MEAN
// 使用 IFileOpenDialog 需要启用 Vista 及以上版本的 Windows SDK 定义
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x06010000
#endif
#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <windowsx.h>

#include "app.h"

#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

using namespace Gdiplus;
using std::string;
using std::wstring;


#include "widget/layout.inc"
#include "widget/colors.inc"
#include "widget/state.inc"
#include "widget/utilities.inc"
#include "widget/http.inc"
#include "widget/task_data.inc"
#include "widget/settings.inc"
#include "widget/drawing_helpers.inc"
#include "widget/rendering.inc"
#include "widget/surface.inc"
#include "widget/context_menu.inc"
#include "widget/hit_testing.inc"
#include "widget/interactions.inc"
#include "widget/inline_add.inc"
#include "widget/window_entry.inc"
