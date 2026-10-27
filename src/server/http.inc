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

