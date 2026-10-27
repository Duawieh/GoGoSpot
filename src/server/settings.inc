// ---- 设置 ----

static bool gWidgetEnabled = true;
static bool gTopmost = false;        // 悬浮清单是否压在其他窗口上面
static bool gAutoStart = false;      // 是否写进开机启动项

// HTTP 线程和悬浮清单线程都会改设置，写文件必须串起来，否则 .tmp 互相踩
static std::mutex gSettingsMutex;

// 便携副本（--data）不得修改全局开机启动项，以免覆盖正式安装的配置
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

// 关闭自启时只删除指向本程序的一项，其他副本可能正引用它
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

