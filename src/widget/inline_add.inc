// ---- 就地添加 ----

// 悬浮清单使用 NOACTIVATE 样式；接收键盘输入前需关联前台线程并调用 SetForegroundWindow。
// 刚取得焦点时系统会短暂反复切换（前台锁未解除、其他线程重新取回焦点），
// 该抖动不应视为用户离开焦点，否则输入框刚打开即被提交，无法输入
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
        // 解除输入队列关联后，原前台线程可能重新获得焦点；此处再次设置前台窗口
        SetFocus(g_hwnd);
    }
}

// 有模态窗口开着（文件选择框、确认框、右键菜单）时鼠标必然在卡片外面，
// 模态窗口打开期间忽略鼠标离开触发的收起逻辑，避免悬浮清单在对话框后方收起。
static int g_modalDepth = 0;

struct ModalScope {
    ModalScope() { ++g_modalDepth; }
    ~ModalScope() { --g_modalDepth; }
};

// 这两个 GUID 在 libuuid 里，为了不给编译命令加 -luuid，直接写死
static const CLSID kFileOpenDialogClsid =
    {0xDC1C5A9C, 0xE88A, 0x4dde, {0xA5, 0xA1, 0x60, 0xF8, 0x2A, 0x20, 0xAE, 0xF7}};
static const IID kFileOpenDialogIid =
    {0xd57c7288, 0xd4ad, 0x4768, {0xbe, 0x02, 0x9d, 0x96, 0x95, 0x32, 0xd9, 0x60}};

// 系统选择框：files=true 时选择文件（支持多选），false 时选择文件夹；返回完整本地路径
static void pickLink(bool files) {
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(kFileOpenDialogClsid, nullptr, CLSCTX_ALL,
                                kFileOpenDialogIid, (void**)&dlg)) || !dlg) {
        if (SUCCEEDED(hrInit)) CoUninitialize();
        return;
    }

    DWORD opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM | FOS_ALLOWMULTISELECT;
    opts |= files ? FOS_FILEMUSTEXIST : FOS_PICKFOLDERS;
    dlg->SetOptions(opts);
    dlg->SetTitle(files ? L"选择要打开的文件" : L"选择文件夹");

    bool ok;
    {
        ModalScope ms;
        ok = SUCCEEDED(dlg->Show(g_hwnd));
    }
    if (ok) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items)) && items) {
            DWORD n = 0;
            items->GetCount(&n);
            for (DWORD i = 0; i < n && g_editLinks.size() < 8; ++i) {
                IShellItem* it = nullptr;
                if (FAILED(items->GetItemAt(i, &it)) || !it) continue;
                PWSTR p = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                    wstring one = p;
                    bool dup = false;
                    for (const auto& x : g_editLinks) if (x == one) dup = true;
                    if (!dup) g_editLinks.push_back(one);
                    CoTaskMemFree(p);
                }
                it->Release();
            }
            items->Release();
        }
    }

    dlg->Release();
    if (SUCCEEDED(hrInit)) CoUninitialize();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// 从剪贴板取，按行拆，一行一个
// 网址输入框里的 Ctrl+V：只取第一行，去首尾空白，最多 200 字
static void pasteLinkUrl() {
    bool ok = false;
    for (int i = 0; i < 5 && !ok; ++i) ok = OpenClipboard(g_hwnd) != FALSE;
    if (!ok) return;

    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* p = (const wchar_t*)GlobalLock(h);
        if (p) {
            wstring text = p;
            GlobalUnlock(h);
            size_t nl = text.find_first_of(L"\r\n");
            if (nl != wstring::npos) text = text.substr(0, nl);
            size_t a = text.find_first_not_of(L" \t");
            size_t b = text.find_last_not_of(L" \t");
            text = (a == wstring::npos) ? wstring() : text.substr(a, b - a + 1);
            if (text.size() > 200) text = text.substr(0, 200);
            g_editUrl = text;
            g_caretOn = true;
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

// 网址交给默认浏览器；本地路径不按关联程序打开，直接在资源管理器里选中。
// “开始工作”操作应在资源管理器中定位本地材料，而非通过关联程序打开文档。
static void openLinks(const std::vector<wstring>& links) {
    for (const wstring& one : links) {
        if (one.empty()) continue;
        bool web = one.rfind(L"http://", 0) == 0 || one.rfind(L"https://", 0) == 0;
        if (web) {
            ShellExecuteW(nullptr, L"open", one.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else {
            wstring arg = L"/select,\"" + one + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
        }
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
    g_pickingEst = false;
    g_estEditCol = -1;
    g_estEditText.clear();
    g_editLinks.clear();
    g_editUrl.clear();
    g_linkFocus = false;
    g_pickMenu = false;
    if (g_draft.valid) restoreEdit();        // 上次没收完的，接着填
    g_caretOn = true;
    KillTimer(g_hwnd, TIMER_COLLAPSE);       // 正在打字，别收起
    SetTimer(g_hwnd, TIMER_CARET, 530, nullptr);
    grabKeyboardFocus();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// 今天往后 addDays 天的 23:59，格式和 due 字段一致。
// 使用 FILETIME 加减天数，跨月跨年无需自行计算，也不受时区影响。
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

// 收起时保存未提交输入，并在再次打开时恢复
static void stashEdit() {
    g_draft.valid = true;
    g_draft.title = g_editText;
    g_draft.customDue = g_customDue;
    g_draft.url = g_editUrl;
    g_draft.due = g_editDue;
    g_draft.est = g_editEst;
    g_draft.caret = g_caret;
    g_draft.links = g_editLinks;
}

static void restoreEdit() {
    g_editText = g_draft.title;
    g_customDue = g_draft.customDue;
    g_editUrl = g_draft.url;
    g_editDue = g_draft.due;
    g_editEst = g_draft.est;
    g_caret = g_draft.caret;
    g_editLinks = g_draft.links;
}

// commit=true 时提交（空标题不提交）；keepDraft=true 时将输入保存为草稿
static void finishEdit(bool commit, bool keepDraft) {
    if (!g_editing) return;

    wstring title = g_editText;
    size_t a = title.find_first_not_of(L" \t\r\n");
    size_t b = title.find_last_not_of(L" \t\r\n");
    title = (a == wstring::npos) ? wstring() : title.substr(a, b - a + 1);

    bool willCommit = commit && !title.empty();
    if (!willCommit && keepDraft) stashEdit();   // 收起或空标题提交失败时保留草稿
    else g_draft.valid = false;                  // 提交成功或明确取消：作废

    g_editing = false;
    KillTimer(g_hwnd, TIMER_CARET);
    g_editText.clear();

    if (willCommit) {
        if (g_port == 0) findServerPort();
        if (g_port != 0) {
            wstring path = L"/api/add?title=" + urlEncode(wtou8(title));
            if (g_editDue >= 0 && g_editDue < 3)
                path += L"&due=" + urlEncode(wtou8(dueAfterDays(g_editDue)));
            else if (g_editDue == 3 && !g_customDue.empty())
                path += L"&due=" + urlEncode(wtou8(g_customDue));

            // 将选取的文件与网址合并至 links；无效网址不加入
            std::vector<wstring> all = g_editLinks;
            if (!g_editUrl.empty() && looksLikeLinkW(g_editUrl)) all.push_back(g_editUrl);
            if (!all.empty()) {
                wstring lk;
                for (const wstring& one : all) {
                    if (!lk.empty()) lk += L'|';
                    lk += urlEncode(wtou8(one));
                }
                path += L"&links=" + lk;
            }
            int estMin = 0;
            if (g_editEst >= 0 && g_editEst < EST_CUSTOM) estMin = EST_MIN[g_editEst];
            else if (g_editEst == EST_CUSTOM)             estMin = g_editEstCustom;
            if (estMin > 0) path += L"&estimate=" + std::to_wstring(estMin);
            string body;
            httpRequest(L"POST", path, body);

            // 新任务通常没有截止时间并排在列表末尾；滚动至底部以显示新任务
            g_scroll = 0x7fffffff;
        }
        refreshAsync();
    }

    g_customDue.clear();
    g_pickingDate = false;
    g_pickingEst = false;
    g_estEditCol = -1;
    g_estEditText.clear();
    g_editLinks.clear();
    g_editUrl.clear();
    g_linkFocus = false;
    g_pickMenu = false;

    // 光标位于窗口外时启动收起计时器。
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

// 推一帧：窄条宽度与外圈大小使用相同插值系数趋近目标值。
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
        // 浮点运算存在误差，接近目标值即视为到位，否则计时器无法停止。
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

                // 补充检测展开后鼠标直接移出窗口的情况，以处理系统未发送离开事件的场景
                // （隐藏窗口抓取背景、切换 DPI 都会丢失该追踪），此处按真实光标位置补充判断。
                // 托盘临时会话使用独立计时器；编辑、拖动或模态对话框期间不触发收起。
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
                // 更新一帧动画后再绘制，确保最终帧到达目标位置
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
                    kickProg(h);   // 窄条到达目标宽度后恢复圆点外环
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

                // 窄条展开时淡出圆点外环，收起时恢复外环
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
                // 窄条仍在滑动时不停止计时：g_pillW 需滑动结束后才归零，
                // 定时器需等待窄条收回完成后再停止，否则圆点外环无法恢复
                bool settling = (g_progAnim < 1.0f) || fabs(g_dotRingA - target) >= 0.02f ||
                                g_pillW != g_pillTarget;
                if (settling) {
                    if (g_progSlow) { SetTimer(h, TIMER_PROG, 16, nullptr); g_progSlow = false; }
                } else if (needBreath()) {
                    // 仅剩紫色呼吸：25 帧足够，避免静止卡片持续全速重绘
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
                // 进度按绝对时间计算，不随帧率变化
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
                // 收起状态仍需刷新任务数据，以更新圆点右侧的窄条
                refreshAsync();
                // 在控制台启动的任务也需在悬浮清单中启动呼吸动画
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
                applyTopmost();          // 同时恢复窗口层级，避免下次显示时仍保持置顶
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
                // 位移超过 4px 时判定为拖动，否则按单击处理
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

                // 鼠标悬停时仅展开窄条；单击窄条时展开卡片。
                // 判定只认圆点本身和已经伸出来的窄条：透明角落、圆环都不算。
                // 外圈会随动画扩张；若将其纳入命中范围，边界变化会导致命中状态反复切换。
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
                // 拖动卡片时同步更新圆点位置，使收起后圆点返回卡片当前左上角，
                // 否则卡片移动后圆点会留在原位。此处保存窗口原点而非可见圆角，
                // 这样“展开 → 收起”来回几趟位置也不会一点点漂。
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

            // 自定义预估里点开的那一格：只收数字，打满两位自动收下并跳到下一格
            if (g_pickingEst && g_estEditCol >= 0) {
                if (ch == 8) {
                    if (!g_estEditText.empty()) g_estEditText.pop_back();
                } else if (ch >= L'0' && ch <= L'9') {
                    if (g_estEditText.size() < 2) g_estEditText += ch;
                    if (g_estEditText.size() == 2) {
                        int col = g_estEditCol;
                        commitEstEdit();
                        if (col < 2) beginEstEdit(col + 1);
                    }
                } else {
                    return 0;
                }
                g_caretOn = true;
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }

            // 网址输入框：只收可见字符和退格
            if (g_linkFocus) {
                if (ch == 8) {
                    if (!g_editUrl.empty()) g_editUrl.pop_back();
                } else if (ch >= 32 && ch != 127) {
                    if (g_editUrl.size() < 200) g_editUrl += ch;
                } else {
                    return 0;
                }
                g_caretOn = true;
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }

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

            // 正在直接输入某一格：Enter收下、Tab 跳到下一格、Esc 放弃这次输入
            if (g_pickingEst && g_estEditCol >= 0) {
                if (w == VK_RETURN || w == VK_TAB) {
                    int col = g_estEditCol;
                    commitEstEdit();
                    if (w == VK_TAB && col < 2) beginEstEdit(col + 1);
                    InvalidateRect(h, nullptr, FALSE);
                    return 0;
                }
                if (w == VK_ESCAPE) {
                    g_estEditCol = -1;
                    g_estEditText.clear();
                    InvalidateRect(h, nullptr, FALSE);
                    return 0;
                }
            }

            // 网址输入框：Ctrl+V 粘一条，Enter跳到标题，Esc 离开输入框
            if (g_linkFocus) {
                if (w == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) { pasteLinkUrl(); return 0; }
                if (w == VK_RETURN || w == VK_ESCAPE) {
                    g_linkFocus = false;
                    g_caretOn = true;
                    InvalidateRect(h, nullptr, FALSE);
                    return 0;
                }
            }

            if (w == VK_ESCAPE) {
                // 存在弹层时先关闭弹层，避免直接丢弃整条输入
                if (g_pickMenu) {
                    g_pickMenu = false;
                    InvalidateRect(h, nullptr, FALSE);
                    return 0;
                }
                if (g_pickingEst) {
                    g_pickingEst = false;
                    g_estEditCol = -1;
                    g_estEditText.clear();
                    InvalidateRect(h, nullptr, FALSE);
                    return 0;
                }
                if (g_pickingDate) {
                    g_pickingDate = false;
                    g_editDue = g_customDue.empty() ? 4 : 3;
                    InvalidateRect(h, nullptr, FALSE);
                    return 0;
                }
                finishEdit(false);
                return 0;
            }
            if (w == VK_RETURN) { finishEdit(true, true); return 0; }   // 空标题不提交，保留草稿

            // Ctrl+V 由本程序读取剪贴板：该窗口无编辑控件，系统不会代为处理
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

        // 输入法组字窗跟随输入框，否则候选框会定位到窗口左上角
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
            // 焦点转移到其他位置时按提交处理；刚取得焦点时的抖动除外。
            // 右键菜单会临时抢前台，那种失焦不算离开输入
            if (g_editing && !g_menuOpen && GetTickCount() - g_focusAt > 250)
                finishEdit(true, true);
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
                // 释放事件仍可能落在圆点区域，因此圆点需纳入点击判定，以保持拖动与单击行为一致
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
                // × 仅收起卡片，不关闭悬浮清单：圆点保留，单击即可重新展开
                if (btn == 1) { collapseWidget(); return 0; }
                if (btn == 4) { finishEdit(true, true); toggleTopmost(); return 0; }

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

                if (g_pickingEst) {
                    int hit = hitEstPicker(cx, cy);
                    if (hit == 1) {
                        g_estEditCol = -1;
                        g_estEditText.clear();
                        g_pickingEst = false;
                    } else if (hit == 2) {
                        commitEstEdit();
                        applyEstPicker();
                    } else if (hit >= 30) {                  // 点数字：直接输入
                        commitEstEdit();
                        beginEstEdit(hit - 30);
                    } else if (hit >= 20) {                  // 加
                        commitEstEdit();
                        int i = hit - 20;
                        if (i == 0)      g_estDay = (g_estDay + 1) % 70;
                        else if (i == 1) g_estHour = (g_estHour + 1) % 24;
                        else             g_estMinute = (g_estMinute + 5) % 60;
                    } else if (hit >= 10) {                  // 减
                        commitEstEdit();
                        int i = hit - 10;
                        if (i == 0)      g_estDay = (g_estDay + 69) % 70;
                        else if (i == 1) g_estHour = (g_estHour + 23) % 24;
                        else             g_estMinute = (g_estMinute + 55) % 60;
                    }
                    InvalidateRect(h, nullptr, FALSE);
                    return 0;
                }

                // 菜单展开期间捕获点击：单击菜单项执行操作，单击其他区域关闭菜单
                if (g_pickMenu) {
                    RectF mf = pickMenuFile(), md = pickMenuFolder();
                    bool onFile = inRect(cx, cy, mf.X, mf.Y, mf.Width, mf.Height);
                    bool onDir  = inRect(cx, cy, md.X, md.Y, md.Width, md.Height);
                    g_pickMenu = false;
                    InvalidateRect(h, nullptr, FALSE);
                    if (onFile) { g_linkFocus = false; pickLink(true); }
                    else if (onDir) { g_linkFocus = false; pickLink(false); }
                    return 0;
                }

                if (hitCancel(cx, cy)) { finishEdit(false); return 0; }

                int linkHit = hitLinkRow(cx, cy);
                if (linkHit == 1) {
                    g_linkFocus = false;
                    g_pickMenu = !g_pickMenu;
                    InvalidateRect(h, nullptr, FALSE);
                    return 0;
                }
                if (linkHit == 2) {
                    g_linkFocus = true;
                    g_caretOn = true;
                    InvalidateRect(h, nullptr, FALSE);
                    return 0;
                }
                if (linkHit == 3) {
                    g_editLinks.clear();
                    g_editUrl.clear();
                    g_linkFocus = false;
                    InvalidateRect(h, nullptr, FALSE);
                    return 0;
                }
                g_linkFocus = false;             // 点到别处就离开网址输入框

                int chip = hitChip(cx, cy);
                if (chip >= 0) {
                    if (chip < 100) {                        // 截止那一排
                        if (chip == 3) openCalendar();       // 自定义：开日历
                        else g_editDue = chip;
                    } else {                                 // 预估那一排
                        int i = chip - 100;
                        if (i == EST_CUSTOM) openEstPicker();
                        else if (i == EST_CHIPS - 1) g_editEst = -1;   // 不设
                        else g_editEst = i;
                    }
                    InvalidateRect(h, nullptr, FALSE);
                    return 0;
                }

                if (!hitInput(cx, cy)) finishEdit(true, true);   // 输入框外单击时提交；标题为空时保留草稿
                return 0;
            }

            if (btn == 1) { collapseWidget(); return 0; }   // 隐藏卡片并保留圆点；通过托盘菜单关闭悬浮清单
            if (btn == 4) { toggleTopmost(); return 0; }
            if (btn == 2) { startEdit(); return 0; }
            if (btn == 3) { openFullApp(); return 0; }

            int row = hitRow(cx, cy);
            if (row >= 0 && !g_leaving) {          // 动画期间禁用任务行命中，避免位置变化导致误操作
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
            // 右键落在某条任务上给这条任务的操作，其余位置给悬浮清单菜单
            int rmx = GET_X_LPARAM(l), rmy = GET_Y_LPARAM(l);
            int rcx, rcy;
            toCard(rmx, rmy, rcx, rcy);

            int row = g_expanded ? hitRow(rcx, rcy) : -1;
            int idx = (row >= 0) ? g_scroll + row : -1;
            bool onTask = (idx >= 0 && idx < (int)g_tasks.size());

            wstring taskId;
            bool pinned = false;
            MenuItemDef items[5];
            int n = 0;
            if (onTask) {
                taskId = g_tasks[idx].id;
                pinned = g_tasks[idx].pinned;
                items[n++] = { pinned ? L"取消置顶" : L"置顶", 12, false };
                items[n++] = { L"推迟一天", 10, false };
                items[n++] = { L"删除任务", 11, true };
                items[n++] = { L"打开控制台", 1, false };
            } else {
                items[n++] = { L"打开控制台", 1, false };
                items[n++] = { L"添加任务", 2, true };
                items[n++] = { L"刷新", 4, true };
                items[n++] = { L"关闭悬浮清单", 5, false };
            }

            POINT pt;
            GetCursorPos(&pt);
            int cmd = widgetPopupMenu(pt.x, pt.y, items, n);

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
                case 12: pinTask(taskId, !pinned); break;
                default: break;
            }
            return 0;
        }

        case WM_SYSCOMMAND:
            // 不随“显示桌面”最小化，否则 Win+D 后无法找到。
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
            // 输入时窗口持有键盘焦点，按下 Alt+F4 时仅应收起悬浮清单，
            // 不能把整个程序带走（程序退出走的是托盘菜单）
            collapseWidget();
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

