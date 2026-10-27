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

    const JVal* get(const char* key) const {
        for (const auto& kv : obj) if (kv.first == key) return &kv.second;
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

