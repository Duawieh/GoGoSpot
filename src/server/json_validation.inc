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

