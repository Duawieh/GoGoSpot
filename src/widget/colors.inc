// ---- 配色 ----
//
// 一套配色 = 一组语义色。绘制代码只问语义（"红档的光环色"），不问主题。
// 加一个主题 = 再写一个派生类，不用回 rendering 里翻 if。
//
// 日间那套跟控制台的浅色主题对齐（--text / --border / --accent 那几个值）。
// 规律：深底提亮、浅底压深，色相不变；光环、窄条、截止文字各有自己的一档，
// 因为它们压在不同底色上，直接共用语义色会糊。

struct Palette {
    // 底色与文字
    Color card;
    Color border;
    Color text;
    Color textDim;
    Color textFaint;
    Color accent;
    Color accent2;
    Color hover;
    Color track;
    Color glass;
    Color panel;
    Color ringPad;            // 圆点外圈的垫底色

    // 四个语义档
    Color red;                // 急
    Color amber;              // 近
    Color purple;             // 不紧急但快到期
    Color green;              // 进度前半段
    Color progAmber;          // 进度条上的「近」：日间比 amber 亮一档，压在白底上不发褐

    // 圆点光环：日间要比 red/amber 更亮，深色环在亮底上会看成黑圈
    Color haloRed;
    Color haloAmber;

    // 窄条左端那层极薄的暖色：日间单独取浅色，直接压 red/amber 会发橙
    Color tintRed;
    Color tintAmber;

    // 截止时间文字：比语义色再淡一档，压在行底上
    Color dueRed;
    Color dueAmber;
    Color duePurple;

    // 行内辅助小字（逾期多久 / 还剩多少）
    Color metaRed;
    Color metaAmber;

    Color title;              // 行标题
    Color inkTone;            // 低透明度描边 / 填充的基准色

    // 窄条
    Color pillFill;
    BYTE  pillFillA;          // 底色透明度（日间要透出壁纸）
    Color pillFillHover;
    Color pillShadow;         // 日间投影
    Color pillEdge;           // 夜间那圈白描边

    // 面板 / 弹层
    Color panelBg;
    Color popupBg;
    Color checkboxIdle;
    Color menuBg;
    Color menuBorder;
    Color menuSep;

    // 卡片投影：日间用淡蓝且砍半，夜间纯黑
    Color cardShadow;
    REAL  cardShadowK;

    // 卡片上那层极淡的紫渐变，左上偏冷、右下偏紫
    Color washA;
    Color washB;

    virtual ~Palette() {}

    // 结构差异：夜间靠外发光浮起来，日间靠投影
    virtual bool glow() const = 0;
};

struct DayPalette : Palette {
    DayPalette() {
        card      = Color(253, 253, 255, 255);
        border    = Color(34, 17, 24, 39);
        text      = Color(255, 19, 26, 41);
        textDim   = Color(255, 74, 82, 100);
        textFaint = Color(255, 96, 104, 120);
        accent    = Color(255, 64, 92, 240);
        accent2   = Color(255, 139, 92, 246);
        hover     = Color(14, 17, 24, 39);
        track     = Color(20, 17, 24, 39);
        glass     = Color(104, 249, 249, 252);
        panel     = Color(247, 250, 250, 253);
        ringPad   = Color(226, 245, 245, 248);

        red    = Color(255, 232, 69, 95);
        amber  = Color(255, 168, 92, 10);
        purple = Color(255, 124, 82, 216);
        green  = Color(255, 22, 163, 127);
        progAmber = Color(255, 224, 158, 46);   // 接近圆点外圈的金色，再深一点才看得出进度

        haloRed   = Color(255, 240, 120, 138);
        haloAmber = Color(255, 246, 196, 92);

        tintRed   = Color(255, 240, 150, 168);
        tintAmber = Color(255, 240, 190, 74);

        dueRed    = Color(255, 172, 52, 76);
        dueAmber  = Color(255, 184, 124, 8);
        duePurple = Color(255, 104, 72, 166);

        metaRed   = Color(255, 168, 40, 62);
        metaAmber = Color(255, 188, 128, 10);

        title   = Color(255, 34, 38, 46);
        inkTone = Color(255, 17, 24, 39);

        pillFill      = Color(255, 252, 252, 254);
        pillFillA     = 238;
        pillFillHover = Color(255, 252, 252, 254);
        pillShadow    = Color(255, 24, 38, 76);
        pillEdge      = Color(0, 255, 255, 255);

        panelBg      = Color(250, 252, 252, 254);
        popupBg      = Color(10, 17, 24, 39);
        checkboxIdle = Color(96, 17, 24, 39);
        menuBg       = Color(250, 252, 252, 254);
        menuBorder   = Color(34, 17, 24, 39);
        menuSep      = Color(26, 17, 24, 39);

        cardShadow  = Color(255, 24, 38, 76);
        cardShadowK = 0.42f;

        washA = Color(24, 150, 140, 255);
        washB = Color(46, 170, 130, 255);
    }
    bool glow() const override { return false; }
};

struct NightPalette : Palette {
    NightPalette() {
        card      = Color(251, 22, 25, 31);
        border    = Color(40, 255, 255, 255);
        text      = Color(255, 233, 237, 246);
        textDim   = Color(255, 153, 163, 186);
        textFaint = Color(255, 115, 125, 145);
        accent    = Color(255, 108, 140, 255);
        accent2   = Color(255, 169, 123, 255);
        hover     = Color(14, 255, 255, 255);
        track     = Color(26, 255, 255, 255);
        glass     = Color(190, 15, 18, 25);
        panel     = Color(244, 16, 19, 25);
        ringPad   = Color(226, 12, 15, 22);

        red    = Color(255, 255, 92, 112);
        amber  = Color(255, 255, 189, 74);
        purple = Color(255, 178, 132, 255);
        green  = Color(255, 92, 206, 138);
        progAmber = amber;                      // 夜间 amber 本身就够亮，不另取色

        haloRed   = red;
        haloAmber = amber;

        tintRed   = red;
        tintAmber = amber;

        dueRed    = Color(255, 255, 186, 198);
        dueAmber  = Color(255, 255, 214, 150);
        duePurple = Color(255, 214, 198, 255);

        metaRed   = Color(255, 255, 150, 170);
        metaAmber = Color(255, 250, 205, 146);

        title   = Color(255, 248, 249, 252);
        inkTone = Color(255, 255, 255, 255);

        pillFill      = Color(255, 40, 42, 50);
        pillFillA     = 255;
        pillFillHover = Color(255, 58, 60, 68);
        pillShadow    = Color(255, 0, 0, 0);
        pillEdge      = Color(30, 255, 255, 255);

        panelBg      = Color(248, 26, 30, 38);
        popupBg      = Color(30, 0, 0, 0);
        checkboxIdle = Color(70, 255, 255, 255);
        menuBg       = Color(250, 25, 29, 38);
        menuBorder   = Color(46, 255, 255, 255);
        menuSep      = Color(26, 255, 255, 255);

        cardShadow  = Color(255, 0, 0, 0);
        cardShadowK = 1.0f;

        washA = Color(16, 96, 128, 255);
        washB = Color(42, 158, 116, 255);
    }
    bool glow() const override { return true; }
};

static const DayPalette   g_dayPalette;
static const NightPalette g_nightPalette;
static const Palette*     g_pal = &g_nightPalette;

// 绘制代码统一从这里取色
static const Palette& pal() { return *g_pal; }
