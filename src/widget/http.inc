// ---- HTTP ----

struct HttpSession {
    HINTERNET ses = nullptr;
    HINTERNET con = nullptr;
    HttpSession() {
        ses = WinHttpOpen(L"GoGoSpotWidget/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (ses) {
            WinHttpSetTimeouts(ses, 600, 600, 900, 900);
            con = WinHttpConnect(ses, L"127.0.0.1", (INTERNET_PORT)g_port, 0);
        }
    }
    ~HttpSession() {
        if (con) WinHttpCloseHandle(con);
        if (ses) WinHttpCloseHandle(ses);
    }
    bool valid() const { return ses && con; }
};

static bool httpRequest(const wchar_t* verb, const wstring& path, string& out) {
    HttpSession s;
    if (!s.valid()) return false;

    HINTERNET req = WinHttpOpenRequest(s.con, verb, path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!req) return false;

    bool ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != 0;
    if (ok) ok = WinHttpReceiveResponse(req, nullptr) != 0;

    if (ok) {
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
            string buf(avail, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(req, &buf[0], avail, &got) || got == 0) break;
            out.append(buf.data(), got);
        }
    }
    WinHttpCloseHandle(req);
    return ok;
}

// 只放行 unreserved，其余按 UTF-8 字节转 %XX，中文标题才能塞进查询串
static wstring urlEncode(const string& s) {
    static const char* hex = "0123456789ABCDEF";
    string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return u8tow(out);
}

// 先读 data/.port，读不到再逐个端口试探。
static int readPortFile() {
    wstring path = g_root + L"\\data\\.port";
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return 0;
    char buf[32] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return 0;
    int v = atoi(buf);
    return (v > 0 && v < 65536) ? v : 0;
}

static int findServerPort() {
    int hint = readPortFile();
    if (hint) {
        g_port = hint;
        string body;
        if (httpRequest(L"GET", L"/api/health", body) && body.find("GoGoSpot") != string::npos) {
            return hint;
        }
    }
    for (int p = PORT_SCAN_START; p <= PORT_SCAN_END; ++p) {
        if (p == hint) continue;
        g_port = p;
        string body;
        if (httpRequest(L"GET", L"/api/health", body) && body.find("GoGoSpot") != string::npos) {
            return p;
        }
    }
    g_port = 0;
    return 0;
}

