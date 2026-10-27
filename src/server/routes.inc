// ---- 路由 ----

// Host 里取出主机名：去掉端口，[::1]:8765 先取方括号里的，再转小写。
// 主机名必须完整匹配；前缀匹配会错误放行 127.0.0.1.evil.com。
static bool hostIsLocal(const string& raw) {
    string h = raw;
    if (!h.empty() && h[0] == '[') {
        size_t rb = h.find(']');
        if (rb == string::npos) return false;
        h = h.substr(1, rb - 1);
    } else {
        size_t colon = h.find(':');
        if (colon != string::npos) h = h.substr(0, colon);
    }
    std::transform(h.begin(), h.end(), h.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    return h == "127.0.0.1" || h == "localhost" ||
           h == "::1" || h == "0:0:0:0:0:0:0:1";
}

// Origin 是完整 URL，只取 scheme://host[:port] 那一段再交给 hostIsLocal
static bool originIsLocal(const string& raw) {
    size_t sep = raw.find("://");
    if (sep == string::npos) return false;
    string rest = raw.substr(sep + 3);
    size_t slash = rest.find('/');
    if (slash != string::npos) rest = rest.substr(0, slash);
    return hostIsLocal(rest);
}

// 来源校验。
//
// Host 不分方法一律校验：DNS rebinding 会把 evil.com 解析到 127.0.0.1，
// 浏览器发来的 Host 还是 evil.com，只守写接口等于让读接口裸奔。
// 写接口再加 Origin / Sec-Fetch-Site：浏览器发起的跨站请求一定带其中一个，
// 控制台是 same-origin，命令行工具两个都不带，照常放行。
static bool sameOriginRequest(const std::map<string, string>& headers, bool isWrite) {
    auto host = headers.find("host");
    if (host != headers.end() && !host->second.empty() && !hostIsLocal(host->second)) {
        return false;
    }
    if (!isWrite) return true;

    auto site = headers.find("sec-fetch-site");
    if (site != headers.end() &&
        (site->second == "cross-site" || site->second == "same-site")) return false;

    auto org = headers.find("origin");
    if (org != headers.end()) {
        if (org->second == "null" || org->second.empty()) return false;
        if (!originIsLocal(org->second)) return false;
    }
    return true;
}

static Response serveStatic(const string& path) {
    string rel = path;
    if (rel == "/" || rel.empty()) rel = "/index.html";

    // 目录穿越防护
    if (rel.find("..") != string::npos) return jsonError(400, "invalid path");

    // 去掉开头的 '/'
    while (!rel.empty() && rel[0] == '/') rel.erase(rel.begin());

    wstring full = gWebDir + L"\\" + u8tow(rel);
    if (!fileExists(full)) return jsonError(404, "file not found");

    string body;
    if (!readFileAll(full, body)) return jsonError(500, "read failed");

    size_t dot = rel.find_last_of('.');
    string ext = (dot == string::npos) ? "" : rel.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)tolower(c); });

    Response r;
    r.contentType = mimeOf(ext);
    r.body = std::move(body);
    return r;
}

// 任务文件必须能解析为对象且包含 tasks 数组；文件损坏时报错，不将原始内容返回前端。
// strict 模式要求每个元素均为对象，防止控制台将标量渲染为空白任务。
// 读取时允许不完整对象，以兼容历史数据并保留数据修复入口。
static bool looksLikeTasksFile(const string& s, bool strict = false) {
    JVal root;
    JParser p(s);
    if (!p.parse(root) || root.kind != JVal::Obj) return false;
    while (p.i < s.size() && isspace((unsigned char)s[p.i])) ++p.i;
    if (p.i != s.size()) return false;      // 对象后面还挂着垃圾，一样当坏文件
    JVal* tasks = root.get("tasks");
    if (!tasks || tasks->kind != JVal::Arr) return false;
    if (strict) {
        for (const auto& t : tasks->arr) if (t.kind != JVal::Obj) return false;
    }
    return true;
}

// 没有截止时间的重复任务无法生成下一期，但界面仍会显示重复周期。
// 持久化前统一清除此类 repeat，并返回是否发生修改。
static string dueOf(const JVal& t);   // 定义在“重复任务”那段

static bool dropOrphanRepeat(JVal& root) {
    JVal* tasks = root.get("tasks");
    if (!tasks || tasks->kind != JVal::Arr) return false;
    bool changed = false;
    for (auto& t : tasks->arr) {
        if (t.kind != JVal::Obj) continue;
        JVal* r = t.get("repeat");
        if (!r || r->kind != JVal::Str || r->str.empty()) continue;
        if (dueToUnixMs(dueOf(t)) > 0) continue;
        r->str.clear();
        changed = true;
    }
    return changed;
}

static Response apiTasks(const string& method, const string& body) {
    std::lock_guard<std::mutex> lk(gDataMutex);

    if (method == "GET") {
        Response r;
        string raw;
        if (!readFileAll(gDataFile, raw) || raw.empty()) {
            raw = defaultData();
        } else if (!looksLikeTasksFile(raw)) {
            return jsonError(500, "数据文件已损坏，请从备份恢复");
        }
        r.contentType = "application/json; charset=utf-8";
        r.body = raw;
        return r;
    }

    if (method == "PUT" || method == "POST") {
        if (body.size() > MAX_BODY) return jsonError(413, "payload too large");
        if (!looksLikeJson(body)) return jsonError(400, "body is not valid JSON");
        if (!looksLikeTasksFile(body, true)) {
            return jsonError(400, "tasks 必须是对象数组");
        }

        // 仅需修正“重复但无截止时间”一种情况，修改后重新序列化并写入
        string payload = body;
        JVal root;
        JParser p(body);
        if (p.parse(root) && dropOrphanRepeat(root)) {
            payload.clear();
            jsonWrite(root, payload);
        }

        ensureDir(gDataDir);
        string backup = makeBackupLocked();
        if (!writeFileAtomic(gDataFile, payload)) return jsonError(500, "write failed");

        string extra = "\"savedAt\":\"" + isoNow() + "\",\"bytes\":" + std::to_string(payload.size());
        if (!backup.empty()) extra += ",\"backup\":\"" + backup + "\"";
        return jsonOk(extra);
    }

    return jsonError(405, "method not allowed");
}

static Response apiBackup() {
    std::lock_guard<std::mutex> lk(gDataMutex);
    string name = makeBackupLocked();
    if (name.empty()) return jsonError(400, "还没有可备份的数据文件");
    return jsonOk("\"backup\":\"" + name + "\"");
}

static Response apiBackups() {
    std::lock_guard<std::mutex> lk(gDataMutex);
    auto list = listBackups();
    string arr = "[";
    for (size_t i = 0; i < list.size(); ++i) {
        if (i) arr += ",";
        arr += "{\"name\":\"" + list[i].name + "\",\"time\":\"" + list[i].time +
               "\",\"size\":" + std::to_string(list[i].size) + "}";
    }
    arr += "]";
    Response r;
    r.body = "{\"ok\":true,\"backups\":" + arr + "}";
    return r;
}

static Response apiRestore(const string& name) {
    // 只接受纯文件名
    if (name.empty() || name.find('/') != string::npos || name.find('\\') != string::npos ||
        name.find("..") != string::npos) {
        return jsonError(400, "invalid backup name");
    }
    std::lock_guard<std::mutex> lk(gDataMutex);
    wstring src = gBackupDir + L"\\" + u8tow(name);
    if (!fileExists(src)) return jsonError(404, "backup not found");

    string content;
    if (!readFileAll(src, content)) return jsonError(500, "read backup failed");
    // 用户放入的损坏文件也可能出现在备份目录中，先确认能解析再覆盖现场
    if (!looksLikeTasksFile(content)) return jsonError(400, "备份内容已损坏，未覆盖当前数据");
    makeBackupLocked();                       // 恢复前也留一份现场
    if (!writeFileAtomic(gDataFile, content)) return jsonError(500, "write failed");
    return jsonOk("\"restored\":\"" + name + "\",\"bytes\":" + std::to_string(content.size()));
}

static Response apiDeleteBackup(const string& name) {
    // 只接受备份目录下的纯文件名，以阻止路径穿越
    if (name.empty() || name.find('/') != string::npos || name.find('\\') != string::npos ||
        name.find("..") != string::npos) {
        return jsonError(400, "invalid backup name");
    }
    if (name.size() < 6 || name.compare(name.size() - 5, 5, ".json") != 0) {
        return jsonError(400, "not a backup file");
    }

    std::lock_guard<std::mutex> lk(gDataMutex);
    wstring target = gBackupDir + L"\\" + u8tow(name);
    if (!fileExists(target)) return jsonError(404, "backup not found");
    if (!DeleteFileW(target.c_str())) return jsonError(500, "delete failed");

    return jsonOk("\"deleted\":\"" + name + "\"");
}

static Response apiInfo() {
    Response r;
    DWORD up = (GetTickCount() - gStartTick) / 1000;
    string body = "{\"ok\":true,\"app\":\"";
    body += APP_TAG;
    body += "\",\"version\":\"" + string(GOGOSPOT_VERSION) + "\",";
    body += "\"dataFile\":\"" + wtou8(gDataFile) + "\",";
    body += "\"backupDir\":\"" + wtou8(gBackupDir) + "\",";
    body += "\"webDir\":\"" + wtou8(gWebDir) + "\",";
    body += "\"uptime\":" + std::to_string(up) + ",";
    body += "\"dataExists\":" + string(fileExists(gDataFile) ? "true" : "false") + "}";

    // 路径里的反斜杠需要转义成合法 JSON
    string esc;
    for (char c : body) {
        if (c == '\\') esc += "\\\\";
        else esc += c;
    }
    r.body = esc;
    return r;
}

// 任务上的链接：http(s) 或本地路径，都当字符串存，打开时交给 ShellExecute
static string cleanLink(const string& s) {
    string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\t' || c == '\n' || c == '\r') continue;
        out += c;
    }
    return utf8Clip(out, 2048);
}

// 只有 http(s) 算网址，其余一律按本地路径处理
static bool isWebUrl(const string& s) {
    return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0;
}

// 链接统一成 {url, protocol, call}：网址交给浏览器，本地路径在文件夹中显示
static JVal makeLink(const string& raw) {
    string url = cleanLink(raw);
    size_t a = url.find_first_not_of(" \t\r\n");
    size_t b = url.find_last_not_of(" \t\r\n");
    url = (a == string::npos) ? string() : url.substr(a, b - a + 1);

    bool web = isWebUrl(url);
    JVal o;
    o.kind = JVal::Obj;

    JVal vU; vU.kind = JVal::Str; vU.str = url;
    o.obj.emplace_back("url", std::move(vU));

    JVal vP; vP.kind = JVal::Str;
    vP.str = !web ? "file" : (url.rfind("https://", 0) == 0 ? "https" : "http");
    o.obj.emplace_back("protocol", std::move(vP));

    JVal vC; vC.kind = JVal::Str; vC.str = web ? "browser" : "explorer";
    o.obj.emplace_back("call", std::move(vC));
    return o;
}

// 打开一项：call 说用浏览器就交给系统默认浏览器，否则在文件夹里选中它。
// 未提供 call 时，根据网址或路径类型判定打开方式，以兼容历史数据。
static Response apiOpen(const string& rawUrl, const string& call) {
    string url = cleanLink(rawUrl);
    size_t a = url.find_first_not_of(" \t\r\n");
    size_t b = url.find_last_not_of(" \t\r\n");
    url = (a == string::npos) ? string() : url.substr(a, b - a + 1);
    if (url.empty()) return jsonError(400, "empty url");
    if (url.size() > 2048) return jsonError(400, "too long");

    wstring w = u8tow(url);

    // 本地路径在资源管理器中定位；关联材料操作不调用默认文件关联程序。
    bool reveal = (call == "explorer") || (call.empty() && !isWebUrl(url));
    HINSTANCE h;
    if (reveal) {
        wstring arg = L"/select,\"" + w + L"\"";
        h = ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        h = ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    if ((INT_PTR)h <= 32) return jsonError(500, "cannot open");
    return jsonOk("\"opened\":true");
}

// 文件选择器 GUID 定义于 libuuid；此处提供常量定义以避免链接额外库
static const CLSID kFileOpenDialogClsid =
    {0xDC1C5A9C, 0xE88A, 0x4dde, {0xA5, 0xA1, 0x60, 0xF8, 0x2A, 0x20, 0xAE, 0xF7}};
static const IID kFileOpenDialogIid =
    {0xd57c7288, 0xd4ad, 0x4768, {0xbe, 0x02, 0x9d, 0x96, 0x95, 0x32, 0xd9, 0x60}};

// 原生选择框：mode=file 多选文件，mode=folder 选文件夹。
// 浏览器端无法读取本地绝对路径，因此由本机进程显示原生文件选择对话框。
static Response apiPick(const string& mode) {
    bool folder = (mode == "folder");
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(kFileOpenDialogClsid, nullptr, CLSCTX_ALL,
                                kFileOpenDialogIid, (void**)&dlg)) || !dlg) {
        if (SUCCEEDED(hrInit)) CoUninitialize();
        return jsonError(500, "cannot create dialog");
    }

    DWORD opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM | FOS_ALLOWMULTISELECT;
    opts |= folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST;
    dlg->SetOptions(opts);
    dlg->SetTitle(folder ? L"选择文件夹" : L"选择要打开的文件");

    // 以前台窗口作为 owner，使对话框显示在浏览器窗口前方
    HRESULT hr = dlg->Show(GetForegroundWindow());
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        dlg->Release();
        if (SUCCEEDED(hrInit)) CoUninitialize();
        return jsonOk("\"paths\":[]");
    }
    if (FAILED(hr)) {
        dlg->Release();
        if (SUCCEEDED(hrInit)) CoUninitialize();
        return jsonError(500, "dialog failed");
    }

    string out = "\"paths\":[";
    IShellItemArray* items = nullptr;
    if (SUCCEEDED(dlg->GetResults(&items)) && items) {
        DWORD n = 0;
        items->GetCount(&n);
        for (DWORD i = 0; i < n; ++i) {
            IShellItem* it = nullptr;
            if (FAILED(items->GetItemAt(i, &it)) || !it) continue;
            PWSTR path = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                string u = wtou8(path);
                string safe;
                for (char c : u) {
                    if (c == '"' || c == '\\') { safe += '\\'; safe += c; }
                    else if ((unsigned char)c < 0x20) safe += ' ';
                    else safe += c;
                }
                if (out.back() != '[') out += ',';
                out += "\"" + safe + "\"";
                CoTaskMemFree(path);
            }
            it->Release();
        }
        items->Release();
    }
    out += "]";

    dlg->Release();
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return jsonOk(out);
}


// 悬浮清单用：纯文本格式，每行对应一条未完成任务，字段以制表符分隔，避免引入 JSON 解析器。
static Response apiWidget() {
    std::lock_guard<std::mutex> lk(gDataMutex);

    JVal root;
    if (!loadTasksRoot(root)) return jsonError(500, "read failed");

    JVal* tasks = root.get("tasks");
    int pending = 0, total = 0;
    string rows;

    if (tasks && tasks->kind == JVal::Arr) {
        for (auto& t : tasks->arr) {
            if (t.kind != JVal::Obj) continue;
            ++total;

            JVal* done = t.get("done");
            bool isDone = (done && done->kind == JVal::Bool && done->b);
            if (isDone) continue;
            ++pending;

            JVal* id = t.get("id");
            JVal* title = t.get("title");
            JVal* est = t.get("estimate");

            // id 也需过滤：里面有制表符或换行会把一行拆成两行，悬浮清单整份解析错位
            string rid = (id && id->kind == JVal::Str) ? id->str : "";
            for (char& c : rid) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
            if (rid.size() > 128) rid.resize(128);
            rows += rid;
            rows += '\t';
            rows += '0';
            rows += '\t';
            rows += (est && est->kind == JVal::Num) ? std::to_string((long long)est->num) : "0";
            rows += '\t';

            JVal* due = t.get("due");
            long long dueMs = (due && due->kind == JVal::Str) ? dueToUnixMs(due->str) : 0;
            rows += std::to_string(dueMs);
            rows += '\t';

            string tt = (title && title->kind == JVal::Str) ? title->str : "";
            for (char& c : tt) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
            rows += tt;
            rows += '\t';

            JVal* links = t.get("links");
            bool firstLink = true;
            if (links && links->kind == JVal::Arr) {
                for (const auto& one : links->arr) {
                    // 老数据是裸字符串，新数据是 {url, protocol, call}，两种都认
                    string u;
                    if (one.kind == JVal::Str) {
                        u = one.str;
                    } else if (one.kind == JVal::Obj) {
                        const JVal* p = one.get("url");
                        if (p && p->kind == JVal::Str) u = p->str;
                    }
                    if (u.empty()) continue;
                    if (!firstLink) rows += '|';
                    rows += cleanLink(u);
                    firstLink = false;
                }
            }
            rows += '\t';

            // 正在进行：计时开始时刻，0 表示未开始
            JVal* sa = t.get("startedAt");
            long long started = (sa && sa->kind == JVal::Num) ? (long long)sa->num : 0;
            rows += std::to_string(started);
            rows += '\t';

            // 置顶：排在列表最前面
            JVal* pin = t.get("pinned");
            rows += (pin && pin->kind == JVal::Bool && pin->b) ? "1" : "0";
            rows += '\n';
        }
    }

    Response r;
    r.contentType = "text/plain; charset=utf-8";
    r.body = "#" + std::to_string(pending) + "\t" + std::to_string(total) + "\n" + rows;
    return r;
}

