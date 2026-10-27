// ---- 重复任务 ----

static bool validDue(const string& s);
static string newTaskId();

// 把 "YYYY-MM-DDTHH:mm" 往后推一个周期。rep 是 daily/weekly/biweekly/monthly
static string nextDueOf(const string& due, const string& rep) {
    if (!validDue(due)) return "";
    long long ms = dueToUnixMs(due);
    if (ms <= 0) return "";

    if (rep == "daily" || rep == "weekly" || rep == "biweekly") {
        int days = (rep == "daily") ? 1 : (rep == "weekly" ? 7 : 14);
        ms += (long long)days * 86400000LL;
    } else if (rep == "monthly") {
        // 走一遍本地时间，按自然月加，月底不足就夹到当月最后一天
        ULARGE_INTEGER u;
        u.QuadPart = (ULONGLONG)(ms + 11644473600000LL) * 10000ULL;
        FILETIME ft;
        ft.dwLowDateTime = u.LowPart;
        ft.dwHighDateTime = u.HighPart;

        SYSTEMTIME utc{};
        FileTimeToSystemTime(&ft, &utc);
        SYSTEMTIME loc{};
        if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &loc)) return "";

        loc.wMonth = (WORD)(loc.wMonth + 1);
        if (loc.wMonth > 12) { loc.wMonth = 1; loc.wYear = (WORD)(loc.wYear + 1); }
        int dim = daysOfMonth(loc.wYear, loc.wMonth);
        if (loc.wDay > dim) loc.wDay = (WORD)dim;

        SYSTEMTIME back{};
        if (!TzSpecificLocalTimeToSystemTime(nullptr, &loc, &back)) return "";
        FileTimeToSystemTime(&ft, &utc);          // 只为复用 ft
        SystemTimeToFileTime(&back, &ft);
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        ms = (long long)(u.QuadPart / 10000ULL) - 11644473600000LL;
    } else {
        return "";
    }

    // ms -> 本地 "YYYY-MM-DDTHH:mm"
    ULARGE_INTEGER u2;
    u2.QuadPart = (ULONGLONG)(ms + 11644473600000LL) * 10000ULL;
    FILETIME ft2;
    ft2.dwLowDateTime = u2.LowPart;
    ft2.dwHighDateTime = u2.HighPart;
    SYSTEMTIME utc2{}, loc2{};
    FileTimeToSystemTime(&ft2, &utc2);
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc2, &loc2)) return "";

    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d",
             loc2.wYear, loc2.wMonth, loc2.wDay, loc2.wHour, loc2.wMinute);
    return buf;
}

static string repeatOf(const JVal& t) {
    JVal* r = const_cast<JVal&>(t).get("repeat");
    return (r && r->kind == JVal::Str) ? r->str : string();
}

static string dueOf(const JVal& t) {
    JVal* d = const_cast<JVal&>(t).get("due");
    return (d && d->kind == JVal::Str) ? d->str : string();
}

// 清掉原任务的重复标记，保证同一条只派生一次
static void clearRepeat(JVal& t) {
    JVal* r = t.get("repeat");
    if (r) { r->kind = JVal::Str; r->str.clear(); }
}

// 复制一份当下一期：标题、标签、链接、预估、重复规则都跟着走，只换 id 和日期。
// 复制和清标记必须在同一次扩容之前做完 —— push_back 之后 src 就是悬垂引用，
// 上一版在这里丢过一次写入，那条任务于是每 20 秒派生一份，永远停不下来。
static void spawnNext(JVal& tasks, JVal& src, const string& dueStr) {
    JVal copy = src;
    clearRepeat(src);

    for (auto& kv : copy.obj) {
        if (kv.first == "id") {
            kv.second.kind = JVal::Str;
            kv.second.str = newTaskId();
        } else if (kv.first == "done") {
            kv.second.kind = JVal::Bool;
            kv.second.b = false;
        } else if (kv.first == "completedAt") {
            kv.second.kind = JVal::Nul;
            kv.second.str.clear();
        } else if (kv.first == "startedAt") {
            kv.second.kind = JVal::Nul;
            kv.second.num = 0;
            kv.second.str.clear();
        } else if (kv.first == "createdAt") {
            kv.second.kind = JVal::Str;
            kv.second.str = isoUtcNow();
        } else if (kv.first == "due") {
            kv.second.kind = JVal::Str;
            kv.second.str = dueStr;
        }
    }
    tasks.arr.push_back(std::move(copy));
}

// 派生时把 due 一路推到严格晚于现在：逾期很久的重复任务只补一条，不会每轮都涨。
// 推了 400 期还没追平就当数据有问题，放弃这次派生。
static string advanceDuePast(const string& due, const string& rep, long long now) {
    string d = due;
    for (int i = 0; i < 400; ++i) {
        long long ms = dueToUnixMs(d);
        if (ms <= 0) return "";
        if (ms > now) return d;
        string nd = nextDueOf(d, rep);
        if (nd.empty() || nd == d) return "";
        d = nd;
    }
    return "";
}

static long long nowUnixMs();   // 定义在后面

// 计时状态：记录开始时刻（Unix 毫秒），停止时置为 0
static Response apiStart(const string& id, bool on) {
    if (id.empty()) return jsonError(400, "missing id");

    std::lock_guard<std::mutex> lk(gDataMutex);

    JVal root;
    if (!loadTasksRoot(root)) return jsonError(500, "read failed");

    JVal* tasks = root.get("tasks");
    if (!tasks || tasks->kind != JVal::Arr) return jsonError(404, "no tasks");

    for (auto& t : tasks->arr) {
        if (t.kind != JVal::Obj) continue;
        JVal* tid = t.get("id");
        if (!tid || tid->kind != JVal::Str || tid->str != id) continue;

        JVal* sa = t.get("startedAt");
        if (!sa) {
            JVal nv;
            nv.kind = JVal::Nul;
            t.obj.emplace_back("startedAt", std::move(nv));
            sa = t.get("startedAt");
        }
        if (on) {
            sa->kind = JVal::Num;
            sa->num = (double)nowUnixMs();
            sa->str.clear();
        } else {
            sa->kind = JVal::Nul;
            sa->num = 0;
            sa->str.clear();
        }

        if (!saveTasksRoot(root)) return jsonError(500, "write failed");
        return jsonOk("\"startedAt\":" + std::to_string(on ? (long long)nowUnixMs() : 0));
    }
    return jsonError(404, "not found");
}

// 置顶：排在列表最前面。控制台和悬浮清单共用这一个标记。
static Response apiPin(const string& id, bool on) {
    if (id.empty()) return jsonError(400, "missing id");

    std::lock_guard<std::mutex> lk(gDataMutex);

    JVal root;
    if (!loadTasksRoot(root)) return jsonError(500, "read failed");

    JVal* tasks = root.get("tasks");
    if (!tasks || tasks->kind != JVal::Arr) return jsonError(404, "no tasks");

    for (auto& t : tasks->arr) {
        if (t.kind != JVal::Obj) continue;
        JVal* tid = t.get("id");
        if (!tid || tid->kind != JVal::Str || tid->str != id) continue;

        JVal* pin = t.get("pinned");
        if (!pin) {
            JVal nv;
            nv.kind = JVal::Bool;
            nv.b = false;
            t.obj.emplace_back("pinned", std::move(nv));
            pin = t.get("pinned");
        }
        pin->kind = JVal::Bool;
        pin->b = on;
        pin->num = 0;
        pin->str.clear();

        if (!saveTasksRoot(root)) return jsonError(500, "write failed");
        return jsonOk(string("\"pinned\":") + (on ? "true" : "false"));
    }
    return jsonError(404, "not found");
}

// 逾期没做的重复任务：到点了也派生下一期，避免逾期后重复任务链中断
static void sweepOverdueRepeating() {
    std::lock_guard<std::mutex> lk(gDataMutex);

    JVal root;
    if (!loadTasksRoot(root)) return;

    JVal* tasks = root.get("tasks");
    if (!tasks || tasks->kind != JVal::Arr) return;

    long long now = nowUnixMs();
    bool changed = false;
    int spawned = 0;
    const int MAX_SPAWN_PER_SWEEP = 64;    // 数据出问题时别把内存和磁盘打满

    size_t n = tasks->arr.size();          // 派生会把新任务 push 到末尾，这里只扫原来的
    for (size_t i = 0; i < n && spawned < MAX_SPAWN_PER_SWEEP; ++i) {
        JVal& t = tasks->arr[i];
        if (t.kind != JVal::Obj) continue;

        string rep = repeatOf(t);
        if (rep.empty()) continue;

        JVal* done = t.get("done");
        if (done && done->kind == JVal::Bool && done->b) continue;

        long long dueMs = dueToUnixMs(dueOf(t));
        if (dueMs <= 0 || dueMs > now) continue;

        string nd = advanceDuePast(dueOf(t), rep, now);
        if (nd.empty()) continue;

        spawnNext(*tasks, t, nd);
        ++spawned;
        changed = true;
    }

    if (changed) saveTasksRoot(root);
}

// 从任务对象里取一个字符串 / 数字字段，字段缺失时返回默认值
static string strField(const JVal& t, const char* key) {
    JVal* v = const_cast<JVal&>(t).get(key);
    return (v && v->kind == JVal::Str) ? v->str : string();
}
static long long numField(const JVal& t, const char* key) {
    JVal* v = const_cast<JVal&>(t).get(key);
    return (v && v->kind == JVal::Num) ? (long long)v->num : 0;
}

// 进行中任务达到 100%（预计时长耗尽）时发送系统通知。
// 起始时间算法与悬浮清单和控制台保持一致：min(计时开始时刻, 截止 - 预估)。
static void sweepTimerDone() {
    // 同一次计时只通知一次。key 用 id#开始时刻，重开计时就是新的 key。
    // 首次扫描仅登记状态；启动时已达到 100% 的任务不视为刚完成。
    static std::vector<string> told;
    static bool primed = false;

    std::lock_guard<std::mutex> lk(gDataMutex);

    JVal root;
    if (!loadTasksRoot(root)) return;
    JVal* tasks = root.get("tasks");
    if (!tasks || tasks->kind != JVal::Arr) return;

    long long now = nowUnixMs();
    for (auto& t : tasks->arr) {
        if (t.kind != JVal::Obj) continue;

        long long started = numField(t, "startedAt");
        long long est = numField(t, "estimate") * 60000LL;
        if (started <= 0 || est <= 0) continue;      // 没在进行，或者没填预估

        long long start = started;
        long long dueMs = dueToUnixMs(dueOf(t));
        if (dueMs > 0 && dueMs - est < start) start = dueMs - est;
        if (now - start < est) continue;             // 还没走完

        string key = strField(t, "id") + "#" + std::to_string(started);
        bool seen = false;
        for (size_t i = 0; i < told.size(); ++i) {
            if (told[i] == key) { seen = true; break; }
        }
        if (seen) continue;
        if (told.size() > 500) told.clear();          // 别无限长，真到这一步清空重来也不碍事
        told.push_back(key);
        if (!primed) continue;

        wstring msg = L"【" + u8tow(strField(t, "title")) + L"】计时结束";
        appNotify(msg.c_str());
    }
    primed = true;
}

static Response apiToggle(const string& id) {
    if (id.empty()) return jsonError(400, "missing id");

    std::lock_guard<std::mutex> lk(gDataMutex);

    JVal root;
    if (!loadTasksRoot(root)) return jsonError(500, "read failed");

    JVal* tasks = root.get("tasks");
    if (!tasks || tasks->kind != JVal::Arr) return jsonError(404, "no tasks");

    for (auto& t : tasks->arr) {
        if (t.kind != JVal::Obj) continue;
        JVal* tid = t.get("id");
        if (!tid || tid->kind != JVal::Str || tid->str != id) continue;

        JVal* done = t.get("done");
        bool nowDone = !(done && done->kind == JVal::Bool && done->b);

        if (done) {
            done->kind = JVal::Bool;
            done->b = nowDone;
        } else {
            JVal nv; nv.kind = JVal::Bool; nv.b = nowDone;
            t.obj.emplace_back("done", std::move(nv));
        }

        JVal* ca = t.get("completedAt");
        if (ca) {
            if (nowDone) {
                ca->kind = JVal::Str;
                ca->str = isoUtcNow();
            } else {
                ca->kind = JVal::Nul;
                ca->str.clear();
            }
        }

        // 标记完成时同时结束“正在进行”，避免进度条继续推进
        if (nowDone) {
            JVal* sa = t.get("startedAt");
            if (sa) {
                sa->kind = JVal::Nul;
                sa->num = 0;
                sa->str.clear();
            }
        }

        // 重复任务：完成时一并生成下一期，spawnNext 会清除自身的重复标记
        if (nowDone) {
            string rep = repeatOf(t);
            if (!rep.empty()) {
                string nd = nextDueOf(dueOf(t), rep);
                if (!nd.empty()) spawnNext(*tasks, t, nd);
            }
        }

        if (!saveTasksRoot(root)) return jsonError(500, "write failed");
        return jsonOk("\"id\":\"" + id + "\",\"done\":" + (nowDone ? "true" : "false"));
    }

    return jsonError(404, "task not found");
}

// 悬浮清单就地添加：只给标题，其余填默认值，和控制台新建的形状一致
//
// id 用“系统时间 + 进程内自增序号 + 线程 id”拼，不再用 rand()：
// MinGW 的 rand() 状态是每线程一份，srand 只在第一个请求线程跑过，
// 之后每个新线程都从同一个默认种子开始，所有 /api/add 会拿到同一个 id。
static string newTaskId() {
    static std::atomic<unsigned long long> seq{ 0 };

    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER t;
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;

    unsigned long long c = (unsigned long long)(++seq);
    unsigned long long hi = t.QuadPart ^ (c << 24) ^ ((unsigned long long)GetCurrentThreadId() << 48);
    unsigned long long lo = t.QuadPart + c * 0x9E3779B97F4A7C15ULL;
    lo ^= lo >> 30; lo *= 0xBF58476D1CE4E5B9ULL;
    lo ^= lo >> 27; lo *= 0x94D049BB133111EBULL;
    lo ^= lo >> 31;

    char buf[48];
    snprintf(buf, sizeof buf, "%08x-%04x-%04x-%04x-%08x%04x",
             (unsigned)(hi >> 32), (unsigned)((hi >> 16) & 0xFFFFu),
             (unsigned)(hi & 0xFFFFu), (unsigned)((lo >> 48) & 0xFFFFu),
             (unsigned)(lo >> 32), (unsigned)(lo & 0xFFFFu));
    return string(buf);
}

// "YYYY-MM-DDTHH:mm"：分隔符位置、年月日时分范围、当月天数均需合法，否则视为未填写
static bool validDue(const string& s) {
    if (s.size() < 16) return false;
    for (size_t i = 0; i < 16; ++i) {
        char c = s[i];
        if (i == 4 || i == 7)      { if (c != '-') return false; }
        else if (i == 10)          { if (c != 'T' && c != ' ') return false; }
        else if (i == 13)          { if (c != ':') return false; }
        else if (c < '0' || c > '9') return false;
    }
    int y  = atoi(s.substr(0, 4).c_str());
    int mo = atoi(s.substr(5, 2).c_str());
    int d  = atoi(s.substr(8, 2).c_str());
    int h  = atoi(s.substr(11, 2).c_str());
    int mi = atoi(s.substr(14, 2).c_str());
    if (y < 1970 || y > 2200 || mo < 1 || mo > 12) return false;
    if (d < 1 || d > daysOfMonth(y, mo)) return false;
    if (h > 23 || mi > 59) return false;
    return true;
}

// "a|b|c" -> 字符串数组。竖线在 Windows 文件名里非法，URL 里也该转义，当分隔符够安全。
static std::vector<string> splitLinks(const string& s) {
    std::vector<string> out;
    string cur;
    for (char c : s) {
        if (c == '|') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    if (out.size() > 8) out.resize(8);
    return out;
}

static Response apiAdd(const string& rawTitle, const string& rawDue, int estimate,
                       const string& rawLinks) {
    // 去首尾空白；URL 解码后是 UTF-8 字节，截断必须落在字符边界上
    size_t a = rawTitle.find_first_not_of(" \t\r\n");
    size_t b = rawTitle.find_last_not_of(" \t\r\n");
    string title = (a == string::npos) ? string() : rawTitle.substr(a, b - a + 1);
    title = utf8Clip(title, 2000);
    if (title.empty()) return jsonError(400, "empty title");

    std::lock_guard<std::mutex> lk(gDataMutex);

    JVal root;
    if (!loadTasksRoot(root)) return jsonError(500, "read failed");

    JVal* tasks = root.get("tasks");
    if (!tasks || tasks->kind != JVal::Arr) return jsonError(404, "no tasks");

    string id = newTaskId();

    JVal t;
    t.kind = JVal::Obj;

    JVal vId;   vId.kind = JVal::Str;  vId.str = id;            t.obj.emplace_back("id", std::move(vId));
    JVal vTi;   vTi.kind = JVal::Str;  vTi.str = title;         t.obj.emplace_back("title", std::move(vTi));
    JVal vDo;   vDo.kind = JVal::Bool; vDo.b = false;           t.obj.emplace_back("done", std::move(vDo));
    if (estimate < 0) estimate = 0;
    // 跨天估算是正常需求（72 小时这种），上限只防脏数据，和控制台输入框的五位数对齐
    if (estimate > 99999) estimate = 99999;
    JVal vEs;   vEs.kind = JVal::Num;  vEs.num = estimate;      t.obj.emplace_back("estimate", std::move(vEs));

    if (validDue(rawDue)) {
        JVal vDu; vDu.kind = JVal::Str; vDu.str = rawDue.substr(0, 16);
        t.obj.emplace_back("due", std::move(vDu));
    } else {
        JVal vDu; vDu.kind = JVal::Nul;
        t.obj.emplace_back("due", std::move(vDu));
    }

    JVal vLks;
    vLks.kind = JVal::Arr;
    for (const string& one : splitLinks(rawLinks)) {
        if (one.empty()) continue;
        vLks.arr.push_back(makeLink(one));
    }
    t.obj.emplace_back("links", std::move(vLks));
    JVal vTg;   vTg.kind = JVal::Arr;                           t.obj.emplace_back("tags", std::move(vTg));
    JVal vCr;   vCr.kind = JVal::Str;  vCr.str = isoUtcNow();   t.obj.emplace_back("createdAt", std::move(vCr));
    JVal vCa;   vCa.kind = JVal::Nul;                           t.obj.emplace_back("completedAt", std::move(vCa));

    tasks->arr.push_back(std::move(t));

    if (!saveTasksRoot(root)) return jsonError(500, "write failed");
    return jsonOk("\"id\":\"" + id + "\"");
}

// Unix 毫秒 -> "YYYY-MM-DDTHH:mm"（本地时间）
static long long nowUnixMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (long long)((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

static string msToDueStr(long long ms) {
    ULONGLONG t = (ULONGLONG)ms * 10000ULL + 116444736000000000ULL;
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)(t & 0xFFFFFFFFULL);
    ft.dwHighDateTime = (DWORD)(t >> 32);
    SYSTEMTIME utc{}, loc{};
    FileTimeToSystemTime(&ft, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &loc);

    // due 格式 "YYYY-MM-DDTHH:mm"，和控制台保持一致
    char b[32];
    snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d",
             loc.wYear, loc.wMonth, loc.wDay, loc.wHour, loc.wMinute);
    return string(b);
}

// 悬浮清单右键“推迟一天”：在原截止时间上加 24 小时；没有截止时间就从现在算
static Response apiSnooze(const string& id) {
    if (id.empty()) return jsonError(400, "missing id");

    std::lock_guard<std::mutex> lk(gDataMutex);

    JVal root;
    if (!loadTasksRoot(root)) return jsonError(500, "read failed");
    JVal* tasks = root.get("tasks");
    if (!tasks || tasks->kind != JVal::Arr) return jsonError(404, "no tasks");

    for (auto& t : tasks->arr) {
        if (t.kind != JVal::Obj) continue;
        JVal* tid = t.get("id");
        if (!tid || tid->kind != JVal::Str || tid->str != id) continue;

        JVal* due = t.get("due");
        long long cur = (due && due->kind == JVal::Str) ? dueToUnixMs(due->str) : 0;
        long long base = (cur > nowUnixMs()) ? cur : nowUnixMs();
        string next = msToDueStr(base + 24LL * 3600000LL);

        if (due) {
            due->kind = JVal::Str;
            due->str = next;
        } else {
            JVal nv; nv.kind = JVal::Str; nv.str = next;
            t.obj.emplace_back("due", std::move(nv));
        }

        if (!saveTasksRoot(root)) return jsonError(500, "write failed");
        return jsonOk("\"due\":\"" + next + "\"");
    }
    return jsonError(404, "task not found");
}

// 悬浮清单右键“删除”：只删这一条，其余原样保留
static Response apiRemove(const string& id) {
    if (id.empty()) return jsonError(400, "missing id");

    std::lock_guard<std::mutex> lk(gDataMutex);

    JVal root;
    if (!loadTasksRoot(root)) return jsonError(500, "read failed");
    JVal* tasks = root.get("tasks");
    if (!tasks || tasks->kind != JVal::Arr) return jsonError(404, "no tasks");

    for (size_t i = 0; i < tasks->arr.size(); ++i) {
        JVal& t = tasks->arr[i];
        if (t.kind != JVal::Obj) continue;
        JVal* tid = t.get("id");
        if (!tid || tid->kind != JVal::Str || tid->str != id) continue;

        tasks->arr.erase(tasks->arr.begin() + (long)i);
        if (!saveTasksRoot(root)) return jsonError(500, "write failed");
        return jsonOk("\"removed\":true");
    }
    return jsonError(404, "task not found");
}

static Response handleApi(const string& method, const string& path,
                          const std::map<string, string>& q, const string& body) {
    if (path == "/api/health") {
        Response r;
        r.body = "{\"ok\":true,\"app\":\"" + string(APP_TAG) + "\",\"version\":\"" + string(GOGOSPOT_VERSION) + "\"}";
        return r;
    }
    if (path == "/api/info")     return apiInfo();

    if (path == "/api/open") {
        if (method != "POST") return jsonError(405, "use POST");
        auto it = q.find("url");
        string url = (it == q.end()) ? string() : it->second;
        auto itCall = q.find("call");
        return apiOpen(url, itCall == q.end() ? string() : itCall->second);
    }
    // 控制台弹原生选择框取绝对路径：mode=file 文件，mode=folder 文件夹
    if (path == "/api/pick") {
        if (method != "POST") return jsonError(405, "use POST");
        auto it = q.find("mode");
        return apiPick((it == q.end() || it->second.empty()) ? string("file") : it->second);
    }
    if (path == "/api/settings") {
        auto dump = []() {
            return string("\"widget\":") + (gWidgetEnabled ? "true" : "false") +
                   ",\"topmost\":" + (gTopmost ? "true" : "false") +
                   ",\"autostart\":" + (gAutoStart ? "true" : "false");
        };
        if (method == "GET") return jsonOk(dump());
        if (method != "POST") return jsonError(405, "use POST");

        // body 里给哪个键就改哪个，没给的保持不动
        bool v = false, any = false;
        if (readBoolField(body, "widget", v))    { appSetWidgetEnabled(v); any = true; }
        if (readBoolField(body, "topmost", v))   { appSetTopmost(v);       any = true; }
        if (readBoolField(body, "autostart", v)) { appSetAutoStart(v);     any = true; }
        if (!any) return jsonError(400, "no known key");
        return jsonOk(dump());
    }
    // 悬浮清单标题栏上那个置顶按钮走这里：只改一个开关，和控制台改的是同一个值
    if (path == "/api/topmost") {
        if (method != "POST") return jsonError(405, "use POST");
        auto on = q.find("on");
        bool flag = (on == q.end()) ? true : (on->second != "0");
        appSetTopmost(flag);
        return jsonOk(string("\"topmost\":") + (flag ? "true" : "false"));
    }
    if (path == "/api/tasks")    return apiTasks(method, body);
    if (path == "/api/widget")   return apiWidget();
    if (path == "/api/start") {
        if (method != "POST") return jsonError(405, "use POST");
        auto it = q.find("id");
        auto on = q.find("on");
        bool flag = (on == q.end()) ? true : (on->second != "0");
        return apiStart(it == q.end() ? string() : it->second, flag);
    }
    if (path == "/api/pin") {
        if (method != "POST") return jsonError(405, "use POST");
        auto it = q.find("id");
        auto on = q.find("on");
        bool flag = (on == q.end()) ? true : (on->second != "0");
        return apiPin(it == q.end() ? string() : it->second, flag);
    }
    if (path == "/api/toggle") {
        if (method != "POST") return jsonError(405, "use POST");
        auto it = q.find("id");
        return apiToggle(it == q.end() ? string() : it->second);
    }
    if (path == "/api/add") {
        if (method != "POST") return jsonError(405, "use POST");
        auto itT = q.find("title");
        auto itD = q.find("due");
        auto itE = q.find("estimate");
        auto itL = q.find("links");
        int est = (itE == q.end()) ? 0 : atoi(itE->second.c_str());
        return apiAdd(itT == q.end() ? string() : itT->second,
                      itD == q.end() ? string() : itD->second, est,
                      itL == q.end() ? string() : itL->second);
    }
    if (path == "/api/snooze") {
        if (method != "POST") return jsonError(405, "use POST");
        auto it = q.find("id");
        return apiSnooze(it == q.end() ? string() : it->second);
    }
    if (path == "/api/remove") {
        if (method != "POST") return jsonError(405, "use POST");
        auto it = q.find("id");
        return apiRemove(it == q.end() ? string() : it->second);
    }
    if (path == "/api/backup")   return apiBackup();
    if (path == "/api/backups")  return apiBackups();
    if (path == "/api/restore") {
        auto it = q.find("name");
        return apiRestore(it == q.end() ? string() : it->second);
    }
    if (path == "/api/deletebackup") {
        if (method != "POST") return jsonError(405, "use POST");
        auto it = q.find("name");
        return apiDeleteBackup(it == q.end() ? string() : it->second);
    }
    if (path == "/api/export") {
        std::lock_guard<std::mutex> lk(gDataMutex);
        string raw;
        if (!readFileAll(gDataFile, raw) || raw.empty()) raw = defaultData();
        Response r;
        r.body = raw;
        r.headers.push_back({"Content-Disposition",
            "attachment; filename=\"gogospot-" + stampForFile() + ".json\""});
        return r;
    }
    return jsonError(404, "unknown api");
}

