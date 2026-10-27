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

// 出错时暂停后退出，避免双击运行时窗口一闪而过；stdin 已关闭时立即返回。
static void pauseBeforeExit() {
    printf("  按 Enter 键退出…\n");
    int c;
    while ((c = getchar()) != '\n' && c != EOF) {}
}

// 程序使用 -mwindows 构建，标准输出不可见；启动失败时通过消息框提供错误信息。
static void showStartupError(const wstring& msg) {
    MessageBoxW(nullptr, msg.c_str(), L"GoGoSpot 启动失败", MB_ICONERROR | MB_OK);
}

