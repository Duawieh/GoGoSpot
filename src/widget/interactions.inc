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

// 交互结束后补一次刷新：勾选、拖动、唤起、菜单操作都不必等下一个定时器。
// 投递一条消息排队，等当前处理函数返回后再跑，不会卡住交互本身。
static void refreshAfterInteraction() {
    if (!g_hwnd || g_refreshPending) return;
    g_refreshPending = true;
    PostMessageW(g_hwnd, WM_APP_REFRESH, 0, 0);
}

