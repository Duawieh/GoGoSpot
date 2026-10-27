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
static const UINT_PTR TIMER_TICK     = 11;   // 任务计时期间每秒重绘，以更新已用时

// 后台改设置后，通过这个自定义消息通知窗口（可跨线程 Post）
static const UINT WM_APP_WIDGET_ENABLED = WM_APP + 1;
static const UINT WM_APP_TOPMOST        = WM_APP + 2;
static const UINT WM_APP_TRAY_OPEN      = WM_APP + 3;

// 部分 Windows SDK 未定义该常量，缺失时在此补充
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

// 收起状态下窗口只有这么大，中间画一个圆点当锚点
static const int DOT_SIZE = 64;

// 圆点右边那条窄条，只写最急的一条：红色常驻，黄色鼠标悬停时展开。
static const int PILL_X   = 50;     // 起点，正好让开圆点那圈柔光
static const int PILL_H   = 40;
static const int PILL_W   = 236;    // 上限宽度，长标题到这里就省略号
static const int PILL_MIN = 96;     // 短标题仍保持此最小宽度；剩余空间不足时隐藏窄条

// 外圈半径：常态 RING_R_IDLE，鼠标停上来涨到 RING_R，和窄条共用一套动画。
static const REAL RING_R      = PILL_H / 2.0f;   // 悬停时：直径正好等于窄条高度
static const REAL RING_R_IDLE = 16.0f;   // 常态

// 紧急分级：剩余 <= min(预估, 12h) 红色，24h 内黄色。
static const long long URG_RED_CAP_MS = 12LL * 3600000LL;
static const long long URG_YELLOW_MS  = 24LL * 3600000LL;

