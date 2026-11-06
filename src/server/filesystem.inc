// ---- 文件系统 ----

static wstring exeDir() {
    wchar_t buf[4096];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0) return L".";
    wstring p(buf, n);
    size_t pos = p.find_last_of(L"\\/");
    return (pos == wstring::npos) ? wstring(L".") : p.substr(0, pos);
}

static bool dirExists(const wstring& d) {
    DWORD a = GetFileAttributesW(d.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool fileExists(const wstring& f) {
    DWORD a = GetFileAttributesW(f.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool ensureDir(const wstring& dir) {
    if (dirExists(dir)) return true;
    size_t pos = dir.find_last_of(L"\\/");
    if (pos != wstring::npos && pos > 2) ensureDir(dir.substr(0, pos));
    if (CreateDirectoryW(dir.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

static bool readFileAll(const wstring& path, string& out) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return false; }
    if (sz.QuadPart > (LONGLONG)MAX_BODY) { CloseHandle(h); return false; }

    out.resize((size_t)sz.QuadPart);
    size_t off = 0;
    while (off < out.size()) {
        DWORD rd = 0;
        DWORD want = (DWORD)std::min<size_t>(out.size() - off, 1u << 20);
        if (!ReadFile(h, &out[off], want, &rd, nullptr) || rd == 0) break;
        off += rd;
    }
    CloseHandle(h);
    out.resize(off);
    return true;
}

// 先写 .tmp 再原子替换，避免进程中断把数据文件写坏
static bool writeFileAtomic(const wstring& path, const string& data) {
    wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    size_t off = 0;
    bool ok = true;
    while (off < data.size()) {
        DWORD wr = 0;
        DWORD want = (DWORD)std::min<size_t>(data.size() - off, 1u << 20);
        if (!WriteFile(h, data.data() + off, want, &wr, nullptr) || wr == 0) { ok = false; break; }
        off += wr;
    }
    FlushFileBuffers(h);
    CloseHandle(h);

    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

static bool copyFileTo(const wstring& from, const wstring& to) {
    return CopyFileW(from.c_str(), to.c_str(), FALSE) != 0;
}

struct BackupEntry {
    string name;
    string time;
    unsigned long long size;
};

static std::vector<BackupEntry> listBackups() {
    std::vector<BackupEntry> out;
    WIN32_FIND_DATAW fd{};
    wstring pat = gBackupDir + L"\\*.json";
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        SYSTEMTIME st{};
        FILETIME ft = fd.ftLastWriteTime, local;
        FileTimeToLocalFileTime(&ft, &local);   // FILETIME 是 UTC，显示前转本地时间
        FileTimeToSystemTime(&local, &st);
        BackupEntry e;
        e.name = wtou8(fd.cFileName);
        e.time = fmtTime(st, true, true);
        e.size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        out.push_back(e);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(out.begin(), out.end(), [](const BackupEntry& a, const BackupEntry& b) {
        return a.name > b.name;     // 文件名带时间戳，倒序即最新在前
    });
    return out;
}

// 调用方需持有 gDataMutex
static void pruneBackups() {
    auto list = listBackups();
    if ((int)list.size() <= MAX_BACKUPS) return;
    for (size_t i = (size_t)MAX_BACKUPS; i < list.size(); ++i) {
        DeleteFileW((gBackupDir + L"\\" + u8tow(list[i].name)).c_str());
    }
}

// 上一次备份的时刻。删除这类破坏性操作靠它做冷却：
// 连着删十条只在第一次留一份，后面删的都还能从那份里找回来。
// 清空已完成、定时备份、手动备份都会把它推后，所以删完再点别的不会重复备。
static long long gLastBackupMs = 0;
static const long long BACKUP_COOLDOWN_MS = 60LL * 1000LL;

static long long nowUnixMs();   // 定义在「任务」那段，这里先用

// 调用方需持有 gDataMutex；返回备份文件名，失败返回空
static string makeBackupLocked() {
    if (!fileExists(gDataFile)) return string();
    ensureDir(gBackupDir);
    string name = "tasks-" + stampForFile() + ".json";
    wstring dst = gBackupDir + L"\\" + u8tow(name);
    if (!copyFileTo(gDataFile, dst)) return string();
    gLastBackupMs = nowUnixMs();
    pruneBackups();
    return name;
}

// 破坏性操作用：冷却期内跳过。返回是否真的备了。
// 调用方需持有 gDataMutex。
static bool backupIfIdle() {
    if (nowUnixMs() - gLastBackupMs < BACKUP_COOLDOWN_MS) return false;
    return !makeBackupLocked().empty();
}

// ---- 自动备份 ----
//
// 不再每次覆盖都备一份：勾选完成、改标题这类改动随手就能改回来，备了只是噪音，
// 还会把 40 份的额度几天就用光。现在只在固定的三个时刻自动备份，
// 另外控制台「清空已完成」会先手动触发一次（那是真删数据）。
// 时刻按本地时间算；程序没开着就等下次启动后的第一轮检查补上。
// 三个点都落在白天到睡前，因为改任务基本集中在这段时间。
static const int AUTO_BACKUP_HOURS[] = { 10, 16, 22 };
static const int AUTO_BACKUP_SLOTS = 3;

// 备份名里的时间戳前缀，例如 "20261106-120000"
static string stampOf(const SYSTEMTIME& st) {
    char b[24];
    snprintf(b, sizeof b, "%04d%02d%02d-%02d%02d%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return string(b);
}

// 最近一个已经过去的备份时刻。还没到今天的第一个时刻点时，退回昨天最后一个。
static SYSTEMTIME lastAutoSlot() {
    SYSTEMTIME st;
    GetLocalTime(&st);

    int hour = -1;
    for (int i = 0; i < AUTO_BACKUP_SLOTS; ++i) {
        if (AUTO_BACKUP_HOURS[i] <= st.wHour && AUTO_BACKUP_HOURS[i] > hour) hour = AUTO_BACKUP_HOURS[i];
    }
    if (hour < 0) {
        hour = AUTO_BACKUP_HOURS[AUTO_BACKUP_SLOTS - 1];
        // 往前推一天。走 FILETIME 是为了跨月、跨年时日期能正确回退
        FILETIME ft{};
        if (SystemTimeToFileTime(&st, &ft)) {
            ULARGE_INTEGER u;
            u.LowPart = ft.dwLowDateTime;
            u.HighPart = ft.dwHighDateTime;
            u.QuadPart -= 24ULL * 3600ULL * 10000000ULL;
            ft.dwLowDateTime = u.LowPart;
            ft.dwHighDateTime = u.HighPart;
            FileTimeToSystemTime(&ft, &st);
        }
    }

    st.wHour = (WORD)hour;
    st.wMinute = 0;
    st.wSecond = 0;
    st.wMilliseconds = 0;
    return st;
}

// 这一档之后有没有备份过。文件名是零填充的时间戳，直接按字符串比就行，
// 省掉本地时间和 UTC 的来回换算。
static bool hasBackupSince(const string& stamp) {
    auto list = listBackups();
    for (size_t i = 0; i < list.size(); ++i) {
        const string& n = list[i].name;          // tasks-YYYYMMDD-HHMMSS-mmm.json
        if (n.size() > 6 + 15 && n.compare(6, 15, stamp) >= 0) return true;
    }
    return false;
}

// 到点就补一份。内部自己拿锁，调用方不必持有。
static void sweepAutoBackup() {
    string stamp = stampOf(lastAutoSlot());

    std::lock_guard<std::mutex> lk(gDataMutex);
    if (hasBackupSince(stamp)) return;
    makeBackupLocked();
}

// 文件最后写入时间，毫秒；取不到返回 0
static long long fileMtimeMs(const wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fd{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fd)) return 0;
    ULARGE_INTEGER u;
    u.LowPart = fd.ftLastWriteTime.dwLowDateTime;
    u.HighPart = fd.ftLastWriteTime.dwHighDateTime;
    return (long long)((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

static long long newestBackupMtimeMs() {
    auto list = listBackups();                  // 已按名字倒序，[0] 最新
    if (list.empty()) return 0;
    return fileMtimeMs(gBackupDir + L"\\" + u8tow(list[0].name));
}

// 退出前留一份。只有当数据文件比最新备份还新时才备，也就是「这次会话确实改过东西」；
// 只是打开看一眼就关掉的话不占额度。
void serverBackupOnExit() {
    std::lock_guard<std::mutex> lk(gDataMutex);
    if (!fileExists(gDataFile)) return;

    long long dataMs = fileMtimeMs(gDataFile);
    long long backupMs = newestBackupMtimeMs();
    if (backupMs > 0 && dataMs <= backupMs) return;
    makeBackupLocked();
}

