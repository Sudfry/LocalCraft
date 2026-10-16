import os
from PIL import Image, ImageDraw
here = os.path.dirname(os.path.abspath(__file__))
ico = os.path.join(here, "app.ico")
sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
imgs = {}
im = Image.open(ico)
for s in sizes:
    im2 = Image.open(ico); im2.size = (s, s); im2.load()
    imgs[s] = im2.convert("RGBA")

# 浅色 / 深色两条背景带
pad = 20
W = sum(sizes) + pad*(len(sizes)+1) + 40
band = 300
canvas = Image.new("RGB", (W, band*2), (255,255,255))
d = ImageDraw.Draw(canvas)
d.rectangle([0, band, W, band*2], fill=(32,33,36))
x = pad
for s in sizes:
    a = imgs[s].resize((s,s), Image.LANCZOS)
    canvas.paste(a, (x, (band-s)//2), a)
    canvas.paste(a, (x, band + (band-s)//2), a)
    x += s + pad
out = os.path.join(here, "..", "docs", "图标预览.png")
canvas.save(os.path.normpath(out))
print("ok", os.path.getsize(os.path.normpath(out)))
