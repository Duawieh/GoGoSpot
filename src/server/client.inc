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

    // 仅支持 Content-Length 定长请求体；chunked 直接拒绝，避免客户端长时间等待
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
        // 客户端可能仍在发送请求体；先返回错误响应，再读取剩余数据。
        // 直接关闭连接会发送 RST，客户端将收到连接重置而非 413 响应。
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

