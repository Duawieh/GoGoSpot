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
    // 关闭时只删除指向本程序的一项，避免影响其他副本的启动项
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

    // 悬浮清单启用时以其作为主界面；停用时才打开控制台
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
        swprintf(m, 160, L"%d-%d 端口都被占用了，后台启动失败。\n"
                         L"可以用 --port 指定一个别的端口，比如 GoGoSpot.exe --port 8800",
                 portStart, portEnd);
        showStartupError(m);
        WSACleanup();
        pauseBeforeExit();
        return -1;
    }

    // 记录端口，悬浮清单直接读取，无需逐个探测
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
