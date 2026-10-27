// ---- 呈现到屏幕 ----

// UpdateLayeredWindow 要预乘 alpha，GDI+ 写的是非预乘值，直接画会发虚。
static void premultiplyAlpha(BYTE* bits, int w, int h) {
    int n = w * h;
    for (int i = 0; i < n; ++i) {
        BYTE* p = bits + (size_t)i * 4;      // BGRA
        BYTE a = p[3];
        if (a == 255) continue;
        if (a == 0) { p[0] = p[1] = p[2] = 0; continue; }
        p[0] = (BYTE)((p[0] * a + 127) / 255);
        p[1] = (BYTE)((p[1] * a + 127) / 255);
        p[2] = (BYTE)((p[2] * a + 127) / 255);
    }
}

static int dotPixels() { return (int)(DOT_SIZE * dotUnit() + 0.5f); }

