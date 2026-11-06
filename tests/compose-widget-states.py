# 把折叠态的不同形态合成一张 README 配图。
# 输入是 tests/cap.cpp 抓的窗口截图，黑底 RGBA，所以画布也用纯黑，接缝看不出来。
# 上一行是圆点态，下一行是窄条态；单行排四张会到 1500px，塞进 README 字就太小了。
import os
import sys
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build', 'test', 'shots3')
OUT = os.path.join(ROOT, 'docs', 'widget-collapsed.png')

ROWS = [
    [
        ('w-dot.png', '常规折叠状态的悬浮清单'),
        ('w-dot-amber.png', '有剩余完成时间较紧张的任务'),
    ],
    [
        ('w-pill-urgent.png', '有紧急任务时自动亮起并提示'),
        ('w-pill-run.png', '有正在进行的限时任务'),
    ],
]

GAP = 88
ROW_GAP = 40
MARGIN = 56
CAP_GAP = 26
CAP_H = 30
TOP = 26
FONT_SIZE = 19


def loadFont(size):
    for name in ('msyh.ttc', 'msyhl.ttc', 'simhei.ttf'):
        p = os.path.join(os.environ.get('WINDIR', r'C:\Windows'), 'Fonts', name)
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def main():
    font = loadFont(FONT_SIZE)
    probe = ImageDraw.Draw(Image.new('RGB', (1, 1)))

    rows = []
    for spec in ROWS:
        items = []
        for fn, cap in spec:
            p = os.path.join(SRC, fn)
            if not os.path.exists(p):
                sys.exit('缺少输入图：' + p)
            im = Image.open(p).convert('RGB')
            # 槽宽取图和标题里较宽的那个，否则窄图的标题会顶到画布外被裁掉
            slot = max(im.width, int(probe.textlength(cap, font=font)) + 8)
            items.append((im, cap, slot))
        rows.append(items)

    rowW = [sum(s for _, _, s in items) + GAP * (len(items) - 1) for items in rows]
    bodyH = max(im.height for items in rows for im, _, _ in items)
    rowH = bodyH + CAP_GAP + CAP_H
    width = max(rowW) + MARGIN * 2
    height = TOP * 2 + rowH * len(rows) + ROW_GAP * (len(rows) - 1)

    canvas = Image.new('RGB', (width, height), (0, 0, 0))
    draw = ImageDraw.Draw(canvas)

    y = TOP
    for items, w in zip(rows, rowW):
        x = (width - w) // 2
        for im, cap, slot in items:
            canvas.paste(im, (x + (slot - im.width) // 2, y + (bodyH - im.height) // 2))
            tw = draw.textlength(cap, font=font)
            draw.text((x + (slot - tw) / 2, y + bodyH + CAP_GAP), cap,
                      font=font, fill=(138, 143, 158))
            x += slot + GAP
        y += rowH + ROW_GAP

    canvas.save(OUT, optimize=True)
    print('wrote', OUT, canvas.size)


if __name__ == '__main__':
    main()
