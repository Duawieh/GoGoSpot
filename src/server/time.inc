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

// 备份和导出用：20261021-153012-345。包含毫秒，支持同一秒内多次备份且避免文件名冲突
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

