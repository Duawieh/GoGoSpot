// 命中区自检：行尾置顶、正在进行两个按钮的圆心，与 hitPin / hitProgress 的判断是否一致。
// 直接包含 widget.cpp 编译，只跑断言，不建窗口。
#include "../src/widget.cpp"

void appSetWidgetEnabled(bool) {}
void appQuit() {}
bool appTopmostEnabled() { return false; }
void appSetTopmost(bool) {}

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

    wprintf(L"\n%s\n", bad == 0 ? L"全部通过" : L"有失败项");
    return bad;
}
