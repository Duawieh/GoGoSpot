// 截图工具：按标题找悬浮清单窗口，可选先投一条消息，再把窗口抓成 PNG。
// 只读窗口像素，不参与正式构建。用法：cap.exe <标题片段> <输出.png> [消息号] [等待毫秒]
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <cstdio>

using namespace Gdiplus;

static int pngClsid(CLSID* clsid) {
    UINT n = 0, size = 0;
    GetImageEncodersSize(&n, &size);
    if (size == 0) return -1;
    std::vector<BYTE> buf(size);
    ImageCodecInfo* info = (ImageCodecInfo*)buf.data();
    GetImageEncoders(n, size, info);
    for (UINT i = 0; i < n; ++i) {
        if (wcscmp(info[i].MimeType, L"image/png") == 0) { *clsid = info[i].Clsid; return (int)i; }
    }
    return -1;
}

struct FindCtx { const wchar_t* want; HWND found; };

static BOOL CALLBACK enumProc(HWND h, LPARAM p) {
    wchar_t title[256] = {0};
    GetWindowTextW(h, title, 255);
    FindCtx* c = (FindCtx*)p;
    if (wcsstr(title, c->want)) { c->found = h; return FALSE; }
    return TRUE;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { wprintf(L"用法: cap.exe <标题片段> <输出.png> [消息号] [等待毫秒]\n"); return 2; }

    // 不声明 DPI 感知的话，GetWindowRect 拿到的是虚拟化后的尺寸，右边会被裁掉
    typedef BOOL (WINAPI *SetCtxFn)(HANDLE);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    SetCtxFn setCtx = u ? (SetCtxFn)GetProcAddress(u, "SetProcessDpiAwarenessContext") : nullptr;
    if (setCtx) setCtx((HANDLE)-4); else SetProcessDPIAware();

    GdiplusStartupInput in;
    ULONG_PTR token = 0;
    GdiplusStartup(&token, &in, nullptr);

    FindCtx c{argv[1], nullptr};
    EnumWindows(enumProc, (LPARAM)&c);
    if (!c.found) { wprintf(L"没找到窗口\n"); return 1; }

    if (argc >= 4) {
        UINT msg = (UINT)_wtoi(argv[3]);
        if (msg) PostMessageW(c.found, msg, 0, 0);
    }
    int wait = (argc >= 5) ? _wtoi(argv[4]) : 500;
    Sleep(wait);

    RECT wr{};
    GetWindowRect(c.found, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    if (w <= 0 || h <= 0) { wprintf(L"窗口尺寸异常\n"); return 1; }

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP hbmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, hbmp);

    // 6 号参数给 1 就改成直接抄屏幕：PrintWindow 要等目标线程处理 WM_PRINT，
    // 分层窗口在狂重绘的时候会卡住，抄屏幕没这个问题（窗口得在最上层）
    int fromScreen = (argc >= 7) ? _wtoi(argv[6]) : 0;
    BOOL ok;
    if (fromScreen) {
        ok = BitBlt(mem, 0, 0, w, h, screen, wr.left, wr.top, SRCCOPY | CAPTUREBLT);
    } else {
        ok = PrintWindow(c.found, mem, 2);           // 2 = PW_RENDERFULLCONTENT
    }
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    if (!ok) { wprintf(L"抓图失败\n"); return 1; }

    Bitmap src(hbmp, nullptr);
    int scale = (argc >= 6) ? _wtoi(argv[5]) : 1;
    if (scale < 1) scale = 1;

    CLSID clsid;
    Status st = GenericError;
    if (pngClsid(&clsid) < 0) {
        wprintf(L"没有 PNG 编码器\n");
    } else if (scale == 1) {
        st = src.Save(argv[2], &clsid, nullptr);
    } else {
        // 圆点只有 70px，放大几倍才看得出呼吸的深浅
        Bitmap big(w * scale, h * scale, PixelFormat32bppARGB);
        Graphics og(&big);
        og.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        og.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        og.DrawImage(&src, Rect(0, 0, w * scale, h * scale), 0, 0, w, h, UnitPixel);
        st = big.Save(argv[2], &clsid, nullptr);
    }
    wprintf(L"%s %dx%d -> %s\n", st == Ok ? L"OK" : L"FAIL", w, h, argv[2]);

    DeleteObject(hbmp);
    GdiplusShutdown(token);
    return st == Ok ? 0 : 1;
}
