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
    // 缺少“#待办 总数”行表示后台返回了错误 JSON，不能按空列表处理
    g_loadError = !sawHeader;

    // 数据首次到达：存在未填预估的进行中任务时立即恢复呼吸动画，不等下次刷新
    if (needBreath() && !g_progOn) kickProg();

    // 存在进行中任务时每秒重绘，以更新卡片中的“已进行”时长和窄条倒计时
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

