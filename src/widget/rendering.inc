// ---- 绘制 ----

// GoGoSpot 标识：3/4 圆弧 + 圆点，比例取自 1024 画布。
void drawMark(Graphics& g, float cx, float cy, float box, Brush* brush) {
    const REAL S  = box;
    const REAL R  = 0.342f * S;     // 圆弧中心线半径
    const REAL W  = 0.090f * S;     // 线宽
    const REAL DR = 0.201f * S;     // 圆点半径
    const REAL BX = 0.182f * S;     // 横杠起点

    LinearGradientBrush grad(
        PointF(cx - S / 2, cy - S / 2), PointF(cx + S / 2, cy + S / 2),
        Color(255, 86, 118, 255), Color(255, 180, 118, 255));
    Brush* br = brush ? brush : &grad;

    Pen pen(br, W);
    pen.SetLineCap(LineCapRound, LineCapFlat, DashCapFlat);

    // 横杠 + 3/4 圆弧（缺口在右上）
    g.DrawLine(&pen, cx + BX, cy, cx + R, cy);
    g.DrawArc(&pen, cx - R, cy - R, R * 2, R * 2, 0.0f, 270.0f);

    g.FillEllipse(br, cx - DR, cy - DR, DR * 2, DR * 2);
}

static void progressOf(const Task& t, long long nowMs, long long& startMs, REAL& ratio);
static void capsuleProgress(Graphics& g, const RectF& r, REAL t, Pen& pen);
static Color fadeCol(const Color& c, REAL a);
static Color progressColorOf(const Task& t, REAL ratio);

// 收起态的窄条：vis 是这一帧露出的宽度，整条左移 (full - vis)，以平滑展开效果呈现。
// 两套外观，跟 Windows 的浅色/深色模式走：
//   日间——浅色磨砂底 + 暖色渐变，浅壁纸上不扎眼；
//   夜间——纯灰块 + 边缘外发光，颜色只留在那圈光里。
// 透明度都用 smoothstep，起步慢、中段陡、到满宽前就收平。
static void paintPill(Graphics& g, REAL vis, REAL full) {
    const Task* t = pillTask();
    if (!t || vis <= 0 || full <= 0) return;
    if (vis > full) vis = full;

    REAL prog = vis / full;
    REAL fade = prog * prog * (3.0f - 2.0f * prog);   // smoothstep
    REAL shadowFade = fade * fade * fade;
    auto A = [&](BYTE a) { return (BYTE)(a * fade); };
    auto S = [&](BYTE a) { return (BYTE)(a * shadowFade); };

    const Urgency u    = urgencyOf(*t);
    const Color   warm = urgencyColor(u);

    // 左端固定在圆点右侧，宽度从 0 长到 full——以平滑展开效果呈现，
    // 但不需要任何裁切，边缘永远是完整的圆角
    RectF r((REAL)PILL_X, (REAL)((DOT_SIZE - PILL_H) / 2), vis, (REAL)PILL_H);
    const REAL rad = (vis < PILL_H) ? vis / 2.0f : PILL_H / 2.0f;

    GraphicsPath p;
    roundRect(p, r, rad);

    if (g_dayMode) {
        // 浅色背景下通过投影增强面板底色的层次感
        for (int i = 3; i >= 1; --i) {
            REAL grow = (REAL)i * 1.2f;
            GraphicsPath sp;
            roundRect(sp, RectF(r.X - grow, r.Y - grow + 1.5f,
                                r.Width + grow * 2, r.Height + grow * 2), rad + grow);
            SolidBrush sb(Color(S((BYTE)(8 + (3 - i) * 5)), 0, 0, 0));
            g.FillPath(&sb, &sp);
        }

        SolidBrush base(Color(A(232), 249, 249, 252));
        g.FillPath(&base, &p);

        BYTE gA = g_pillHover ? 76 : 58;
        LinearGradientBrush wb(PointF(r.X, r.Y), PointF(r.X + r.Width, r.Y),
                               Color(A(gA), warm.GetR(), warm.GetG(), warm.GetB()),
                               Color(A(12), warm.GetR(), warm.GetG(), warm.GetB()));
        g.FillPath(&wb, &p);

        Pen border(Color(A(g_pillHover ? 128 : 96), warm.GetR(), warm.GetG(), warm.GetB()), 1.0f);
        g.DrawPath(&border, &p);
    } else {
        // 外发光：九层往外扩的圆角矩形，越外越淡。深灰块靠这圈光浮起来，
        // 同时替代投影。
        // 裁去胶囊本体区域，避免下层发光色透过半透明背景造成偏色。
        {
            REAL growMax = 9 * 1.3f;
            GraphicsPath outer;
            roundRect(outer, RectF(r.X - growMax, r.Y - growMax,
                                   r.Width + growMax * 2, r.Height + growMax * 2),
                      rad + growMax);
            Region clip(&outer);
            Region body(&p);
            clip.Exclude(&body);

            GraphicsState st = g.Save();
            g.SetClip(&clip, CombineModeReplace);

            REAL glowK = g_pillHover ? 1.25f : 1.0f;
            for (int i = 9; i >= 1; --i) {
                REAL grow = i * 1.3f;
                REAL k = i / 9.0f;
                BYTE a = A((BYTE)((52 * (1 - k) * (1 - k) + 4) * glowK));
                GraphicsPath gp;
                roundRect(gp, RectF(r.X - grow, r.Y - grow,
                                    r.Width + grow * 2, r.Height + grow * 2), rad + grow);
                SolidBrush sb(Color(a, warm.GetR(), warm.GetG(), warm.GetB()));
                g.FillPath(&sb, &gp);
            }
            g.Restore(st);
        }

        BYTE bg = g_pillHover ? 58 : 40;
        SolidBrush base(Color(A(255), bg, (BYTE)(bg + 2), (BYTE)(bg + 10)));
        g.FillPath(&base, &p);

        Pen hi(Color(A(30), 255, 255, 255), 1.0f);
        g.DrawPath(&hi, &p);
    }

    // 正在进行的任务：沿着胶囊内侧跑一圈进度
    if (t->startedAt > 0) {
        long long startMs = 0;
        REAL ratio = 0;
        progressOf(*t, nowMs(), startMs, ratio);

        RectF track(r.X + 3.5f, r.Y + 3.5f, r.Width - 7.0f, r.Height - 7.0f);
        if (track.Width > track.Height) {
            GraphicsPath tp;
            roundRect(tp, track, track.Height / 2.0f);
            Pen trackPen(Color(A(46), 255, 255, 255), 2.0f);
            g.DrawPath(&trackPen, &tp);

            REAL anim = (g_progAll || (!g_progId.empty() && t->id == g_progId)) ? g_progAnim : 1.0f;
            if (!hasEstimate(*t)) {
                // 没填预估：整圈呼吸，不表示具体比例。环形固定用紫色，
                // 不跟紧急程度走——那是行内填充和圆点外圈的事。
                BYTE a = (BYTE)(A((BYTE)(130 + 105 * breatheK())) * anim);
                Pen ringPen(Color(a, C_PURPLE.GetR(), C_PURPLE.GetG(), C_PURPLE.GetB()), 2.6f);
                ringPen.SetStartCap(LineCapRound);
                ringPen.SetEndCap(LineCapRound);
                g.DrawPath(&ringPen, &tp);
            } else {
                ratio *= anim;
                if (ratio > 0.002f) {
                    // 和列表里的进度条同一套：按占比分绿 / 黄 / 红
                    Color pc = progressColorOf(*t, ratio);
                    Pen ringPen(Color(A(255), pc.GetR(), pc.GetG(), pc.GetB()), 2.6f);
                    ringPen.SetStartCap(LineCapRound);
                    ringPen.SetEndCap(LineCapRound);
                    capsuleProgress(g, track, ratio, ringPen);
                }
            }
        }
    }

    // 左边那道竖条，和控制台列表里紧急任务的样式对齐
    GraphicsPath bar;
    roundRect(bar, RectF(r.X + 11, r.Y + 11, 2.5f, r.Height - 22), 1.25f);
    SolidBrush bb(Color(A(245), warm.GetR(), warm.GetG(), warm.GetB()));
    g.FillPath(&bb, &bar);

    // 右侧那一列：平时只有剩余时间；进行中改成上下两行——上面剩余时间，下面倒计时
    wstring due = dueText(*t);
    bool running = (t->startedAt > 0);
    long long runStart = 0;
    REAL runRatio = 0;
    wstring tail;                       // 第二行：倒计时，没预估就写“正在进行”
    if (running) {
        progressOf(*t, nowMs(), runStart, runRatio);
        tail = hasEstimate(*t)
             ? clockText((long long)t->estimate * 60000LL - (nowMs() - runStart))
             : wstring(L"正在进行");
    }

    REAL dueW = 0;
    auto needW = [&](const wstring& s) {
        if (s.empty()) return (REAL)0;
        RectF meas;
        g.MeasureString(s.c_str(), -1, smallFont(), PointF(0, 0), &meas);
        return meas.Width + 2;
    };
    dueW = needW(due);
    if (running) {
        REAL w2 = needW(tail);
        if (w2 > dueW) dueW = w2;
    }
    if (dueW > 64) dueW = 64;

    // 排版按满宽算，画的时候裁到当前宽度——文字是随着窄条长出来一点点露出来的，
    // 不会因为宽度不够被挤成省略号
    REAL left  = (REAL)PILL_X + 22;
    REAL right = (REAL)PILL_X + full - 15;
    REAL titleW = right - left - (dueW > 0 ? dueW + 8 : 0);

    GraphicsState textState = g.Save();
    g.SetClip(r, CombineModeIntersect);

    Color titleCol = g_dayMode ? Color(A(255), 34, 38, 46)
                               : Color(A(255), 248, 249, 252);
    drawText(g, t->title, titleFont(), RectF(left, r.Y, titleW, r.Height),
             titleCol, true, 0);

    if (dueW > 0) {
        Color dueCol;
        if (u == URG_RED) {
            dueCol = g_dayMode ? Color(A(255), 172, 52, 76) : Color(A(255), 255, 186, 198);
        } else if (u == URG_YELLOW) {
            dueCol = g_dayMode ? Color(A(255), 152, 96, 18) : Color(A(255), 255, 214, 150);
        } else {
            dueCol = g_dayMode ? Color(A(255), 104, 72, 166) : Color(A(255), 214, 198, 255);
        }

        if (running) {
            // 两行：上面剩余时间，下面倒计时。倒计时的颜色跟进度条同一档。
            // 40px 高的窄条塞两行 11px 小字，行心定在 12.5 / 27.5，
            // 上下各留 7px——再往上顶就贴边了
            drawText(g, due, smallFont(), RectF(right - dueW, r.Y + 3.5f, dueW, 18),
                     dueCol, true, 2);
            Color tailCol = hasEstimate(*t) ? progressColorOf(*t, runRatio) : C_PURPLE;
            drawText(g, tail, smallFont(), RectF(right - dueW, r.Y + 18.5f, dueW, 18),
                     tailCol, true, 2);
        } else {
            drawText(g, due, smallFont(), RectF(right - dueW, r.Y, dueW, r.Height),
                     dueCol, true, 2);
        }
    }

    g.Restore(textState);
}

// 圆点外圈：红色套红圈，黄色套黄圈；八层柔光 + 实环 + 白芯，画在最底层。
static void paintUrgencyRing(Graphics& g, REAL cx, REAL cy, Urgency u) {
    if (u == URG_NONE) return;
    const Color c = (u == URG_RED) ? C_RED : C_AMBER;

    REAL r = RING_R_IDLE + (RING_R - RING_R_IDLE) * g_ringT;

    // 八层柔光：越往外越淡，铺到环外 8px
    for (int i = 8; i >= 1; --i) {
        REAL t = i / 8.0f;
        REAL rr = r + i * 1.0f;
        BYTE a = (BYTE)(62 * (1 - t) * (1 - t) + 8);
        Pen glow(Color(a, c.GetR(), c.GetG(), c.GetB()), 4.0f);
        g.DrawEllipse(&glow, cx - rr, cy - rr, rr * 2, rr * 2);
    }

    Pen pen(Color(245, c.GetR(), c.GetG(), c.GetB()), 2.8f);
    g.DrawEllipse(&pen, cx - r, cy - r, r * 2, r * 2);

    // 白色高光增强环形进度的层次感
    Pen core(Color(185, 255, 253, 254), 1.1f);
    g.DrawEllipse(&core, cx - r, cy - r, r * 2, r * 2);
}

// 收起状态：中间画标识，外圈柔光保留
static void paintDot(Graphics& g) {
    // 圆点按屏幕大小再放大一档，里面的窄条、圆环、字号跟着一起放大
    g.ScaleTransform(g_dotK, g_dotK);

    REAL cx = DOT_SIZE / 2.0f;
    REAL cy = cx;

    // 紧急圈先铺，窄条和圆点都压在它上面
    paintUrgencyRing(g, cx, cy, worstUrgency());

    // 圆点外那圈进度：内壁贴着圆点，外径比紧急光环小；窄条展开时淡出
    if (g_dotRingA > 0.02f) {
        const Task* pt = pillTask();
        if (pt && pt->startedAt > 0) {
            long long st = 0;
            REAL r2 = 0;
            progressOf(*pt, nowMs(), st, r2);
            REAL anim = (g_progAll || (!g_progId.empty() && pt->id == g_progId)) ? g_progAnim : 1.0f;
            const REAL rr = 12.2f;      // 内壁 9.6，外沿 14.8，都在光环里面

            if (!hasEstimate(*pt)) {
                // 未填写预估时以紫色呼吸环表示任务处于进行状态，不显示具体进度比例。
                BYTE a = (BYTE)(255 * g_dotRingA * (0.55f + 0.45f * breatheK()) * anim);
                Pen pen(Color(a, C_PURPLE.GetR(), C_PURPLE.GetG(), C_PURPLE.GetB()), 5.2f);
                g.DrawEllipse(&pen, cx - rr, cy - rr, rr * 2, rr * 2);
            } else {
                r2 *= anim;
                if (r2 > 0.002f) {
                    Color c2 = progressColorOf(*pt, r2);
                    Pen pen(Color((BYTE)(255 * g_dotRingA), c2.GetR(), c2.GetG(), c2.GetB()), 5.2f);
                    pen.SetStartCap(LineCapRound);
                    pen.SetEndCap(LineCapRound);
                    g.DrawArc(&pen, cx - rr, cy - rr, rr * 2, rr * 2, -90.0f, -r2 * 360.0f);
                }
            }
        }
    }

    if (g_pillW > 0) {
        // 窄条从圆点右边“长”出来，不再靠裁切藏左端。
        // 裁切会使半透明底与外发光在边界处产生硬线，该线会显露于圆点上下两侧。
        paintPill(g, (REAL)g_pillW / dotUnit(), (REAL)g_pillFull / dotUnit());
    }

    for (int i = 3; i >= 1; --i) {
        REAL rr = 9.0f + i * 2.4f;
        SolidBrush b(Color((BYTE)(10 + (3 - i) * 7), 255, 255, 255));
        g.FillEllipse(&b, cx - rr, cy - rr, rr * 2, rr * 2);
    }

    // 深色外圈用于提升浅色壁纸上的边缘对比度；负空间透明，因此提高该层不透明度。
    SolidBrush ring(Color(226, 12, 15, 22));
    g.FillEllipse(&ring, cx - 9.2f, cy - 9.2f, 18.4f, 18.4f);

    drawMark(g, cx, cy, 20.0f);
}

// ---------------------------------------------------------------- 就地添加面板

static REAL panelTop()      { return (REAL)(CARD_H - PANEL_H); }
static REAL panelInputTop() { return panelTop() + 8; }
static REAL panelDueTop()   { return panelTop() + 46; }
static REAL panelEstTop()   { return panelTop() + 76; }
static REAL panelLinkTop()  { return panelTop() + 106; }

// “关联材料”那一行：选择 / 网址输入框 / 清空
static RectF linkPickRect()   { return RectF(68, panelLinkTop(), 44, (REAL)CHIP_H); }
static RectF linkInputRect()  { return RectF(116, panelLinkTop(), 166, (REAL)CHIP_H); }
static RectF linkClearRect()  { return RectF(286, panelLinkTop(), 22, (REAL)CHIP_H); }

// 选择来源的菜单：Windows 的选择框不能文件和文件夹混选，这里分两行
static RectF pickMenuRect()   { return RectF(68, panelLinkTop() - 62, 104, 58); }
static RectF pickMenuFile()   { return RectF(76, panelLinkTop() - 58, 88, 26); }
static RectF pickMenuFolder() { return RectF(76, panelLinkTop() - 30, 88, 26); }

// 仅接受网址与绝对路径，其余视为无效，不加入任务
static bool looksLikeLinkW(const wstring& u) {
    if (u.rfind(L"http://", 0) == 0 || u.rfind(L"https://", 0) == 0) return true;
    if (u.rfind(L"ftp://", 0) == 0 || u.rfind(L"file://", 0) == 0) return true;
    if (u.size() > 2 && u[1] == L':' && (u[2] == L'\\' || u[2] == L'/')) return true;
    if (u.rfind(L"\\\\", 0) == 0) return true;
    return false;
}

// 输入框右端的 ×：单击放弃本次添加
static RectF cancelRect() { return RectF((REAL)CARD_W - 36, panelInputTop() + 4, 22, 22); }

// 卡片坐标系（不含 SHADOW 边距）里第 row 排第 i 个选项的位置。
// 画和点击判定都用它，两边不会对不上。
static RectF chipRect(int row, int i, int count) {
    const REAL x0 = 44, x1 = (REAL)CARD_W - 10, gap = 6.0f;
    REAL w = (x1 - x0 - gap * (count - 1)) / count;
    REAL y = (row == 0) ? panelDueTop() : panelEstTop();
    return RectF(x0 + i * (w + gap), y, w, (REAL)CHIP_H);
}

static void paintChip(Graphics& g, const RectF& r, const wchar_t* label, bool on) {
    GraphicsPath p;
    roundRect(p, r, 12.0f);
    if (on) {
        SolidBrush b(Color(58, C_ACCENT.GetR(), C_ACCENT.GetG(), C_ACCENT.GetB()));
        g.FillPath(&b, &p);
        Pen pen(Color(150, C_ACCENT.GetR(), C_ACCENT.GetG(), C_ACCENT.GetB()), 1.0f);
        g.DrawPath(&pen, &p);
    } else {
        SolidBrush b(Color(26, 255, 255, 255));
        g.FillPath(&b, &p);
    }
    drawText(g, label, smallFont(), r,
             on ? Color(255, 236, 246, 255) : C_TEXT_DIM, true, 1);
}

// 选择来源的两行菜单，浮在“关联材料”那一行上方
static void paintPickMenu(Graphics& g) {
    RectF box = pickMenuRect();
    box.X += SHADOW;
    box.Y += SHADOW;

    GraphicsPath p;
    roundRect(p, box, 10.0f);
    SolidBrush bg(Color(248, 26, 30, 38));
    g.FillPath(&bg, &p);
    Pen pen(Color(38, 255, 255, 255), 1.0f);
    g.DrawPath(&pen, &p);

    RectF f = pickMenuFile(), d = pickMenuFolder();
    f.X += SHADOW; f.Y += SHADOW;
    d.X += SHADOW; d.Y += SHADOW;
    drawText(g, L"选择文件", smallFont(), f, C_TEXT_DIM, true, 0);
    drawText(g, L"选择文件夹", smallFont(), d, C_TEXT_DIM, true, 0);
}

// 编辑面板：底栏整块让出来，上面加一排截止、一排预估
static void paintEditPanel(Graphics& g) {
    const REAL top = (REAL)SHADOW + panelTop();

    // 压一层暗底，和上面的任务列表分开
    SolidBrush pb(Color(30, 0, 0, 0));
    g.FillRectangle(&pb, (REAL)SHADOW, top, (REAL)CARD_W, (REAL)PANEL_H);
        Pen sep(Color(18, 255, 255, 255), 1.0f);
    g.DrawLine(&sep, SHADOW + 14.0f, top + 0.5f, SHADOW + CARD_W - 14.0f, top + 0.5f);

    // 输入框
    RectF inR((REAL)SHADOW + 10, (REAL)SHADOW + panelInputTop(),
              (REAL)CARD_W - 20, 30);
    GraphicsPath ip;
    roundRect(ip, inR, 8.0f);
    SolidBrush ib(Color(34, 255, 255, 255));
    g.FillPath(&ib, &ip);
    Pen ibr(Color(96, C_ACCENT.GetR(), C_ACCENT.GetG(), C_ACCENT.GetB()), 1.0f);
    g.DrawPath(&ibr, &ip);

    const REAL tx = inR.X + 9;
    const REAL tw = inR.Width - 18 - 26;        // 右边留出 × 的位置

    // 光标位置：量出光标前那段的宽度
    if (g_caret < 0) g_caret = 0;
    if (g_caret > (int)g_editText.size()) g_caret = (int)g_editText.size();
    RectF prefixBox;
    if (g_caret > 0) {
        wstring pre = g_editText.substr(0, (size_t)g_caret);
        g.MeasureString(pre.c_str(), -1, bodyFont(), PointF(0, 0), &prefixBox);
    }
    REAL caretOff = prefixBox.Width;

    if (g_editText.empty()) {
        drawText(g, L"任务标题，Enter 添加", bodyFont(),
                 RectF(tx, inR.Y, tw, inR.Height), C_TEXT_FAINT, true, 0);
        if (g_caretOn) {
            SolidBrush cb(C_ACCENT);
            g.FillRectangle(&cb, tx, inR.Y + 7, 1.6f, inR.Height - 14);
        }
    } else {
        RectF meas;
        g.MeasureString(g_editText.c_str(), -1, bodyFont(), PointF(0, 0), &meas);

        // 超长时跟着光标滚：光标跑到框外就把它拉回可见范围
        REAL scroll = 0;
        if (meas.Width > tw - 2 && caretOff > tw - 4) scroll = caretOff - (tw - 4);

        GraphicsState st = g.Save();
        g.SetClip(inR, CombineModeIntersect);
        drawText(g, g_editText, bodyFont(),
                 RectF(tx - scroll, inR.Y, meas.Width + 4, inR.Height), C_TEXT, true, 0);
        g.Restore(st);

        if (g_caretOn) {
            REAL caretX = tx + caretOff - scroll;
            if (caretX < tx) caretX = tx;
            if (caretX > tx + tw) caretX = tx + tw;
            SolidBrush cb(C_ACCENT);
            g.FillRectangle(&cb, caretX, inR.Y + 7, 1.6f, inR.Height - 14);
        }
    }

    // 输入框右侧的 ×：单击放弃本次添加
    {
        RectF cr = cancelRect();
        cr.X += SHADOW;
        cr.Y += SHADOW;
        if (g_hoverCancel) {
            GraphicsPath hp;
            roundRect(hp, cr, 6.0f);
            SolidBrush hb(C_HOVER);
            g.FillPath(&hb, &hp);
        }
        Pen xp(g_hoverCancel ? C_TEXT : C_TEXT_FAINT, 1.5f);
        xp.SetStartCap(LineCapRound);
        xp.SetEndCap(LineCapRound);
        REAL cxc = cr.X + cr.Width / 2, cyc = cr.Y + cr.Height / 2;
        g.DrawLine(&xp, cxc - 4.5f, cyc - 4.5f, cxc + 4.5f, cyc + 4.5f);
        g.DrawLine(&xp, cxc + 4.5f, cyc - 4.5f, cxc - 4.5f, cyc + 4.5f);
    }

    // 截止
    drawText(g, L"截止", smallFont(),
             RectF((REAL)SHADOW + 10, (REAL)SHADOW + panelDueTop(), 30, (REAL)CHIP_H),
             C_TEXT_FAINT, true, 0);
    for (int i = 0; i < 5; ++i) {
        RectF r = chipRect(0, i, 5);
        r.X += SHADOW; r.Y += SHADOW;
        const wchar_t* label = DUE_LABEL[i];
        wchar_t picked[16];
        if (i == 3 && !g_customDue.empty()) {
            int y = 0, mo = 0, d = 0;
            if (swscanf(g_customDue.c_str(), L"%d-%d-%d", &y, &mo, &d) == 3) {
                swprintf(picked, 16, L"%d-%d", mo, d);
                label = picked;
            }
        }
        paintChip(g, r, label, g_editDue == i);
    }

    // 关联材料：选文件 + 网址输入框（只收一条）
    drawText(g, L"从这里开始", smallFont(),
             RectF((REAL)SHADOW + 10, (REAL)SHADOW + panelLinkTop(), 62, (REAL)CHIP_H),
             C_TEXT_FAINT, true, 0);
    {
        RectF pick = linkPickRect();
        pick.X += SHADOW; pick.Y += SHADOW;
        paintChip(g, pick, L"选择", g_pickMenu);

        RectF ir = linkInputRect();
        ir.X += SHADOW; ir.Y += SHADOW;
        GraphicsPath ip;
        roundRect(ip, ir, 8.0f);
        SolidBrush ib(Color(26, 255, 255, 255));
        g.FillPath(&ib, &ip);
        Pen ibr(g_linkFocus
                    ? Color(150, C_ACCENT.GetR(), C_ACCENT.GetG(), C_ACCENT.GetB())
                    : Color(36, 255, 255, 255), 1.0f);
        g.DrawPath(&ibr, &ip);

        const REAL tx = ir.X + 8;
        const REAL tw = ir.Width - 16;
        if (g_editUrl.empty()) {
            if (!g_linkFocus) {
                // 未填网址时，占位文字显示已选数量
                wstring hint = g_editLinks.empty()
                    ? wstring(L"网址")
                    : (L"已添加 " + std::to_wstring(g_editLinks.size()) + L" 项");
                drawText(g, hint, smallFont(),
                         RectF(tx, ir.Y, tw, ir.Height), C_TEXT_FAINT, true, 0);
            } else if (g_caretOn) {
                SolidBrush cb(C_ACCENT);
                g.FillRectangle(&cb, tx, ir.Y + 5, 1.4f, ir.Height - 10);
            }
        } else {
            RectF meas;
            g.MeasureString(g_editUrl.c_str(), -1, smallFont(), PointF(0, 0), &meas);
            // 超出宽度时跟随光标滚动，保证输入内容可见
            REAL scroll = (meas.Width > tw - 4) ? (meas.Width - tw + 4) : 0;
            GraphicsState st = g.Save();
            g.SetClip(ir, CombineModeIntersect);
            drawText(g, g_editUrl, smallFont(),
                     RectF(tx - scroll, ir.Y, meas.Width + 4, ir.Height), C_TEXT, true, 0);
            if (g_linkFocus && g_caretOn) {
                SolidBrush cb(C_ACCENT);
                g.FillRectangle(&cb, tx - scroll + meas.Width + 1, ir.Y + 5, 1.4f, ir.Height - 10);
            }
            g.Restore(st);
        }

        if (!g_editLinks.empty()) {
            Pen xp(C_TEXT_FAINT, 1.4f);
            xp.SetStartCap(LineCapRound);
            xp.SetEndCap(LineCapRound);
            REAL xx = SHADOW + 297, xy = SHADOW + panelLinkTop() + CHIP_H / 2.0f;
            g.DrawLine(&xp, xx - 4, xy - 4, xx + 4, xy + 4);
            g.DrawLine(&xp, xx + 4, xy - 4, xx - 4, xy + 4);
        }
    }

    // 预估。-1（没选）落在“不设”那一格，自定义落在“自定义”那一格
    drawText(g, L"预估", smallFont(),
             RectF((REAL)SHADOW + 10, (REAL)SHADOW + panelEstTop(), 30, (REAL)CHIP_H),
             C_TEXT_FAINT, true, 0);
    int onEst = (g_editEst < 0) ? (EST_CHIPS - 1)
              : (g_editEst == EST_CUSTOM ? EST_CUSTOM : g_editEst);
    for (int i = 0; i < EST_CHIPS; ++i) {
        RectF r = chipRect(1, i, EST_CHIPS);
        r.X += SHADOW; r.Y += SHADOW;
        paintChip(g, r, EST_LABEL[i], i == onEst);
    }

    if (g_pickMenu) paintPickMenu(g);
}

// ---------------------------------------------------------------- 自选日期

// 这个月 1 号是周几，0 = 周一
static int firstWeekday(int y, int m) {
    SYSTEMTIME st{};
    st.wYear = (WORD)y;
    st.wMonth = (WORD)m;
    st.wDay = 1;
    FILETIME ft{};
    SystemTimeToFileTime(&st, &ft);
    FileTimeToSystemTime(&ft, &st);
    return (st.wDayOfWeek + 6) % 7;
}

static RectF calPrev()    { return RectF(14, 52, 26, 26); }
static RectF calNext()    { return RectF((REAL)CARD_W - 40, 52, 26, 26); }
static RectF calCell(int i) {
    return RectF(20 + (i % 7) * 40, 108 + (i / 7) * 30, 40, 30);
}
static RectF calHourDec() { return RectF(56, 296, 22, 26); }
static RectF calHourInc() { return RectF(116, 296, 22, 26); }
static RectF calMinDec()  { return RectF(150, 296, 22, 26); }
static RectF calMinInc()  { return RectF(210, 296, 22, 26); }
static RectF calCancel()  { return RectF(56, 352, 92, 32); }
static RectF calOk()      { return RectF(172, 352, 92, 32); }

static void paintMini(Graphics& g, const RectF& r, const wchar_t* text, bool primary) {
    GraphicsPath p;
    roundRect(p, r, 8.0f);
    if (primary) {
        SolidBrush b(Color(238, C_ACCENT.GetR(), C_ACCENT.GetG(), C_ACCENT.GetB()));
        g.FillPath(&b, &p);
    } else {
        SolidBrush b(Color(26, 255, 255, 255));
        g.FillPath(&b, &p);
    }
    drawText(g, text, smallFont(), r, primary ? Color(255, 255, 255, 255) : C_TEXT_DIM, true, 1);
}

// 日历面板覆盖标题栏以下区域，用于选择日期和时间
static void paintCalendar(Graphics& g) {
    SolidBrush bg(Color(244, 16, 19, 25));
    g.FillRectangle(&bg, (REAL)SHADOW, (REAL)(SHADOW + TITLE_H),
                    (REAL)CARD_W, (REAL)(CARD_H - TITLE_H));

    wchar_t buf[48];
    swprintf(buf, 48, L"%d 年 %d 月", g_pickYear, g_pickMonth);
    drawText(g, buf, bodyFont(),
             RectF((REAL)SHADOW + 40, (REAL)SHADOW + 52, (REAL)CARD_W - 80, 26),
             C_TEXT, true, 1);

    {
        Pen pen(C_TEXT_DIM, 1.6f);
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        RectF l = calPrev(), r = calNext();
        REAL lx = SHADOW + l.X + l.Width / 2, ly = SHADOW + l.Y + l.Height / 2;
        g.DrawLine(&pen, lx + 3, ly - 5, lx - 3, ly);
        g.DrawLine(&pen, lx - 3, ly, lx + 3, ly + 5);
        REAL rx = SHADOW + r.X + r.Width / 2, ry = SHADOW + r.Y + r.Height / 2;
        g.DrawLine(&pen, rx - 3, ry - 5, rx + 3, ry);
        g.DrawLine(&pen, rx + 3, ry, rx - 3, ry + 5);
    }

    {
        const wchar_t* wd[7] = { L"一", L"二", L"三", L"四", L"五", L"六", L"日" };
        for (int c = 0; c < 7; ++c) {
            drawText(g, wd[c], smallFont(),
                     RectF((REAL)SHADOW + 20 + c * 40, (REAL)SHADOW + 84, 40, 18),
                     C_TEXT_FAINT, true, 1);
        }
    }

    {
        int first = firstWeekday(g_pickYear, g_pickMonth);
        int dim = daysOfMonth(g_pickYear, g_pickMonth);
        for (int i = 0; i < 42; ++i) {
            int d = i - first + 1;
            if (d < 1 || d > dim) continue;
            RectF cell = calCell(i);
            cell.X += SHADOW;
            cell.Y += SHADOW;
            if (d == g_pickDay) {
                GraphicsPath p;
                roundRect(p, RectF(cell.X + 5, cell.Y + 2, cell.Width - 10, cell.Height - 4), 7.0f);
                SolidBrush b(Color(215, C_ACCENT.GetR(), C_ACCENT.GetG(), C_ACCENT.GetB()));
                g.FillPath(&b, &p);
            }
            wchar_t db[8];
            swprintf(db, 8, L"%d", d);
            drawText(g, db, smallFont(), cell,
                     d == g_pickDay ? Color(255, 255, 255, 255) : C_TEXT_DIM, true, 1);
        }
    }

    {
        drawText(g, L"时间", smallFont(),
                 RectF((REAL)SHADOW + 20, (REAL)SHADOW + 296, 34, 26), C_TEXT_FAINT, true, 0);

        RectF hd = calHourDec(), hi = calHourInc(), md = calMinDec(), mi = calMinInc();
        paintMini(g, RectF(SHADOW + hd.X, SHADOW + hd.Y, hd.Width, hd.Height), L"−", false);
        paintMini(g, RectF(SHADOW + hi.X, SHADOW + hi.Y, hi.Width, hi.Height), L"+", false);
        paintMini(g, RectF(SHADOW + md.X, SHADOW + md.Y, md.Width, md.Height), L"−", false);
        paintMini(g, RectF(SHADOW + mi.X, SHADOW + mi.Y, mi.Width, mi.Height), L"+", false);

        wchar_t tb[8];
        swprintf(tb, 8, L"%02d", g_pickHour);
        drawText(g, tb, bodyFont(),
                 RectF((REAL)SHADOW + 80, (REAL)SHADOW + 296, 34, 26), C_TEXT, true, 1);
        drawText(g, L":", bodyFont(),
                 RectF((REAL)SHADOW + 140, (REAL)SHADOW + 296, 14, 26), C_TEXT_DIM, true, 1);
        swprintf(tb, 8, L"%02d", g_pickMin);
        drawText(g, tb, bodyFont(),
                 RectF((REAL)SHADOW + 174, (REAL)SHADOW + 296, 34, 26), C_TEXT, true, 1);
    }

    {
        RectF c = calCancel(), o = calOk();
        paintMini(g, RectF(SHADOW + c.X, SHADOW + c.Y, c.Width, c.Height), L"取消", false);
        paintMini(g, RectF(SHADOW + o.X, SHADOW + o.Y, o.Width, o.Height), L"确定", true);
    }
}

// ---------------------------------------------------------------- 自定义预估

// 三列：日 / 时 / 分。每列上减下加，中间是数字。
// 数字区与上下按钮间保留 8px 间距，避免编辑边框与按钮相接
static REAL estColX(int i)   { return 38.0f + i * 84.0f; }
static RectF estMinus(int i) { return RectF(estColX(i) + 23, 120, 30, 26); }
static RectF estValue(int i) { return RectF(estColX(i), 154, 76, 32); }
static RectF estPlus(int i)  { return RectF(estColX(i) + 23, 194, 30, 26); }
static RectF estCancel()     { return RectF(56, 352, 92, 32); }
static RectF estOk()         { return RectF(172, 352, 92, 32); }

static int estColMax(int col) { return col == 0 ? 69 : (col == 1 ? 23 : 59); }

// 提交正在输入的那一格：空着就当没改，超范围夹回去
static void commitEstEdit() {
    if (g_estEditCol < 0) return;
    if (!g_estEditText.empty()) {
        int v = _wtoi(g_estEditText.c_str());
        int hi = estColMax(g_estEditCol);
        if (v < 0) v = 0;
        if (v > hi) v = hi;
        if (g_estEditCol == 0)      g_estDay = v;
        else if (g_estEditCol == 1) g_estHour = v;
        else                        g_estMinute = v;
    }
    g_estEditCol = -1;
    g_estEditText.clear();
}

// 单击数字格时清空输入缓冲，新输入替换当前值
static void beginEstEdit(int col) {
    g_estEditCol = col;
    g_estEditText.clear();
    g_caretOn = true;
}

// 日历面板覆盖标题栏以下区域，布局空间大于控制台中的单行日期选择器
static void paintEstPicker(Graphics& g) {
    SolidBrush bg(Color(244, 16, 19, 25));
    g.FillRectangle(&bg, (REAL)SHADOW, (REAL)(SHADOW + TITLE_H),
                    (REAL)CARD_W, (REAL)(CARD_H - TITLE_H));

    drawText(g, L"自定义预估", bodyFont(),
             RectF((REAL)SHADOW + 20, (REAL)SHADOW + 52, (REAL)CARD_W - 40, 26),
             C_TEXT, true, 1);

    const wchar_t* caps[3] = { L"日", L"时", L"分" };
    int vals[3] = { g_estDay, g_estHour, g_estMinute };
    for (int i = 0; i < 3; ++i) {
        drawText(g, caps[i], smallFont(),
                 RectF((REAL)SHADOW + estColX(i), (REAL)SHADOW + 94, 76, 20),
                 C_TEXT_FAINT, true, 1);

        RectF m = estMinus(i), p = estPlus(i);
        paintMini(g, RectF(SHADOW + m.X, SHADOW + m.Y, m.Width, m.Height), L"−", false);
        paintMini(g, RectF(SHADOW + p.X, SHADOW + p.Y, p.Width, p.Height), L"+", false);

        RectF vb = estValue(i);
        vb.X += SHADOW;
        vb.Y += SHADOW;
        if (g_estEditCol == i) {
            // 正在输入：加个框，光标跟在数字后面
            GraphicsPath p;
            roundRect(p, vb, 8.0f);
            SolidBrush fill(Color(30, 255, 255, 255));
            g.FillPath(&fill, &p);
            Pen pen(Color(96, C_ACCENT.GetR(), C_ACCENT.GetG(), C_ACCENT.GetB()), 1.0f);
            g.DrawPath(&pen, &p);

            drawText(g, g_estEditText, bodyFont(), vb, C_TEXT, true, 1);
            if (g_caretOn) {
                RectF meas;
                if (!g_estEditText.empty())
                    g.MeasureString(g_estEditText.c_str(), -1, bodyFont(), PointF(0, 0), &meas);
                REAL cx = vb.X + vb.Width / 2 + meas.Width / 2 + 1;
                SolidBrush cb(C_ACCENT);
                g.FillRectangle(&cb, cx, vb.Y + 9, 1.6f, vb.Height - 18);
            }
        } else {
            wchar_t nb[8];
            swprintf(nb, 8, L"%02d", vals[i]);
            drawText(g, nb, bodyFont(), vb, C_TEXT, true, 1);
        }
    }

    int total = g_estDay * 1440 + g_estHour * 60 + g_estMinute;
    wstring tip = total > 0 ? (L"合计 " + shortDuration(total)) : wstring(L"未设置时长");
    drawText(g, tip, smallFont(),
             RectF((REAL)SHADOW + 20, (REAL)SHADOW + 248, (REAL)CARD_W - 40, 24),
             C_TEXT_DIM, true, 1);

    RectF c = estCancel(), o = estOk();
    paintMini(g, RectF(SHADOW + c.X, SHADOW + c.Y, c.Width, c.Height), L"取消", false);
    paintMini(g, RectF(SHADOW + o.X, SHADOW + o.Y, o.Width, o.Height), L"确定", true);
}

// 正在进行：起始时间和用时占比
//   预计最晚开始 = 截止 - 预计用时
//   起始 = min(计时开始时刻, 预计最晚开始)
//   占比 = (现在 - 起始) / 预计用时
// 没填预估的算不出占比，这里直接给 0，界面改用呼吸 / 固定填充表示。
static void progressOf(const Task& t, long long nowMs, long long& startMs, REAL& ratio) {
    if (!hasEstimate(t)) {
        startMs = t.startedAt;
        ratio = 0;
        return;
    }

    long long estMs = (long long)t.estimate * 60000LL;
    long long latest = (t.dueMs > 0) ? (t.dueMs - estMs) : t.startedAt;
    startMs = (t.startedAt < latest) ? t.startedAt : latest;

    long long elapsed = nowMs - startMs;
    if (elapsed < 0) elapsed = 0;
    ratio = (REAL)elapsed / (REAL)estMs;
    if (ratio > 1.0f) ratio = 1.0f;
}

// 胶囊形的“跑圈”：沿着轮廓顺时针走 t 圈，走多少画多少
static void capsuleProgress(Graphics& g, const RectF& r, REAL t, Pen& pen) {
    const REAL rad = r.Height / 2.0f;
    REAL straight = r.Width - r.Height;
    if (straight < 0) straight = 0;

    const REAL arc = 3.14159265f * rad;
    REAL left = t * (2.0f * straight + 2.0f * arc);
    if (left <= 0) return;

    // 上边：左 → 右
    if (left > 0 && straight > 0) {
        REAL use = (left < straight) ? left : straight;
        g.DrawLine(&pen, r.X + rad, r.Y, r.X + rad + use, r.Y);
        left -= use;
    }
    // 右半圆：-90° 往 +180° 走
    if (left > 0) {
        REAL use = (left < arc) ? left : arc;
        g.DrawArc(&pen, r.X + r.Width - 2 * rad, r.Y, 2 * rad, 2 * rad,
                  -90.0f, use / arc * 180.0f);
        left -= use;
    }
    // 下边：右 → 左
    if (left > 0 && straight > 0) {
        REAL use = (left < straight) ? left : straight;
        REAL x1 = r.X + r.Width - rad;
        g.DrawLine(&pen, x1 - use, r.Y + r.Height, x1, r.Y + r.Height);
        left -= use;
    }
    // 左半圆：90° 往 +180° 走
    if (left > 0) {
        REAL use = (left < arc) ? left : arc;
        g.DrawArc(&pen, r.X, r.Y, 2 * rad, 2 * rad, 90.0f, use / arc * 180.0f);
    }
}

// 进度条的颜色：<=60% 绿，<=80% 黄，再往上红。
// 平时就是这三个纯色，只在跨过 60% / 80% 那两个点时放一段过渡动画，
// 不是跟着进度一直渐变。
static Color tierColor(int tier) {
    return (tier == 0) ? C_GREEN : (tier == 1 ? C_AMBER : C_RED);
}

static Color lerpColor(Color a, Color b, REAL k) {
    if (k < 0) k = 0;
    if (k > 1) k = 1;
    auto mix = [&](BYTE x, BYTE y) { return (BYTE)(x + (y - x) * k); };
    return Color(255, mix(a.GetR(), b.GetR()), mix(a.GetG(), b.GetG()), mix(a.GetB(), b.GetB()));
}

// 记录每条任务上次的档位与切换时刻，切换时据此确定过渡起点
struct TierState { wchar_t id[40] = {0}; int tier = 0; int from = 0; DWORD at = 0; };
static TierState g_tiers[12];

static Color progressColorOf(const Task& t, REAL ratio) {
    int tier = (ratio <= 0.6f) ? 0 : (ratio <= 0.8f ? 1 : 2);

    TierState* slot = nullptr;
    for (auto& st : g_tiers) {
        if (st.id[0] && t.id == st.id) { slot = &st; break; }
        if (!st.id[0] && !slot) slot = &st;
    }
    if (!slot) return tierColor(tier);

    if (!slot->id[0]) {                       // 第一次见到这条
        wcsncpy(slot->id, t.id.c_str(), 39);
        slot->id[39] = 0;
        slot->tier = tier;
        slot->from = tier;
        slot->at = 0;
        return tierColor(tier);
    }

    if (slot->tier != tier) {                 // 刚跨过阈值
        slot->from = slot->tier;
        slot->tier = tier;
        slot->at = GetTickCount();
        kickProg();
    }

    if (slot->at) {
        REAL k = (REAL)(GetTickCount() - slot->at) / 360.0f;
        if (k < 1.0f) return lerpColor(tierColor(slot->from), tierColor(slot->tier), k);
    }
    return tierColor(tier);
}

// 整行染色：底色随进度从左往右铺开，进行中时紧急底色让位。
// 整行背景按任务进度填充；进行中状态使用进度色。
static void paintRowBar(Graphics& g, const RectF& rowR, REAL ratio, Color pc, REAL alpha) {
    if (ratio <= 0.002f) ratio = 0.002f;

    GraphicsPath hp;
    roundRect(hp, rowR, 10.0f);
    SolidBrush base(fadeCol(Color(10, pc.GetR(), pc.GetG(), pc.GetB()), alpha));
    g.FillPath(&base, &hp);

    REAL w = rowR.Width * ratio;
    if (w < 6) w = 6;

    // 填充块不重复计算圆角：宽度小于圆角直径时四段圆弧会相互穿透，
    // 刚启动计时任务时会短暂出现放大的胶囊轮廓。
    // 改成按行本身的圆角裁一刀——右端是齐口，左端跟着行的圆角走，适用于任意填充宽度。
    GraphicsState st = g.Save();
    g.SetClip(&hp);
    LinearGradientBrush lg(PointF(rowR.X, rowR.Y), PointF(rowR.X + w, rowR.Y),
                           fadeCol(Color(132, pc.GetR(), pc.GetG(), pc.GetB()), alpha),
                           fadeCol(Color(20, pc.GetR(), pc.GetG(), pc.GetB()), alpha));
    g.FillRectangle(&lg, RectF(rowR.X, rowR.Y, w, rowR.Height));
    g.Restore(st);
}

// 图钉：标题栏那个“置顶悬浮清单”按钮用。上窄下宽的钉头加一根针。
// 11px 尺寸无法保留原图的圆角细节，缩小后保留整体轮廓。
static void paintPin(Graphics& g, REAL cx, REAL cy, REAL k, Color col) {
    PointF head[4] = {
        { cx - 1.9f * k, cy - 4.8f * k },
        { cx + 1.9f * k, cy - 4.8f * k },
        { cx + 3.5f * k, cy - 1.0f * k },
        { cx - 3.5f * k, cy - 1.0f * k },
    };
    GraphicsPath p;
    p.AddPolygon(head, 4);
    roundRect(p, RectF(cx - 2.5f * k, cy - 6.0f * k, 5.0f * k, 1.5f * k), 0.6f * k);
    SolidBrush b(col);
    g.FillPath(&b, &p);

    Pen needle(col, 1.3f * k);
    needle.SetStartCap(LineCapRound);
    needle.SetEndCap(LineCapRound);
    g.DrawLine(&needle, cx, cy - 1.0f * k, cx, cy + 3.6f * k);
}

// 上载标记：一道顶边加一支向上的箭头。任务“浮到最前面”用它，
// 和控制台 .task-pin 里那个 SVG 是同一个形状。
static void paintRise(Graphics& g, REAL cx, REAL cy, REAL k, Color col) {
    Pen pen(col, 1.4f * k);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawLine(&pen, cx - 4.0f * k, cy - 4.6f * k, cx + 4.0f * k, cy - 4.6f * k);
    g.DrawLine(&pen, cx, cy + 4.4f * k, cx, cy - 1.6f * k);
    g.DrawLine(&pen, cx - 2.6f * k, cy + 1.0f * k, cx, cy - 1.6f * k);
    g.DrawLine(&pen, cx + 2.6f * k, cy + 1.0f * k, cx, cy - 1.6f * k);
}

// 画一行任务。dx 是横向偏移（勾掉后往右滑用），alpha 是整体透明度。
static Color fadeCol(const Color& c, REAL a) {
    REAL v = (REAL)c.GetA() * a;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return Color((BYTE)v, c.GetR(), c.GetG(), c.GetB());
}

static void paintRow(Graphics& g, const Task& t, int rowIdx, REAL rowY, REAL dx,
                     REAL alpha, bool checked) {
    RectF rowR((REAL)SHADOW + 6 + dx, rowY, (REAL)CARD_W - 12, (REAL)ROW_H);

    Urgency u = checked ? URG_NONE : urgencyOf(t);
    Color uc = (u == URG_RED) ? C_RED : C_AMBER;

    // 进行中：进度色铺在最底层，文字、勾选框、暖色竖条、按钮都压在它上面

    if (t.startedAt > 0) {
        long long startMs = 0;
        REAL ratio = 0;
        progressOf(t, nowMs(), startMs, ratio);

        REAL anim = (g_progAll || (!g_progId.empty() && t.id == g_progId)) ? g_progAnim : 1.0f;
        if (!hasEstimate(t)) {
            // 没填预估：固定八成填充，颜色跟紧急程度走，不跟着时间变
            paintRowBar(g, rowR, 0.8f * anim, urgencyColor(u), alpha);
        } else {
            paintRowBar(g, rowR, ratio * anim, progressColorOf(t, ratio), alpha);
        }
    }


    // 临近截止：整行暖色 + 左侧竖条，红黄同形状不同色。
    // 进行中任务使用进度色作为行底色，避免底色叠加造成对比度下降
    if (u != URG_NONE) {
        if (t.startedAt <= 0) {
            GraphicsPath hp;
            roundRect(hp, rowR, 10.0f);
            LinearGradientBrush lg(PointF(rowR.X, rowR.Y), PointF(rowR.X + rowR.Width, rowR.Y),
                                   fadeCol(Color(u == URG_RED ? 46 : 38, uc.GetR(), uc.GetG(), uc.GetB()), alpha),
                                   fadeCol(Color(8, uc.GetR(), uc.GetG(), uc.GetB()), alpha));
            g.FillPath(&lg, &hp);
        }
        SolidBrush bar(fadeCol(Color(235, uc.GetR(), uc.GetG(), uc.GetB()), alpha));
        g.FillRectangle(&bar, rowR.X + 3, rowY + 13, 2.5f, (REAL)ROW_H - 26);
    } else if (rowIdx >= 0 && g_hoverRow == rowIdx && !g_dragging) {
        GraphicsPath hp;
        roundRect(hp, rowR, 10.0f);
        SolidBrush hb(fadeCol(C_HOVER, alpha));
        g.FillPath(&hb, &hp);
    }

    RectF cb(rowR.X + 10, rowY + (ROW_H - 18) / 2.0f, 18, 18);
    if (checked) {
        GraphicsPath p;
        roundRect(p, cb, 5.5f);
        SolidBrush b(fadeCol(Color(255, 108, 140, 255), alpha));
        g.FillPath(&b, &p);
        Pen check(fadeCol(Color(255, 255, 255, 255), alpha), 1.9f);
        check.SetStartCap(LineCapRound);
        check.SetEndCap(LineCapRound);
        g.DrawLine(&check, cb.X + 4.5f, cb.Y + 9.2f, cb.X + 7.6f, cb.Y + 12.4f);
        g.DrawLine(&check, cb.X + 7.6f, cb.Y + 12.4f, cb.X + 13.2f, cb.Y + 5.8f);
    } else {
        GraphicsPath p;
        roundRect(p, cb, 5.5f);
        Pen border(u != URG_NONE ? fadeCol(Color(150, uc.GetR(), uc.GetG(), uc.GetB()), alpha)
                                 : fadeCol(Color(70, 255, 255, 255), alpha), 1.5f);
        g.DrawPath(&border, &p);
    }

    // 标题（一行）+ 时间/时长（第二行）
    REAL titleX = rowR.X + 38;
    REAL titleW = rowR.Width - 38 - 10 - 30 - 26;   // 右边留给“置顶”“正在进行”

    // 包含关联材料的任务以强调色显示标题，标识可打开关联内容
    Color titleCol = checked ? C_TEXT_FAINT : (t.links.empty() ? C_TEXT : C_ACCENT);
    drawText(g, t.title, bodyFont(), RectF(titleX, rowY + 7, titleW, 21),
             fadeCol(titleCol, alpha), true, 0);

    if (checked) {
        RectF meas;
        g.MeasureString(t.title.c_str(), -1, bodyFont(), PointF(0, 0), &meas);
        REAL lineW = (meas.Width < titleW - 4) ? meas.Width : titleW - 8;
        Pen strike(fadeCol(C_TEXT_FAINT, alpha), 1.0f);
        g.DrawLine(&strike, titleX, rowY + 17.5f, titleX + lineW, rowY + 17.5f);
    }

        // 右侧按钮：未运行时仅在悬停时显示 ▶；运行时始终显示 ■（停止计时）
    {
        REAL pcx = rowR.X + rowR.Width - 19;
        REAL pcy = rowY + ROW_H / 2.0f;
        const REAL pr = 9.0f;
        bool running = (t.startedAt > 0);

        // 左侧按钮为置顶开关；置顶任务始终显示该按钮，其余任务仅在悬停时显示
        REAL qcx = rowR.X + rowR.Width - 45;
        if (t.pinned || (rowIdx >= 0 && g_hoverRow == rowIdx && !g_dragging)) {
            Color bc = t.pinned ? C_ACCENT : C_TEXT_FAINT;
            if (t.pinned) {
                SolidBrush bg(fadeCol(Color(56, bc.GetR(), bc.GetG(), bc.GetB()), alpha));
                g.FillEllipse(&bg, qcx - pr, pcy - pr, pr * 2, pr * 2);
            }
            Pen ring(fadeCol(Color(t.pinned ? 190 : 110, bc.GetR(), bc.GetG(), bc.GetB()), alpha), 1.0f);
            g.DrawEllipse(&ring, qcx - pr, pcy - pr, pr * 2, pr * 2);
            paintRise(g, qcx, pcy, 0.85f, fadeCol(bc, alpha));
        }

        if (running || (rowIdx >= 0 && g_hoverRow == rowIdx && !g_dragging)) {
            Color bc = urgencyColor(u);
            SolidBrush bgc(fadeCol(Color(running ? 72 : 56, bc.GetR(), bc.GetG(), bc.GetB()), alpha));
            g.FillEllipse(&bgc, pcx - pr, pcy - pr, pr * 2, pr * 2);
            Pen ring(fadeCol(Color(running ? 190 : 150, bc.GetR(), bc.GetG(), bc.GetB()), alpha), 1.0f);
            g.DrawEllipse(&ring, pcx - pr, pcy - pr, pr * 2, pr * 2);

            if (running) {
                SolidBrush sb(fadeCol(bc, alpha));
                g.FillRectangle(&sb, pcx - 3.2f, pcy - 3.2f, 6.4f, 6.4f);
            } else {
                PointF tri[3] = { { pcx - 2.0f, pcy - 4.2f }, { pcx + 4.2f, pcy }, { pcx - 2.0f, pcy + 4.2f } };
                SolidBrush tb(fadeCol(bc, alpha));
                g.FillPolygon(&tb, tri, 3);
            }
        }
    }

    // 第二行：有截止就写截止，有预估就写预估，两样都没有就整行不画。
    // 原来两段都用 -- 占位，想让中间那个 · 对齐，可整行是一串文本左对齐拼出来的，
    // 占位并没对齐任何东西，只剩一排“-- · --”占着地方。
    wstring meta = dueText(t);
    wstring dur = shortDuration(t.estimate);
    if (!meta.empty() && !dur.empty()) meta += L"  ·  " + dur;
    else if (meta.empty()) meta = dur;

    // 进行中再补一段：有预估就数“已进行”多久，没预估只说“正在进行”
    if (t.startedAt > 0) {
        long long st = 0;
        REAL r2 = 0;
        progressOf(t, nowMs(), st, r2);
        if (!meta.empty()) meta += L"  ·  ";
        meta += hasEstimate(t) ? (L"已进行 " + clockText(nowMs() - st)) : wstring(L"正在进行");
    }
    if (!meta.empty()) {
        Color mc = (u == URG_NONE) ? C_TEXT_FAINT
                 : (u == URG_RED ? Color(255, 255, 150, 170) : Color(255, 250, 205, 146));
        // 进行中任务整行使用进度色填充；为辅助文字添加白色描边以提升可读性
        Color edge(t.startedAt > 0 ? (BYTE)(80 * alpha) : (BYTE)0, 255, 255, 255);
        drawText(g, meta, smallFont(), RectF(titleX, rowY + 29, titleW, 16),
                 fadeCol(mc, alpha), true, 0, edge);
    }
}

static void paint(HDC dc) {
    Graphics g(dc);
    g.ScaleTransform(g_scale, g_scale);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.Clear(Color(0, 0, 0, 0));

    if (!g_expanded) {
        paintDot(g);
        return;
    }

    RectF card((REAL)SHADOW, (REAL)SHADOW, (REAL)CARD_W, (REAL)CARD_H);
    RectF fillR(card.X - 0.5f, card.Y - 0.5f, card.Width + 1.0f, card.Height + 1.0f);

    // 投影：由外向内叠几层低透明度圆角矩形，成本低但足够柔和
    for (int i = 7; i >= 1; --i) {
        REAL grow = (REAL)i * 1.7f;
        int alpha = 4 + (7 - i) * 6;
        RectF r(card.X - grow, card.Y - grow + 2.0f,
                card.Width + grow * 2, card.Height + grow * 2);
        GraphicsPath p;
        roundRect(p, r, RADIUS + grow);
        SolidBrush b(Color((BYTE)alpha, 0, 0, 0));
        g.FillPath(&b, &p);
    }

    // 展开态
    {
        GraphicsPath p;
        roundRect(p, fillR, RADIUS);

        // 毛玻璃：抓屏模糊图 + 半透明深色罩，只靠半透明出不来磨砂感。
        // 位图使用设备像素；绘制坐标已按 g_scale 缩放，尺寸需除以 g_scale 以匹配 1:1 像素比例
        if (g_bgReady && g_bgBits) {
            Bitmap glass(g_surfW, g_surfH, g_surfW * 4, PixelFormat32bppARGB, g_bgBits);
            g.SetClip(&p);
            g.DrawImage(&glass, (REAL)0, (REAL)0,
                        (REAL)g_surfW / g_scale, (REAL)g_surfH / g_scale);
            g.ResetClip();
            SolidBrush tint(C_GLASS);
            g.FillPath(&tint, &p);
        } else {
            SolidBrush bg(C_CARD);
            g.FillPath(&bg, &p);
        }

        // 描边内缩半像素，否则一半笔画落在面板外。
        GraphicsPath bp;
        roundRect(bp, RectF(card.X + 0.5f, card.Y + 0.5f,
                            card.Width - 1.0f, card.Height - 1.0f), RADIUS - 0.5f);
        Pen border(C_BORDER, 1.0f);
        g.DrawPath(&border, &bp);
    }

    // 标题栏
    {
        // 图标：品牌方块 + 反白标识，和控制台顶栏一致
        RectF iconR((REAL)(SHADOW + 15), (REAL)(SHADOW + 13), 20.0f, 20.0f);
        GraphicsPath p;
        roundRect(p, iconR, 6.0f);
        LinearGradientBrush bg(
            PointF(iconR.X, iconR.Y),
            PointF(iconR.X + iconR.Width, iconR.Y + iconR.Height),
            Color(255, 78, 107, 255), Color(255, 160, 107, 255));
        g.FillPath(&bg, &p);

        SolidBrush white(Color(255, 255, 255, 255));
        drawMark(g, iconR.X + iconR.Width / 2, iconR.Y + iconR.Height / 2, 17.0f, &white);

        drawText(g, L"待办", titleFont(),
                 RectF((REAL)(SHADOW + 42), (REAL)(SHADOW + 12), 70, 22), C_TEXT, true, 0);

        wchar_t cnt[16];
        swprintf(cnt, 16, L"%d", g_pending);
        drawText(g, cnt, smallFont(),
                 RectF((REAL)(SHADOW + 76), (REAL)(SHADOW + 13), 40, 20), C_TEXT_FAINT, true, 0);

        {
            REAL bx = (REAL)(SHADOW + CARD_W - 34);
            RectF br(bx, (REAL)(SHADOW + 13), 22, 22);
            if (g_hoverBtn == 1) {
                GraphicsPath hp;
                roundRect(hp, br, 7.0f);
                SolidBrush hb(C_HOVER);
                g.FillPath(&hb, &hp);
            }
            Pen pen(C_TEXT_FAINT, 1.5f);
            pen.SetStartCap(LineCapRound);
            pen.SetEndCap(LineCapRound);
            g.DrawLine(&pen, br.X + 7, br.Y + 7, br.X + 15, br.Y + 15);
            g.DrawLine(&pen, br.X + 15, br.Y + 7, br.X + 7, br.Y + 15);
        }

        // 置顶开关：图钉。开着的时候点亮，和控制台那个开关同一个值
        {
            RectF br((REAL)(SHADOW + CARD_W - 60), (REAL)(SHADOW + 13), 22, 22);
            if (g_hoverBtn == 4) {
                GraphicsPath hp;
                roundRect(hp, br, 7.0f);
                SolidBrush hb(C_HOVER);
                g.FillPath(&hb, &hp);
            }
            paintPin(g, br.X + 11.0f, br.Y + 12.3f, 1.15f,
                     g_topmost ? C_ACCENT : C_TEXT_FAINT);
        }
    }

    {
        Pen sep(Color(18, 255, 255, 255), 1.0f);
        g.DrawLine(&sep, SHADOW + 14.0f, SHADOW + (REAL)TITLE_H - 0.5f,
                   SHADOW + CARD_W - 14.0f, SHADOW + (REAL)TITLE_H - 0.5f);
    }

    // 挑日期 / 填自定义预估时整块让给弹层
    if (g_pickingDate) {
        paintCalendar(g);
        return;
    }
    if (g_pickingEst) {
        paintEstPicker(g);
        return;
    }

    // 任务列表
    int maxScroll = (int)g_tasks.size() - VISIBLE_ROWS;
    if (maxScroll < 0) maxScroll = 0;
    if (g_scroll > maxScroll) g_scroll = maxScroll;

    if (!g_loaded) {
        drawText(g, L"正在连接后台", smallFont(),
                 RectF((REAL)SHADOW, (REAL)(SHADOW + LIST_TOP), (REAL)CARD_W, (REAL)ROW_H),
                 C_TEXT_FAINT, true, 1);
    } else if (g_loadError) {
        // 和空列表分开：数据损坏时要引导用户去恢复备份，不能显示成空态
        REAL midY = (REAL)(SHADOW + LIST_TOP + (LIST_BOTTOM - LIST_TOP) / 2 - 30);
        drawText(g, L"无法读取任务数据", bodyFont(),
                 RectF((REAL)SHADOW, midY, (REAL)CARD_W, 26), C_RED, true, 1);
        drawText(g, L"可在控制台中从历史备份恢复", smallFont(),
                 RectF((REAL)SHADOW, midY + 26, (REAL)CARD_W, 22), C_TEXT_FAINT, true, 1);
    } else if (g_tasks.empty()) {
        REAL midY = (REAL)(SHADOW + LIST_TOP + (LIST_BOTTOM - LIST_TOP) / 2 - 30);
        drawText(g, L"暂无待办任务", bodyFont(),
                 RectF((REAL)SHADOW, midY, (REAL)CARD_W, 26), C_TEXT_DIM, true, 1);
        drawText(g, L"点击下方“添加任务”新建", smallFont(),
                 RectF((REAL)SHADOW, midY + 26, (REAL)CARD_W, 22), C_TEXT_FAINT, true, 1);
    }

    // 任务行随列表滚动，超出可视区域时裁切；编辑状态下列表绘制区域止于面板上沿
    REAL listBottom = g_editing ? (REAL)(SHADOW + CARD_H - PANEL_H)
                                : (REAL)(SHADOW + LIST_BOTTOM);
    GraphicsState listState = g.Save();
    g.SetClip(RectF((REAL)SHADOW, (REAL)(SHADOW + LIST_TOP),
                    (REAL)CARD_W, listBottom - (SHADOW + LIST_TOP)), CombineModeIntersect);

    // 任务标记完成后，该行向右滑出并淡出，后续任务行依次补位
    DWORD leaveElapsed = g_leaving ? (GetTickCount() - g_leaveStart) : 0;

    for (int i = 0; i < VISIBLE_ROWS; ++i) {
        int idx = g_scroll + i;
        if (idx >= (int)g_tasks.size()) break;
        const Task& t = g_tasks[idx];

        REAL rowY = (REAL)(SHADOW + LIST_TOP + i * ROW_H);
        if (g_leaving && idx >= g_leaveRow) {
            // 后续任务行依次延迟 30ms 开始动画，形成连续补位效果
            REAL delay = (idx - g_leaveRow) * 30.0f;
            REAL t2 = ((REAL)leaveElapsed - 170.0f - delay) / 180.0f;
            if (t2 < 0.0f) t2 = 0.0f;
            if (t2 > 1.0f) t2 = 1.0f;
            t2 = t2 * t2 * (3.0f - 2.0f * t2);
            rowY += (REAL)ROW_H * (1.0f - t2);
        }

        paintRow(g, t, i, rowY, 0.0f, 1.0f,
                 !g_justDone.empty() && t.id == g_justDone);
    }

    // 刚完成的任务向右滑动 70px 并淡出，动画结束后隐藏
    if (g_leaving && g_leaveRow >= 0) {
        REAL t1 = (REAL)leaveElapsed / 170.0f;
        if (t1 > 1.0f) t1 = 1.0f;
        REAL y = (REAL)(SHADOW + LIST_TOP + (g_leaveRow - g_scroll) * ROW_H);
        paintRow(g, g_leaveTask, -1, y, 70.0f * t1, 1.0f - t1, true);
    }
    g.Restore(listState);

    if (maxScroll > 0 && !g_editing) {
        // 宽度设为 4px 并提高透明度；2~3px 在深色背景上的对比度不足，
        // 用于提示列表仍可继续滚动
        REAL trackX = (REAL)(SHADOW + CARD_W - 8);
        REAL trackTop = (REAL)(SHADOW + LIST_TOP + 6);
        REAL trackH = (REAL)(VISIBLE_ROWS * ROW_H - 12);
        REAL thumbH = trackH * VISIBLE_ROWS / (REAL)g_tasks.size();
        REAL thumbY = trackTop + (trackH - thumbH) * g_scroll / (REAL)maxScroll;
        GraphicsPath p;
        roundRect(p, RectF(trackX, thumbY, 4, thumbH), 2.0f);
        SolidBrush b(Color(72, 255, 255, 255));
        g.FillPath(&b, &p);
    }

    if (g_editing) {
        paintEditPanel(g);
    } else {
        REAL footY = (REAL)(SHADOW + CARD_H - FOOT_H);
        Pen sep(Color(18, 255, 255, 255), 1.0f);
        g.DrawLine(&sep, SHADOW + 14.0f, footY + 0.5f, SHADOW + CARD_W - 14.0f, footY + 0.5f);

        RectF addR((REAL)SHADOW + 6, footY + 6, (REAL)CARD_W / 2 - 8, (REAL)FOOT_H - 14);
        RectF openR((REAL)SHADOW + (REAL)CARD_W / 2, footY + 6,
                    (REAL)CARD_W / 2 - 8, (REAL)FOOT_H - 14);

        if (g_hoverBtn == 2) {
            GraphicsPath hp;
            roundRect(hp, addR, 9.0f);
            SolidBrush hb(C_HOVER);
            g.FillPath(&hb, &hp);
        }
        if (g_hoverBtn == 3) {
            GraphicsPath hp;
            roundRect(hp, openR, 9.0f);
            SolidBrush hb(C_HOVER);
            g.FillPath(&hb, &hp);
        }

        Pen plus(C_ACCENT, 1.6f);
        plus.SetStartCap(LineCapRound);
        plus.SetEndCap(LineCapRound);
        REAL px = addR.X + 14, py = addR.Y + addR.Height / 2;
        g.DrawLine(&plus, px, py - 5, px, py + 5);
        g.DrawLine(&plus, px - 5, py, px + 5, py);

        drawText(g, L"添加任务", bodyFont(),
                 RectF(addR.X + 26, addR.Y, addR.Width - 26, addR.Height), C_TEXT_DIM, true, 0);

        drawText(g, L"打开控制台", smallFont(),
                 RectF(openR.X, openR.Y, openR.Width - 10, openR.Height), C_TEXT_FAINT, true, 2);
    }
}

