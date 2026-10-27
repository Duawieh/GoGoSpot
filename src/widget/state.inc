// ---- 状态 ----

struct Task {
    wstring id;
    wstring title;
    int estimate = 0;
    long long dueMs = 0;        // Unix 毫秒，0 表示没设截止时间
    std::vector<wstring> links; // 关联材料：可以挂多个链接或本地文件
    long long startedAt = 0;    // 正在进行：计时开始时刻，0 表示没在进行
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
static bool  g_widgetOn = true;      // 悬浮清单总开关，关闭时隐藏整个窗口
static float g_fade = 0.0f;          // 展开时的淡入进度 0..1
static bool  g_bgReady = false;      // 模糊背景是否已抓取

static wstring g_justDone;          // 刚勾选的任务，短暂显示删除线
static int   g_hoverRow = -1;
static int   g_hoverBtn = -1;       // 0=置顶 1=关闭 2=添加 3=控制台

static BYTE*   g_bits    = nullptr;
static HDC     g_memDC   = nullptr;
static HBITMAP g_bmp     = nullptr;
static HDC     g_screenDC = nullptr;
static HWND    g_hwnd = nullptr;
static HWND    g_desktopParent = nullptr;   // 挂到桌面后指向 Progman

static HDC   g_bgDC = nullptr;       // 模糊后的背景
static HDC   g_rawDC = nullptr;      // 原始抓屏
static HDC   g_smallDC = nullptr;    // 缩小用于模糊
static BYTE* g_bgBits = nullptr;
static HBITMAP g_bgBmp = nullptr, g_rawBmp = nullptr, g_smallBmp = nullptr;

static POINT g_dragOrigin{};
static POINT g_winOrigin{};


// 系统 DPI 缩放：界面按 96 DPI 设计，绘制时统一缩放。
static float g_scale = 1.0f;      // DPI 缩放，卡片按它走
static float g_dotK  = 1.0f;      // 圆点额外放大倍数，按屏幕大小定
static float dotUnit() { return g_scale * g_dotK; }   // 圆点坐标系 -> 设备像素

// 没填预估用时的任务没有进度可算：环形改成紫色呼吸，行内填充固定 60%
static bool hasEstimate(const Task& t) { return t.estimate > 0; }

// 呼吸系数 0..1，1.8 秒一轮。根据当前时刻计算，不累积状态，因此结果不受帧率波动影响
static REAL breatheK() {
    REAL ph = (REAL)(GetTickCount() % 1800) / 1800.0f;
    return 0.5f - 0.5f * cosf(ph * 6.2831853f);
}

// 存在未填写预估时长且正在进行的任务时，呼吸动画需持续更新，定时器保持运行
static bool needBreath() {
    for (size_t i = 0; i < g_tasks.size(); ++i) {
        if (g_tasks[i].startedAt > 0 && g_tasks[i].estimate <= 0) return true;
    }
    return false;
}

static bool    g_progSlow  = false;   // 定时器已经降到呼吸用的慢档
static bool    g_progOn    = false;   // 定时器在跑，别重复叫

// 动画启动时将刷新频率恢复至 60 帧/秒，随后由定时器切换至低频模式。
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

// 离屏位图尺寸。展开态是卡片大小，收起态是“圆点 + 窄条”，
// 高 DPI 下窄条可能宽于卡片；离屏位图按较大尺寸分配，避免窄条右侧被裁切。
static int   g_surfW = WIN_W;
static int   g_surfH = WIN_H;

// 展开那一刻圆点在哪。卡片贴边时会翻到另一侧，收起要回到圆点原位，
// 否则反复开合后圆点位置会逐渐偏移。
static int   g_dotX = 0;
static int   g_dotY = 0;

// 窄条当前宽度（设备像素）；TIMER_SLIDE 逐帧趋近 g_pillTarget，实现平滑展开与收起。
static int   g_pillW      = 0;
static int   g_pillFull   = 0;      // 完全展开时的宽度，0 表示没有可显示的任务
static int   g_pillTarget = 0;
static bool  g_pillWant   = false;  // 鼠标停在圆点上（用于控制黄色任务窄条的显示）
static bool  g_pillHover  = false;  // 鼠标停在窄条上，亮一点，提示可以点

// 外圈展开程度 0..1，和窄条共用 TIMER_SLIDE，动画同步。
static REAL  g_ringT      = 0.0f;
static REAL  g_ringTarget = 0.0f;

// 底栏就地输入框：点击“添加任务”进入，Enter 提交，Esc 取消，无需打开控制台。
static bool    g_editing  = false;
static wstring g_editText;
static int     g_caret    = 0;       // 光标位置，0..g_editText.size()
static bool    g_caretOn  = false;

// 日/夜配色：跟 Windows 的“应用模式”走。
// 日间用浅底 + 暖色渐变，夜间用纯灰块 + 外发光。
static bool    g_dayMode  = false;

// 置顶模式使窗口保持在普通窗口上方。owned 窗口不支持 topmost，因此需移除 owner 关联。
static bool    g_topmost  = false;
static bool    g_hiddenForFullscreen = false;

// 托盘点开的临时会话：展开 + 置顶，2 秒没交互就收回，收回后恢复原状
static bool    g_trayOpen = false;
static DWORD   g_trayIdle = 0;
static bool    g_trayWasEnabled = false;   // 打开前清单是开着还是关着

// 收起状态下记录按下位置；仅当位移超过阈值时判定为拖动，否则按单击处理。
// 圆点与窄条均可拖动，同时需保留单击展开，因此以位移区分两者。
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

// 自定义预估：弹层里正在编辑的 日 / 时 / 分
static bool    g_pickingEst = false;
static int     g_estDay = 0, g_estHour = 1, g_estMinute = 0;
static int     g_editEstCustom = 60;     // 确定下来的自定义分钟数
static int     g_estEditCol = -1;        // 正在直接输入的那一列：0 日 1 时 2 分，-1 没有
static wstring g_estEditText;            // 输入中的数字

// 关联材料：任务链接或本地文件
static std::vector<wstring> g_editLinks;
static wstring g_editUrl;                // 网址，只允许一条
static bool    g_linkFocus = false;      // 网址输入框是否处于输入状态
static bool    g_pickMenu = false;       // “选择”的两行菜单是否展开

// 右键菜单展开期间暂停悬浮清单的置顶调整，避免菜单被遮挡
static bool    g_menuOpen = false;

// 卡片失焦时收起；未提交输入暂存为草稿，以便再次打开时恢复
struct EditDraft {
    bool valid = false;
    wstring title, customDue, url;
    int due = 4, est = -1;
    int caret = 0;
    std::vector<wstring> links;
};
static EditDraft g_draft;

// 进度条动画：g_progAnim 从 0 递增至 1，仅作用于刚启动的任务（g_progId）
static REAL    g_progAnim  = 1.0f;
static DWORD   g_progStart = 0;
static wstring g_progId;
static bool    g_progAll = false;   // 为真时所有进度条都吃出现动画（展开清单时用）
static REAL    g_dotRingA  = 0.0f;    // 圆点外那圈环的透明度
static bool    g_dotRingOn = false;   // 上一次算出来的目标状态，用来抓“刚重新出现”

// 预估时长：三档常用的 + 自定义 + 不设。自定义走弹层，能填 日/时/分
static const int    EST_MIN[3] = { 30, 60, 120 };
static const wchar_t* EST_LABEL[5] = { L"30分", L"1时", L"2时", L"自定义", L"不设" };
static const int    EST_CHIPS = 5;
static const int    EST_CUSTOM = 3;          // “自定义”那一格的下标
static const wchar_t* DUE_LABEL[5] = { L"今天", L"明天", L"后天", L"自定义", L"不设" };

// 任务标记完成后，该行向右滑出并淡出，后续任务行依次补位。
// g_leaveRow 是它在新列表里原来的位置。
static bool    g_leaving    = false;
static Task    g_leaveTask;
static int     g_leaveRow   = -1;
static DWORD   g_leaveStart = 0;

