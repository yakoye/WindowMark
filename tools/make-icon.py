"""wmicon.png -> res/wmicon.ico

源图是 1254x1254、24bpp 无 alpha，图形外面套了一圈白色圆角底板（macOS 风格）。
直接拿去做托盘图标，在深色任务栏上就是一个白方块。

所以这里做两件事：

1. 从四个角漫水填充，把外圈和白色底板变成透明。窗口内部是浅紫色且被紫色边框整圈围住，
   漫水进不去，所以不会被掏空 —— 靠颜色阈值做不到这一点，两处白几乎一样（254,254,254）。
2. 裁到图形本身再留一点边，然后生成多尺寸 ICO。多尺寸是关键：Windows 会按用途挑
   16/20/24/32 等尺寸，只塞一张大图会让它自己缩，缩出来发虚。

改完重新生成：
    python tools/make-icon.py
"""

from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "wmicon.png"
TARGET = ROOT / "res" / "wmicon.ico"

# Windows 会用到的尺寸。16 是托盘和标题栏，256 是资源管理器的超大图标。
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]

# 漫水填充的容差。白底板和外圈几乎同色，容差太小会留下一圈白边，
# 太大会顺着阴影漏进图形里。
TOLERANCE = 24

# 裁完之后四周留的空白，占图形边长的比例。完全贴边的图标在托盘里显得局促。
PADDING_RATIO = 0.04


def main() -> None:
    image = Image.open(SOURCE).convert("RGBA")
    width, height = image.size

    # 用一个源图里不可能出现的颜色做标记
    sentinel = (255, 0, 255, 0)
    for corner in ((0, 0), (width - 1, 0), (0, height - 1), (width - 1, height - 1)):
        ImageDraw.floodfill(image, corner, sentinel, thresh=TOLERANCE)

    pixels = image.load()
    for y in range(height):
        for x in range(width):
            if pixels[x, y] == sentinel:
                pixels[x, y] = (0, 0, 0, 0)

    box = image.getbbox()
    if box is None:
        raise SystemExit("漫水填充把整张图都吃掉了，调小 TOLERANCE")
    glyph = image.crop(box)
    print(f"原图 {width}x{height} -> 裁到 {glyph.width}x{glyph.height}")

    side = max(glyph.width, glyph.height)
    pad = int(side * PADDING_RATIO)
    canvas = Image.new("RGBA", (side + pad * 2, side + pad * 2), (0, 0, 0, 0))
    canvas.paste(glyph, (pad + (side - glyph.width) // 2, pad + (side - glyph.height) // 2))

    TARGET.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(TARGET, format="ICO", sizes=[(s, s) for s in SIZES])
    print(f"已生成 {TARGET}  尺寸 {SIZES}")


if __name__ == "__main__":
    main()
