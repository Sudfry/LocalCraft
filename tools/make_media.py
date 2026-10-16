# -*- coding: utf-8 -*-
"""
造带 EXIF / ID3 的测试文件。

为什么需要这个：EXIF 和 ID3 的解析是纯手写的，光靠"能编译"证明不了任何事。
必须拿真实文件跑一遍，看能不能读出正确的日期、机型、艺术家。
而且要找图片质量参差的情况 —— 有大端字节序的、有缺字段的、
有整个 EXIF 段都没有的，这些才是解析器真正会翻车的地方。
"""
import os
import struct
import sys

from PIL import Image

OUT = sys.argv[1] if len(sys.argv) > 1 else "."


# ---------------------------------------------------------------- EXIF 写入

def build_exif(date_str, make, model, iso=None, fnum=None,
               exposure=None, focal=None, lens=None, be=False):
    """手工构造一个最小可用的 EXIF/TIFF 块。

    用 Pillow 的 Exif 对象也行，但它对"大端字节序"这种变体不好控制，
    而字节序恰恰是最容易写错的地方，所以这里直接拼字节。
    """
    e = ">" if be else "<"
    hdr = b"MM\x00*" if be else b"II*\x00"

    entries = []          # (tag, type, count, payload)
    def ascii_(tag, s):
        b = s.encode("ascii") + b"\x00"
        entries.append((tag, 2, len(b), b))
    def short_(tag, v):
        entries.append((tag, 3, 1, struct.pack(e + "H", v)))
    def long_(tag, v):
        entries.append((tag, 4, 1, struct.pack(e + "I", v)))
    def rational_(tag, num, den):
        entries.append((tag, 5, 1, struct.pack(e + "II", num, den)))

    ascii_(0x010F, make)                      # Make
    ascii_(0x0110, model)                     # Model
    ascii_(0x0132, date_str.replace("-", ":") + " 00:00:00")   # DateTime
    long_(0x8769, 0)                          # ExifIFD 指针，稍后回填

    exif_entries = []
    def ex_ascii(tag, s):
        b = s.encode("ascii") + b"\x00"
        exif_entries.append((tag, 2, len(b), b))
    def ex_short(tag, v):
        exif_entries.append((tag, 3, 1, struct.pack(e + "H", v)))
    def ex_rational(tag, num, den):
        exif_entries.append((tag, 5, 1, struct.pack(e + "II", num, den)))

    ex_ascii(0x9003, date_str.replace("-", ":") + " 14:30:22")  # DateTimeOriginal
    if iso is not None:
        ex_short(0x8827, iso)
    if fnum is not None:
        ex_rational(0x829D, int(round(fnum * 10)), 10)
    if exposure is not None:
        num, den = exposure
        ex_rational(0x829A, num, den)
    if focal is not None:
        ex_rational(0x920A, int(focal), 1)
    if lens:
        ex_ascii(0xA434, lens)
    ex_short(0xA002, 1200)                    # ExifImageWidth
    ex_short(0xA003, 800)                     # ExifImageHeight

    entries.sort(key=lambda x: x[0])
    exif_entries.sort(key=lambda x: x[0])

    # 布局：[TIFF 头 8B] [IFD0] [ExifIFD] [数据区]
    ifd0_off = 8
    ifd0_size = 2 + 12 * len(entries) + 4
    exif_off = ifd0_off + ifd0_size
    exif_size = 2 + 12 * len(exif_entries) + 4
    data_off = exif_off + exif_size

    # 两个 IFD 共用**同一个**数据区，偏移必须累计。
    # 一开始这里两次调用都从 data_off 重新开始算，结果 ExifIFD 的条目
    # 全都指向了 IFD0 的数据 —— DateTimeOriginal 读出来是垃圾，
    # 日期就变成空的。这是造测试数据的脚本自己的 bug。
    def emit(entries, data_base):
        body = struct.pack(e + "H", len(entries))
        extra = b""
        for tag, typ, cnt, payload in entries:
            tsize = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8}[typ] * cnt
            if tag == 0x8769:                 # 回填 ExifIFD 指针
                payload = struct.pack(e + "I", exif_off)
                tsize = 4
            if tsize <= 4:
                val = payload + b"\x00" * (4 - len(payload))
            else:
                val = struct.pack(e + "I", data_base + len(extra))
                extra += payload
                if len(extra) % 2:
                    extra += b"\x00"
            body += struct.pack(e + "HHI", tag, typ, cnt) + val
        body += struct.pack(e + "I", 0)       # 下一个 IFD 偏移 = 0
        return body, extra

    b0, x0 = emit(entries, data_off)
    b1, x1 = emit(exif_entries, data_off + len(x0))
    tiff = hdr + struct.pack(e + "I", ifd0_off) + b0 + b1 + x0 + x1
    return b"Exif\x00\x00" + tiff


def make_jpeg(path, exif_block=None, size=(1200, 800), color=(90, 120, 160)):
    img = Image.new("RGB", size, color)
    if exif_block:
        img.save(path, "JPEG", quality=70, exif=exif_block)
    else:
        img.save(path, "JPEG", quality=70)


# ---------------------------------------------------------------- ID3 写入

def synchsafe(n):
    return bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F])


def id3_frame(fid, text, enc=1):
    """文本帧。enc=1 是带 BOM 的 UTF-16 小端，中文最常用的写法。"""
    if enc == 1:
        payload = b"\x01" + "\ufeff".encode("utf-16-le") + text.encode("utf-16-le") + b"\x00\x00"
    else:
        payload = b"\x03" + text.encode("utf-8") + b"\x00"
    return fid.encode("ascii") + struct.pack(">I", len(payload)) + b"\x00\x00" + payload


def make_mp3(path, title, artist, album, year, track, genre, with_v1=False):
    frames = b""
    if title:  frames += id3_frame("TIT2", title)
    if artist: frames += id3_frame("TPE1", artist)
    if album:  frames += id3_frame("TALB", album)
    if year:   frames += id3_frame("TYER", year)
    if track:  frames += id3_frame("TRCK", track)
    if genre:  frames += id3_frame("TCON", genre)

    v2 = b"ID3\x03\x00\x00" + synchsafe(len(frames)) + frames
    # 一段静音 MP3 帧，够让它像个真文件
    audio = bytes.fromhex("fffb90640000000000000000000000000000000000000000000000000000") * 30

    data = v2 + audio
    if with_v1:
        def pad(s, n):
            b = s.encode("latin-1", "replace")[:n]
            return b + b"\x00" * (n - len(b))
        v1 = (b"TAG" + pad(title or "", 30) + pad(artist or "", 30) +
              pad(album or "", 30) + pad(year or "", 4) + b"\x00" +
              bytes([int(track) if track and track.isdigit() else 0]) +
              bytes([17]))
        data += v1
    with open(path, "wb") as f:
        f.write(data)


# ---------------------------------------------------------------- 主流程

os.makedirs(OUT, exist_ok=True)

# 1) 正常照片：有完整 EXIF，小端
make_jpeg(os.path.join(OUT, "IMG_0001.jpg"),
          build_exif("2026-03-15", "Canon", "Canon EOS R6",
                     iso=400, fnum=2.8, exposure=(1, 250), focal=50,
                     lens="RF50mm F1.8 STM"))

# 2) 大端字节序 —— 解析器最容易写错的地方（不是所有相机都是小端）
make_jpeg(os.path.join(OUT, "IMG_0002.jpg"),
          build_exif("2025-07-04", "NIKON CORPORATION", "NIKON Z6_2",
                     iso=1600, fnum=4.0, exposure=(1, 60), focal=85,
                     lens="NIKKOR Z 85mm", be=True))

# 3) 只有最小字段：没有 ISO / 光圈 / 焦距
make_jpeg(os.path.join(OUT, "IMG_0003.jpg"),
          build_exif("2024-01-01", "Apple", "iPhone 15 Pro"),
          color=(200, 160, 120))

# 4) 完全没有 EXIF 的 JPEG —— 用来验证"未知"占位符
make_jpeg(os.path.join(OUT, "no_exif.jpg"), None, color=(30, 30, 30))

# 5) 文件名本身就带日期，但 EXIF 是另一天（验证确实读的是 EXIF 不是文件名）
make_jpeg(os.path.join(OUT, "2020-01-01_旧名字.jpg"),
          build_exif("2026-11-20", "SONY", "ILCE-7M4", iso=100, fnum=1.8,
                     exposure=(1, 1000), focal=35))

# 6) 带 ID3v2 的 mp3（中文标签，UTF-16 编码）
make_mp3(os.path.join(OUT, "track01.mp3"),
         "夜曲", "周杰伦", "十一月的萧邦", "2005", "1", "Pop")

# 7) 只有 ID3v1 的 mp3（老格式，固定偏移）
make_mp3(os.path.join(OUT, "track02.mp3"),
         "Lemon", "米津玄師", "BOOTLEG", "2018", "3", "JPop", with_v1=True)

# 8) 什么都没有的 mp3
with open(os.path.join(OUT, "plain.mp3"), "wb") as f:
    f.write(b"\xff\xfb\x90\x64" + b"\x00" * 400)

files = sorted(os.listdir(OUT))
print("已生成 %d 个文件：" % len(files))
for n in files:
    print("  %-28s %8d 字节" % (n, os.path.getsize(os.path.join(OUT, n))))
