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

// 调用方需持有 gDataMutex；返回备份文件名，失败返回空
static string makeBackupLocked() {
    if (!fileExists(gDataFile)) return string();
    ensureDir(gBackupDir);
    string name = "tasks-" + stampForFile() + ".json";
    wstring dst = gBackupDir + L"\\" + u8tow(name);
    if (!copyFileTo(gDataFile, dst)) return string();
    pruneBackups();
    return name;
}

