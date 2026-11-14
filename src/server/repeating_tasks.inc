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

// 循环锚点：最初设定的循环日期。推迟只改 due，不动它，
// 所以下一期仍按原节奏生成，被推迟的那一期就成了「错位」。
// 老数据没有这个字段，退回 due（等于和以前一样）。
static string originOf(const JVal& t) {
    JVal* o = const_cast<JVal&>(t).get("route_origin_date");
    if (o && o->kind == JVal::Str && validDue(o->str)) return o->str;
    return dueOf(t);
}

// 清掉原任务的重复标记，保证同一条只派生一次
static void clearRepeat(JVal& t) {
    JVal* r = t.get("repeat");
    if (r) { r->kind = JVal::Str; r->str.clear(); }
}

// 复制一份当下一期：标题、标签、链接、预估、重复规则都跟着走，只换 id 和日期。
// 复制和清标记必须在同一次扩容之前做完 —— push_back 之后 src 就是悬垂引用，
// 上一版在这里丢过一次写入，那条任务于是每 20 秒派生一份，永远停不下来。
// 下面两个定义在文件后半段，派生函数要用
static long long numField(const JVal& t, const char* key);
static string msToDueStr(long long ms);

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
    // 新一期的锚点就是它的 due，即原锚点加一个周期；复制过来的旧锚点必须覆盖掉
    setStrField(copy, "route_origin_date", dueStr);

    // 提醒任务的时间轴是「开始 → 截止」：due 往前推了一期，开始时刻也要跟着推，
    // 否则新一期会变成「开始在过去、截止在将来」的怪状态
    JVal* tp = copy.get("TaskType");
    if (tp && tp->kind == JVal::Str && tp->str == "remind") {
        long long est = numField(copy, "estimate");
        if (est <= 0) est = 60;
        long long dms = dueToUnixMs(dueStr);
        if (dms > 0) setStrField(copy, "Start", msToDueStr(dms - est * 60000LL));
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

        // 提醒任务不关心何时结束，没有计时这回事
        JVal* tp = t.get("TaskType");
        if (tp && tp->kind == JVal::Str && tp->str == "remind")
            return jsonError(400, "reminder task cannot be started");

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

        // 从锚点推下一期：推迟过的任务会回到原来的节奏上
        string nd = advanceDuePast(originOf(t), rep, now);
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

// 到点提醒。
//   过程任务：计时走完预计时长（100%）时提醒一次，起始时间与界面一致：
//             min(计时开始时刻, 截止 - 预估)
//   提醒任务：到达 Start 时提醒一次，与计时无关
// 首次扫描只登记状态：启动时已经过点的任务不补弹通知。
static void sweepTimerDone() {
    // 同一次提醒只发一次。过程任务的 key 用 id#开始时刻，重开计时就是新的 key；
    // 提醒任务用 id@开始时刻，改了开始时间就重新算一次。
    static std::vector<string> told;
    static bool primed = false;

    std::lock_guard<std::mutex> lk(gDataMutex);

    JVal root;
    if (!loadTasksRoot(root)) return;
    JVal* tasks = root.get("tasks");
    if (!tasks || tasks->kind != JVal::Arr) return;

    // 登记 key；返回 true 表示这条已经通知过
    auto seen = [&](const string& key) -> bool {
        for (size_t i = 0; i < told.size(); ++i) {
            if (told[i] == key) return true;
        }
        if (told.size() > 500) told.clear();          // 别无限长，真到这一步清空重来也不碍事
        told.push_back(key);
        return false;
    };

    long long now = nowUnixMs();
    for (auto& t : tasks->arr) {
        if (t.kind != JVal::Obj) continue;

        JVal* tp = t.get("TaskType");
        if (tp && tp->kind == JVal::Str && tp->str == "remind") {
            JVal* st = t.get("Start");
            long long s = (st && st->kind == JVal::Str) ? dueToUnixMs(st->str) : 0;
            if (s <= 0 || now < s) continue;
            if (now - s > 24LL * 3600000LL) continue;   // 太久以前的不补，避免开机弹一堆旧提醒
            if (seen(strField(t, "id") + "@" + std::to_string(s))) continue;
            if (!primed) continue;

            wstring msg = L"【" + u8tow(strField(t, "title")) + L"】开始";
            appNotify(msg.c_str());
            continue;
        }

        long long started = numField(t, "startedAt");
        long long est = numField(t, "estimate") * 60000LL;
        if (started <= 0 || est <= 0) continue;      // 没在进行，或者没填预估

        long long start = started;
        long long dueMs = dueToUnixMs(dueOf(t));
        if (dueMs > 0 && dueMs - est < start) start = dueMs - est;
        if (now - start < est) continue;             // 还没走完

        if (seen(strField(t, "id") + "#" + std::to_string(started))) continue;
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
                // 从锚点加一个周期；推迟过的那一期不会把后面的节奏带偏
                string nd = nextDueOf(originOf(t), rep);
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
                       const string& rawLinks, const string& rawType,
                       const string& rawStart, int advance) {
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

    bool remind = (rawType == "remind");

    // 提醒任务的 due 不从界面取：它等于「开始 + 持续时间」，交给 normalizeTaskKinds 算
    if (!remind && validDue(rawDue)) {
        JVal vDu; vDu.kind = JVal::Str; vDu.str = rawDue.substr(0, 16);
        t.obj.emplace_back("due", std::move(vDu));
    } else {
        JVal vDu; vDu.kind = JVal::Nul;
        t.obj.emplace_back("due", std::move(vDu));
    }

    {
        JVal vTp; vTp.kind = JVal::Str; vTp.str = remind ? "remind" : "process";
        t.obj.emplace_back("TaskType", std::move(vTp));
    }
    if (remind) {
        if (validDue(rawStart)) {
            JVal vSt; vSt.kind = JVal::Str; vSt.str = rawStart.substr(0, 16);
            t.obj.emplace_back("Start", std::move(vSt));
        }
        if (advance < 0) advance = 0;
        if (advance > 99999) advance = 99999;
        JVal vAd; vAd.kind = JVal::Num; vAd.num = advance;
        t.obj.emplace_back("Advance", std::move(vAd));
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

    normalizeTaskKinds(root);          // 提醒任务在这里补出 due
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

// 提醒任务的 due 由「开始 + 持续时间」推出来：持续时间没填按 1 小时，
// 其他任务靠这个时长给它预留时间。过程任务只认 due，这里不动。
// 返回值表示有没有真的改过，PUT 靠它决定要不要重新序列化。
static bool normalizeTaskKinds(JVal& root) {
    JVal* tasks = root.get("tasks");
    if (!tasks || tasks->kind != JVal::Arr) return false;

    bool changed = false;
    for (auto& t : tasks->arr) {
        if (t.kind != JVal::Obj) continue;

        JVal* tp = t.get("TaskType");
        bool remind = tp && tp->kind == JVal::Str && tp->str == "remind";
        if (!remind) continue;

        // 提前提醒：负数没有意义，缺省补 0
        JVal* adv = t.get("Advance");
        if (!adv) {
            JVal v; v.kind = JVal::Num; v.num = 0;
            t.obj.emplace_back("Advance", std::move(v));
            changed = true;
        } else if (adv->kind != JVal::Num || adv->num < 0) {
            adv->kind = JVal::Num;
            adv->num = 0;
            changed = true;
        }

        JVal* st = t.get("Start");
        if (!st || st->kind != JVal::Str || !validDue(st->str)) continue;

        long long s = dueToUnixMs(st->str);
        if (s <= 0) continue;
        long long est = numField(t, "estimate");
        if (est <= 0) est = 60;
        string want = msToDueStr(s + est * 60000LL);

        JVal* due = t.get("due");
        if (!due) {
            JVal v; v.kind = JVal::Str; v.str = want;
            t.obj.emplace_back("due", std::move(v));
            changed = true;
        } else if (due->kind != JVal::Str || due->str != want) {
            due->kind = JVal::Str;
            due->str = want;
            changed = true;
        }
    }
    return changed;
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

        // 提醒任务的时间轴是「开始 → 截止」，推迟该动开始时刻；
        // 只改 due 会让开始时间留在原地，等于把时长拉长一天。
        JVal* tp = t.get("TaskType");
        JVal* stv = t.get("Start");
        if (tp && tp->kind == JVal::Str && tp->str == "remind" &&
            stv && stv->kind == JVal::Str && validDue(stv->str)) {
            long long s0 = dueToUnixMs(stv->str);
            if (s0 <= 0) return jsonError(400, "task has no start time");
            long long base0 = (s0 > nowUnixMs()) ? s0 : nowUnixMs();
            string ns = msToDueStr(base0 + 24LL * 3600000LL);
            stv->str = ns;

            long long est0 = numField(t, "estimate");
            if (est0 <= 0) est0 = 60;
            string nd = msToDueStr(dueToUnixMs(ns) + est0 * 60000LL);
            JVal* due0 = t.get("due");
            if (due0) { due0->kind = JVal::Str; due0->str = nd; }
            else { JVal v; v.kind = JVal::Str; v.str = nd; t.obj.emplace_back("due", std::move(v)); }

            if (!saveTasksRoot(root)) return jsonError(500, "write failed");
            return jsonOk("\"start\":\"" + ns + "\",\"due\":\"" + nd + "\"");
        }

        JVal* due = t.get("due");
        long long cur = (due && due->kind == JVal::Str) ? dueToUnixMs(due->str) : 0;
        if (cur <= 0) return jsonError(400, "task has no due date");   // 没有截止时间就无从推迟

        // 老数据没有锚点：把推迟前的 due 记成锚点，下一期仍按原节奏走。
        // 已经有锚点的任务不动它，推迟只改 due。
        JVal* rep = t.get("repeat");
        bool repeating = rep && rep->kind == JVal::Str && !rep->str.empty();
        JVal* rod = t.get("route_origin_date");
        if (repeating && due && due->kind == JVal::Str && !due->str.empty() &&
            (!rod || rod->kind != JVal::Str || rod->str.empty())) {
            setStrField(t, "route_origin_date", due->str);
        }

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

        // 删除是破坏性操作；连着删只留第一份，靠一分钟冷却压掉后面的
        backupIfIdle();
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
                   ",\"autostart\":" + (gAutoStart ? "true" : "false") +
                   ",\"themeMode\":" + std::to_string(gThemeMode) +
                   ",\"systemLight\":" + (systemUsesLightTheme() ? "true" : "false");
        };
        if (method == "GET") return jsonOk(dump());
        if (method != "POST") return jsonError(405, "use POST");

        // body 里给哪个键就改哪个，没给的保持不动
        bool v = false, any = false;
        if (readBoolField(body, "widget", v))    { appSetWidgetEnabled(v); any = true; }
        if (readBoolField(body, "topmost", v))   { appSetTopmost(v);       any = true; }
        if (readBoolField(body, "autostart", v)) { appSetAutoStart(v);     any = true; }
        int tm = 2;
        if (readIntField(body, "themeMode", tm)) { appSetThemeMode(tm);    any = true; }
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
    // 控制台的三档主题开关：日间 / 跟随系统 / 夜间，也同步给悬浮清单
    if (path == "/api/theme") {
        if (method != "POST") return jsonError(405, "use POST");
        auto it = q.find("mode");
        string m = (it == q.end()) ? string("system") : it->second;
        int mode = (m == "light") ? 0 : (m == "dark" ? 1 : 2);
        appSetThemeMode(mode);
        return jsonOk(string("\"themeMode\":") + std::to_string(mode));
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
        auto itY = q.find("type");
        auto itS = q.find("start");
        auto itA = q.find("advance");
        int est = (itE == q.end()) ? 0 : atoi(itE->second.c_str());
        int adv = (itA == q.end()) ? 0 : atoi(itA->second.c_str());
        return apiAdd(itT == q.end() ? string() : itT->second,
                      itD == q.end() ? string() : itD->second, est,
                      itL == q.end() ? string() : itL->second,
                      itY == q.end() ? string() : itY->second,
                      itS == q.end() ? string() : itS->second, adv);
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
    if (path == "/api/backup")   return apiBackup(q.find("cooldown") != q.end());
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

