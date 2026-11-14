// 命中区自检：行尾置顶、正在进行两个按钮的圆心，与 hitPin / hitProgress 的判断是否一致。
// 直接包含 widget.cpp 编译，只跑断言，不建窗口。
#include "../src/widget.cpp"

// 外部接口的空实现：widget.cpp 只用到这几个，桩住就能单独链接
void appSetWidgetEnabled(bool) {}
void appQuit() {}
bool appTopmostEnabled() { return false; }
void appSetTopmost(bool) {}
int appThemeMode() { return 2; }
bool appOpenLocalPath(const std::wstring&) { return false; }
const wchar_t* serverDataDir() { return L"."; }

int wmain() {
    // paintRow 里画出来的两个圆心（卡片坐标：toCard 已经减掉 SHADOW）
    const int right = 6 + (CARD_W - 12);
    const int pinCx = right - 45;
    const int playCx = right - 19;
    const int cy = LIST_TOP + ROW_H / 2;

    int bad = 0;
    auto ck = [&](const wchar_t* what, bool got, bool want) {
        wprintf(L"%s  %s\n", got == want ? L"PASS" : L"FAIL", what);
        if (got != want) ++bad;
    };

    ck(L"点圆心 = 命中置顶", hitPin(pinCx, cy, 0), true);
    ck(L"圆心左 20px = 不命中置顶", hitPin(pinCx - 20, cy, 0), false);
    ck(L"圆心右 20px = 不命中置顶", hitPin(pinCx + 20, cy, 0), false);
    ck(L"点圆心 = 命中正在进行", hitProgress(playCx, cy, 0), true);
    ck(L"圆心左 20px = 不命中正在进行", hitProgress(playCx - 20, cy, 0), false);
    ck(L"圆心右 20px = 不命中正在进行", hitProgress(playCx + 20, cy, 0), false);
    ck(L"置顶那一格不抢正在进行", hitProgress(pinCx, cy, 0), false);
    ck(L"正在进行那一格不抢置顶", hitPin(playCx, cy, 0), false);
    ck(L"行首不命中任何一个", hitPin(60, cy, 0) || hitProgress(60, cy, 0), false);

    // 编辑面板三排 chip：返回 row*100 + 格号，三排各自独立不串行
    g_editing = true;
    auto centerOf = [](int row, int i, int count, int& ox, int& oy) {
        RectF r = chipRect(row, i, count);
        ox = (int)(r.X + r.Width / 2);
        oy = (int)(r.Y + r.Height / 2);
    };

    int mx = 0, my = 0;
    centerOf(0, 1, TYPE_CHIPS, mx, my);
    ck(L"类型行第二格 = 命中 1", hitChip(mx, my) == 1, true);
    centerOf(0, 0, TYPE_CHIPS, mx, my);
    ck(L"类型行第一格 = 命中 0", hitChip(mx, my) == 0, true);

    centerOf(1, 3, 5, mx, my);
    ck(L"时间行第四格 = 命中 103", hitChip(mx, my) == 103, true);
    centerOf(1, 0, 5, mx, my);
    ck(L"时间行第一格 = 命中 100", hitChip(mx, my) == 100, true);

    centerOf(2, 0, EST_CHIPS, mx, my);
    ck(L"预估行第一格 = 命中 200", hitChip(mx, my) == 200, true);
    centerOf(2, EST_CHIPS - 1, EST_CHIPS, mx, my);
    ck(L"预估行末格（不设）= 命中最后一格", hitChip(mx, my) == 200 + EST_CHIPS - 1, true);

    // 三排之间的空隙不该命中任何一格
    ck(L"类型行上方的空白不命中", hitChip(mx, (int)panelTypeTop() - 12) < 0, true);
    ck(L"类型行与时间行之间不命中",
       hitChip(mx, (int)(panelTypeTop() + CHIP_H + 4)) < 0, true);
    g_editing = false;

    wprintf(L"\n%s\n", bad == 0 ? L"全部通过" : L"有失败项");
    return bad;
}
