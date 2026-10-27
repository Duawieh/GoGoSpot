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

