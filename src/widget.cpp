// GoGoSpot 悬浮清单：常驻桌面的圆角面板，显示未完成任务，可以直接勾选。
// 纯 Win32 + GDI+ 分层窗口，圆角和投影自己画；数据走 /api/widget 和 /api/toggle。

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <commdlg.h>
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

// ---- 布局 ----

static const int CARD_W   = 320;
static const int CARD_H   = 424;
static const int SHADOW   = 16;            // 四周留给投影的透明边距
static const int WIN_W    = CARD_W + SHADOW * 2;
static const int WIN_H    = CARD_H + SHADOW * 2;

static const int TITLE_H  = 46;
static const int FOOT_H   = 44;
static const int ROW_H    = 54;
static const REAL RADIUS  = 16.0f;

static const int LIST_TOP    = TITLE_H;
static const int LIST_BOTTOM = CARD_H - FOOT_H;
static const int VISIBLE_ROWS = (LIST_BOTTOM - LIST_TOP) / ROW_H;   // 6

// 就地添加时从底部拉出来的面板：输入框 + 截止 / 预估两排选项
static const int PANEL_H = 138;
static const int CHIP_H  = 24;

static const UINT_PTR TIMER_REFRESH  = 1;
static const UINT_PTR TIMER_SETTLE   = 2;
static const UINT_PTR TIMER_FADE     = 3;
static const UINT_PTR TIMER_COLLAPSE = 4;
static const UINT_PTR TIMER_ZGUARD   = 5;
static const UINT_PTR TIMER_SLIDE    = 6;
static const UINT_PTR TIMER_CARET    = 7;
static const UINT_PTR TIMER_ROWS     = 8;
static const UINT_PTR TIMER_TRAY     = 9;
static const UINT_PTR TIMER_PROG     = 10;
static const UINT_PTR TIMER_TICK     = 11;   // 有任务在跑时每秒重画，计时器才走得动

// 后台改设置后，通过这个自定义消息通知窗口（可跨线程 Post）
static const UINT WM_APP_WIDGET_ENABLED = WM_APP + 1;
static const UINT WM_APP_TOPMOST        = WM_APP + 2;
static const UINT WM_APP_TRAY_OPEN      = WM_APP + 3;

// 老 SDK 里没有这个常量，自己兜一个
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

// 收起状态下窗口只有这么大，中间画一个圆点当锚点
static const int DOT_SIZE = 64;

// 圆点右边那条窄条，只写最急的一条：红色常驻，黄色鼠标停上来才拉出。
static const int PILL_X   = 50;     // 起点，正好让开圆点那圈柔光
static const int PILL_H   = 40;
static const int PILL_W   = 236;    // 上限宽度，长标题到这里就省略号
static const int PILL_MIN = 96;     // 太短的标题也撑到这么宽；剩余空间不够就干脆不显示

// 外圈半径：常态 RING_R_IDLE，鼠标停上来涨到 RING_R，和窄条共用一套动画。
static const REAL RING_R      = PILL_H / 2.0f;   // 悬停时：直径正好等于窄条高度
static const REAL RING_R_IDLE = 16.0f;   // 常态

// 紧急分级：剩余 <= min(预估, 12h) 红色，24h 内黄色。
static const long long URG_RED_CAP_MS = 12LL * 3600000LL;
static const long long URG_YELLOW_MS  = 24LL * 3600000LL;

// ---- 配色 ----

static const Color C_CARD      (251, 22, 25, 31);
static const Color C_BORDER    (40, 255, 255, 255);
static const Color C_TEXT      (255, 233, 237, 246);
static const Color C_TEXT_DIM  (255, 153, 163, 186);
static const Color C_TEXT_FAINT(255, 115, 125, 145);
static const Color C_ACCENT    (255, 108, 140, 255);
static const Color C_ACCENT2   (255, 169, 123, 255);
static const Color C_HOVER     (14, 255, 255, 255);
static const Color C_TRACK     (26, 255, 255, 255);
static const Color C_GLASS     (190, 15, 18, 25);   // 罩在模糊背景上的深色

// 紧急分级的两档色：红=马上要交，黄=一天之内
static const Color C_RED       (255, 255, 92, 112);
static const Color C_AMBER     (255, 255, 189, 74);
static const Color C_PURPLE    (255, 178, 132, 255);   // 不紧急但快到期的那档
static const Color C_GREEN     (255,  92, 206, 138);   // 进行中的环，不紧急时用

// ---- 状态 ----

struct Task {
    wstring id;
    wstring title;
    int estimate = 0;
    long long dueMs = 0;        // Unix 毫秒，0 表示没设截止时间
    std::vector<wstring> links; // 从这里开始：可以挂多个链接或本地文件
    long long startedAt = 0;    // 正在进行：点下去的时刻，0 表示没在进行
    bool pinned = false;        // 置顶：排在最前面，和控制台共用
};

static std::vector<Task> g_tasks;
static int   g_pending = 0;
static int   g_total   = 0;
static int   g_port    = 0;
static int   g_scroll  = 0;
static bool  g_dragging = false;
static bool  g_loaded  = false;
static bool  g_loadError = false;   // 后台在，但任务文件读不出来（数据坏了或权限问题）

static bool  g_expanded = false;     // 收起（只显示圆点）/ 展开（显示任务）
static bool  g_widgetOn = true;      // 悬浮清单总开关，关掉就整个隐藏
static float g_fade = 0.0f;          // 展开时的淡入进度 0..1
static bool  g_bgReady = false;      // 模糊背景是否已抓取

static wstring g_justDone;          // 刚勾选的任务，短暂显示删除线
static int   g_hoverRow = -1;
static int   g_hoverBtn = -1;       // 0=置顶 1=关闭 2=添加 3=控制台

static BYTE*   g_bits    = nullptr;
static HDC     g_memDC   = nullptr;
static HBITMAP g_bmp     = nullptr;
static HDC     g_screenDC = nullptr;
static ULONG_PTR g_gdiToken = 0;
static HWND    g_hwnd = nullptr;
static HWND    g_desktopParent = nullptr;   // 挂到桌面后指向 Progman

static HDC   g_bgDC = nullptr;       // 模糊后的背景
static HDC   g_rawDC = nullptr;      // 原始抓屏
static HDC   g_smallDC = nullptr;    // 缩小用于模糊
static BYTE* g_bgBits = nullptr;
static HBITMAP g_bgBmp = nullptr, g_rawBmp = nullptr, g_smallBmp = nullptr;

static POINT g_dragOrigin{};
static POINT g_winOrigin{};

static wstring g_root;

// 系统 DPI 缩放：界面按 96 DPI 设计，绘制时统一缩放。
static float g_scale = 1.0f;      // DPI 缩放，卡片按它走
static float g_dotK  = 1.0f;      // 圆点额外放大倍数，按屏幕大小定
static float dotUnit() { return g_scale * g_dotK; }   // 圆点坐标系 -> 设备像素

// 没填预估用时的任务没有进度可算：环形改成紫色呼吸，行内填充固定 60%
static bool hasEstimate(const Task& t) { return t.estimate > 0; }

// 呼吸系数 0..1，1.8 秒一轮。按当前时刻直接算，不存状态，掉帧也不会走样
static REAL breatheK() {
    REAL ph = (REAL)(GetTickCount() % 1800) / 1800.0f;
    return 0.5f - 0.5f * cosf(ph * 6.2831853f);
}

// 只要还有「没填预估、正在进行」的任务，呼吸就得一直动，定时器不能停
static bool needBreath() {
    for (size_t i = 0; i < g_tasks.size(); ++i) {
        if (g_tasks[i].startedAt > 0 && g_tasks[i].estimate <= 0) return true;
    }
    return false;
}

static bool    g_progSlow  = false;   // 定时器已经降到呼吸用的慢档
static bool    g_progOn    = false;   // 定时器在跑，别重复叫

// 要播动画的地方统一从这里起步：先回到 60 帧，慢档由定时器自己再切回去。
// WM_CREATE 里 g_hwnd 还没赋值，所以允许把窗口句柄传进来。
static void kickProg(HWND h = nullptr) {
    if (!h) h = g_hwnd;
    if (!h) return;
    g_progSlow = false;
    g_progOn = true;
    SetTimer(h, TIMER_PROG, 16, nullptr);
}

static int   g_winW = WIN_W;
static int   g_winH = WIN_H;

// 离屏位图尺寸。展开态是卡片大小，收起态是「圆点 + 窄条」，
// 高 DPI 下窄条可能比卡片还宽，位图得按大的那个开，否则窄条右侧会被切掉。
static int   g_surfW = WIN_W;
static int   g_surfH = WIN_H;

// 展开那一刻圆点在哪。卡片贴边时会翻到另一侧，收起要回到圆点原位，
// 不然开合几次圆点就顺着屏幕漂走了。
static int   g_dotX = 0;
static int   g_dotY = 0;

// 窄条当前宽度（设备像素），TIMER_SLIDE 每帧推向 g_pillTarget，所以拉出缩回都是滑的。
static int   g_pillW      = 0;
static int   g_pillFull   = 0;      // 完全展开时的宽度，0 表示没有可显示的任务
static int   g_pillTarget = 0;
static bool  g_pillWant   = false;  // 鼠标停在圆点上（黄色任务靠它才露头）
static bool  g_pillHover  = false;  // 鼠标停在窄条上，亮一点，提示可以点

// 外圈展开程度 0..1，和窄条共用 TIMER_SLIDE，动画同步。
static REAL  g_ringT      = 0.0f;
static REAL  g_ringTarget = 0.0f;

// 底栏就地输入框：点「添加任务」进入，回车提交，Esc 取消，不用开控制台。
static bool    g_editing  = false;
static wstring g_editText;
static int     g_caret    = 0;       // 光标位置，0..g_editText.size()
static bool    g_caretOn  = false;

// 日/夜配色：跟 Windows 的「应用模式」走。
// 日间用浅底 + 暖色渐变，夜间用纯灰块 + 外发光。
static bool    g_dayMode  = false;

// 置顶模式：压在普通窗口上面。owned 窗口不能置顶，所以这时要把 owner 摘掉。
static bool    g_topmost  = false;
static bool    g_hiddenForFullscreen = false;

// 托盘点开的临时会话：展开 + 置顶，2 秒没交互就收回，收回后恢复原状
static bool    g_trayOpen = false;
static DWORD   g_trayIdle = 0;
static bool    g_trayWasEnabled = false;   // 打开前清单是开着还是关着

// 收起态的按下：先记着位置，移动超过阈值才算拖动，没动就当点击。
// 圆点和窄条都要能拖，又不能把「点一下展开」吃掉，只能靠位移区分。
static bool    g_pressPending = false;
static POINT   g_pressPt{};
static POINT   g_pressWin{};

// 就地添加时选的截止 / 预估。due：0 今天 1 明天 2 后天 3 自选 4 不设；est -1 = 不设
static int     g_editDue = 4;
static int     g_editEst = -1;
static bool    g_hoverCancel = false;

// 自选日期：日历面板的状态
static bool    g_pickingDate = false;
static int     g_pickYear = 0, g_pickMonth = 0, g_pickDay = 0;
static int     g_pickHour = 23, g_pickMin = 59;
static wstring g_customDue;              // "YYYY-MM-DDTHH:mm"

// 从这里开始：这条任务挂的链接或本地文件
static std::vector<wstring> g_editLinks;

// 进度条的动画：g_progAnim 从 0 涨到 1，只作用在刚点下去的那条（g_progId）
static REAL    g_progAnim  = 1.0f;
static DWORD   g_progStart = 0;
static wstring g_progId;
static bool    g_progAll = false;   // 为真时所有进度条都吃出现动画（展开清单时用）
static REAL    g_dotRingA  = 0.0f;    // 圆点外那圈环的透明度
static bool    g_dotRingOn = false;   // 上一次算出来的目标状态，用来抓「刚重新出现」

// 预估时长六档，和控制台一致
static const int    EST_MIN[6] = { 10, 30, 60, 120, 300, 1440 };
static const wchar_t* EST_LABEL[6] = { L"10分", L"30分", L"1时", L"2时", L"5时", L"1天" };
static const wchar_t* DUE_LABEL[5] = { L"今天", L"明天", L"后天", L"自选", L"不设" };

// 勾掉一条之后：那条先往右滑出去淡掉，下面的行再一条条跟上来。
// g_leaveRow 是它在新列表里原来的位置。
static bool    g_leaving    = false;
static Task    g_leaveTask;
static int     g_leaveRow   = -1;
static DWORD   g_leaveStart = 0;

// ---- 工具 ----

static wstring u8tow(const string& s) {
    if (s.empty()) return wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return wstring();
    wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static string wtou8(const wstring& w) {
    if (w.empty()) return string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return string();
    string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static wstring exeDir() {
    wchar_t buf[4096];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0) return L".";
    wstring p(buf, n);
    size_t pos = p.find_last_of(L"\\/");
    return (pos == wstring::npos) ? wstring(L".") : p.substr(0, pos);
}

// ---- HTTP ----

struct HttpSession {
    HINTERNET ses = nullptr;
    HINTERNET con = nullptr;
    HttpSession() {
        ses = WinHttpOpen(L"GoGoSpotWidget/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (ses) {
            WinHttpSetTimeouts(ses, 600, 600, 900, 900);
            con = WinHttpConnect(ses, L"127.0.0.1", (INTERNET_PORT)g_port, 0);
        }
    }
    ~HttpSession() {
        if (con) WinHttpCloseHandle(con);
        if (ses) WinHttpCloseHandle(ses);
    }
    bool valid() const { return ses && con; }
};

static bool httpRequest(const wchar_t* verb, const wstring& path, string& out) {
    HttpSession s;
    if (!s.valid()) return false;

    HINTERNET req = WinHttpOpenRequest(s.con, verb, path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!req) return false;

    bool ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != 0;
    if (ok) ok = WinHttpReceiveResponse(req, nullptr) != 0;

    if (ok) {
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
            string buf(avail, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(req, &buf[0], avail, &got) || got == 0) break;
            out.append(buf.data(), got);
        }
    }
    WinHttpCloseHandle(req);
    return ok;
}

// 只放行 unreserved，其余按 UTF-8 字节转 %XX，中文标题才能塞进查询串
static wstring urlEncode(const string& s) {
    static const char* hex = "0123456789ABCDEF";
    string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return u8tow(out);
}

// 先读 data/.port，读不到再逐个端口试探。
static int readPortFile() {
    wstring path = g_root + L"\\data\\.port";
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return 0;
    char buf[32] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return 0;
    int v = atoi(buf);
    return (v > 0 && v < 65536) ? v : 0;
}

static int findServerPort() {
    int hint = readPortFile();
    if (hint) {
        g_port = hint;
        string body;
        if (httpRequest(L"GET", L"/api/health", body) && body.find("GoGoSpot") != string::npos) {
            return hint;
        }
    }
    for (int p = PORT_SCAN_START; p <= PORT_SCAN_END; ++p) {
        if (p == hint) continue;
        g_port = p;
        string body;
        if (httpRequest(L"GET", L"/api/health", body) && body.find("GoGoSpot") != string::npos) {
            return p;
        }
    }
    g_port = 0;
    return 0;
}

// ---- 数据 ----

static void splitTabs(const string& line, std::vector<string>& out) {
    out.clear();
    size_t start = 0;
    for (size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == '\t') {
            out.push_back(line.substr(start, i - start));
            start = i + 1;
        }
    }
}

static bool fetchTasks() {
    if (g_port == 0 && findServerPort() == 0) return false;

    string body;
    if (!httpRequest(L"GET", L"/api/widget", body)) {
        g_port = 0;                       // 下次重新找端口
        return false;
    }

    std::vector<Task> list;
    int pending = 0, total = 0;

    size_t pos = 0;
    bool first = true;
    bool sawHeader = false;
    while (pos < body.size()) {
        size_t nl = body.find('\n', pos);
        if (nl == string::npos) nl = body.size();
        string line = body.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        if (first && line[0] == '#') {
            std::vector<string> f;
            splitTabs(line.substr(1), f);
            if (f.size() >= 2) {
                pending = atoi(f[0].c_str());
                total = atoi(f[1].c_str());
            }
            first = false;
            sawHeader = true;
            continue;
        }
        first = false;

        std::vector<string> f;
        splitTabs(line, f);
        if (f.size() < 5) continue;

        Task t;
        t.id = u8tow(f[0]);
        t.estimate = atoi(f[2].c_str());
        t.dueMs = _strtoi64(f[3].c_str(), nullptr, 10);
        t.title = u8tow(f[4]);
        if (f.size() >= 7) t.startedAt = _strtoi64(f[6].c_str(), nullptr, 10);
        if (f.size() >= 8) t.pinned = (f[7] == "1");
        if (f.size() >= 6 && !f[5].empty()) {
            size_t start = 0;
            for (size_t i = 0; i <= f[5].size(); ++i) {
                if (i == f[5].size() || f[5][i] == '|') {
                    if (i > start) t.links.push_back(u8tow(f[5].substr(start, i - start)));
                    start = i + 1;
                }
            }
        }
        list.push_back(std::move(t));
    }

    // 置顶的先排最前面；同组内按到期时间，没截止的垫底，同时刻保持原来的先后
    std::stable_sort(list.begin(), list.end(), [](const Task& a, const Task& b) {
        if (a.pinned != b.pinned) return a.pinned;
        if (a.dueMs == 0 && b.dueMs == 0) return false;
        if (a.dueMs == 0) return false;
        if (b.dueMs == 0) return true;
        return a.dueMs < b.dueMs;
    });

    g_tasks = std::move(list);
    g_pending = pending;
    g_total = total;
    g_loaded = true;
    // 没有 "#待办 总数" 这一行说明后台回的是错误 JSON，别把它当成「全部完成了」
    g_loadError = !sawHeader;

    // 数据刚到：有没填预估的任务在跑就把呼吸接上，别等下一次刷新
    if (needBreath() && !g_progOn) kickProg();

    // 有任务在跑就得每秒重画一次，卡片里的「已进行」和窄条里的倒计时才走得动
    bool anyRunning = false;
    for (size_t i = 0; i < g_tasks.size(); ++i) {
        if (g_tasks[i].startedAt > 0) { anyRunning = true; break; }
    }
    static bool tickOn = false;
    if (anyRunning && !tickOn) {
        SetTimer(g_hwnd, TIMER_TICK, 1000, nullptr);
        tickOn = true;
    } else if (!anyRunning && tickOn) {
        KillTimer(g_hwnd, TIMER_TICK);
        tickOn = false;
    }

    int maxScroll = (int)g_tasks.size() - VISIBLE_ROWS;
    if (maxScroll < 0) maxScroll = 0;
    if (g_scroll > maxScroll) g_scroll = maxScroll;
    if (g_scroll < 0) g_scroll = 0;
    return true;
}

static void toggleTask(const wstring& id) {
    if (g_port == 0) return;
    wstring path = L"/api/toggle?id=" + id;
    string body;
    httpRequest(L"POST", path, body);
}

// ---- 窗口位置持久化 ----

static wstring iniPath() { return g_root + L"\\data\\widget.ini"; }

// 读窗口位置；没存过或者文件坏了返回 false
static bool loadSettings(POINT& pt) {
    pt.x = -1; pt.y = -1;
    FILE* f = _wfopen(iniPath().c_str(), L"rb");
    if (!f) return false;
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return false;
    int x = 0, y = 0;
    if (sscanf(buf, "x=%d\ny=%d", &x, &y) < 2) return false;
    pt.x = x; pt.y = y;
    return true;
}

static void saveSettings() {
    RECT rc{};
    GetWindowRect(g_hwnd, &rc);
    char buf[128];
    snprintf(buf, sizeof buf, "x=%ld\ny=%ld\n", (long)rc.left, (long)rc.top);
    FILE* f = _wfopen(iniPath().c_str(), L"wb");
    if (!f) return;
    fwrite(buf, 1, strlen(buf), f);
    fclose(f);
}

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
// outline 不透明时给文字描一圈边：进度色铺满整行时，底下那行小字全靠它才分得出来
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

    // 描边得走路径：先把文字转成图形，用笔画一圈再填字身。
    // 直接 DrawString 两遍做不出「外描边」，只会糊成一团。
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

// 紧急程度分两档，值大的更急，直接拿来取最大值
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
//   1. 红色任务里，先挑「已经在做」的那条（多条就比进度）；都没在做才按 ddl 最近
//   2. 一条红的都没有 → 说明用户已经自己取舍过了，显示进度最多的那条
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
        if (!g_justDone.empty() && t.id == g_justDone) continue;   // 刚勾掉的先别冒出来

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

        if (t.dueMs > 0) {                                        // 没截止时间的不算「快结束」
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

// ---- 绘制 ----

// GoGoSpot 标识：3/4 圆弧 + 圆点，比例取自 1024 画布。
void drawMark(Graphics& g, float cx, float cy, float box, Brush* brush) {
    const REAL S  = box;
    const REAL R  = 0.342f * S;     // 圆弧中心线半径
    const REAL W  = 0.090f * S;     // 线宽
    const REAL DR = 0.201f * S;     // 圆点半径
    const REAL BX = 0.182f * S;     // 横杠起点

    LinearGradientBrush grad(
        PointF(cx - S / 2, cy - S / 2), PointF(cx + S / 2, cy + S / 2),
        Color(255, 86, 118, 255), Color(255, 180, 118, 255));
    Brush* br = brush ? brush : &grad;

    Pen pen(br, W);
    pen.SetLineCap(LineCapRound, LineCapFlat, DashCapFlat);

    // 横杠 + 3/4 圆弧（缺口在右上）
    g.DrawLine(&pen, cx + BX, cy, cx + R, cy);
    g.DrawArc(&pen, cx - R, cy - R, R * 2, R * 2, 0.0f, 270.0f);

    g.FillEllipse(br, cx - DR, cy - DR, DR * 2, DR * 2);
}

static void progressOf(const Task& t, long long nowMs, long long& startMs, REAL& ratio);
static void capsuleProgress(Graphics& g, const RectF& r, REAL t, Pen& pen);
static Color fadeCol(const Color& c, REAL a);
static Color progressColorOf(const Task& t, REAL ratio);

// 收起态的窄条：vis 是这一帧露出的宽度，整条左移 (full - vis)，像从圆点背后抽出来。
// 两套外观，跟 Windows 的浅色/深色模式走：
//   日间——浅色磨砂底 + 暖色渐变，浅壁纸上不扎眼；
//   夜间——纯灰块 + 边缘外发光，颜色只留在那圈光里。
// 透明度都用 smoothstep，起步慢、中段陡、到满宽前就收平。
static void paintPill(Graphics& g, REAL vis, REAL full) {
    const Task* t = pillTask();
    if (!t || vis <= 0 || full <= 0) return;
    if (vis > full) vis = full;

    REAL prog = vis / full;
    REAL fade = prog * prog * (3.0f - 2.0f * prog);   // smoothstep
    REAL shadowFade = fade * fade * fade;
    auto A = [&](BYTE a) { return (BYTE)(a * fade); };
    auto S = [&](BYTE a) { return (BYTE)(a * shadowFade); };

    const Urgency u    = urgencyOf(*t);
    const Color   warm = urgencyColor(u);

    // 左端固定在圆点右侧，宽度从 0 长到 full——像从圆点背后抽出来，
    // 但不需要任何裁切，边缘永远是完整的圆角
    RectF r((REAL)PILL_X, (REAL)((DOT_SIZE - PILL_H) / 2), vis, (REAL)PILL_H);
    const REAL rad = (vis < PILL_H) ? vis / 2.0f : PILL_H / 2.0f;

    GraphicsPath p;
    roundRect(p, r, rad);

    if (g_dayMode) {
        // 投影先垫一层：浅底压在浅壁纸上，得靠它浮起来
        for (int i = 3; i >= 1; --i) {
            REAL grow = (REAL)i * 1.2f;
            GraphicsPath sp;
            roundRect(sp, RectF(r.X - grow, r.Y - grow + 1.5f,
                                r.Width + grow * 2, r.Height + grow * 2), rad + grow);
            SolidBrush sb(Color(S((BYTE)(8 + (3 - i) * 5)), 0, 0, 0));
            g.FillPath(&sb, &sp);
        }

        SolidBrush base(Color(A(232), 249, 249, 252));
        g.FillPath(&base, &p);

        BYTE gA = g_pillHover ? 76 : 58;
        LinearGradientBrush wb(PointF(r.X, r.Y), PointF(r.X + r.Width, r.Y),
                               Color(A(gA), warm.GetR(), warm.GetG(), warm.GetB()),
                               Color(A(12), warm.GetR(), warm.GetG(), warm.GetB()));
        g.FillPath(&wb, &p);

        Pen border(Color(A(g_pillHover ? 128 : 96), warm.GetR(), warm.GetG(), warm.GetB()), 1.0f);
        g.DrawPath(&border, &p);
    } else {
        // 外发光：九层往外扩的圆角矩形，越外越淡。深灰块靠这圈光浮起来，
        // 也顺带替掉了投影。
        // 裁掉药丸本体：发光垫在下面的话，半透明的时候会透上来，整条发褐。
        {
            REAL growMax = 9 * 1.3f;
            GraphicsPath outer;
            roundRect(outer, RectF(r.X - growMax, r.Y - growMax,
                                   r.Width + growMax * 2, r.Height + growMax * 2),
                      rad + growMax);
            Region clip(&outer);
            Region body(&p);
            clip.Exclude(&body);

            GraphicsState st = g.Save();
            g.SetClip(&clip, CombineModeReplace);

            REAL glowK = g_pillHover ? 1.25f : 1.0f;
            for (int i = 9; i >= 1; --i) {
                REAL grow = i * 1.3f;
                REAL k = i / 9.0f;
                BYTE a = A((BYTE)((52 * (1 - k) * (1 - k) + 4) * glowK));
                GraphicsPath gp;
                roundRect(gp, RectF(r.X - grow, r.Y - grow,
                                    r.Width + grow * 2, r.Height + grow * 2), rad + grow);
                SolidBrush sb(Color(a, warm.GetR(), warm.GetG(), warm.GetB()));
                g.FillPath(&sb, &gp);
            }
            g.Restore(st);
        }

        BYTE bg = g_pillHover ? 58 : 40;
        SolidBrush base(Color(A(255), bg, (BYTE)(bg + 2), (BYTE)(bg + 10)));
        g.FillPath(&base, &p);

        Pen hi(Color(A(30), 255, 255, 255), 1.0f);
        g.DrawPath(&hi, &p);
    }

    // 正在进行的任务：沿着胶囊内侧跑一圈进度
    if (t->startedAt > 0) {
        long long startMs = 0;
        REAL ratio = 0;
        progressOf(*t, nowMs(), startMs, ratio);

        RectF track(r.X + 3.5f, r.Y + 3.5f, r.Width - 7.0f, r.Height - 7.0f);
        if (track.Width > track.Height) {
            GraphicsPath tp;
            roundRect(tp, track, track.Height / 2.0f);
            Pen trackPen(Color(A(46), 255, 255, 255), 2.0f);
            g.DrawPath(&trackPen, &tp);

            REAL anim = (g_progAll || (!g_progId.empty() && t->id == g_progId)) ? g_progAnim : 1.0f;
            if (!hasEstimate(*t)) {
                // 没填预估：整圈呼吸，不表示具体比例。环形固定用紫色，
                // 不跟紧急程度走——那是行内填充和圆点外圈的事。
                BYTE a = (BYTE)(A((BYTE)(130 + 105 * breatheK())) * anim);
                Pen ringPen(Color(a, C_PURPLE.GetR(), C_PURPLE.GetG(), C_PURPLE.GetB()), 2.6f);
                ringPen.SetStartCap(LineCapRound);
                ringPen.SetEndCap(LineCapRound);
                g.DrawPath(&ringPen, &tp);
            } else {
                ratio *= anim;
                if (ratio > 0.002f) {
                    // 和列表里的进度条同一套：按占比分绿 / 黄 / 红
                    Color pc = progressColorOf(*t, ratio);
                    Pen ringPen(Color(A(255), pc.GetR(), pc.GetG(), pc.GetB()), 2.6f);
                    ringPen.SetStartCap(LineCapRound);
                    ringPen.SetEndCap(LineCapRound);
                    capsuleProgress(g, track, ratio, ringPen);
                }
            }
        }
    }

    // 左边那道竖条，和控制台列表里紧急任务的样式对齐
    GraphicsPath bar;
    roundRect(bar, RectF(r.X + 11, r.Y + 11, 2.5f, r.Height - 22), 1.25f);
    SolidBrush bb(Color(A(245), warm.GetR(), warm.GetG(), warm.GetB()));
    g.FillPath(&bb, &bar);

    // 右侧那一列：平时只有剩余时间；进行中改成上下两行——上面剩余时间，下面倒计时
    wstring due = dueText(*t);
    bool running = (t->startedAt > 0);
    long long runStart = 0;
    REAL runRatio = 0;
    wstring tail;                       // 第二行：倒计时，没预估就写「正在进行」
    if (running) {
        progressOf(*t, nowMs(), runStart, runRatio);
        tail = hasEstimate(*t)
             ? clockText((long long)t->estimate * 60000LL - (nowMs() - runStart))
             : wstring(L"正在进行");
    }

    REAL dueW = 0;
    auto needW = [&](const wstring& s) {
        if (s.empty()) return (REAL)0;
        RectF meas;
        g.MeasureString(s.c_str(), -1, smallFont(), PointF(0, 0), &meas);
        return meas.Width + 2;
    };
    dueW = needW(due);
    if (running) {
        REAL w2 = needW(tail);
        if (w2 > dueW) dueW = w2;
    }
    if (dueW > 64) dueW = 64;

    // 排版按满宽算，画的时候裁到当前宽度——文字是随着窄条长出来一点点露出来的，
    // 不会因为宽度不够被挤成省略号
    REAL left  = (REAL)PILL_X + 22;
    REAL right = (REAL)PILL_X + full - 15;
    REAL titleW = right - left - (dueW > 0 ? dueW + 8 : 0);

    GraphicsState textState = g.Save();
    g.SetClip(r, CombineModeIntersect);

    Color titleCol = g_dayMode ? Color(A(255), 34, 38, 46)
                               : Color(A(255), 248, 249, 252);
    drawText(g, t->title, titleFont(), RectF(left, r.Y, titleW, r.Height),
             titleCol, true, 0);

    if (dueW > 0) {
        Color dueCol;
        if (u == URG_RED) {
            dueCol = g_dayMode ? Color(A(255), 172, 52, 76) : Color(A(255), 255, 186, 198);
        } else if (u == URG_YELLOW) {
            dueCol = g_dayMode ? Color(A(255), 152, 96, 18) : Color(A(255), 255, 214, 150);
        } else {
            dueCol = g_dayMode ? Color(A(255), 104, 72, 166) : Color(A(255), 214, 198, 255);
        }

        if (running) {
            // 两行：上面剩余时间，下面倒计时。倒计时的颜色跟进度条同一档。
            // 40px 高的窄条塞两行 11px 小字，行心定在 12.5 / 27.5，
            // 上下各留 7px——再往上顶就贴边了
            drawText(g, due, smallFont(), RectF(right - dueW, r.Y + 3.5f, dueW, 18),
                     dueCol, true, 2);
            Color tailCol = hasEstimate(*t) ? progressColorOf(*t, runRatio) : C_PURPLE;
            drawText(g, tail, smallFont(), RectF(right - dueW, r.Y + 18.5f, dueW, 18),
                     tailCol, true, 2);
        } else {
            drawText(g, due, smallFont(), RectF(right - dueW, r.Y, dueW, r.Height),
                     dueCol, true, 2);
        }
    }

    g.Restore(textState);
}

// 圆点外圈：红色套红圈，黄色套黄圈；八层柔光 + 实环 + 白芯，画在最底层。
static void paintUrgencyRing(Graphics& g, REAL cx, REAL cy, Urgency u) {
    if (u == URG_NONE) return;
    const Color c = (u == URG_RED) ? C_RED : C_AMBER;

    REAL r = RING_R_IDLE + (RING_R - RING_R_IDLE) * g_ringT;

    // 八层柔光：越往外越淡，铺到环外 8px
    for (int i = 8; i >= 1; --i) {
        REAL t = i / 8.0f;
        REAL rr = r + i * 1.0f;
        BYTE a = (BYTE)(62 * (1 - t) * (1 - t) + 8);
        Pen glow(Color(a, c.GetR(), c.GetG(), c.GetB()), 4.0f);
        g.DrawEllipse(&glow, cx - rr, cy - rr, rr * 2, rr * 2);
    }

    Pen pen(Color(245, c.GetR(), c.GetG(), c.GetB()), 2.8f);
    g.DrawEllipse(&pen, cx - r, cy - r, r * 2, r * 2);

    // 白芯：压一道亮线，环才不像一块平涂的色带
    Pen core(Color(185, 255, 253, 254), 1.1f);
    g.DrawEllipse(&core, cx - r, cy - r, r * 2, r * 2);
}

// 收起状态：中间画标识，外圈柔光保留
static void paintDot(Graphics& g) {
    // 圆点按屏幕大小再放大一档，里面的窄条、圆环、字号跟着一起放大
    g.ScaleTransform(g_dotK, g_dotK);

    REAL cx = DOT_SIZE / 2.0f;
    REAL cy = cx;

    // 紧急圈先铺，窄条和圆点都压在它上面
    paintUrgencyRing(g, cx, cy, worstUrgency());

    // 圆点外那圈进度：内壁贴着圆点，外径比紧急光环小；窄条展开时淡出
    if (g_dotRingA > 0.02f) {
        const Task* pt = pillTask();
        if (pt && pt->startedAt > 0) {
            long long st = 0;
            REAL r2 = 0;
            progressOf(*pt, nowMs(), st, r2);
            REAL anim = (g_progAll || (!g_progId.empty() && pt->id == g_progId)) ? g_progAnim : 1.0f;
            const REAL rr = 12.2f;      // 内壁 9.6，外沿 14.8，都在光环里面

            if (!hasEstimate(*pt)) {
                // 没填预估：整圈呼吸，看不出比例，只表示还在做。环形固定紫色。
                BYTE a = (BYTE)(255 * g_dotRingA * (0.55f + 0.45f * breatheK()) * anim);
                Pen pen(Color(a, C_PURPLE.GetR(), C_PURPLE.GetG(), C_PURPLE.GetB()), 5.2f);
                g.DrawEllipse(&pen, cx - rr, cy - rr, rr * 2, rr * 2);
            } else {
                r2 *= anim;
                if (r2 > 0.002f) {
                    Color c2 = progressColorOf(*pt, r2);
                    Pen pen(Color((BYTE)(255 * g_dotRingA), c2.GetR(), c2.GetG(), c2.GetB()), 5.2f);
                    pen.SetStartCap(LineCapRound);
                    pen.SetEndCap(LineCapRound);
                    g.DrawArc(&pen, cx - rr, cy - rr, rr * 2, rr * 2, -90.0f, -r2 * 360.0f);
                }
            }
        }
    }

    if (g_pillW > 0) {
        // 窄条从圆点右边「长」出来，不再靠裁切藏左端。
        // 裁一刀的话，半透明底加外发光会沿着裁切线露一条硬边，圆点上下都能看见。
        paintPill(g, (REAL)g_pillW / dotUnit(), (REAL)g_pillFull / dotUnit());
    }

    for (int i = 3; i >= 1; --i) {
        REAL rr = 9.0f + i * 2.4f;
        SolidBrush b(Color((BYTE)(10 + (3 - i) * 7), 255, 255, 255));
        g.FillEllipse(&b, cx - rr, cy - rr, rr * 2, rr * 2);
    }

    // 深色外圈：浅色壁纸上靠它显形，负空间会透出这层底，所以压得比原来实。
    SolidBrush ring(Color(226, 12, 15, 22));
    g.FillEllipse(&ring, cx - 9.2f, cy - 9.2f, 18.4f, 18.4f);

    drawMark(g, cx, cy, 20.0f);
}

// ---------------------------------------------------------------- 就地添加面板

static REAL panelTop()      { return (REAL)(CARD_H - PANEL_H); }
static REAL panelInputTop() { return panelTop() + 8; }
static REAL panelDueTop()   { return panelTop() + 46; }
static REAL panelEstTop()   { return panelTop() + 76; }
static REAL panelLinkTop()  { return panelTop() + 106; }

// 输入框右端的 ×：点一下放弃这次添加
static RectF cancelRect() { return RectF((REAL)CARD_W - 36, panelInputTop() + 4, 22, 22); }

// 卡片坐标系（不含 SHADOW 边距）里第 row 排第 i 个选项的位置。
// 画和点击判定都用它，两边不会对不上。
static RectF chipRect(int row, int i, int count) {
    const REAL x0 = 44, x1 = (REAL)CARD_W - 10, gap = 6.0f;
    REAL w = (x1 - x0 - gap * (count - 1)) / count;
    REAL y = (row == 0) ? panelDueTop() : panelEstTop();
    return RectF(x0 + i * (w + gap), y, w, (REAL)CHIP_H);
}

static void paintChip(Graphics& g, const RectF& r, const wchar_t* label, bool on) {
    GraphicsPath p;
    roundRect(p, r, 12.0f);
    if (on) {
        SolidBrush b(Color(58, C_ACCENT.GetR(), C_ACCENT.GetG(), C_ACCENT.GetB()));
        g.FillPath(&b, &p);
        Pen pen(Color(150, C_ACCENT.GetR(), C_ACCENT.GetG(), C_ACCENT.GetB()), 1.0f);
        g.DrawPath(&pen, &p);
    } else {
        SolidBrush b(Color(26, 255, 255, 255));
        g.FillPath(&b, &p);
    }
    drawText(g, label, smallFont(), r,
             on ? Color(255, 236, 246, 255) : C_TEXT_DIM, true, 1);
}

// 编辑面板：底栏整块让出来，上面加一排截止、一排预估
static void paintEditPanel(Graphics& g) {
    const REAL top = (REAL)SHADOW + panelTop();

    // 压一层暗底，和上面的任务列表分开
    SolidBrush pb(Color(30, 0, 0, 0));
    g.FillRectangle(&pb, (REAL)SHADOW, top, (REAL)CARD_W, (REAL)PANEL_H);
        Pen sep(Color(18, 255, 255, 255), 1.0f);
    g.DrawLine(&sep, SHADOW + 14.0f, top + 0.5f, SHADOW + CARD_W - 14.0f, top + 0.5f);

    // 输入框
    RectF inR((REAL)SHADOW + 10, (REAL)SHADOW + panelInputTop(),
              (REAL)CARD_W - 20, 30);
    GraphicsPath ip;
    roundRect(ip, inR, 8.0f);
    SolidBrush ib(Color(34, 255, 255, 255));
    g.FillPath(&ib, &ip);
    Pen ibr(Color(96, C_ACCENT.GetR(), C_ACCENT.GetG(), C_ACCENT.GetB()), 1.0f);
    g.DrawPath(&ibr, &ip);

    const REAL tx = inR.X + 9;
    const REAL tw = inR.Width - 18 - 26;        // 右边留出 × 的位置

    // 光标位置：量出光标前那段的宽度
    if (g_caret < 0) g_caret = 0;
    if (g_caret > (int)g_editText.size()) g_caret = (int)g_editText.size();
    RectF prefixBox;
    if (g_caret > 0) {
        wstring pre = g_editText.substr(0, (size_t)g_caret);
        g.MeasureString(pre.c_str(), -1, bodyFont(), PointF(0, 0), &prefixBox);
    }
    REAL caretOff = prefixBox.Width;

    if (g_editText.empty()) {
        drawText(g, L"写点什么，回车添加", bodyFont(),
                 RectF(tx, inR.Y, tw, inR.Height), C_TEXT_FAINT, true, 0);
        if (g_caretOn) {
            SolidBrush cb(C_ACCENT);
            g.FillRectangle(&cb, tx, inR.Y + 7, 1.6f, inR.Height - 14);
        }
    } else {
        RectF meas;
        g.MeasureString(g_editText.c_str(), -1, bodyFont(), PointF(0, 0), &meas);

        // 超长时跟着光标滚：光标跑到框外就把它拉回可见范围
        REAL scroll = 0;
        if (meas.Width > tw - 2 && caretOff > tw - 4) scroll = caretOff - (tw - 4);

        GraphicsState st = g.Save();
        g.SetClip(inR, CombineModeIntersect);
        drawText(g, g_editText, bodyFont(),
                 RectF(tx - scroll, inR.Y, meas.Width + 4, inR.Height), C_TEXT, true, 0);
        g.Restore(st);

        if (g_caretOn) {
            REAL caretX = tx + caretOff - scroll;
            if (caretX < tx) caretX = tx;
            if (caretX > tx + tw) caretX = tx + tw;
            SolidBrush cb(C_ACCENT);
            g.FillRectangle(&cb, caretX, inR.Y + 7, 1.6f, inR.Height - 14);
        }
    }

    // 输入框右边的 ×，点一下放弃这次添加
    {
        RectF cr = cancelRect();
        cr.X += SHADOW;
        cr.Y += SHADOW;
        if (g_hoverCancel) {
            GraphicsPath hp;
            roundRect(hp, cr, 6.0f);
            SolidBrush hb(C_HOVER);
            g.FillPath(&hb, &hp);
        }
        Pen xp(g_hoverCancel ? C_TEXT : C_TEXT_FAINT, 1.5f);
        xp.SetStartCap(LineCapRound);
        xp.SetEndCap(LineCapRound);
        REAL cxc = cr.X + cr.Width / 2, cyc = cr.Y + cr.Height / 2;
        g.DrawLine(&xp, cxc - 4.5f, cyc - 4.5f, cxc + 4.5f, cyc + 4.5f);
        g.DrawLine(&xp, cxc + 4.5f, cyc - 4.5f, cxc - 4.5f, cyc + 4.5f);
    }

    // 截止
    drawText(g, L"截止", smallFont(),
             RectF((REAL)SHADOW + 10, (REAL)SHADOW + panelDueTop(), 30, (REAL)CHIP_H),
             C_TEXT_FAINT, true, 0);
    for (int i = 0; i < 5; ++i) {
        RectF r = chipRect(0, i, 5);
        r.X += SHADOW; r.Y += SHADOW;
        const wchar_t* label = DUE_LABEL[i];
        wchar_t picked[16];
        if (i == 3 && !g_customDue.empty()) {
            int y = 0, mo = 0, d = 0;
            if (swscanf(g_customDue.c_str(), L"%d-%d-%d", &y, &mo, &d) == 3) {
                swprintf(picked, 16, L"%d-%d", mo, d);
                label = picked;
            }
        }
        paintChip(g, r, label, g_editDue == i);
    }

    // 从这里开始：挂链接或本地文件
    drawText(g, L"从这里开始", smallFont(),
             RectF((REAL)SHADOW + 10, (REAL)SHADOW + panelLinkTop(), 62, (REAL)CHIP_H),
             C_TEXT_FAINT, true, 0);
    {
        RectF pick((REAL)SHADOW + 74, (REAL)SHADOW + panelLinkTop(), 52, (REAL)CHIP_H);
        RectF paste((REAL)SHADOW + 128, (REAL)SHADOW + panelLinkTop(), 40, (REAL)CHIP_H);
        paintChip(g, pick, L"选文件", false);
        paintChip(g, paste, L"粘贴", false);

        if (!g_editLinks.empty()) {
            wchar_t buf[24];
            swprintf(buf, 24, L"已加 %d 项", (int)g_editLinks.size());
            drawText(g, buf, smallFont(),
                     RectF((REAL)SHADOW + 172, (REAL)SHADOW + panelLinkTop(), 70, (REAL)CHIP_H),
                     C_TEXT_DIM, true, 0);

            Pen xp(C_TEXT_FAINT, 1.4f);
            xp.SetStartCap(LineCapRound);
            xp.SetEndCap(LineCapRound);
            REAL xx = SHADOW + 252, xy = SHADOW + panelLinkTop() + CHIP_H / 2.0f;
            g.DrawLine(&xp, xx - 4, xy - 4, xx + 4, xy + 4);
            g.DrawLine(&xp, xx + 4, xy - 4, xx - 4, xy + 4);
        }
    }

    // 预估
    drawText(g, L"预估", smallFont(),
             RectF((REAL)SHADOW + 10, (REAL)SHADOW + panelEstTop(), 30, (REAL)CHIP_H),
             C_TEXT_FAINT, true, 0);
    for (int i = 0; i < 6; ++i) {
        RectF r = chipRect(1, i, 6);
        r.X += SHADOW; r.Y += SHADOW;
        paintChip(g, r, EST_LABEL[i], g_editEst == i);
    }
}

// ---------------------------------------------------------------- 自选日期

// 这个月 1 号是周几，0 = 周一
static int firstWeekday(int y, int m) {
    SYSTEMTIME st{};
    st.wYear = (WORD)y;
    st.wMonth = (WORD)m;
    st.wDay = 1;
    FILETIME ft{};
    SystemTimeToFileTime(&st, &ft);
    FileTimeToSystemTime(&ft, &st);
    return (st.wDayOfWeek + 6) % 7;
}

static RectF calPrev()    { return RectF(14, 52, 26, 26); }
static RectF calNext()    { return RectF((REAL)CARD_W - 40, 52, 26, 26); }
static RectF calCell(int i) {
    return RectF(20 + (i % 7) * 40, 108 + (i / 7) * 30, 40, 30);
}
static RectF calHourDec() { return RectF(56, 296, 22, 26); }
static RectF calHourInc() { return RectF(116, 296, 22, 26); }
static RectF calMinDec()  { return RectF(150, 296, 22, 26); }
static RectF calMinInc()  { return RectF(210, 296, 22, 26); }
static RectF calCancel()  { return RectF(56, 352, 92, 32); }
static RectF calOk()      { return RectF(172, 352, 92, 32); }

static void paintMini(Graphics& g, const RectF& r, const wchar_t* text, bool primary) {
    GraphicsPath p;
    roundRect(p, r, 8.0f);
    if (primary) {
        SolidBrush b(Color(238, C_ACCENT.GetR(), C_ACCENT.GetG(), C_ACCENT.GetB()));
        g.FillPath(&b, &p);
    } else {
        SolidBrush b(Color(26, 255, 255, 255));
        g.FillPath(&b, &p);
    }
    drawText(g, text, smallFont(), r, primary ? Color(255, 255, 255, 255) : C_TEXT_DIM, true, 1);
}

// 日历面板：盖住标题以下的整块，用来挑任意日期和时间
static void paintCalendar(Graphics& g) {
    SolidBrush bg(Color(244, 16, 19, 25));
    g.FillRectangle(&bg, (REAL)SHADOW, (REAL)(SHADOW + TITLE_H),
                    (REAL)CARD_W, (REAL)(CARD_H - TITLE_H));

    wchar_t buf[48];
    swprintf(buf, 48, L"%d 年 %d 月", g_pickYear, g_pickMonth);
    drawText(g, buf, bodyFont(),
             RectF((REAL)SHADOW + 40, (REAL)SHADOW + 52, (REAL)CARD_W - 80, 26),
             C_TEXT, true, 1);

    {
        Pen pen(C_TEXT_DIM, 1.6f);
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        RectF l = calPrev(), r = calNext();
        REAL lx = SHADOW + l.X + l.Width / 2, ly = SHADOW + l.Y + l.Height / 2;
        g.DrawLine(&pen, lx + 3, ly - 5, lx - 3, ly);
        g.DrawLine(&pen, lx - 3, ly, lx + 3, ly + 5);
        REAL rx = SHADOW + r.X + r.Width / 2, ry = SHADOW + r.Y + r.Height / 2;
        g.DrawLine(&pen, rx - 3, ry - 5, rx + 3, ry);
        g.DrawLine(&pen, rx + 3, ry, rx - 3, ry + 5);
    }

    {
        const wchar_t* wd[7] = { L"一", L"二", L"三", L"四", L"五", L"六", L"日" };
        for (int c = 0; c < 7; ++c) {
            drawText(g, wd[c], smallFont(),
                     RectF((REAL)SHADOW + 20 + c * 40, (REAL)SHADOW + 84, 40, 18),
                     C_TEXT_FAINT, true, 1);
        }
    }

    {
        int first = firstWeekday(g_pickYear, g_pickMonth);
        int dim = daysOfMonth(g_pickYear, g_pickMonth);
        for (int i = 0; i < 42; ++i) {
            int d = i - first + 1;
            if (d < 1 || d > dim) continue;
            RectF cell = calCell(i);
            cell.X += SHADOW;
            cell.Y += SHADOW;
            if (d == g_pickDay) {
                GraphicsPath p;
                roundRect(p, RectF(cell.X + 5, cell.Y + 2, cell.Width - 10, cell.Height - 4), 7.0f);
                SolidBrush b(Color(215, C_ACCENT.GetR(), C_ACCENT.GetG(), C_ACCENT.GetB()));
                g.FillPath(&b, &p);
            }
            wchar_t db[8];
            swprintf(db, 8, L"%d", d);
            drawText(g, db, smallFont(), cell,
                     d == g_pickDay ? Color(255, 255, 255, 255) : C_TEXT_DIM, true, 1);
        }
    }

    {
        drawText(g, L"时间", smallFont(),
                 RectF((REAL)SHADOW + 20, (REAL)SHADOW + 296, 34, 26), C_TEXT_FAINT, true, 0);

        RectF hd = calHourDec(), hi = calHourInc(), md = calMinDec(), mi = calMinInc();
        paintMini(g, RectF(SHADOW + hd.X, SHADOW + hd.Y, hd.Width, hd.Height), L"−", false);
        paintMini(g, RectF(SHADOW + hi.X, SHADOW + hi.Y, hi.Width, hi.Height), L"+", false);
        paintMini(g, RectF(SHADOW + md.X, SHADOW + md.Y, md.Width, md.Height), L"−", false);
        paintMini(g, RectF(SHADOW + mi.X, SHADOW + mi.Y, mi.Width, mi.Height), L"+", false);

        wchar_t tb[8];
        swprintf(tb, 8, L"%02d", g_pickHour);
        drawText(g, tb, bodyFont(),
                 RectF((REAL)SHADOW + 80, (REAL)SHADOW + 296, 34, 26), C_TEXT, true, 1);
        drawText(g, L":", bodyFont(),
                 RectF((REAL)SHADOW + 140, (REAL)SHADOW + 296, 14, 26), C_TEXT_DIM, true, 1);
        swprintf(tb, 8, L"%02d", g_pickMin);
        drawText(g, tb, bodyFont(),
                 RectF((REAL)SHADOW + 174, (REAL)SHADOW + 296, 34, 26), C_TEXT, true, 1);
    }

    {
        RectF c = calCancel(), o = calOk();
        paintMini(g, RectF(SHADOW + c.X, SHADOW + c.Y, c.Width, c.Height), L"取消", false);
        paintMini(g, RectF(SHADOW + o.X, SHADOW + o.Y, o.Width, o.Height), L"确定", true);
    }
}

// 正在进行：起始时间和用时占比
//   预计最晚开始 = 截止 - 预计用时
//   起始 = min(点下去的时刻, 预计最晚开始)
//   占比 = (现在 - 起始) / 预计用时
// 没填预估的算不出占比，这里直接给 0，界面改用呼吸 / 固定填充表示。
static void progressOf(const Task& t, long long nowMs, long long& startMs, REAL& ratio) {
    if (!hasEstimate(t)) {
        startMs = t.startedAt;
        ratio = 0;
        return;
    }

    long long estMs = (long long)t.estimate * 60000LL;
    long long latest = (t.dueMs > 0) ? (t.dueMs - estMs) : t.startedAt;
    startMs = (t.startedAt < latest) ? t.startedAt : latest;

    long long elapsed = nowMs - startMs;
    if (elapsed < 0) elapsed = 0;
    ratio = (REAL)elapsed / (REAL)estMs;
    if (ratio > 1.0f) ratio = 1.0f;
}

// 胶囊形的「跑圈」：沿着轮廓顺时针走 t 圈，走多少画多少
static void capsuleProgress(Graphics& g, const RectF& r, REAL t, Pen& pen) {
    const REAL rad = r.Height / 2.0f;
    REAL straight = r.Width - r.Height;
    if (straight < 0) straight = 0;

    const REAL arc = 3.14159265f * rad;
    REAL left = t * (2.0f * straight + 2.0f * arc);
    if (left <= 0) return;

    // 上边：左 → 右
    if (left > 0 && straight > 0) {
        REAL use = (left < straight) ? left : straight;
        g.DrawLine(&pen, r.X + rad, r.Y, r.X + rad + use, r.Y);
        left -= use;
    }
    // 右半圆：-90° 往 +180° 走
    if (left > 0) {
        REAL use = (left < arc) ? left : arc;
        g.DrawArc(&pen, r.X + r.Width - 2 * rad, r.Y, 2 * rad, 2 * rad,
                  -90.0f, use / arc * 180.0f);
        left -= use;
    }
    // 下边：右 → 左
    if (left > 0 && straight > 0) {
        REAL use = (left < straight) ? left : straight;
        REAL x1 = r.X + r.Width - rad;
        g.DrawLine(&pen, x1 - use, r.Y + r.Height, x1, r.Y + r.Height);
        left -= use;
    }
    // 左半圆：90° 往 +180° 走
    if (left > 0) {
        REAL use = (left < arc) ? left : arc;
        g.DrawArc(&pen, r.X, r.Y, 2 * rad, 2 * rad, 90.0f, use / arc * 180.0f);
    }
}

// 进度条的颜色：<=60% 绿，<=80% 黄，再往上红。
// 平时就是这三个纯色，只在跨过 60% / 80% 那两个点时放一段过渡动画，
// 不是跟着进度一直渐变。
static Color tierColor(int tier) {
    return (tier == 0) ? C_GREEN : (tier == 1 ? C_AMBER : C_RED);
}

static Color lerpColor(Color a, Color b, REAL k) {
    if (k < 0) k = 0;
    if (k > 1) k = 1;
    auto mix = [&](BYTE x, BYTE y) { return (BYTE)(x + (y - x) * k); };
    return Color(255, mix(a.GetR(), b.GetR()), mix(a.GetG(), b.GetG()), mix(a.GetB(), b.GetB()));
}

// 每条任务记一下上次的档位和切档时刻，切档时才知道从哪个颜色过渡过来
struct TierState { wchar_t id[40] = {0}; int tier = 0; int from = 0; DWORD at = 0; };
static TierState g_tiers[12];

static Color progressColorOf(const Task& t, REAL ratio) {
    int tier = (ratio <= 0.6f) ? 0 : (ratio <= 0.8f ? 1 : 2);

    TierState* slot = nullptr;
    for (auto& st : g_tiers) {
        if (st.id[0] && t.id == st.id) { slot = &st; break; }
        if (!st.id[0] && !slot) slot = &st;
    }
    if (!slot) return tierColor(tier);

    if (!slot->id[0]) {                       // 第一次见到这条
        wcsncpy(slot->id, t.id.c_str(), 39);
        slot->id[39] = 0;
        slot->tier = tier;
        slot->from = tier;
        slot->at = 0;
        return tierColor(tier);
    }

    if (slot->tier != tier) {                 // 刚跨过阈值
        slot->from = slot->tier;
        slot->tier = tier;
        slot->at = GetTickCount();
        kickProg();
    }

    if (slot->at) {
        REAL k = (REAL)(GetTickCount() - slot->at) / 360.0f;
        if (k < 1.0f) return lerpColor(tierColor(slot->from), tierColor(slot->tier), k);
    }
    return tierColor(tier);
}

// 整行染色：底色随进度从左往右铺开，进行中时紧急底色让位。
// 试过十几种画法，最后定的是这一种，其余样式连同预览脚本都已经删掉。
static void paintRowBar(Graphics& g, const RectF& rowR, REAL ratio, Color pc, REAL alpha) {
    if (ratio <= 0.002f) ratio = 0.002f;

    GraphicsPath hp;
    roundRect(hp, rowR, 10.0f);
    SolidBrush base(fadeCol(Color(10, pc.GetR(), pc.GetG(), pc.GetB()), alpha));
    g.FillPath(&base, &hp);

    REAL w = rowR.Width * ratio;
    if (w < 6) w = 6;

    // 填充块别自己再算一次圆角：宽度小于圆角直径时那四段圆弧会互相穿过去，
    // 刚点下「正在进行」的那一下会糊出一坨往外鼓的胶囊。
    // 改成按行本身的圆角裁一刀——右端是齐口，左端跟着行的圆角走，任何宽度都正常。
    GraphicsState st = g.Save();
    g.SetClip(&hp);
    LinearGradientBrush lg(PointF(rowR.X, rowR.Y), PointF(rowR.X + w, rowR.Y),
                           fadeCol(Color(132, pc.GetR(), pc.GetG(), pc.GetB()), alpha),
                           fadeCol(Color(20, pc.GetR(), pc.GetG(), pc.GetB()), alpha));
    g.FillRectangle(&lg, RectF(rowR.X, rowR.Y, w, rowR.Height));
    g.Restore(st);
}

// 图钉：标题栏那个「置顶悬浮清单」按钮用。上窄下宽的钉头加一根针。
// 11px 画不出原图那圈圆角，远看是同一个形状就够了。
static void paintPin(Graphics& g, REAL cx, REAL cy, REAL k, Color col) {
    PointF head[4] = {
        { cx - 1.9f * k, cy - 4.8f * k },
        { cx + 1.9f * k, cy - 4.8f * k },
        { cx + 3.5f * k, cy - 1.0f * k },
        { cx - 3.5f * k, cy - 1.0f * k },
    };
    GraphicsPath p;
    p.AddPolygon(head, 4);
    roundRect(p, RectF(cx - 2.5f * k, cy - 6.0f * k, 5.0f * k, 1.5f * k), 0.6f * k);
    SolidBrush b(col);
    g.FillPath(&b, &p);

    Pen needle(col, 1.3f * k);
    needle.SetStartCap(LineCapRound);
    needle.SetEndCap(LineCapRound);
    g.DrawLine(&needle, cx, cy - 1.0f * k, cx, cy + 3.6f * k);
}

// 上载标记：一道顶边加一支向上的箭头。任务「浮到最前面」用它，
// 和控制台 .task-pin 里那个 SVG 是同一个形状。
static void paintRise(Graphics& g, REAL cx, REAL cy, REAL k, Color col) {
    Pen pen(col, 1.4f * k);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawLine(&pen, cx - 4.0f * k, cy - 4.6f * k, cx + 4.0f * k, cy - 4.6f * k);
    g.DrawLine(&pen, cx, cy + 4.4f * k, cx, cy - 1.6f * k);
    g.DrawLine(&pen, cx - 2.6f * k, cy + 1.0f * k, cx, cy - 1.6f * k);
    g.DrawLine(&pen, cx + 2.6f * k, cy + 1.0f * k, cx, cy - 1.6f * k);
}

// 画一行任务。dx 是横向偏移（勾掉后往右滑用），alpha 是整体透明度。
static Color fadeCol(const Color& c, REAL a) {
    REAL v = (REAL)c.GetA() * a;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return Color((BYTE)v, c.GetR(), c.GetG(), c.GetB());
}

static void paintRow(Graphics& g, const Task& t, int rowIdx, REAL rowY, REAL dx,
                     REAL alpha, bool checked) {
    RectF rowR((REAL)SHADOW + 6 + dx, rowY, (REAL)CARD_W - 12, (REAL)ROW_H);

    Urgency u = checked ? URG_NONE : urgencyOf(t);
    Color uc = (u == URG_RED) ? C_RED : C_AMBER;

    // 进行中：进度色铺在最底层，文字、勾选框、暖色竖条、按钮都压在它上面

    if (t.startedAt > 0) {
        long long startMs = 0;
        REAL ratio = 0;
        progressOf(t, nowMs(), startMs, ratio);

        REAL anim = (g_progAll || (!g_progId.empty() && t.id == g_progId)) ? g_progAnim : 1.0f;
        if (!hasEstimate(t)) {
            // 没填预估：固定八成填充，颜色跟紧急程度走，不跟着时间变
            paintRowBar(g, rowR, 0.8f * anim, urgencyColor(u), alpha);
        } else {
            paintRowBar(g, rowR, ratio * anim, progressColorOf(t, ratio), alpha);
        }
    }


    // 临近截止：整行暖色 + 左侧竖条，红黄同形状不同色。
    // 进行中的行底色让给进度，免得两层叠在一起发浑
    if (u != URG_NONE) {
        if (t.startedAt <= 0) {
            GraphicsPath hp;
            roundRect(hp, rowR, 10.0f);
            LinearGradientBrush lg(PointF(rowR.X, rowR.Y), PointF(rowR.X + rowR.Width, rowR.Y),
                                   fadeCol(Color(u == URG_RED ? 46 : 38, uc.GetR(), uc.GetG(), uc.GetB()), alpha),
                                   fadeCol(Color(8, uc.GetR(), uc.GetG(), uc.GetB()), alpha));
            g.FillPath(&lg, &hp);
        }
        SolidBrush bar(fadeCol(Color(235, uc.GetR(), uc.GetG(), uc.GetB()), alpha));
        g.FillRectangle(&bar, rowR.X + 3, rowY + 13, 2.5f, (REAL)ROW_H - 26);
    } else if (rowIdx >= 0 && g_hoverRow == rowIdx && !g_dragging) {
        GraphicsPath hp;
        roundRect(hp, rowR, 10.0f);
        SolidBrush hb(fadeCol(C_HOVER, alpha));
        g.FillPath(&hb, &hp);
    }

    RectF cb(rowR.X + 10, rowY + (ROW_H - 18) / 2.0f, 18, 18);
    if (checked) {
        GraphicsPath p;
        roundRect(p, cb, 5.5f);
        SolidBrush b(fadeCol(Color(255, 108, 140, 255), alpha));
        g.FillPath(&b, &p);
        Pen check(fadeCol(Color(255, 255, 255, 255), alpha), 1.9f);
        check.SetStartCap(LineCapRound);
        check.SetEndCap(LineCapRound);
        g.DrawLine(&check, cb.X + 4.5f, cb.Y + 9.2f, cb.X + 7.6f, cb.Y + 12.4f);
        g.DrawLine(&check, cb.X + 7.6f, cb.Y + 12.4f, cb.X + 13.2f, cb.Y + 5.8f);
    } else {
        GraphicsPath p;
        roundRect(p, cb, 5.5f);
        Pen border(u != URG_NONE ? fadeCol(Color(150, uc.GetR(), uc.GetG(), uc.GetB()), alpha)
                                 : fadeCol(Color(70, 255, 255, 255), alpha), 1.5f);
        g.DrawPath(&border, &p);
    }

    // 标题（一行）+ 时间/时长（第二行）
    REAL titleX = rowR.X + 38;
    REAL titleW = rowR.Width - 38 - 10 - 30 - 26;   // 右边留给「置顶」「正在进行」

    // 挂了材料的行，标题用强调色：一眼能看出点它能跳过去
    Color titleCol = checked ? C_TEXT_FAINT : (t.links.empty() ? C_TEXT : C_ACCENT);
    drawText(g, t.title, bodyFont(), RectF(titleX, rowY + 7, titleW, 21),
             fadeCol(titleCol, alpha), true, 0);

    if (checked) {
        RectF meas;
        g.MeasureString(t.title.c_str(), -1, bodyFont(), PointF(0, 0), &meas);
        REAL lineW = (meas.Width < titleW - 4) ? meas.Width : titleW - 8;
        Pen strike(fadeCol(C_TEXT_FAINT, alpha), 1.0f);
        g.DrawLine(&strike, titleX, rowY + 17.5f, titleX + lineW, rowY + 17.5f);
    }

        // 右边按钮：没进行 → 悬停才显示 ▶；进行中 → 常显 ■（停止进行）
    {
        REAL pcx = rowR.X + rowR.Width - 19;
        REAL pcy = rowY + ROW_H / 2.0f;
        const REAL pr = 9.0f;
        bool running = (t.startedAt > 0);

        // 左边那个：置顶开关。置顶的常显，其余悬停才出来
        REAL qcx = rowR.X + rowR.Width - 45;
        if (t.pinned || (rowIdx >= 0 && g_hoverRow == rowIdx && !g_dragging)) {
            Color bc = t.pinned ? C_ACCENT : C_TEXT_FAINT;
            if (t.pinned) {
                SolidBrush bg(fadeCol(Color(56, bc.GetR(), bc.GetG(), bc.GetB()), alpha));
                g.FillEllipse(&bg, qcx - pr, pcy - pr, pr * 2, pr * 2);
            }
            Pen ring(fadeCol(Color(t.pinned ? 190 : 110, bc.GetR(), bc.GetG(), bc.GetB()), alpha), 1.0f);
            g.DrawEllipse(&ring, qcx - pr, pcy - pr, pr * 2, pr * 2);
            paintRise(g, qcx, pcy, 0.85f, fadeCol(bc, alpha));
        }

        if (running || (rowIdx >= 0 && g_hoverRow == rowIdx && !g_dragging)) {
            Color bc = urgencyColor(u);
            SolidBrush bgc(fadeCol(Color(running ? 72 : 56, bc.GetR(), bc.GetG(), bc.GetB()), alpha));
            g.FillEllipse(&bgc, pcx - pr, pcy - pr, pr * 2, pr * 2);
            Pen ring(fadeCol(Color(running ? 190 : 150, bc.GetR(), bc.GetG(), bc.GetB()), alpha), 1.0f);
            g.DrawEllipse(&ring, pcx - pr, pcy - pr, pr * 2, pr * 2);

            if (running) {
                SolidBrush sb(fadeCol(bc, alpha));
                g.FillRectangle(&sb, pcx - 3.2f, pcy - 3.2f, 6.4f, 6.4f);
            } else {
                PointF tri[3] = { { pcx - 2.0f, pcy - 4.2f }, { pcx + 4.2f, pcy }, { pcx - 2.0f, pcy + 4.2f } };
                SolidBrush tb(fadeCol(bc, alpha));
                g.FillPolygon(&tb, tri, 3);
            }
        }
    }

    // 第二行：有截止就写截止，有预估就写预估，两样都没有就整行不画。
    // 原来两段都用 -- 占位，想让中间那个 · 对齐，可整行是一串文本左对齐拼出来的，
    // 占位并没对齐任何东西，只剩一排「-- · --」占着地方。
    wstring meta = dueText(t);
    wstring dur = shortDuration(t.estimate);
    if (!meta.empty() && !dur.empty()) meta += L"  ·  " + dur;
    else if (meta.empty()) meta = dur;

    // 进行中再补一段：有预估就数「已进行」多久，没预估只说「正在进行」
    if (t.startedAt > 0) {
        long long st = 0;
        REAL r2 = 0;
        progressOf(t, nowMs(), st, r2);
        if (!meta.empty()) meta += L"  ·  ";
        meta += hasEstimate(t) ? (L"已进行 " + clockText(nowMs() - st)) : wstring(L"正在进行");
    }
    if (!meta.empty()) {
        Color mc = (u == URG_NONE) ? C_TEXT_FAINT
                 : (u == URG_RED ? Color(255, 255, 150, 170) : Color(255, 250, 205, 146));
        // 进行中时整行被进度色盖住，这行小字加一圈白边才看得清
        Color edge(t.startedAt > 0 ? (BYTE)(80 * alpha) : (BYTE)0, 255, 255, 255);
        drawText(g, meta, smallFont(), RectF(titleX, rowY + 29, titleW, 16),
                 fadeCol(mc, alpha), true, 0, edge);
    }
}

static void paint(HDC dc) {
    Graphics g(dc);
    g.ScaleTransform(g_scale, g_scale);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.Clear(Color(0, 0, 0, 0));

    if (!g_expanded) {
        paintDot(g);
        return;
    }

    RectF card((REAL)SHADOW, (REAL)SHADOW, (REAL)CARD_W, (REAL)CARD_H);
    RectF fillR(card.X - 0.5f, card.Y - 0.5f, card.Width + 1.0f, card.Height + 1.0f);

    // 投影：由外向内叠几层低透明度圆角矩形，成本低但足够柔和
    for (int i = 7; i >= 1; --i) {
        REAL grow = (REAL)i * 1.7f;
        int alpha = 4 + (7 - i) * 6;
        RectF r(card.X - grow, card.Y - grow + 2.0f,
                card.Width + grow * 2, card.Height + grow * 2);
        GraphicsPath p;
        roundRect(p, r, RADIUS + grow);
        SolidBrush b(Color((BYTE)alpha, 0, 0, 0));
        g.FillPath(&b, &p);
    }

    // 展开态
    {
        GraphicsPath p;
        roundRect(p, fillR, RADIUS);

        // 毛玻璃：抓屏模糊图 + 半透明深色罩，只靠半透明出不来磨砂感。
        // 位图是设备像素，绘制坐标已经按 g_scale 缩放过，尺寸得除回去才对得上 1:1
        if (g_bgReady && g_bgBits) {
            Bitmap glass(g_surfW, g_surfH, g_surfW * 4, PixelFormat32bppARGB, g_bgBits);
            g.SetClip(&p);
            g.DrawImage(&glass, (REAL)0, (REAL)0,
                        (REAL)g_surfW / g_scale, (REAL)g_surfH / g_scale);
            g.ResetClip();
            SolidBrush tint(C_GLASS);
            g.FillPath(&tint, &p);
        } else {
            SolidBrush bg(C_CARD);
            g.FillPath(&bg, &p);
        }

        // 描边内缩半像素，否则一半笔画落在面板外。
        GraphicsPath bp;
        roundRect(bp, RectF(card.X + 0.5f, card.Y + 0.5f,
                            card.Width - 1.0f, card.Height - 1.0f), RADIUS - 0.5f);
        Pen border(C_BORDER, 1.0f);
        g.DrawPath(&border, &bp);
    }

    // 标题栏
    {
        // 图标：品牌方块 + 反白标识，和控制台顶栏一致
        RectF iconR((REAL)(SHADOW + 15), (REAL)(SHADOW + 13), 20.0f, 20.0f);
        GraphicsPath p;
        roundRect(p, iconR, 6.0f);
        LinearGradientBrush bg(
            PointF(iconR.X, iconR.Y),
            PointF(iconR.X + iconR.Width, iconR.Y + iconR.Height),
            Color(255, 78, 107, 255), Color(255, 160, 107, 255));
        g.FillPath(&bg, &p);

        SolidBrush white(Color(255, 255, 255, 255));
        drawMark(g, iconR.X + iconR.Width / 2, iconR.Y + iconR.Height / 2, 17.0f, &white);

        drawText(g, L"待办", titleFont(),
                 RectF((REAL)(SHADOW + 42), (REAL)(SHADOW + 12), 70, 22), C_TEXT, true, 0);

        wchar_t cnt[16];
        swprintf(cnt, 16, L"%d", g_pending);
        drawText(g, cnt, smallFont(),
                 RectF((REAL)(SHADOW + 76), (REAL)(SHADOW + 13), 40, 20), C_TEXT_FAINT, true, 0);

        {
            REAL bx = (REAL)(SHADOW + CARD_W - 34);
            RectF br(bx, (REAL)(SHADOW + 13), 22, 22);
            if (g_hoverBtn == 1) {
                GraphicsPath hp;
                roundRect(hp, br, 7.0f);
                SolidBrush hb(C_HOVER);
                g.FillPath(&hb, &hp);
            }
            Pen pen(C_TEXT_FAINT, 1.5f);
            pen.SetStartCap(LineCapRound);
            pen.SetEndCap(LineCapRound);
            g.DrawLine(&pen, br.X + 7, br.Y + 7, br.X + 15, br.Y + 15);
            g.DrawLine(&pen, br.X + 15, br.Y + 7, br.X + 7, br.Y + 15);
        }

        // 置顶开关：图钉。开着的时候点亮，和控制台那个开关同一个值
        {
            RectF br((REAL)(SHADOW + CARD_W - 60), (REAL)(SHADOW + 13), 22, 22);
            if (g_hoverBtn == 4) {
                GraphicsPath hp;
                roundRect(hp, br, 7.0f);
                SolidBrush hb(C_HOVER);
                g.FillPath(&hb, &hp);
            }
            paintPin(g, br.X + 11.0f, br.Y + 12.3f, 1.15f,
                     g_topmost ? C_ACCENT : C_TEXT_FAINT);
        }
    }

    {
        Pen sep(Color(18, 255, 255, 255), 1.0f);
        g.DrawLine(&sep, SHADOW + 14.0f, SHADOW + (REAL)TITLE_H - 0.5f,
                   SHADOW + CARD_W - 14.0f, SHADOW + (REAL)TITLE_H - 0.5f);
    }

    // 挑日期时整块让给日历
    if (g_pickingDate) {
        paintCalendar(g);
        return;
    }

    // 任务列表
    int maxScroll = (int)g_tasks.size() - VISIBLE_ROWS;
    if (maxScroll < 0) maxScroll = 0;
    if (g_scroll > maxScroll) g_scroll = maxScroll;

    if (!g_loaded) {
        drawText(g, L"等待后台…", smallFont(),
                 RectF((REAL)SHADOW, (REAL)(SHADOW + LIST_TOP), (REAL)CARD_W, (REAL)ROW_H),
                 C_TEXT_FAINT, true, 1);
    } else if (g_loadError) {
        // 和空列表分开：数据坏了要让用户知道去恢复备份，不能显示成「全部完成了」
        REAL midY = (REAL)(SHADOW + LIST_TOP + (LIST_BOTTOM - LIST_TOP) / 2 - 30);
        drawText(g, L"任务数据读不出来", bodyFont(),
                 RectF((REAL)SHADOW, midY, (REAL)CARD_W, 26), C_RED, true, 1);
        drawText(g, L"打开控制台，从历史备份恢复", smallFont(),
                 RectF((REAL)SHADOW, midY + 26, (REAL)CARD_W, 22), C_TEXT_FAINT, true, 1);
    } else if (g_tasks.empty()) {
        REAL midY = (REAL)(SHADOW + LIST_TOP + (LIST_BOTTOM - LIST_TOP) / 2 - 30);
        drawText(g, L"全部完成了", bodyFont(),
                 RectF((REAL)SHADOW, midY, (REAL)CARD_W, 26), C_TEXT_DIM, true, 1);
        drawText(g, L"点底栏的「添加任务」记一条", smallFont(),
                 RectF((REAL)SHADOW, midY + 26, (REAL)CARD_W, 22), C_TEXT_FAINT, true, 1);
    }

    // 行会上滑，超出列表区就裁掉；编辑时列表只画到面板上沿
    REAL listBottom = g_editing ? (REAL)(SHADOW + CARD_H - PANEL_H)
                                : (REAL)(SHADOW + LIST_BOTTOM);
    GraphicsState listState = g.Save();
    g.SetClip(RectF((REAL)SHADOW, (REAL)(SHADOW + LIST_TOP),
                    (REAL)CARD_W, listBottom - (SHADOW + LIST_TOP)), CombineModeIntersect);

    // 勾掉一条之后：那条往右滑出去淡掉，下面的行再一条条跟上来
    DWORD leaveElapsed = g_leaving ? (GetTickCount() - g_leaveStart) : 0;

    for (int i = 0; i < VISIBLE_ROWS; ++i) {
        int idx = g_scroll + i;
        if (idx >= (int)g_tasks.size()) break;
        const Task& t = g_tasks[idx];

        REAL rowY = (REAL)(SHADOW + LIST_TOP + i * ROW_H);
        if (g_leaving && idx >= g_leaveRow) {
            // 每往下一条晚 30ms 起步，看着像一个个跟上来
            REAL delay = (idx - g_leaveRow) * 30.0f;
            REAL t2 = ((REAL)leaveElapsed - 170.0f - delay) / 180.0f;
            if (t2 < 0.0f) t2 = 0.0f;
            if (t2 > 1.0f) t2 = 1.0f;
            t2 = t2 * t2 * (3.0f - 2.0f * t2);
            rowY += (REAL)ROW_H * (1.0f - t2);
        }

        paintRow(g, t, i, rowY, 0.0f, 1.0f,
                 !g_justDone.empty() && t.id == g_justDone);
    }

    // 刚勾掉的那条：往右滑 70px 同时淡出，滑完就看不见了
    if (g_leaving && g_leaveRow >= 0) {
        REAL t1 = (REAL)leaveElapsed / 170.0f;
        if (t1 > 1.0f) t1 = 1.0f;
        REAL y = (REAL)(SHADOW + LIST_TOP + (g_leaveRow - g_scroll) * ROW_H);
        paintRow(g, g_leaveTask, -1, y, 70.0f * t1, 1.0f - t1, true);
    }
    g.Restore(listState);

    if (maxScroll > 0 && !g_editing) {
        // 4px 宽、透明度也提一档：2~3px 压在深色玻璃上几乎看不见，
        // 用户不一定知道列表还能往下滚
        REAL trackX = (REAL)(SHADOW + CARD_W - 8);
        REAL trackTop = (REAL)(SHADOW + LIST_TOP + 6);
        REAL trackH = (REAL)(VISIBLE_ROWS * ROW_H - 12);
        REAL thumbH = trackH * VISIBLE_ROWS / (REAL)g_tasks.size();
        REAL thumbY = trackTop + (trackH - thumbH) * g_scroll / (REAL)maxScroll;
        GraphicsPath p;
        roundRect(p, RectF(trackX, thumbY, 4, thumbH), 2.0f);
        SolidBrush b(Color(72, 255, 255, 255));
        g.FillPath(&b, &p);
    }

    if (g_editing) {
        paintEditPanel(g);
    } else {
        REAL footY = (REAL)(SHADOW + CARD_H - FOOT_H);
        Pen sep(Color(18, 255, 255, 255), 1.0f);
        g.DrawLine(&sep, SHADOW + 14.0f, footY + 0.5f, SHADOW + CARD_W - 14.0f, footY + 0.5f);

        RectF addR((REAL)SHADOW + 6, footY + 6, (REAL)CARD_W / 2 - 8, (REAL)FOOT_H - 14);
        RectF openR((REAL)SHADOW + (REAL)CARD_W / 2, footY + 6,
                    (REAL)CARD_W / 2 - 8, (REAL)FOOT_H - 14);

        if (g_hoverBtn == 2) {
            GraphicsPath hp;
            roundRect(hp, addR, 9.0f);
            SolidBrush hb(C_HOVER);
            g.FillPath(&hb, &hp);
        }
        if (g_hoverBtn == 3) {
            GraphicsPath hp;
            roundRect(hp, openR, 9.0f);
            SolidBrush hb(C_HOVER);
            g.FillPath(&hb, &hp);
        }

        Pen plus(C_ACCENT, 1.6f);
        plus.SetStartCap(LineCapRound);
        plus.SetEndCap(LineCapRound);
        REAL px = addR.X + 14, py = addR.Y + addR.Height / 2;
        g.DrawLine(&plus, px, py - 5, px, py + 5);
        g.DrawLine(&plus, px - 5, py, px + 5, py);

        drawText(g, L"添加任务", bodyFont(),
                 RectF(addR.X + 26, addR.Y, addR.Width - 26, addR.Height), C_TEXT_DIM, true, 0);

        drawText(g, L"打开控制台", smallFont(),
                 RectF(openR.X, openR.Y, openR.Width - 10, openR.Height), C_TEXT_FAINT, true, 2);
    }
}

// ---- 呈现到屏幕 ----

// UpdateLayeredWindow 要预乘 alpha，GDI+ 写的是非预乘值，直接画会发虚。
static void premultiplyAlpha(BYTE* bits, int w, int h) {
    int n = w * h;
    for (int i = 0; i < n; ++i) {
        BYTE* p = bits + (size_t)i * 4;      // BGRA
        BYTE a = p[3];
        if (a == 255) continue;
        if (a == 0) { p[0] = p[1] = p[2] = 0; continue; }
        p[0] = (BYTE)((p[0] * a + 127) / 255);
        p[1] = (BYTE)((p[1] * a + 127) / 255);
        p[2] = (BYTE)((p[2] * a + 127) / 255);
    }
}

static int dotPixels() { return (int)(DOT_SIZE * dotUnit() + 0.5f); }

// 挂在桌面上时，SetWindowPos 用的是父窗口坐标，得先换算
static void placeWindow(int screenX, int screenY, int w, int h) {
    POINT pt{screenX, screenY};
    if (g_desktopParent) ScreenToClient(g_desktopParent, &pt);
    SetWindowPos(g_hwnd, nullptr, pt.x, pt.y, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void present();

// Win+D 会把桌面提到最前，连图标层都盖住悬浮清单；看窗口中心那个点归属谁，
// 落到图标上就自己提上来。被普通窗口盖住则不管。
// 前台是不是全屏程序（全屏视频、游戏、演示）。用系统的通知状态判断，
// 比自己量窗口矩形准：最大化窗口铺满的是工作区，不算全屏。
static bool fullscreenAppInFront() {
    // 头文件里这几个符号要 NTDDI_VISTA 才暴露，干脆运行时取函数地址。
    // 返回值：1 不在 / 2 忙（全屏、演示）/ 3 D3D 全屏 / 4 演示模式 / 5 正常 / 6 静默 / 7 应用
    typedef HRESULT (WINAPI *QueryFn)(int*);
    static QueryFn fn = reinterpret_cast<QueryFn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"shell32.dll"), "SHQueryUserNotificationState")));
    if (fn) {
        int st = 5;
        if (SUCCEEDED(fn(&st))) return st == 2 || st == 3 || st == 4;
    }

    // 老系统没有这个 API，退回量矩形：得连任务栏一起盖住才算全屏
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
        // 直接清样式再 NOTOPMOST，两道都做，免得托盘临时抬上去的那次清不干净
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

// 标题栏那个置顶按钮：改的就是「悬浮清单置顶」这个设置，和控制台同一个值。
// 先按预期值切过去（点下去立刻有反应），再把请求发出去落盘；
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
    // 置顶模式：一直压在普通窗口上面；但前台是全屏程序时先躲开，别打扰看视频打游戏
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

// 读 Windows 的「应用模式」：AppsUseLightTheme = 1 是浅色，走日间配色。
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

    // BitBlt 不写 alpha，顺手压暗，保证浅壁纸上的文字对比度。
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

// 隐藏窗口之后要等合成器真的换掉这一帧，再去抓屏；
// 不等的话 BitBlt 拿到的还是卡片自己，毛玻璃里就会一直印着上一次的列表。
static void flushComposition() {
    typedef HRESULT (WINAPI *PFN_Flush)(void);
    static PFN_Flush fn = reinterpret_cast<PFN_Flush>(reinterpret_cast<void*>(
        GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmFlush")));
    if (fn) fn();
}

// 抓玻璃背景之前必须先把窗口藏起来，否则抓屏抓到的是卡片自己，
// 毛玻璃里就会一直留着上一次的列表。藏和现在同一个消息里做完，
// 合成器来不及出帧，看不出闪。
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
// 否则鼠标从窗口里直接移出去时收不到 WM_MOUSELEAVE，卡片会一直摊着。
// 光标本来就在窗口外面时不要注册：TME_LEAVE 会立刻回一条 WM_MOUSELEAVE，
// 从托盘叫出来的卡片会在半秒内自己收起。
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
// 圆点贴在右下角时直接摊开会有一大半在屏幕外，底栏的「添加任务 / 打开控制台」就点不到了。
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
// 这时把指针搬到卡片上，免得用户找不到它（也会立刻撞上「移开就收起」）。
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

    // 先把窗口藏掉再抓：这时候屏幕上还是圆点，但圆点正好落在卡片左上角，
    // 一起抓进去会在圆角外面留个模糊的圆。抓的位置得是卡片最终要占的那块。
    ShowWindow(g_hwnd, SW_HIDE);
    flushComposition();
    captureBlurred(cardX, cardY);

    g_expanded = true;
    g_fade = 0.0f;

    // 清单刚展开：所有进度条从 0 转出来，别等收起时才补这一段
    g_progAll = true;
    g_progStart = GetTickCount();
    g_progAnim = 0.0f;
    kickProg();

    placeWindow(cardX, cardY, g_winW, g_winH);
    if (g_widgetOn) ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);

    if (bringCursor && (cardX != wr.left || cardY != wr.top)) {
        // 卡片没开在圆点那儿（翻了边或者被夹进工作区）：鼠标留在原处就丢了目标，
        // 还会立刻撞上「移开就收起」。落点是左上角那个徽标——它和圆点原本是同一个
        // 位置，挪过去之后视线能直接对上；那块地方没有点击和悬停，不会误触。
        SetCursorPos(cardX + (int)((SHADOW + 25) * g_scale),
                     cardY + (int)((SHADOW + 23) * g_scale));
    }

    trackLeave();

    SetTimer(g_hwnd, TIMER_FADE, 16, nullptr);
    present();
}

static void finishEdit(bool commit);   // 定义在后面

static void endTraySession();      // 定义在后面，收起时要用

static void collapseWidget() {
    if (!g_expanded) return;

    // 收起前丢掉没提交的输入。
    if (g_editing) finishEdit(false);

    g_expanded = false;
    g_fade = 1.0f;
    KillTimer(g_hwnd, TIMER_FADE);

    int d = dotPixels();

    // 回到圆点位置。展开时卡片可能翻到另一侧，所以不能直接按卡片当前位置收；
    // 但用户拖动卡片时 g_dotX/g_dotY 是跟着走的，收起后圆点就落在卡片现在的左上角
    placeWindow(g_dotX, g_dotY, d + g_pillW, d);
    present();

    // 由托盘叫出来的，收起就算会话结束：层级和开关都要还原
    if (g_trayOpen) endTraySession();
}

// 托盘会话收尾：收起、把打开前的状态放回去（置顶层、显示与否）
static void endTraySession() {
    if (!g_trayOpen) return;
    g_trayOpen = false;
    KillTimer(g_hwnd, TIMER_TRAY);

    if (g_expanded) collapseWidget();      // 这时 g_trayOpen 已经清了，不会再绕回来

    if (!g_trayWasEnabled) {
        g_widgetOn = false;                // 打开前是关着的，再关回去；设置文件不动
        ShowWindow(g_hwnd, SW_HIDE);
    }
    applyTopmost();                        // 回到设置里那一层
}

// 位图按「卡片」和「圆点 + 窄条」里大的那个开：高 DPI 下窄条比卡片还宽
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

// ---- 命中测试 ----

static bool inRect(int x, int y, REAL rx, REAL ry, REAL rw, REAL rh) {
    return x >= rx && x <= rx + rw && y >= ry && y <= ry + rh;
}

// 鼠标窗口坐标 -> 展开态坐标
static void toCard(int mx, int my, int& cx, int& cy) {
    cx = (int)(mx / g_scale) - SHADOW;
    cy = (int)(my / g_scale) - SHADOW;
}

static int hitButton(int cx, int cy) {
    if (cy < 0 || cy > TITLE_H) {
        if (g_editing) return -1;          // 输入中，底栏整块都是输入框
        if (cy > CARD_H - FOOT_H && cy < CARD_H) {
            if (cx > 0 && cx < CARD_W / 2) return 2;
            if (cx >= CARD_W / 2 && cx < CARD_W) return 3;
        }
        return -1;
    }
    if (inRect(cx, cy, CARD_W - 34, 13, 22, 22)) return 1;
    if (inRect(cx, cy, CARD_W - 60, 13, 22, 22)) return 4;
    return -1;
}

// 编辑态下鼠标是不是点在输入框里
static bool hitInput(int cx, int cy) {
    if (!g_editing) return false;
    return inRect(cx, cy, 10, panelInputTop(), CARD_W - 20, 30);
}

// 从这里开始那一行：1 选文件 2 粘贴 3 清除
static int hitLinkRow(int cx, int cy) {
    if (!g_editing) return 0;
    if (inRect(cx, cy, 74, panelLinkTop(), 52, CHIP_H)) return 1;
    if (inRect(cx, cy, 128, panelLinkTop(), 40, CHIP_H)) return 2;
    if (!g_editLinks.empty() && inRect(cx, cy, 244, panelLinkTop(), 22, CHIP_H)) return 3;
    return 0;
}

static void refreshAsync();   // 定义在后面

// 行尾两个按钮的命中区：画的是 18px 的圆，判定给整格，别让人瞄半天。
// cx/cy 是卡片坐标（toCard 已经减掉 SHADOW），这里不能再加 SHADOW，否则整块往右偏一格。
static bool hitProgress(int cx, int cy, int row) {
    if (row < 0) return false;
    REAL right = (REAL)(6 + CARD_W - 12);
    REAL rowTop = (REAL)(LIST_TOP + row * ROW_H);
    return inRect(cx, cy, right - 32, rowTop + 2, 28, ROW_H - 4);
}

// 行尾的置顶按钮，在「正在进行」左边那一格
static bool hitPin(int cx, int cy, int row) {
    if (row < 0) return false;
    REAL right = (REAL)(6 + CARD_W - 12);
    REAL rowTop = (REAL)(LIST_TOP + row * ROW_H);
    return inRect(cx, cy, right - 58, rowTop + 2, 26, ROW_H - 4);
}

// 点一下切换「正在进行」
static void toggleStart(const Task& t) {
    if (g_port == 0) findServerPort();
    if (g_port == 0) return;
    if (t.startedAt == 0) {
        // 开始的那一下让进度条从 0 转出来
        g_progAll = false;
        g_progId = t.id;
        g_progStart = GetTickCount();
        g_progAnim = 0.0f;
        kickProg();
    }
    wstring path = L"/api/start?id=" + urlEncode(wtou8(t.id)) +
                   (t.startedAt > 0 ? L"&on=0" : L"&on=1");
    string body;
    httpRequest(L"POST", path, body);
    refreshAsync();
}

static bool hitCancel(int cx, int cy) {
    if (!g_editing) return false;
    RectF r = cancelRect();
    return inRect(cx, cy, r.X, r.Y, r.Width, r.Height);
}

// 日历上的点击：1 上一月 2 下一月 3 取消 4 确定 5..8 时间加减，100+d 选中某天
static int hitCalendar(int cx, int cy) {
    RectF r;
    r = calPrev();    if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return 1;
    r = calNext();    if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return 2;
    r = calCancel();  if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return 3;
    r = calOk();      if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return 4;
    r = calHourDec(); if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return 5;
    r = calHourInc(); if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return 6;
    r = calMinDec();  if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return 7;
    r = calMinInc();  if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return 8;

    int first = firstWeekday(g_pickYear, g_pickMonth);
    int dim = daysOfMonth(g_pickYear, g_pickMonth);
    for (int i = 0; i < 42; ++i) {
        int d = i - first + 1;
        if (d < 1 || d > dim) continue;
        r = calCell(i);
        if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return 100 + d;
    }
    return 0;
}

// 打开日历：有选过的就从那个日期起，否则从今天 23:59 起
static void openCalendar() {
    g_editDue = 3;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    g_pickYear = st.wYear;
    g_pickMonth = st.wMonth;
    g_pickDay = st.wDay;
    g_pickHour = 23;
    g_pickMin = 59;

    if (!g_customDue.empty()) {
        int y = 0, mo = 0, d = 0, hh = 23, mm = 59;
        if (swscanf(g_customDue.c_str(), L"%d-%d-%dT%d:%d", &y, &mo, &d, &hh, &mm) == 5) {
            g_pickYear = y; g_pickMonth = mo; g_pickDay = d;
            g_pickHour = hh; g_pickMin = mm;
        }
    }
    g_pickingDate = true;
}

static void applyCalendar() {
    int day = g_pickDay;
    int dim = daysOfMonth(g_pickYear, g_pickMonth);
    if (day > dim) day = dim;                 // 换月之后天数可能不够

    wchar_t buf[32];
    swprintf(buf, 32, L"%04d-%02d-%02dT%02d:%02d",
             g_pickYear, g_pickMonth, day, g_pickHour, g_pickMin);
    g_customDue = buf;
    g_editDue = 3;
    g_pickingDate = false;
}

// 点在哪个选项上：返回 row*100+i（0=截止 1=预估），没点中返回 -1
static int hitChip(int cx, int cy) {
    if (!g_editing) return -1;
    for (int row = 0; row < 2; ++row) {
        int n = (row == 0) ? 5 : 6;
        for (int i = 0; i < n; ++i) {
            RectF r = chipRect(row, i, n);
            if (inRect(cx, cy, r.X, r.Y, r.Width, r.Height)) return row * 100 + i;
        }
    }
    return -1;
}

static int hitRow(int cx, int cy) {
    (void)cx;
    if (cy < LIST_TOP || cy >= LIST_BOTTOM) return -1;
    int i = (cy - LIST_TOP) / ROW_H;
    int idx = g_scroll + i;
    if (idx < 0 || idx >= (int)g_tasks.size()) return -1;
    return i;
}

static bool hitCheckbox(int cx, int cy, int row) {
    REAL rowY = (REAL)(LIST_TOP + row * ROW_H);
    return inRect(cx, cy, 6 + 10, rowY + (ROW_H - 18) / 2.0f, 22, 22);
}

// 收起时：鼠标是不是落在中央圆点（含外圈那层柔光）上。
// 这个范围必须固定，不能跟着外圈涨大——否则鼠标停在环上，环一涨就把自己"撑"出判定区，
// 窄条跟着缩回去，环又缩小，来回抖。
static bool hitDot(int mx, int my) {
    REAL dx = (REAL)mx / dotUnit() - DOT_SIZE / 2.0f;
    REAL dy = (REAL)my / dotUnit() - DOT_SIZE / 2.0f;
    const REAL R = 16.0f;                 // 圆点柔光的外沿
    return (dx * dx + dy * dy) <= R * R;
}

// 收起时：鼠标是不是落在窄条上（点它才展开）
static bool hitPill(int mx, int my) {
    if (g_pillW <= 0) return false;
    REAL x0 = (REAL)PILL_X * dotUnit();
    REAL x1 = (REAL)PILL_X * dotUnit() + (REAL)g_pillW;
    REAL y0 = ((DOT_SIZE - PILL_H) / 2.0f) * dotUnit();
    REAL y1 = y0 + (REAL)PILL_H * dotUnit();
    return mx >= x0 && mx <= x1 && my >= y0 && my <= y1;
}

// ---- 交互 ----

static void openFullApp() {
    if (g_port == 0) findServerPort();
    if (g_port == 0) return;
    wchar_t url[128];
    swprintf(url, 128, L"http://127.0.0.1:%d/", g_port);
    ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

static void refreshAsync() {
    if (!fetchTasks()) return;
    syncPillWidth();                 // 紧急任务来了就长横条，走了就收回
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---- 就地添加 ----

// 悬浮清单是 NOACTIVATE 的，打字前得先 attach 前台线程，再 SetForegroundWindow。
// 刚抢到焦点时系统会来回抖一下（前台锁没解开、另一个线程又把焦点拿回去），
// 这一下不该当成「用户点走了」，否则输入框刚打开就被提交掉，字根本打不进去
static DWORD g_focusAt = 0;

static void grabKeyboardFocus() {
    g_focusAt = GetTickCount();

    HWND fg = GetForegroundWindow();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD myThread = GetCurrentThreadId();
    bool attached = (fgThread != 0 && fgThread != myThread);
    if (attached) AttachThreadInput(fgThread, myThread, TRUE);
    SetForegroundWindow(g_hwnd);
    SetFocus(g_hwnd);
    if (attached) {
        AttachThreadInput(fgThread, myThread, FALSE);
        // 脱开输入队列时焦点会被对方收回去，再抢一次
        SetFocus(g_hwnd);
    }
}

// 有模态窗口开着（文件选择框、确认框、右键菜单）时鼠标必然在卡片外面，
// 这段时间不能按「鼠标移开」处理，否则卡片会在对话框后面自己收起。
static int g_modalDepth = 0;

struct ModalScope {
    ModalScope() { ++g_modalDepth; }
    ~ModalScope() { --g_modalDepth; }
};

// 系统文件选择框：拿到完整本地路径
static void pickLinkFile() {
    wchar_t buf[MAX_PATH] = {0};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"选择要打开的文件或材料";
    ofn.lpstrFilter = L"所有文件\0*.*\0\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    bool ok;
    {
        ModalScope ms;
        ok = GetOpenFileNameW(&ofn) != FALSE;
    }
    if (!ok) return;
    if (g_editLinks.size() >= 8) return;
    g_editLinks.push_back(buf);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// 从剪贴板取，按行拆，一行一个
static void pasteLinks() {
    bool ok = false;
    for (int i = 0; i < 5 && !ok; ++i) ok = OpenClipboard(g_hwnd) != FALSE;
    if (!ok) return;

    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* p = (const wchar_t*)GlobalLock(h);
        if (p) {
            wstring text = p;
            GlobalUnlock(h);
            size_t start = 0;
            while (start <= text.size() && g_editLinks.size() < 8) {
                size_t nl = text.find_first_of(L"\r\n", start);
                wstring line = text.substr(start, (nl == wstring::npos ? text.size() : nl) - start);
                size_t a = line.find_first_not_of(L" \t");
                size_t b = line.find_last_not_of(L" \t");
                if (a != wstring::npos) {
                    line = line.substr(a, b - a + 1);
                    bool dup = false;
                    for (const auto& one : g_editLinks) if (one == line) dup = true;
                    if (!dup) g_editLinks.push_back(line);
                }
                if (nl == wstring::npos) break;
                start = nl + 1;
            }
        }
    }
    CloseClipboard();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// 输入框里的 Ctrl+V：只取纯文本，换行换成空格，整串最多 200 字
static void pasteTitle() {
    bool ok = false;
    for (int i = 0; i < 5 && !ok; ++i) ok = OpenClipboard(g_hwnd) != FALSE;
    if (!ok) return;

    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* p = (const wchar_t*)GlobalLock(h);
        if (p) {
            wstring text = p;
            GlobalUnlock(h);
            if (g_caret > (int)g_editText.size()) g_caret = (int)g_editText.size();
            for (wchar_t c : text) {
                if ((int)g_editText.size() >= 200) break;
                if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
                else if (c < 32) continue;
                g_editText.insert((size_t)g_caret, 1, c);
                ++g_caret;
            }
            g_caretOn = true;
        }
    }
    CloseClipboard();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// 交给系统打开：http 走浏览器，本地路径走文件管理器或默认程序
static void openLinks(const std::vector<wstring>& links) {
    for (const wstring& one : links) {
        if (one.empty()) continue;
        ShellExecuteW(nullptr, L"open", one.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

static void startEdit() {
    if (g_editing) return;
    g_editing = true;
    g_editText.clear();
    g_caret = 0;
    g_editDue = 4;                           // 默认不设截止
    g_editEst = -1;                          // 默认不设预估
    g_customDue.clear();
    g_pickingDate = false;
    g_editLinks.clear();
    g_caretOn = true;
    KillTimer(g_hwnd, TIMER_COLLAPSE);       // 正在打字，别收起
    SetTimer(g_hwnd, TIMER_CARET, 530, nullptr);
    grabKeyboardFocus();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// 今天往后 addDays 天的 23:59，格式和 due 字段一致。
// 用 FILETIME 加减天数，跨月跨年不用自己算，也不受时区影响。
static wstring dueAfterDays(int addDays) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    st.wHour = 23;
    st.wMinute = 59;
    st.wSecond = 0;
    st.wMilliseconds = 0;

    FILETIME ft{};
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart += (ULONGLONG)addDays * 864000000000ULL;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;

    SYSTEMTIME out{};
    FileTimeToSystemTime(&ft, &out);

    wchar_t buf[32];
    swprintf(buf, 32, L"%04d-%02d-%02dT%02d:%02d",
             out.wYear, out.wMonth, out.wDay, out.wHour, out.wMinute);
    return buf;
}

// commit=true 就提交（空标题等于取消）
static void finishEdit(bool commit) {
    if (!g_editing) return;
    g_editing = false;
    KillTimer(g_hwnd, TIMER_CARET);

    wstring title = g_editText;
    g_editText.clear();

    size_t a = title.find_first_not_of(L" \t\r\n");
    size_t b = title.find_last_not_of(L" \t\r\n");
    title = (a == wstring::npos) ? wstring() : title.substr(a, b - a + 1);

    if (commit && !title.empty()) {
        if (g_port == 0) findServerPort();
        if (g_port != 0) {
            wstring path = L"/api/add?title=" + urlEncode(wtou8(title));
            if (g_editDue >= 0 && g_editDue < 3)
                path += L"&due=" + urlEncode(wtou8(dueAfterDays(g_editDue)));
            else if (g_editDue == 3 && !g_customDue.empty())
                path += L"&due=" + urlEncode(wtou8(g_customDue));

            if (!g_editLinks.empty()) {
                wstring lk;
                for (const wstring& one : g_editLinks) {
                    if (!lk.empty()) lk += L"|";
                    lk += urlEncode(wtou8(one));
                }
                path += L"&links=" + lk;
            }
            if (g_editEst >= 0 && g_editEst < 6)
                path += L"&estimate=" + std::to_wstring(EST_MIN[g_editEst]);
            string body;
            httpRequest(L"POST", path, body);

            // 新任务多半没截止，排在列表最底下。滚到底，让用户看见它真的加进去了
            g_scroll = 0x7fffffff;
        }
        refreshAsync();
    }

    g_customDue.clear();
    g_pickingDate = false;
    g_editLinks.clear();

    // 鼠标已经离开，把收起计时器补上。
    POINT pt{};
    GetCursorPos(&pt);
    RECT wr{};
    GetWindowRect(g_hwnd, &wr);
    if (!PtInRect(&wr, pt)) SetTimer(g_hwnd, TIMER_COLLAPSE, 340, nullptr);

    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// 右键任务行：置顶 / 推迟一天 / 删除
static void pinTask(const wstring& id, bool on) {
    if (id.empty()) return;
    if (g_port == 0) findServerPort();
    if (g_port == 0) return;
    string body;
    httpRequest(L"POST", L"/api/pin?id=" + id + (on ? L"&on=1" : L"&on=0"), body);
    refreshAsync();
}

static void snoozeTask(const wstring& id) {
    if (id.empty()) return;
    if (g_port == 0) findServerPort();
    if (g_port == 0) return;
    string body;
    httpRequest(L"POST", L"/api/snooze?id=" + id, body);
    refreshAsync();
}

static void removeTask(const wstring& id) {
    if (id.empty()) return;
    if (g_port == 0) findServerPort();
    if (g_port == 0) return;
    string body;
    httpRequest(L"POST", L"/api/remove?id=" + id, body);
    refreshAsync();
}

// 推一帧：窄条宽度和外圈大小用同一系数逼近目标。
// 返回 true 表示都到位，计时器可以停。
static bool stepSlide() {
    int  d  = g_pillTarget - g_pillW;
    REAL rd = g_ringTarget - g_ringT;
    if (d == 0 && rd == 0.0f) return true;

    if (d != 0) {
        int step = (int)(d * 0.34f);
        if (step == 0) step = (d > 0 ? 1 : -1);
        g_pillW += step;
        if ((d > 0 && g_pillW >= g_pillTarget) ||
            (d < 0 && g_pillW <= g_pillTarget)) {
            g_pillW = g_pillTarget;
        }
    }

    if (rd != 0.0f) {
        g_ringT += rd * 0.34f;
        // 浮点永远差一点，够近就落位，不然计时器停不下来。
        if (g_ringTarget - g_ringT < 0.004f && g_ringT - g_ringTarget < 0.004f)
            g_ringT = g_ringTarget;
    }

    return g_pillW == g_pillTarget && g_ringT == g_ringTarget;
}

static void applyScale(float scale);   // 定义在入口那段，WM_DPICHANGED 要用

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE:
            SetTimer(h, TIMER_REFRESH, 3000, nullptr);
            SetTimer(h, TIMER_ZGUARD, 600, nullptr);
            kickProg(h);   // 让圆点那圈环按需淡入
            return 0;

        case WM_DPICHANGED: {
            // 换显示器或改缩放：按新比例重建位图，再按系统建议的矩形摆回去
            int dpi = (int)HIWORD(w);
            if (dpi >= 72 && dpi <= 480) applyScale(dpi / 96.0f);

            RECT* r = (RECT*)l;
            int w2 = g_expanded ? g_winW : dotPixels() + g_pillW;
            int h2 = g_expanded ? g_winH : dotPixels();
            if (r) placeWindow(r->left, r->top, w2, h2);
            else {
                RECT wr{};
                GetWindowRect(h, &wr);
                placeWindow(wr.left, wr.top, w2, h2);
            }

            syncPillWidth();
            present();
            return 0;
        }

        case WM_TIMER:
            if (!g_widgetOn) return 0;
            if (w == TIMER_FADE) {
                g_fade += 0.14f;
                if (g_fade >= 1.0f) {
                    g_fade = 1.0f;
                    KillTimer(h, TIMER_FADE);
                }
                present();
                return 0;
            }
            if (w == TIMER_ZGUARD) {
                keepAboveDesktop();

                // 兜底：展开后鼠标从窗口里直接移出去，系统的离开追踪有时会漏
                // （藏窗口抓背景、切 DPI 都会把它弄丢），这里按真实光标位置补一次判断。
                // 托盘叫出来的卡片有自己的倒计时，编辑中、拖动中、有对话框开着都不算离开。
                if (g_expanded && !g_editing && !g_dragging &&
                    !g_trayOpen && g_modalDepth == 0) {
                    POINT pt{};
                    GetCursorPos(&pt);
                    RECT wr{};
                    GetWindowRect(h, &wr);
                    if (!PtInRect(&wr, pt)) SetTimer(h, TIMER_COLLAPSE, 340, nullptr);
                }
                return 0;
            }
            if (w == TIMER_SLIDE) {
                // 先推一帧再画，最后一帧也得画出来，否则会差最后几像素
                bool done = stepSlide();
                if (!g_expanded) {
                    RECT wr{};
                    GetWindowRect(h, &wr);
                    int dp = dotPixels();
                    placeWindow(wr.left, wr.top, dp + g_pillW, dp);
                }
                present();
                if (done) {
                    KillTimer(h, TIMER_SLIDE);
                    kickProg(h);   // 窄条到位了，圆点环该淡回来了
                }
                return 0;
            }
            if (w == TIMER_CARET) {
                if (!g_editing) { KillTimer(h, TIMER_CARET); return 0; }
                g_caretOn = !g_caretOn;
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (w == TIMER_PROG) {
                DWORD dt = GetTickCount() - g_progStart;
                REAL t = (REAL)dt / 420.0f;
                if (t > 1.0f) t = 1.0f;
                g_progAnim = t * t * (3.0f - 2.0f * t);

                // 窄条一展开就把圆点那圈环隐掉，收起再淡回来
                const Task* pt = pillTask();
                bool wantDot = (g_pillW == 0) && pt && pt->startedAt > 0;

                // 刚重新出现：让它的进度也从 0 转出来
                if (wantDot && !g_dotRingOn) {
                    g_progAll = false;
                    g_progId = pt->id;
                    g_progStart = GetTickCount();
                    g_progAnim = 0.0f;
                }
                g_dotRingOn = wantDot;

                REAL target = wantDot ? 1.0f : 0.0f;
                if (g_dotRingA < target) g_dotRingA += 0.08f;
                else if (g_dotRingA > target) g_dotRingA -= 0.08f;
                if (g_dotRingA < 0) g_dotRingA = 0;
                if (g_dotRingA > 1) g_dotRingA = 1;

                InvalidateRect(h, nullptr, FALSE);
                // 窄条还在滑动的时候别停：g_pillW 要等滑完才归零，
                // 提前停掉的话圆点环就再也不回来了
                bool settling = (g_progAnim < 1.0f) || fabs(g_dotRingA - target) >= 0.02f ||
                                g_pillW != g_pillTarget;
                if (settling) {
                    if (g_progSlow) { SetTimer(h, TIMER_PROG, 16, nullptr); g_progSlow = false; }
                } else if (needBreath()) {
                    // 只剩紫色呼吸：25 帧够用，别让一张静止的卡片一直全速重绘
                    if (!g_progSlow) { SetTimer(h, TIMER_PROG, 48, nullptr); g_progSlow = true; }
                } else {
                    KillTimer(h, TIMER_PROG);
                    g_progOn = false;
                    g_progSlow = false;
                }
                return 0;
            }
            if (w == TIMER_TICK) {
                InvalidateRect(h, nullptr, FALSE);     // 计时文本每秒走一格
                return 0;
            }
            if (w == TIMER_TRAY) {
                if (!g_trayOpen) { KillTimer(h, TIMER_TRAY); return 0; }
                if (GetTickCount() - g_trayIdle >= 2000) endTraySession();
                return 0;
            }
            if (w == TIMER_ROWS) {
                // 进度都按时间算，掉帧也不会变慢
                DWORD dt = GetTickCount() - g_leaveStart;
                int rows = (int)g_tasks.size() - g_leaveRow;
                DWORD total = 170 + (DWORD)(rows > 0 ? rows * 30 : 0) + 180;
                if (dt >= total) {
                    g_leaving = false;
                    g_leaveRow = -1;
                    KillTimer(h, TIMER_ROWS);
                }
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (w == TIMER_COLLAPSE) {
                KillTimer(h, TIMER_COLLAPSE);
                POINT pt{};
                GetCursorPos(&pt);
                RECT wr{};
                GetWindowRect(h, &wr);
                if (!PtInRect(&wr, pt) && !g_editing) collapseWidget();
                return 0;
            }
            if (w == TIMER_SETTLE) {
                KillTimer(h, TIMER_SETTLE);
                g_justDone.clear();
                refreshAsync();
            } else if (w == TIMER_REFRESH && !g_dragging) {
                // 收起时也要取数：圆点右边那条窄条得跟着任务走
                refreshAsync();
                // 在控制台点开始的任务，这边也要把呼吸接上
                if (needBreath() && !g_progOn) kickProg(h);
            }
            return 0;

        case WM_APP_TRAY_OPEN:
            // 不管设置里开没开，先把它叫到前台来；收起时再按原样恢复
            if (!g_trayOpen) g_trayWasEnabled = g_widgetOn;
            g_trayOpen = true;
            g_trayIdle = GetTickCount();

            if (!g_widgetOn) {
                g_widgetOn = true;              // 只是临时露个面，不写设置文件
                ShowWindow(h, SW_SHOWNOACTIVATE);
                findServerPort();
                refreshAsync();
            }
            if (g_hiddenForFullscreen) {
                g_hiddenForFullscreen = false;
                ShowWindow(h, SW_SHOWNOACTIVATE);
            }
            if (!g_expanded) expandWidget();
            SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SetTimer(h, TIMER_TRAY, 200, nullptr);
            return 0;

        case WM_APP_TOPMOST:
            g_topmost = (w != 0);
            applyTopmost();
            return 0;

        case WM_APP_WIDGET_ENABLED: {
            bool on = (w != 0);
            g_widgetOn = on;
            if (!on) {
                g_trayOpen = false;
                KillTimer(h, TIMER_TRAY);
                // 先收回圆点尺寸，避免下次打开是展开态。
                g_expanded = false;
                g_fade = 1.0f;
                KillTimer(h, TIMER_SLIDE);
                g_pillW = g_pillTarget;      // 滑动滑到一半就别停在半路上
                g_ringT = g_ringTarget;
                RECT wr{};
                GetWindowRect(h, &wr);
                int d = dotPixels();
                placeWindow(wr.left, wr.top, d + g_pillW, d);
                ShowWindow(h, SW_HIDE);
                applyTopmost();          // 层级也归位，免得下次露出来还压在最上面
            } else {
                ShowWindow(h, SW_SHOWNOACTIVATE);
                present();
                findServerPort();
                refreshAsync();
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            // 必须先注册离开追踪，否则展开后鼠标移开收不到 WM_MOUSELEAVE
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);

            if (!g_expanded) {
                // 按住拖动：超过 4px 才算拖，否则留着当点击
                if (g_pressPending) {
                    POINT cur;
                    GetCursorPos(&cur);
                    if (!g_dragging &&
                        (abs(cur.x - g_pressPt.x) > 4 || abs(cur.y - g_pressPt.y) > 4)) {
                        g_dragging = true;
                        g_dragOrigin = g_pressPt;
                        g_winOrigin.x = g_pressWin.x;
                        g_winOrigin.y = g_pressWin.y;
                    }
                    if (g_dragging) {
                        int nx = g_winOrigin.x + (cur.x - g_dragOrigin.x);
                        int ny = g_winOrigin.y + (cur.y - g_dragOrigin.y);
                        POINT pt{nx, ny};
                        if (g_desktopParent) ScreenToClient(g_desktopParent, &pt);
                        SetWindowPos(h, nullptr, pt.x, pt.y, 0, 0,
                                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                        return 0;
                    }
                }

                // 鼠标停上来只拉窄条，点窄条才展开。
                // 判定只认圆点本身和已经伸出来的窄条：透明角落、圆环都不算。
                // 圆环会跟着涨大，若拿它当判定区，鼠标会被自己"撑"出去，来回抖。
                int mx = GET_X_LPARAM(l), my = GET_Y_LPARAM(l);
                bool overPill = hitPill(mx, my);
                if (overPill != g_pillHover) {
                    g_pillHover = overPill;
                    InvalidateRect(h, nullptr, FALSE);
                }
                bool want = hitDot(mx, my) || overPill;
                if (want != g_pillWant) {
                    g_pillWant = want;
                    updatePillTarget();
                }
                return 0;
            }
            KillTimer(h, TIMER_COLLAPSE);      // 鼠标还在，别收起
            if (g_trayOpen) g_trayIdle = GetTickCount();

            int mx = GET_X_LPARAM(l), my = GET_Y_LPARAM(l);
            if (g_dragging) {
                POINT cur;
                GetCursorPos(&cur);
                int nx = g_winOrigin.x + (cur.x - g_dragOrigin.x);
                int ny = g_winOrigin.y + (cur.y - g_dragOrigin.y);
                // 拖的是卡片，但圆点得跟着走：收起时它要落回卡片现在的左上角，
                // 不然卡片挪了一圈，圆点却留在原地。存窗口原点而不是可见圆角，
                // 这样「展开 → 收起」来回几趟位置也不会一点点漂。
                g_dotX = nx;
                g_dotY = ny;
                POINT pt{nx, ny};
                if (g_desktopParent) ScreenToClient(g_desktopParent, &pt);
                SetWindowPos(h, nullptr, pt.x, pt.y, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            int cx, cy;
            toCard(mx, my, cx, cy);
            int btn = hitButton(cx, cy);
            int row = (btn < 0 && !g_editing) ? hitRow(cx, cy) : -1;
            bool hovCancel = hitCancel(cx, cy);
            if (btn != g_hoverBtn || row != g_hoverRow || hovCancel != g_hoverCancel) {
                g_hoverBtn = btn;
                g_hoverRow = row;
                g_hoverCancel = hovCancel;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            if (g_hoverBtn != -1 || g_hoverRow != -1) {
                g_hoverBtn = -1;
                g_hoverRow = -1;
                InvalidateRect(h, nullptr, FALSE);
            }
            if (g_pillHover) {
                g_pillHover = false;
                InvalidateRect(h, nullptr, FALSE);
            }
            // 鼠标离开：外圈缩回，黄色窄条也收回；红色任务的不动。
            if (g_pillWant) {
                g_pillWant = false;
                updatePillTarget();
            }
            if (g_expanded && !g_editing) SetTimer(h, TIMER_COLLAPSE, 340, nullptr);
            return 0;

        // ---- 输入框的键盘与输入法
        case WM_CHAR: {
            if (!g_editing) return 0;
            wchar_t ch = (wchar_t)w;
            if (g_caret < 0) g_caret = 0;
            if (g_caret > (int)g_editText.size()) g_caret = (int)g_editText.size();

            if (ch == 8) {                              // 退格：删光标前一个字
                if (g_caret > 0) {
                    g_editText.erase((size_t)(g_caret - 1), 1);
                    --g_caret;
                }
            } else if (ch >= 32 && ch != 127) {         // 控制字符一概不收
                if (g_editText.size() < 200) {
                    g_editText.insert((size_t)g_caret, 1, ch);
                    ++g_caret;
                }
            } else {
                return 0;
            }
            g_caretOn = true;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }

        case WM_KEYDOWN:
            if (!g_editing) return 0;
            if (w == VK_RETURN) { finishEdit(true); return 0; }
            if (w == VK_ESCAPE) { finishEdit(false); return 0; }

            // Ctrl+V 自己取剪贴板：这个窗口没有编辑控件，系统不会替我们做
            if (w == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) { pasteTitle(); return 0; }

            if (w == VK_LEFT)   { if (g_caret > 0) --g_caret; }
            else if (w == VK_RIGHT) { if (g_caret < (int)g_editText.size()) ++g_caret; }
            else if (w == VK_HOME)  { g_caret = 0; }
            else if (w == VK_END)   { g_caret = (int)g_editText.size(); }
            else if (w == VK_DELETE) {
                if (g_caret < (int)g_editText.size()) g_editText.erase((size_t)g_caret, 1);
            } else {
                return 0;
            }
            g_caretOn = true;
            InvalidateRect(h, nullptr, FALSE);
            return 0;

        // 输入法组字窗跟着输入框走，不然候选框会飘到窗口左上角
        case WM_IME_STARTCOMPOSITION: {
            HIMC imc = ImmGetContext(h);
            if (imc) {
                COMPOSITIONFORM cf{};
                cf.dwStyle = CFS_POINT;
                cf.ptCurrentPos.x = (LONG)((SHADOW + 20) * g_scale);
                cf.ptCurrentPos.y = (LONG)((SHADOW + CARD_H - FOOT_H + 24) * g_scale);
                ImmSetCompositionWindow(imc, &cf);
                ImmReleaseContext(h, imc);
            }
            return 0;
        }

        case WM_KILLFOCUS:
            // 点到别处去了，就当提交；但刚抢焦点那一下的系统抖动不算
            if (g_editing && GetTickCount() - g_focusAt > 250) finishEdit(true);
            return 0;

        case WM_LBUTTONDOWN: {
            int mx = GET_X_LPARAM(l), my = GET_Y_LPARAM(l);
            int cx, cy;
            toCard(mx, my, cx, cy);

            // 收起态：圆点和窄条都能拖，先按住记位置，等移动了再定性
            if (!g_expanded) {
                g_pressPending = true;
                GetCursorPos(&g_pressPt);
                RECT wr{};
                GetWindowRect(h, &wr);
                g_pressWin.x = wr.left;
                g_pressWin.y = wr.top;
                SetCapture(h);
                return 0;
            }

            int btn = hitButton(cx, cy);
            if (btn >= 0) return 0;          // 按钮在抬起时处理

            // 标题栏空白处拖动
            if (cy >= 0 && cy <= TITLE_H && cx >= 0 && cx <= CARD_W) {
                g_dragging = true;
                GetCursorPos(&g_dragOrigin);
                RECT wr{};
                GetWindowRect(h, &wr);
                g_winOrigin.x = wr.left;
                g_winOrigin.y = wr.top;
                SetCapture(h);
                return 0;
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            int mx = GET_X_LPARAM(l), my = GET_Y_LPARAM(l);
            if (g_trayOpen) g_trayIdle = GetTickCount();
            if (g_dragging) {
                g_dragging = false;
                g_pressPending = false;
                ReleaseCapture();
                saveSettings();
                // 挪了位置，底下的桌面换了，重抓一次背景
                recaptureBackground();
                present();
                return 0;
            }
            if (g_pressPending) {
                // 按下后没挪动，当点击。圆点本身也算：有窄条时鼠标一停上去窄条就滑出来了，
                // 但用户的点击往往还落在圆点上，不认这个点会让人以为点不动
                g_pressPending = false;
                ReleaseCapture();
                if (!g_expanded && (hitPill(mx, my) || hitDot(mx, my) || g_pillFull == 0))
                    expandWidget(true);
                return 0;
            }

            int cx, cy;
            toCard(mx, my, cx, cy);

            int btn = hitButton(cx, cy);

            if (g_editing) {
                if (btn == 1) { finishEdit(false); appSetWidgetEnabled(false); return 0; }
                if (btn == 4) { finishEdit(true); toggleTopmost(); return 0; }

                if (g_pickingDate) {
                    int hit = hitCalendar(cx, cy);
                    if (hit == 1) {
                        if (--g_pickMonth < 1) { g_pickMonth = 12; --g_pickYear; }
                    } else if (hit == 2) {
                        if (++g_pickMonth > 12) { g_pickMonth = 1; ++g_pickYear; }
                    } else if (hit == 3) {
                        g_pickingDate = false;
                        g_editDue = g_customDue.empty() ? 4 : 3;
                    } else if (hit == 4) {
                        applyCalendar();
                    } else if (hit == 5) {
                        g_pickHour = (g_pickHour + 23) % 24;
                    } else if (hit == 6) {
                        g_pickHour = (g_pickHour + 1) % 24;
                    } else if (hit == 7) {
                        g_pickMin = (g_pickMin + 55) % 60;
                    } else if (hit == 8) {
                        g_pickMin = (g_pickMin + 5) % 60;
                    } else if (hit >= 100) {
                        g_pickDay = hit - 100;
                    }
                    InvalidateRect(h, nullptr, FALSE);
                    return 0;
                }

                if (hitCancel(cx, cy)) { finishEdit(false); return 0; }

                int linkHit = hitLinkRow(cx, cy);
                if (linkHit == 1) { pickLinkFile(); return 0; }
                if (linkHit == 2) { pasteLinks(); return 0; }
                if (linkHit == 3) {
                    g_editLinks.clear();
                    InvalidateRect(h, nullptr, FALSE);
                    return 0;
                }

                int chip = hitChip(cx, cy);
                if (chip >= 0) {
                    if (chip == 3) {                 // 自选：开日历
                        openCalendar();
                    } else if (chip < 100) {
                        g_editDue = chip;
                    } else {
                        int i = chip - 100;
                        g_editEst = (g_editEst == i) ? -1 : i;   // 再点一次就是取消
                    }
                    InvalidateRect(h, nullptr, FALSE);
                    return 0;
                }

                if (!hitInput(cx, cy)) finishEdit(true);   // 点别处就当提交
                return 0;
            }

            if (btn == 1) { appSetWidgetEnabled(false); return 0; }   // 关掉悬浮清单，不是退出程序
            if (btn == 4) { toggleTopmost(); return 0; }
            if (btn == 2) { startEdit(); return 0; }
            if (btn == 3) { openFullApp(); return 0; }

            int row = hitRow(cx, cy);
            if (row >= 0 && !g_leaving) {          // 动画期间不给点，免得勾错行
                int idx = g_scroll + row;
                if (idx >= 0 && idx < (int)g_tasks.size()) {
                    if (hitCheckbox(cx, cy, row)) {
                        // 先把快照和动画状态定下来：toggleTask 会立刻刷新列表，
                        // 刷完 g_tasks[idx] 就不是这一条了
                        g_justDone = g_tasks[idx].id;
                        g_leaveTask = g_tasks[idx];
                        g_leaveRow = idx;
                        g_leaveStart = GetTickCount();
                        g_leaving = true;
                        SetTimer(h, TIMER_ROWS, 16, nullptr);

                        toggleTask(g_leaveTask.id);
                        if (g_pending > 0) --g_pending;
                        SetTimer(h, TIMER_SETTLE, 700, nullptr);

                        InvalidateRect(h, nullptr, FALSE);
                    } else if (hitPin(cx, cy, row)) {
                        wstring pid = g_tasks[idx].id;
                        bool on = !g_tasks[idx].pinned;
                        pinTask(pid, on);
                    } else if (hitProgress(cx, cy, row)) {
                        toggleStart(g_tasks[idx]);
                    } else if (!g_tasks[idx].links.empty()) {
                        // 先复制一份：MessageBox 会跑消息循环，期间列表可能被刷新
                        std::vector<wstring> lks = g_tasks[idx].links;
                        if (lks.size() > 1) {
                            wstring msg = L"打开这条任务挂的 " + std::to_wstring(lks.size()) +
                                          L" 项材料？";
                            int ans;
                            {
                                ModalScope ms;
                                ans = MessageBoxW(h, msg.c_str(), L"GoGoSpot",
                                                  MB_OKCANCEL | MB_ICONQUESTION);
                            }
                            if (ans != IDOK) return 0;
                        }
                        openLinks(lks);
                    }
                }
            }
            return 0;
        }

        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(w);
            int maxScroll = (int)g_tasks.size() - VISIBLE_ROWS;
            if (maxScroll < 0) maxScroll = 0;
            if (delta > 0 && g_scroll > 0) { --g_scroll; InvalidateRect(h, nullptr, FALSE); }
            if (delta < 0 && g_scroll < maxScroll) { ++g_scroll; InvalidateRect(h, nullptr, FALSE); }
            return 0;
        }

        case WM_RBUTTONUP: {
            // 右键落在某条任务上就给这条任务的操作，落在空白处还是悬浮清单菜单
            int rmx = GET_X_LPARAM(l), rmy = GET_Y_LPARAM(l);
            int rcx, rcy;
            toCard(rmx, rmy, rcx, rcy);
            int row = g_expanded ? hitRow(rcx, rcy) : -1;
            int idx = (row >= 0) ? g_scroll + row : -1;
            bool onTask = (idx >= 0 && idx < (int)g_tasks.size());
            wstring taskId = onTask ? g_tasks[idx].id : wstring();
            bool taskPinned = onTask && g_tasks[idx].pinned;

            HMENU menu = CreatePopupMenu();
            if (onTask) {
                AppendMenuW(menu, MF_STRING, 12, taskPinned ? L"取消置顶" : L"置顶");
                AppendMenuW(menu, MF_STRING, 10, L"推迟一天");
                AppendMenuW(menu, MF_STRING, 11, L"删除任务");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, 1, L"打开控制台");
            } else {
                AppendMenuW(menu, MF_STRING, 1, L"打开控制台");
                AppendMenuW(menu, MF_STRING, 2, L"添加任务");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, 4, L"刷新");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, 5, L"关闭悬浮清单");
            }

            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(h);
            int cmd;
            {
                ModalScope ms;      // 菜单开着时鼠标可能已经移出卡片，别让它自己收起
                cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                     pt.x, pt.y, 0, h, nullptr);
            }
            DestroyMenu(menu);

            switch (cmd) {
                case 1: openFullApp(); break;
                case 2:                                  // 展开并直接进输入状态
                    if (!g_expanded) expandWidget();
                    startEdit();
                    break;
                case 4: refreshAsync(); break;
                case 5: appSetWidgetEnabled(false); break;
                case 10: snoozeTask(taskId); break;
                case 11: removeTask(taskId); break;
                case 12: pinTask(taskId, !taskPinned); break;
                default: break;
            }
            return 0;
        }

        case WM_SYSCOMMAND:
            // 别跟着「显示桌面」被最小化，否则 Win+D 后找不到。
            if ((w & 0xFFF0) == SC_MINIMIZE) return 0;
            return DefWindowProcW(h, m, w, l);

        case WM_PAINT: {
            // 分层窗口靠 UpdateLayeredWindow 出图，这里只为让 InvalidateRect 生效。
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            present();
            EndPaint(h, &ps);
            return 0;
        }

        case WM_CLOSE:
            // 输入时窗口会拿到键盘焦点，Alt+F4 顺手按下来只该收起悬浮清单，
            // 不能把整个程序带走（程序退出走的是托盘菜单）
            if (g_editing) finishEdit(false);
            appSetWidgetEnabled(false);
            return 0;

        case WM_DESTROY:
            saveSettings();
            KillTimer(h, TIMER_REFRESH);
            KillTimer(h, TIMER_FADE);
            KillTimer(h, TIMER_COLLAPSE);
            KillTimer(h, TIMER_ZGUARD);
            KillTimer(h, TIMER_SLIDE);
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(h, m, w, l);
    }
}

// ---- 入口 ----

// MinGW 默认的 WINVER 较低，user32 里这个导出要自己声明
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
