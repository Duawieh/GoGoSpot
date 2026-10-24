// GoGoSpot 后台：托管 web/ 静态文件，用 REST 接口读写 data/tasks.json，
// 覆盖写入前自动备份（最多 40 份）。纯 Winsock2 实现，不依赖第三方库。

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>

#include "app.h"

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <map>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <ctime>
#include <utility>

using std::string;
using std::wstring;

// ---- 常量 ----

static const char*   APP_TAG     = "GoGoSpot";
static const size_t  MAX_BODY    = 16u * 1024 * 1024;   // 单次请求体上限 16MB
static const int     MAX_BACKUPS = 40;

// ---- 全局路径 ----

static wstring gRoot;        // exe 所在目录
static wstring gWebDir;      // <root>\web
static wstring gDataDir;     // <root>\data
static wstring gDataFile;    // <root>\data\tasks.json
static wstring gBackupDir;   // <root>\data\backups
static wstring gSettingsFile;   // <root>\data\settings.json
static DWORD   gStartTick = 0;
static SOCKET  g_listener = INVALID_SOCKET;

static std::mutex gDataMutex;   // 保护数据文件的所有读写

// ---- 编码转换 ----

static wstring u8tow(const string& s) {
    if (s.empty()) return wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return wstring();
    wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static string wtou8(const wstring& w) {
    if (w.empty()) return string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return string();
    string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// 按 UTF-8 字符边界截断，顺手把非法序列换成 U+FFFD。
// 写盘的东西一律走这里：切在多字节字符中间会让整个 tasks.json 变成非法 UTF-8，
// 控制台 res.json() 直接抛异常，列表整份读不出来。
static string utf8Clip(const string& in, size_t maxBytes) {
    string out;
    out.reserve(std::min(in.size(), maxBytes));
    size_t i = 0;
    while (i < in.size()) {
        unsigned char c = (unsigned char)in[i];
        size_t len = 1;
        if (c < 0x80) {
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        } else {
            if (out.size() + 3 > maxBytes) break;
            out += "\xEF\xBF\xBD";                 // 孤立的续字节
            ++i;
            continue;
        }

        bool good = (i + len <= in.size());
        for (size_t k = 1; good && k < len; ++k) {
            if (((unsigned char)in[i + k] & 0xC0) != 0x80) good = false;
        }
        if (!good) {
            if (out.size() + 3 > maxBytes) break;
            out += "\xEF\xBF\xBD";
            ++i;
            continue;
        }
        if (out.size() + len > maxBytes) break;    // 放不下整个字符就停在这里
        out.append(in, i, len);
        i += len;
    }
    return out;
}

// ---- 时间 ----

static string fmtTime(const SYSTEMTIME& st, bool withDate, bool withSep) {
    char b[64];
    if (withDate) {
        if (withSep) snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d",
                              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        else         snprintf(b, sizeof b, "%04d%02d%02d-%02d%02d%02d",
                              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    } else {
        snprintf(b, sizeof b, "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    }
    return string(b);
}

// 备份和导出用：20261021-153012-345。带毫秒，同一秒里连按两次也不会互相覆盖
static string stampForFile() {
    SYSTEMTIME st; GetLocalTime(&st);
    char b[40];
    snprintf(b, sizeof b, "%04d%02d%02d-%02d%02d%02d-%03d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return string(b);
}

static string isoNow() {                // 2026-10-21T15:30:12
    SYSTEMTIME st; GetLocalTime(&st);
    return fmtTime(st, true, true);
}

// "YYYY-MM-DDTHH:mm"（本地时间）-> Unix 毫秒。老数据只有日期，按当天 23:59 算。
static long long dueToUnixMs(const string& due) {
    if (due.size() < 10) return 0;
    int y = 0, mo = 0, d = 0, h = 23, mi = 59;
    if (sscanf(due.c_str(), "%d-%d-%d", &y, &mo, &d) != 3) return 0;
    if (due.size() >= 16 && (due[10] == 'T' || due[10] == ' ')) {
        sscanf(due.c_str() + 11, "%d:%d", &h, &mi);
    }
    if (y < 1970 || y > 2200 || mo < 1 || mo > 12 || d < 1 || d > daysOfMonth(y, mo) ||
        h < 0 || h > 23 || mi < 0 || mi > 59) {
        return 0;
    }

    SYSTEMTIME local{};
    local.wYear = (WORD)y; local.wMonth = (WORD)mo; local.wDay = (WORD)d;
    local.wHour = (WORD)h; local.wMinute = (WORD)mi;

    SYSTEMTIME utc{};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) return 0;

    FILETIME ft{};
    if (!SystemTimeToFileTime(&utc, &ft)) return 0;

    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;

    const unsigned long long EPOCH = 116444736000000000ULL;   // 1601 -> 1970
    if (u.QuadPart < EPOCH) return 0;
    return (long long)((u.QuadPart - EPOCH) / 10000ULL);
}

static string isoUtcNow() {             // 2026-10-21T07:30:12.345Z，与前端的格式一致
    SYSTEMTIME st; GetSystemTime(&st);
    char b[48];
    snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return b;
}

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

// ---- JSON 粗校验 ----

// 只检查括号配对和字符串闭合，不解析内容——后端不理解业务字段。
static bool looksLikeJson(const string& s) {
    size_t start = 0;
    while (start < s.size() && isspace((unsigned char)s[start])) start++;
    if (start >= s.size()) return false;
    if (s[start] != '{' && s[start] != '[') return false;

    int depth = 0;
    bool inStr = false, esc = false;
    for (size_t i = start; i < s.size(); ++i) {
        char c = s[i];
        if (inStr) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
        } else {
            if (c == '"') inStr = true;
            else if (c == '{' || c == '[') depth++;
            else if (c == '}' || c == ']') {
                depth--;
                if (depth == 0) return true;
                if (depth < 0) return false;
            }
        }
    }
    return false;
}

// ---- 轻量 JSON ----
//
// 只服务悬浮清单：列出未完成任务、精确改写 done。保留键顺序和未知字段，回写不丢东西。

struct JVal {
    enum Kind { Nul, Bool, Num, Str, Arr, Obj } kind = Nul;
    bool b = false;
    double num = 0;
    string str;
    std::vector<JVal> arr;
    std::vector<std::pair<string, JVal>> obj;

    JVal* get(const char* key) {
        for (auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
};

struct JParser {
    const string& s;
    size_t i = 0;
    bool ok = true;

    explicit JParser(const string& src) : s(src) {}

    void skipWs() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }

    bool parse(JVal& out) {
        skipWs();
        if (i >= s.size()) return false;
        char c = s[i];
        if (c == '{')      return parseObj(out);
        if (c == '[')      return parseArr(out);
        if (c == '"')      return parseStr(out);
        if (c == 't' || c == 'f') return parseBool(out);
        if (c == 'n')      { i += 4; out.kind = JVal::Nul; return true; }
        return parseNum(out);
    }

    bool parseObj(JVal& out) {
        out.kind = JVal::Obj;
        ++i;                       // '{'
        skipWs();
        if (i < s.size() && s[i] == '}') { ++i; return true; }
        while (i < s.size()) {
            skipWs();
            if (i >= s.size() || s[i] != '"') return false;
            JVal key;
            if (!parseStr(key)) return false;
            skipWs();
            if (i >= s.size() || s[i] != ':') return false;
            ++i;
            JVal v;
            if (!parse(v)) return false;
            out.obj.emplace_back(key.str, std::move(v));
            skipWs();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            return false;
        }
        return false;
    }

    bool parseArr(JVal& out) {
        out.kind = JVal::Arr;
        ++i;                       // '['
        skipWs();
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        while (i < s.size()) {
            JVal v;
            if (!parse(v)) return false;
            out.arr.push_back(std::move(v));
            skipWs();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            return false;
        }
        return false;
    }

    bool parseStr(JVal& out) {
        out.kind = JVal::Str;
        ++i;                       // '"'
        while (i < s.size()) {
            char c = s[i];
            if (c == '"') { ++i; return true; }
            if (c == '\\') {
                if (i + 1 >= s.size()) return false;
                char e = s[i + 1];
                switch (e) {
                    case 'n': out.str += '\n'; break;
                    case 't': out.str += '\t'; break;
                    case 'r': out.str += '\r'; break;
                    case 'b': out.str += '\b'; break;
                    case 'f': out.str += '\f'; break;
                    case '"': out.str += '"';  break;
                    case '\\': out.str += '\\'; break;
                    case '/': out.str += '/';  break;
                    case 'u': {
                        if (i + 5 >= s.size()) return false;
                        unsigned cp = (unsigned)strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16);
                        i += 6;
                        // 代理对
                        if (cp >= 0xD800 && cp <= 0xDBFF && i + 5 < s.size() &&
                            s[i] == '\\' && s[i + 1] == 'u') {
                            unsigned lo = (unsigned)strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16);
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                i += 6;
                            }
                        }
                        if (cp < 0x80) {
                            out.str += (char)cp;
                        } else if (cp < 0x800) {
                            out.str += (char)(0xC0 | (cp >> 6));
                            out.str += (char)(0x80 | (cp & 0x3F));
                        } else if (cp < 0x10000) {
                            out.str += (char)(0xE0 | (cp >> 12));
                            out.str += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out.str += (char)(0x80 | (cp & 0x3F));
                        } else {
                            out.str += (char)(0xF0 | (cp >> 18));
                            out.str += (char)(0x80 | ((cp >> 12) & 0x3F));
                            out.str += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out.str += (char)(0x80 | (cp & 0x3F));
                        }
                        continue;
                    }
                    default: out.str += e; break;
                }
                i += 2;
                continue;
            }
            out.str += c;
            ++i;
        }
        return false;
    }

    bool parseNum(JVal& out) {
        size_t st = i;
        while (i < s.size() && (isdigit((unsigned char)s[i]) || s[i] == '-' || s[i] == '+' ||
                                s[i] == '.' || s[i] == 'e' || s[i] == 'E')) ++i;
        if (st == i) return false;
        out.kind = JVal::Num;
        out.num = strtod(s.substr(st, i - st).c_str(), nullptr);
        return true;
    }

    bool parseBool(JVal& out) {
        if (s.compare(i, 4, "true") == 0)  { i += 4; out.kind = JVal::Bool; out.b = true;  return true; }
        if (s.compare(i, 5, "false") == 0) { i += 5; out.kind = JVal::Bool; out.b = false; return true; }
        return false;
    }
};

static void jsonEscapeInto(const string& in, string& out) {
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof b, "\\u%04x", c);
                    out += b;
                } else {
                    out += (char)c;
                }
        }
    }
}

static void jsonWrite(const JVal& v, string& out) {
    switch (v.kind) {
        case JVal::Nul:  out += "null"; break;
        case JVal::Bool: out += v.b ? "true" : "false"; break;
        case JVal::Num: {
            double d = v.num;
            if (d == (double)(long long)d && d < 1e15 && d > -1e15) {
                out += std::to_string((long long)d);
            } else {
                char b[40];
                snprintf(b, sizeof b, "%.17g", d);
                out += b;
            }
            break;
        }
        case JVal::Str: out += '"'; jsonEscapeInto(v.str, out); out += '"'; break;
        case JVal::Arr:
            out += '[';
            for (size_t i = 0; i < v.arr.size(); ++i) {
                if (i) out += ',';
                jsonWrite(v.arr[i], out);
            }
            out += ']';
            break;
        case JVal::Obj:
            out += '{';
            for (size_t i = 0; i < v.obj.size(); ++i) {
                if (i) out += ',';
                out += '"';
                jsonEscapeInto(v.obj[i].first, out);
                out += "\":";
                jsonWrite(v.obj[i].second, out);
            }
            out += '}';
            break;
    }
}

// 调用方需持有 gDataMutex
static bool loadTasksRoot(JVal& root) {
    string raw;
    if (!readFileAll(gDataFile, raw) || raw.empty()) raw = "{\"schema\":1,\"tasks\":[]}";
    JParser p(raw);
    if (!p.parse(root) || root.kind != JVal::Obj) return false;
    return true;
}

static bool saveTasksRoot(const JVal& root) {
    string out;
    jsonWrite(root, out);
    return writeFileAtomic(gDataFile, out);
}

static string defaultData() {

    return "{\"schema\":1,\"updatedAt\":\"" + isoNow() + "\",\"tasks\":[]}";
}

// ---- 设置 ----

static bool gWidgetEnabled = true;
static bool gTopmost = false;        // 悬浮清单是否压在其他窗口上面
static bool gAutoStart = false;      // 是否写进开机启动项

// HTTP 线程和悬浮清单线程都会改设置，写文件必须串起来，否则 .tmp 互相踩
static std::mutex gSettingsMutex;

// 便携副本（--data）不该动全局开机启动项，否则会把正式安装的那条顶掉
static bool gManageAutoRun = true;

// 极简取值：找 "key" 后面第一个 true/false。设置文件只有几行，不值得上解析器。
static bool readBoolField(const string& s, const char* key, bool& out) {
    string k = string("\"") + key + "\"";
    size_t p = s.find(k);
    if (p == string::npos) return false;
    size_t c = s.find(':', p + k.size());
    if (c == string::npos) return false;
    size_t t = s.find_first_not_of(" \t\r\n", c + 1);
    if (t == string::npos) return false;
    if (s.compare(t, 4, "true") == 0)  { out = true;  return true; }
    if (s.compare(t, 5, "false") == 0) { out = false; return true; }
    return false;
}

static void loadSettings() {
    string raw;
    if (!readFileAll(gSettingsFile, raw)) return;
    readBoolField(raw, "widget", gWidgetEnabled);
    readBoolField(raw, "topmost", gTopmost);
    readBoolField(raw, "autostart", gAutoStart);
}

static void saveSettings() {
    std::lock_guard<std::mutex> lk(gSettingsMutex);
    writeFileAtomic(gSettingsFile,
        string("{\n  \"widget\": ") + (gWidgetEnabled ? "true" : "false") +
        ",\n  \"topmost\": " + (gTopmost ? "true" : "false") +
        ",\n  \"autostart\": " + (gAutoStart ? "true" : "false") + "\n}\n");
}

bool appWidgetEnabled() { return gWidgetEnabled; }

void appSetWidgetEnabled(bool on) {
    gWidgetEnabled = on;
    saveSettings();
    widgetNotifyEnabled(on);        // 通知窗口显示/隐藏，跨线程安全
}

bool appTopmostEnabled() { return gTopmost; }

void appSetTopmost(bool on) {
    gTopmost = on;
    saveSettings();
    widgetNotifyTopmost(on);
}

// 写 HKCU 的 Run 项。路径每次现取，exe 挪了位置也能对上。
static void applyAutoRun(bool on) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) {
        return;
    }
    if (on) {
        wchar_t exe[4096];
        DWORD n = GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
        if (n > 0) {
            wstring cmd = L"\"" + wstring(exe, n) + L"\"";
            RegSetValueExW(k, L"GoGoSpot", 0, REG_SZ, (const BYTE*)cmd.c_str(),
                           (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        }
    } else {
        RegDeleteValueW(k, L"GoGoSpot");
    }
    RegCloseKey(k);
}

// 关掉自启时只删指向自己的那一项：别的副本可能正拿它当入口
static void removeAutoRunIfOurs() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k) != ERROR_SUCCESS) {
        return;
    }
    wchar_t cur[4096] = {0};
    DWORD sz = (DWORD)sizeof(cur) - sizeof(wchar_t), type = 0;
    if (RegQueryValueExW(k, L"GoGoSpot", nullptr, &type, (LPBYTE)cur, &sz) == ERROR_SUCCESS) {
        wchar_t exe[4096];
        DWORD n = GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
        if (n > 0 && wstring(L"\"") + wstring(exe, n) + L"\"" == cur) {
            RegDeleteValueW(k, L"GoGoSpot");
        }
    }
    RegCloseKey(k);
}

bool appAutoStartEnabled() { return gAutoStart; }

void appSetAutoStart(bool on) {
    gAutoStart = on;
    saveSettings();
    if (!gManageAutoRun) return;      // 便携副本不碰全局启动项
    if (on) applyAutoRun(true);
    else    removeAutoRunIfOurs();
}

// ---- HTTP 工具 ----

struct Response {
    int status = 200;
    string contentType = "application/json; charset=utf-8";
    string body;
    std::vector<std::pair<string, string>> headers;
};

static const char* statusText(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default:  return "OK";
    }
}

static string mimeOf(const string& ext) {
    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "css")  return "text/css; charset=utf-8";
    if (ext == "js")   return "application/javascript; charset=utf-8";
    if (ext == "json") return "application/json; charset=utf-8";
    if (ext == "svg")  return "image/svg+xml";
    if (ext == "png")  return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif")  return "image/gif";
    if (ext == "ico")  return "image/x-icon";
    if (ext == "woff2") return "font/woff2";
    if (ext == "woff") return "font/woff";
    if (ext == "txt" || ext == "md") return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

static bool sendAll(SOCKET s, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        int n = send(s, data + off, (int)std::min<size_t>(len - off, 1u << 20), 0);
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

static void sendResponse(SOCKET s, const Response& r, bool headOnly = false) {
    string head;
    head.reserve(512);
    head += "HTTP/1.1 " + std::to_string(r.status) + " " + statusText(r.status) + "\r\n";
    head += "Content-Type: " + r.contentType + "\r\n";
    head += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
    head += "Cache-Control: no-store\r\n";
    head += "X-Content-Type-Options: nosniff\r\n";
    head += "Connection: close\r\n";
    for (auto& h : r.headers) head += h.first + ": " + h.second + "\r\n";
    head += "\r\n";
    if (!sendAll(s, head.data(), head.size())) return;
    if (!headOnly && !r.body.empty()) sendAll(s, r.body.data(), r.body.size());
}

static Response jsonError(int code, const string& msg) {
    Response r;
    r.status = code;
    string safe;
    for (char c : msg) {
        if (c == '"' || c == '\\') { safe += '\\'; safe += c; }
        else if ((unsigned char)c < 0x20) safe += ' ';
        else safe += c;
    }
    r.body = "{\"ok\":false,\"error\":\"" + safe + "\"}";
    return r;
}

static Response jsonOk(const string& extra) {
    Response r;
    r.body = "{\"ok\":true" + (extra.empty() ? string() : "," + extra) + "}";
    return r;
}

static string urlDecode(const string& in) {
    string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex(in[i + 1]), lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) { out += (char)(hi * 16 + lo); i += 2; continue; }
        }
        if (in[i] == '+') out += ' ';
        else out += in[i];
    }
    return out;
}

static std::map<string, string> parseQuery(const string& q) {
    std::map<string, string> m;
    size_t i = 0;
    while (i < q.size()) {
        size_t amp = q.find('&', i);
        if (amp == string::npos) amp = q.size();
        string kv = q.substr(i, amp - i);
        size_t eq = kv.find('=');
        if (eq != string::npos) m[urlDecode(kv.substr(0, eq))] = urlDecode(kv.substr(eq + 1));
        else if (!kv.empty()) m[urlDecode(kv)] = "";
        i = amp + 1;
    }
    return m;
}

// ---- 路由 ----

// Host 里取出主机名：去掉端口，[::1]:8765 先取方括号里的，再转小写。
// 比较必须整段相等 —— 用 find() 的话 127.0.0.1.evil.com 也能混过去。
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

// 任务文件必须能解析成对象且带 tasks 数组，坏文件宁可报错也别原样吐给前端。
// strict 再多要求每个元素都是对象：写入端用，挡住 {"tasks":[1,2]} 这种，
// 否则控制台会把数字当成没有标题的任务，画出两行空白。读的时候放宽，
// 免得一份脏数据让整个程序打不开。
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

// 没有截止时间的重复任务永远等不到下一期，界面上却写着“每天”。
// 写盘前统一把这种 repeat 清掉，返回是否改动过。
static string dueOf(const JVal& t);   // 定义在「重复任务」那段

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

        // 只剩「重复但没截止」这一种要顺手改掉，改过就重新序列化再写
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
    // 手放的坏文件也能出现在备份目录里，先确认能解析再覆盖现场
    if (!looksLikeTasksFile(content)) return jsonError(400, "备份内容已损坏，未覆盖当前数据");
    makeBackupLocked();                       // 恢复前也留一份现场
    if (!writeFileAtomic(gDataFile, content)) return jsonError(500, "write failed");
    return jsonOk("\"restored\":\"" + name + "\",\"bytes\":" + std::to_string(content.size()));
}

static Response apiDeleteBackup(const string& name) {
    // 只接受备份目录下的纯文件名，挡掉路径穿越
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

// 用系统默认程序打开：http 走浏览器，本地路径走文件管理器或关联程序
static Response apiOpen(const string& rawUrl) {
    string url = cleanLink(rawUrl);
    size_t a = url.find_first_not_of(" \t\r\n");
    size_t b = url.find_last_not_of(" \t\r\n");
    url = (a == string::npos) ? string() : url.substr(a, b - a + 1);
    if (url.empty()) return jsonError(400, "empty url");
    if (url.size() > 2048) return jsonError(400, "too long");

    wstring w = u8tow(url);
    HINSTANCE h = ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)h <= 32) return jsonError(500, "cannot open");
    return jsonOk("\"opened\":true");
}


// 悬浮清单用：纯文本，一行一条未完成任务，字段用 \t 分隔，省掉一个 JSON 解析器。
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

            // id 也得洗：里面有制表符或换行会把一行拆成两行，悬浮清单整份解析错位
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
                    if (one.kind != JVal::Str || one.str.empty()) continue;
                    if (!firstLink) rows += '|';
                    rows += cleanLink(one.str);
                    firstLink = false;
                }
            }
            rows += '\t';

            // 正在进行：点下去的时刻，0 表示没在进行
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

// 正在进行：记下点下去的时刻（epoch 毫秒），取消就置空
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

// 逾期没做的重复任务：到点了也派生下一期，免得一过期这条链就断了
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

// 从任务对象里取一个字符串 / 数字字段，取不到给默认值
static string strField(const JVal& t, const char* key) {
    JVal* v = const_cast<JVal&>(t).get(key);
    return (v && v->kind == JVal::Str) ? v->str : string();
}
static long long numField(const JVal& t, const char* key) {
    JVal* v = const_cast<JVal&>(t).get(key);
    return (v && v->kind == JVal::Num) ? (long long)v->num : 0;
}

// 进行中的任务走到 100%（时间进度用完）就发一条系统通知。
// 起始时间的算法和悬浮清单、控制台保持同一套：min(点下去的时刻, 截止 - 预估)。
static void sweepTimerDone() {
    // 同一次计时只通知一次。key 用 id#开始时刻，重开计时就是新的 key。
    // 第一次扫只登记不通知：程序刚起来时已经到 100% 的那些不算「刚走完」。
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

        // 勾完成就把「正在进行」停掉，别让进度条继续跑
        if (nowDone) {
            JVal* sa = t.get("startedAt");
            if (sa) {
                sa->kind = JVal::Nul;
                sa->num = 0;
                sa->str.clear();
            }
        }

        // 重复任务：勾完成就顺手把下一期建出来，spawnNext 会清掉自己的重复标记
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
// id 用「系统时间 + 进程内自增序号 + 线程 id」拼，不再用 rand()：
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

// "YYYY-MM-DDTHH:mm"：分隔符位置、年月日时分范围、当月天数都得对，不然宁可当成没填
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
    if (estimate > 1440) estimate = 1440;
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
        JVal v; v.kind = JVal::Str; v.str = one;
        vLks.arr.push_back(std::move(v));
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

// 悬浮清单右键「推迟一天」：在原截止时间上加 24 小时；没有截止时间就从现在算
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

// 悬浮清单右键「删除」：只删这一条，其余原样保留
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
        return apiOpen(url);
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

// ---- 单连接处理 ----

static void handleClient(SOCKET c) {
    DWORD tv = 20000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    string head;
    char buf[8192];
    size_t sep = string::npos;
    while ((sep = head.find("\r\n\r\n")) == string::npos) {
        int n = recv(c, buf, (int)sizeof(buf), 0);
        if (n <= 0) { closesocket(c); return; }
        head.append(buf, (size_t)n);
        if (head.size() > (1u << 16)) { closesocket(c); return; }
    }

    string headerPart = head.substr(0, sep);
    string body = head.substr(sep + 4);

    // ---- 请求行
    size_t eol = headerPart.find("\r\n");
    string reqLine = (eol == string::npos) ? headerPart : headerPart.substr(0, eol);
    size_t sp1 = reqLine.find(' ');
    size_t sp2 = (sp1 == string::npos) ? string::npos : reqLine.find(' ', sp1 + 1);
    if (sp1 == string::npos || sp2 == string::npos) { closesocket(c); return; }

    string method = reqLine.substr(0, sp1);
    string target = reqLine.substr(sp1 + 1, sp2 - sp1 - 1);

    // HEAD 照 GET 处理，但只发响应头，不发响应体
    bool headOnly = (method == "HEAD");
    if (headOnly) method = "GET";

    string path = target, query;
    size_t qm = target.find('?');
    if (qm != string::npos) { path = target.substr(0, qm); query = target.substr(qm + 1); }

    // ---- 头部
    std::map<string, string> headers;
    {
        size_t pos = (eol == string::npos) ? headerPart.size() : eol + 2;
        while (pos < headerPart.size()) {
            size_t next = headerPart.find("\r\n", pos);
            if (next == string::npos) next = headerPart.size();
            string line = headerPart.substr(pos, next - pos);
            size_t colon = line.find(':');
            if (colon != string::npos) {
                string k = line.substr(0, colon);
                string v = line.substr(colon + 1);
                while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(v.begin());
                std::transform(k.begin(), k.end(), k.begin(),
                               [](unsigned char ch) { return (char)tolower(ch); });
                headers[k] = v;
            }
            pos = next + 2;
        }
    }

    // 只支持 Content-Length 定长请求体，chunked 明确拒掉，别让客户端干等
    auto itTe = headers.find("transfer-encoding");
    if (itTe != headers.end() && itTe->second.find("chunked") != string::npos) {
        sendResponse(c, jsonError(411, "不支持 chunked，请带 Content-Length"));
        closesocket(c);
        return;
    }

    // ---- 补齐请求体
    size_t need = 0;
    auto itLen = headers.find("content-length");
    if (itLen != headers.end()) need = (size_t)strtoull(itLen->second.c_str(), nullptr, 10);
    if (need > MAX_BODY) {
        sendResponse(c, jsonError(413, "payload too large"), headOnly);
        // 客户端多半还在发：先把响应送出去，再把剩下的读掉。
        // 直接 close 会发 RST，对方看到的是「连接被重置」而不是 413。
        shutdown(c, SD_SEND);
        DWORD drain = 800;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&drain, sizeof(drain));
        DWORD t0 = GetTickCount();
        while (GetTickCount() - t0 < 3000) {
            int n = recv(c, buf, (int)sizeof(buf), 0);
            if (n <= 0) break;
        }
        closesocket(c);
        return;
    }

    while (body.size() < need) {
        int n = recv(c, buf, (int)std::min<size_t>(sizeof(buf), need - body.size()), 0);
        if (n <= 0) break;
        body.append(buf, (size_t)n);
    }
    if (body.size() > need) body.resize(need);

    // ---- 分派
    Response r;
    if (path.rfind("/api/", 0) == 0) {
        // 读接口也要验 Host（挡 DNS rebinding），写接口再加 Origin / Sec-Fetch-Site
        if (!sameOriginRequest(headers, method != "GET")) {
            r = jsonError(403, "cross-site request rejected");
        } else {
            r = handleApi(method, path, parseQuery(query), body);
        }
    } else if (method != "GET") {
        r = jsonError(405, "method not allowed");
    } else {
        r = serveStatic(path);
    }

    sendResponse(c, r, headOnly);
    shutdown(c, SD_SEND);
    closesocket(c);
}

// ---- 后台启动 ----

// 探测端口上是否已有本程序实例：非阻塞 connect + select 短超时，避开防火墙丢 SYN。
static bool portAlive(int port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);

    bool connected = false;
    int rc = connect(s, (sockaddr*)&a, sizeof(a));
    if (rc == 0) {
        connected = true;
    } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set wf, ef;
        FD_ZERO(&wf); FD_SET(s, &wf);
        FD_ZERO(&ef); FD_SET(s, &ef);
        timeval tv{0, 60000};   // 60ms
        int sel = select(0, nullptr, &wf, &ef, &tv);
        if (sel > 0 && FD_ISSET(s, &wf)) {
            int err = 0, len = (int)sizeof(err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
            connected = (err == 0);
        }
    }
    if (!connected) { closesocket(s); return false; }

    // 恢复阻塞模式并设置收发超时，做一次 HTTP 握手校验
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    DWORD tv2 = 400;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv2, sizeof(tv2));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv2, sizeof(tv2));

    const char* req = "GET /api/health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    bool ours = false;
    if (send(s, req, (int)strlen(req), 0) > 0) {
        // 响应头和体分两次发出，单次 recv 常只拿到头，要接着收到标识或对端关闭。
        char buf[512] = {0};
        int got = 0;
        while (got < (int)sizeof(buf) - 1) {
            int n = recv(s, buf + got, (int)sizeof(buf) - 1 - got, 0);
            if (n <= 0) break;
            got += n;
            if (strstr(buf, APP_TAG) != nullptr) { ours = true; break; }
        }
    }
    closesocket(s);
    return ours;
}

static SOCKET makeListener(int port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&one, sizeof(one));

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(s, (sockaddr*)&a, sizeof(a)) != 0) { closesocket(s); return INVALID_SOCKET; }
    if (listen(s, SOMAXCONN) != 0)              { closesocket(s); return INVALID_SOCKET; }
    return s;
}

static void openBrowser(int port) {
    wstring url = L"http://127.0.0.1:" + std::to_wstring(port) + L"/";
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// 出错时停一下再退出，双击运行时窗口不会一闪而过；stdin 已关闭时立即返回。
static void pauseBeforeExit() {
    printf("  按 Enter 键退出…\n");
    int c;
    while ((c = getchar()) != '\n' && c != EOF) {}
}

// 这是个 -mwindows 程序，双击运行时没有控制台，printf 谁也看不见。
// 启动失败必须弹个框，否则用户看到的就是「双击了，没反应」。
static void showStartupError(const wstring& msg) {
    MessageBoxW(nullptr, msg.c_str(), L"GoGoSpot 没能启动", MB_ICONERROR | MB_OK);
}

// ---- 启动 ----

// 返回监听端口；返回 <0 表示不该继续（已有实例在跑，或启动失败）
int serverStart(int argc, char** argv) {
    bool autoOpen = true;
    string dataArg;
    int portArg = 0;

    for (int i = 1; i < argc; ++i) {
        string a = argv[i];
        if (a == "--no-browser") {
            autoOpen = false;
        } else if (a == "--data" && i + 1 < argc) {
            dataArg = argv[++i];
        } else if (a == "--port" && i + 1 < argc) {
            portArg = atoi(argv[++i]);
            if (portArg <= 0 || portArg > 65535) {
                printf("  [错误] --port 需要 1-65535，收到 %d\n\n", portArg);
                pauseBeforeExit();
                return -1;
            }
        }
    }

    gStartTick = GetTickCount();

    gRoot = exeDir();
    gWebDir = gRoot + L"\\web";
    gDataDir = gRoot + L"\\data";
    gDataFile = gDataDir + L"\\tasks.json";
    gBackupDir = gDataDir + L"\\backups";
    gSettingsFile = gDataDir + L"\\settings.json";

    // --data 指定另一份数据文件，配合 --port 可以并行开多个互不干扰的实例。
    // 设置文件和备份目录都跟着数据目录走，否则便携副本会改到正式安装的那份设置。
    if (!dataArg.empty()) {
        gDataFile = u8tow(dataArg);
        size_t slash = gDataFile.find_last_of(L"\\/");
        gDataDir = (slash == wstring::npos) ? gRoot : gDataFile.substr(0, slash);
        gBackupDir = gDataDir + L"\\backups";
        gSettingsFile = gDataDir + L"\\settings.json";
        gManageAutoRun = false;
    }

    int portStart = portArg > 0 ? portArg : PORT_SCAN_START;
    int portEnd   = portArg > 0 ? portArg : PORT_SCAN_END;

    printf("\n");
    printf("  GoGoSpot  v%s\n", GOGOSPOT_VERSION);
    printf("  ----------------------------------------------------\n");

    if (!dirExists(gWebDir)) {
        printf("  [错误] 找不到前端目录: %s\n", wtou8(gWebDir).c_str());
        printf("  请确认 web 文件夹与 exe 位于同一目录。\n\n");
        showStartupError(L"找不到前端目录：\n" + gWebDir +
                         L"\n\n把 web 文件夹和 GoGoSpot.exe 放在同一层再运行。");
        pauseBeforeExit();
        return -1;
    }
    ensureDir(gDataDir);
    ensureDir(gBackupDir);
    loadSettings();
    // 开着自启就每次启动重写一遍，exe 挪了位置也能对上；
    // 关着只删指向自己的那条，别把别的副本的启动项顺手抹掉
    if (gManageAutoRun) {
        if (gAutoStart) applyAutoRun(true);
        else            removeAutoRunIfOurs();
    }

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("  [错误] 网络初始化失败\n");
        showStartupError(L"网络初始化失败（WSAStartup），这台机器上的 Winsock 可能有问题。");
        pauseBeforeExit();
        return -1;
    }

    // 悬浮清单开着就以它为主界面，不再弹浏览器；关掉了才需要控制台
    const bool wantBrowser = autoOpen && !gWidgetEnabled;
    if (!wantBrowser && autoOpen) {
        printf("  悬浮清单已开启，直接用它，不打开浏览器。\n");
    }

    // 已有实例在跑就直接打开它。
    // 显式指定了 --data 或 --port 说明想要一个独立实例，这时不该抢端口。
    bool isolated = !dataArg.empty() || portArg > 0;
    for (int p = portStart; !isolated && p <= portEnd; ++p) {
        if (portAlive(p)) {
            printf("  检测到已在运行的实例（端口 %d）。\n\n", p);
            if (wantBrowser) openBrowser(p);
            WSACleanup();
            return -1;
        }
    }

    SOCKET listener = INVALID_SOCKET;
    int port = 0;
    for (int p = portStart; p <= portEnd; ++p) {
        listener = makeListener(p);
        if (listener != INVALID_SOCKET) { port = p; break; }
    }
    if (listener == INVALID_SOCKET) {
        printf("  [错误] %d-%d 端口均被占用，无法启动。\n\n", portStart, portEnd);
        wchar_t m[160];
        swprintf(m, 160, L"%d-%d 端口都被占用了，后台没能启动。\n"
                         L"可以用 --port 指定一个别的端口，比如 GoGoSpot.exe --port 8800",
                 portStart, portEnd);
        showStartupError(m);
        WSACleanup();
        pauseBeforeExit();
        return -1;
    }

    // 记下端口，悬浮清单直接读，不用逐个试探
    writeFileAtomic(gDataDir + L"\\.port", std::to_string(port));

    printf("  数据文件: %s\n", wtou8(gDataFile).c_str());
    printf("  备份目录: %s\n", wtou8(gBackupDir).c_str());
    printf("  后台地址: http://127.0.0.1:%d/\n", port);

    if (wantBrowser) {
        std::thread([port]() {
            Sleep(500);
            openBrowser(port);
        }).detach();
    }

    // 每 20 秒扫一次逾期的重复任务，到点就派生下一期
    std::thread([]() {
        for (;;) {
            Sleep(20000);
            sweepOverdueRepeating();
        }
    }).detach();

    // 每 5 秒看一次有没有任务计时走完，走完就发系统通知
    std::thread([]() {
        for (;;) {
            Sleep(5000);
            sweepTimerDone();
        }
    }).detach();

    // accept 循环交给后台线程，主线程留给窗口和托盘
    g_listener = listener;
    std::thread([]() {
        for (;;) {
            sockaddr_in cli{};
            int len = (int)sizeof(cli);
            SOCKET c = accept(g_listener, (sockaddr*)&cli, &len);
            if (c == INVALID_SOCKET) {
                if (g_listener == INVALID_SOCKET || WSAGetLastError() == WSAEINTR) break;
                continue;
            }
            std::thread(handleClient, c).detach();
        }
    }).detach();

    return port;
}

void serverStop() {
    SOCKET s = g_listener;
    g_listener = INVALID_SOCKET;
    if (s != INVALID_SOCKET) closesocket(s);
}
