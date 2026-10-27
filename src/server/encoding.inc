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

// 按 UTF-8 字符边界截断，非法序列替换为 U+FFFD。
// 持久化前统一验证并截断 UTF-8，避免多字节字符序列不完整导致任务文件无法解析。
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

