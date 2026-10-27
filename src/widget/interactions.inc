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

