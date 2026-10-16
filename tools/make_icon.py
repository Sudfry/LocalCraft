"""
生成 LocalCraft 的应用图标（src/app.ico）。

设计
----
和界面左上角的 logo 保持一致：**圆角方块 + 文件夹 + 对勾**。
  · 底：靛蓝渐变（#6366F1 → #4F46E5），和 --brand 配色同源
  · 文件夹：纯白实心 —— 小尺寸下比描边清晰得多
  · 对勾：靛蓝色，画在文件夹上，寓意"可回退、不会丢文件"

为什么要自己画而不是找个图标库
----------------------------
项目坚持零第三方依赖，而且图标必须能嵌进 exe 的 RC 资源。
用 Pillow 直接画，尺寸可控、可复现，改配色只要改两个常量。

抗锯齿做法
----------
每个尺寸都先按 4 倍画，再用 LANCZOS 缩回去。
直接在小画布上画圆角会有明显的锯齿。
"""
import os
from PIL import Image, ImageDraw

# 直接写进 src/ —— app.rc 里的 "101 ICON app.ico" 是相对 src/ 找的
# （build.ps1 里 windres 带的是 -I src）。
# 不再在 tools/ 下留一份副本，省 361KB 的重复。
_HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(_HERE, "..", "src", "app.ico"))

# 配色
C_TOP    = (99, 102, 241)     # #6366F1
C_BOTTOM = (79, 70, 229)      # #4F46E5
SS = 4                        # 超采样倍数

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]


def render(size):
    """画一张 size×size 的图标（内部先按 SS 倍画再缩回去）。"""
    S = size * SS
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))

    # ---- 圆角方块 + 垂直渐变 ----
    grad = Image.new("RGB", (1, S))
    for y in range(S):
        t = y / max(1, S - 1)
        grad.putpixel((0, y), (
            round(C_TOP[0] + (C_BOTTOM[0] - C_TOP[0]) * t),
            round(C_TOP[1] + (C_BOTTOM[1] - C_TOP[1]) * t),
            round(C_TOP[2] + (C_BOTTOM[2] - C_TOP[2]) * t),
        ))
    grad = grad.resize((S, S), Image.NEAREST)

    radius = int(S * 0.22)
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=radius, fill=255)
    img.paste(grad, (0, 0), mask)

    d = ImageDraw.Draw(img)

    # ---- 白色文件夹（实心）----
    # 几何按 24×24 的 viewBox 换算，和界面里的 SVG 比例一致
    u = S / 24.0

    def R(x0, y0, x1, y1):
        return [x0 * u, y0 * u, x1 * u, y1 * u]

    # 小尺寸要换一套更简化的字形。
    # 16~32px 下，"文件夹的舌头 + 细对勾"会糊成一坨 ——
    # 所以这里去掉舌头、把文件夹放大、对勾加粗，靠轮廓本身让人认出来。
    simple = size <= 32

    if simple:
        d.rounded_rectangle(R(3.0, 6.5, 21.0, 19.5), radius=2.6 * u,
                            fill=(255, 255, 255, 255))
        lw = max(1, int(3.3 * u))
        pts = [(8.4 * u, 13.1 * u), (11.0 * u, 15.7 * u), (16.0 * u, 10.6 * u)]
    else:
        # 后半部分（文件夹的"舌头"）
        d.rounded_rectangle(R(3.6, 5.2, 13.2, 12.0), radius=2.0 * u,
                            fill=(255, 255, 255, 255))
        # 前半部分（主体）
        d.rounded_rectangle(R(3.6, 8.0, 20.4, 19.0), radius=2.2 * u,
                            fill=(255, 255, 255, 255))
        lw = max(1, int(2.1 * u))
        pts = [(9.4 * u, 13.4 * u), (11.4 * u, 15.4 * u), (15.2 * u, 11.4 * u)]

    # ---- 对勾：用底色画在白色文件夹上，形成"镂空"效果 ----
    d.line(pts, fill=(79, 70, 229, 255), width=lw, joint="curve")
    # 两端补圆头，避免线头是方的
    for (px, py) in (pts[0], pts[-1]):
        r = lw / 2.0
        d.ellipse([px - r, py - r, px + r, py + r], fill=(79, 70, 229, 255))

    return img.resize((size, size), Image.LANCZOS)


def main():
    frames = [render(s) for s in SIZES]
    # 以最大的那张为基准，其余作为多尺寸帧一起写进 ico
    big = frames[-1]
    big.save(OUT, format="ICO", sizes=[(s, s) for s in SIZES],
             append_images=frames[:-1], bitmap_format="bmp")
    print("已生成", OUT, os.path.getsize(OUT), "字节")
    for s in SIZES:
        print("   %3d×%-3d" % (s, s))

    # 顺便导一张 PNG，方便放进文档
    png = os.path.normpath(os.path.join(_HERE, "..", "docs", "图标预览.png"))
    frames[-2].save(png)
    print("已生成", os.path.normpath(png))


if __name__ == "__main__":
    main()
