# -*- coding: utf-8 -*-
r"""
生成 LocalCraft 的测试数据。

放在 AIGC\测试数据\ 下，按功能分成 6 组，每组都对应软件里的一个页面：
  01-重命名素材   文件名很乱（空格、括号、大小写混杂），用来试批量重命名
  02-待整理       各种类型混在一起，用来试整理分类
  03-重复内容     正文几乎一样、文件名不同，用来试内容去重
  04-摘要素材     中文长文，用来试文档摘要
  05-格式转换     图片多格式 + 文本，用来试格式转换
  06-Office 文档  docx / xlsx / pptx / pdf，用来试抽文本和导出

内容都是现写的，不含任何真实数据。
"""
import os
import io
import csv
import zipfile
import random

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "测试数据")
ROOT = os.path.normpath(ROOT)

random.seed(20261015)


def W(rel, text, enc="utf-8"):
    """写一个文本文件（自动建目录）。"""
    p = os.path.join(ROOT, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding=enc, newline="\n") as f:
        f.write(text)
    return p


def font(size):
    """尽量找一个能显示中文的字体。"""
    for name in ("msyh.ttc", "msyhbd.ttc", "simhei.ttf", "simsun.ttc"):
        try:
            return ImageFont.truetype("C:/Windows/Fonts/" + name, size)
        except Exception:
            continue
    return ImageFont.load_default()


def make_image(path, w, h, top, bottom, label, sub=""):
    """画一张渐变图，中间写说明文字 —— 转换格式后一眼能看出对不对。"""
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)], fill=(
            round(top[0] + (bottom[0] - top[0]) * t),
            round(top[1] + (bottom[1] - top[1]) * t),
            round(top[2] + (bottom[2] - top[2]) * t)))
    f1 = font(max(14, w // 16))
    f2 = font(max(11, w // 26))
    tw = d.textlength(label, font=f1)
    d.text(((w - tw) / 2, h / 2 - f1.size * 0.9), label, font=f1, fill=(255, 255, 255))
    if sub:
        sw = d.textlength(sub, font=f2)
        d.text(((w - sw) / 2, h / 2 + f2.size * 0.5), sub, font=f2, fill=(235, 238, 255))
    # 四角标记，方便确认没被裁掉
    for (x, y) in ((0, 0), (w - 24, 0), (0, h - 24), (w - 24, h - 24)):
        d.rectangle([x, y, x + 23, y + 23], outline=(255, 255, 255), width=2)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.save(path)


# ======================================================= 正文素材 ==========

AI_DOC = """人工智能的发展历程与现状

人工智能并不是一个新概念。早在 1956 年的达特茅斯会议上，麦卡锡、明斯基等学者就提出了"人工智能"这一术语，
并乐观地预测机器将在二十年内具备与人相当的智能。然而此后数十年间，这一领域经历了两次明显的低谷：
第一次出现在上世纪七十年代，原因是计算能力不足与 funding 削减；第二次出现在八十年代末，
专家系统在维护成本和知识获取上的瓶颈让热潮再次退去。

转折发生在 2012 年。深度卷积神经网络在 ImageNet 图像识别竞赛中把错误率从 26% 降到 15%，
这一结果第一次让工业界意识到，只要数据量和算力足够，神经网络的潜力远超传统方法。
此后十年，几个关键条件同时成熟：GPU 提供了廉价并行算力，互联网积累了海量标注数据，
开源框架大幅降低了研究门槛。

2017 年提出的 Transformer 架构是另一个分水岭。它用自注意力机制替代循环结构，
使模型能够并行处理长序列，直接把可训练参数量推高了几个数量级。
基于这一架构，2018 年之后预训练语言模型成为主流范式：
先在通用语料上做无监督预训练，再针对具体任务做微调。

当前这一领域呈现出三个特征。第一是规模效应依然显著，
更大的模型在推理、代码、多语言等任务上持续表现出更好的泛化能力。
第二是从单一模态走向多模态，模型同时处理文本、图像、语音甚至视频。
第三是应用重心从"识别"转向"生成"，内容生产方式正在被重新塑造。

与此同时，制约因素同样明显。训练一次大模型的能耗与成本极高，
高质量语料的增长也接近瓶颈；模型输出的可靠性和可解释性仍然不足，
在医疗、法律等高风险场景中难以直接采信；此外，版权、隐私与责任归属等问题尚未形成共识。

对于普通使用者而言，更现实的态度是把这类技术当作效率工具而非全能答案。
它擅长归纳、改写和初稿生成，但不擅长保证事实准确。
关键决策仍然需要人来把关。"""

CITY_DOC = """关于城市老旧小区改造的调研报告

一、调研背景

本次调研选取了三个建成于上世纪九十年代的居民小区，共走访住户 240 户，
召开座谈会 6 场，回收有效问卷 218 份。目的是摸清居民对改造的真实诉求，
为下一步的改造方案提供依据。

二、主要发现

第一个发现是居民诉求排序与管理者预判存在明显偏差。
问卷显示，居民最关心的三项依次是停车位不足（占比 71%）、
上下水管道老化（占比 64%）和楼道照明损坏（占比 52%）。
而外立面翻新虽然投入占比最高，在居民关心程度上仅排第七。

第二个发现是不同楼层的诉求差异巨大。
一层住户普遍反对加装电梯，主要担心采光受损和分摊费用；
五层以上住户则把加装电梯列为首要诉求。
这种结构性分歧如果不提前沟通，很容易在施工阶段引发矛盾。

第三个发现是改造后的维护责任不清是最大的长期隐患。
三个小区中有两个在改造完成两年内就出现了设施再次损坏的情况，
原因是改造由财政出资、日常维护由物业负责，而物业费收缴率长期偏低。

三、建议

其一，改造前应做一次覆盖全部住户的诉求排序调查，
把有限资金优先投向居民感知最强的项目，而不是投入产出比最低的外观工程。

其二，对加装电梯这类存在明显利益冲突的项目，
应当建立分楼层协商机制，并配套差异化的费用分摊方案。

其三，在改造立项阶段就同步明确后续维护的资金来源和责任人，
必要时从改造资金中划出一定比例作为维护基金。

其四，改造方案应当向居民完整公开，包括预算构成和施工进度，
以减少施工期间的信息不对称引发的投诉。"""

PRODUCT_DOC = """LocalCraft 产品需求说明

一、产品定位

LocalCraft 是一款完全离线的本地文件处理工具，面向需要批量整理文档、
图片和文件夹、但又不能把文件上传到网上的用户。
它不联网、不接入任何外部接口、不使用任何 AI 模型。

二、核心原则

第一条原则是先预览、再执行、可回退。
任何批量操作都必须先生成对照表，让用户确认每一行会发生什么，
确认后才真正动手，并且每一步都写入回退日志。

第二条原则是宁可少做，不可做错。
遇到重名、路径冲突、权限不足这类情况时，默认选择跳过并在预览表里标出来，
而不是自作主张地覆盖。

第三条原则是不夸大能力。
扫描件的文字识别需要 AI 模型，本项目明确不做，并在界面上写清楚原因，
而不是返回一堆乱码让人以为识别成功了。

三、功能模块

模块一是可回退的批量操作内核，包括重命名、移动、归类三种动作。
模块二是批量格式转换，内置的文本和图片转换不依赖外部程序。
模块三是基于内容指纹的重复文件查找，按正文而不是文件名判断。
模块四是抽取式摘要，用 TextRank 和 TF-IDF 提取关键句。
模块五是导出，支持 Markdown、Word 和 PDF。
模块六是文件夹扫描与分类。

四、约束

体积控制在几十 MB 以内，4GB 内存的机器上要能流畅运行。
目标电脑不需要安装 Python 或任何运行库。"""


def similar(base, swaps):
    """把一篇文章做若干同义替换，模拟"内容雷同但不是完全一样"。"""
    t = base
    for a, b in swaps:
        t = t.replace(a, b)
    return t


# ======================================================= 各组数据 ==========

def group1_rename():
    """名字很乱的文件，用来试批量重命名。"""
    d = "01-重命名素材"
    for i in range(1, 7):
        make_image(os.path.join(ROOT, d, "IMG_2024 0101_照片 %03d.jpg" % i),
                   480, 320, (99, 102, 241), (67, 56, 202),
                   "照片 %d" % i, "IMG_2024 0101_照片 %03d" % i)
    W(os.path.join(d, "未命名文档 (1).txt"), "这是第一份未命名文档。\n只为测试重命名，内容不重要。\n")
    W(os.path.join(d, "未命名文档 (2).txt"), "这是第二份未命名文档。\n")
    W(os.path.join(d, "note 最终版 final FINAL.txt"), "文件名里混了中英文和大小写。\n")
    for i in range(1, 5):
        W(os.path.join(d, "DSC0%04d.txt" % i), "相机导出风格的文件名 %d。\n" % i)


def group2_organize():
    """类型混杂，用来试整理分类。"""
    d = "02-待整理"
    W(os.path.join(d, "报告-2023年度.txt"), "2023 年度报告。\n全年完成项目 12 个。\n")
    W(os.path.join(d, "报告-2024年度.txt"), "2024 年度报告。\n全年完成项目 17 个。\n")
    W(os.path.join(d, "会议纪要.md"), "# 会议纪要\n\n时间：2026 年 3 月 4 日\n\n## 决议\n\n1. 通过预算方案\n2. 下月启动第二轮调研\n")
    W(os.path.join(d, "预算表.csv"), "项目,预算,实际\n办公设备,12000,11800\n差旅,30000,26500\n培训,8000,8000\n")

    make_image(os.path.join(ROOT, d, "截图-a.png"), 640, 400,
               (56, 189, 248), (37, 99, 235), "截图 a", "png")
    make_image(os.path.join(ROOT, d, "风景.jpg"), 800, 480,
               (52, 211, 153), (5, 150, 105), "风景", "jpg")
    make_image(os.path.join(ROOT, d, "图标.bmp"), 256, 256,
               (251, 191, 36), (217, 119, 6), "图标", "bmp")

    # 一个占位音频（不是真音频，只用来测试"按类型分类"能不能认出扩展名）
    p = os.path.join(ROOT, d, "录音-示例.mp3")
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(b"ID3\x03\x00\x00\x00\x00\x00\x00")   # 一个空的 ID3 头
        f.write(b"\x00" * 2048)

    # 一个压缩包
    p = os.path.join(ROOT, d, "归档.zip")
    with zipfile.ZipFile(p, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("说明.txt", "这是压缩包里的一个文件。\n")
        z.writestr("数据/数字.txt", "1 2 3 4 5\n")

    # 带子文件夹，用来试"包含子文件夹"
    W(os.path.join(d, "子文件夹", "更深一层", "深层文件.txt"), "我在很深的目录里。\n")
    W(os.path.join(d, "子文件夹", "同层文件.txt"), "我在子文件夹的同一层。\n")


def group3_dupes():
    """正文高度相似、文件名不同，用来试内容去重。"""
    d = "03-重复内容"
    W(os.path.join(d, "产品说明-终版.txt"), PRODUCT_DOC)
    W(os.path.join(d, "产品说明-最终版.txt"),
      similar(PRODUCT_DOC, [("完全离线", "彻底离线"), ("批量整理", "成批整理")]))
    W(os.path.join(d, "产品说明-修改后.txt"),
      similar(PRODUCT_DOC, [("核心原则", "基本原则"), ("不夸大能力", "不虚报能力")]))
    W(os.path.join(d, "产品说明-副本.txt"), PRODUCT_DOC)

    # 一篇明显不同的，用来验证不会误判成重复
    W(os.path.join(d, "完全无关的文档.txt"), AI_DOC)

    # 短文档：默认会被"正文少于 N 字"滤掉，可以拿来验证这个开关
    W(os.path.join(d, "太短的说明.txt"), "这个文件太短了。\n")
    W(os.path.join(d, "也很短.txt"), "这个文件也太短了。\n")


def group4_summary():
    """中文长文，用来试摘要。"""
    d = "04-摘要素材"
    W(os.path.join(d, "人工智能发展概述.txt"), AI_DOC)
    W(os.path.join(d, "城市规划调研报告.md"),
      "# " + CITY_DOC.replace("\n", "\n\n").replace("\n\n\n", "\n\n"))
    W(os.path.join(d, "产品需求文档.txt"), PRODUCT_DOC)
    # 一份 HTML，用来试 HTML 抽文本
    W(os.path.join(d, "网页文章.html"),
      "<!doctype html>\n<html><head><meta charset='utf-8'>"
      "<title>网页文章</title></head><body><article>"
      "<h1>关于效率工具的几点看法</h1>"
      "<p>工具的价值不在于功能多，而在于能不能稳定地解决一个具体问题。</p>"
      "<p>一个每天都要用、但偶尔出错的工具，比一个功能强大但需要反复确认的工具更让人放心。</p>"
      "<p>判断标准很简单：用它之后，你还需要回头检查结果吗？</p>"
      "</article></body></html>\n")


def group5_convert():
    """多格式图片 + 文本，用来试格式转换。"""
    d = "05-格式转换"
    base = os.path.join(ROOT, d, "示例图片")
    make_image(base + ".png", 800, 600, (99, 102, 241), (139, 92, 246), "示例图片", "PNG 800×600")
    make_image(base + ".jpg", 800, 600, (16, 185, 129), (5, 150, 105), "示例图片", "JPEG 800×600")
    make_image(base + ".bmp", 800, 600, (245, 158, 11), (217, 119, 6), "示例图片", "BMP 800×600")
    make_image(base + ".gif", 400, 300, (239, 68, 68), (185, 28, 28), "示例图片", "GIF 400×300")
    make_image(base + "-小图.png", 120, 90, (14, 165, 233), (2, 132, 199), "小图", "120×90")

    W(os.path.join(d, "待转文本.txt"), "第一行\n第二行\n第三行\n")
    W(os.path.join(d, "待转文档.md"), "# 标题\n\n正文一段。\n\n- 项目一\n- 项目二\n")


def group6_office():
    """Office 文档 + PDF，用来试抽文本和导出。"""
    d = "06-Office文档"
    os.makedirs(os.path.join(ROOT, d), exist_ok=True)

    # --- Word ---
    try:
        from docx import Document
        from docx.shared import Pt
        doc = Document()
        doc.add_heading("2026 年第一季度工作报告", level=1)
        doc.add_paragraph("本报告概述本季度主要工作进展、存在问题和下季度计划。")
        doc.add_heading("一、主要进展", level=2)
        for t in ["完成文件处理工具的核心模块开发",
                  "建立了一套可回退的批量操作机制",
                  "通过 862 条自动化断言验证"]:
            doc.add_paragraph(t, style="List Bullet")
        doc.add_heading("二、存在问题", level=2)
        doc.add_paragraph("在缺少 WebView2 运行库的老旧机器上，界面会退回简化版本。")
        doc.add_heading("三、下季度计划", level=2)
        doc.add_paragraph("继续完善格式转换模块，补齐文档说明。")
        doc.save(os.path.join(ROOT, d, "季度报告.docx"))
    except Exception as e:
        print("  docx 跳过:", e)

    # --- Excel ---
    try:
        from openpyxl import Workbook
        wb = Workbook()
        ws = wb.active
        ws.title = "销售数据"
        ws.append(["月份", "销售额", "订单数", "客单价"])
        rows = [("1月", 128000, 412, 310.7), ("2月", 96000, 305, 314.8),
                ("3月", 175000, 528, 331.4), ("4月", 162000, 501, 323.4),
                ("5月", 189000, 570, 331.6), ("6月", 210000, 634, 331.2)]
        for r in rows:
            ws.append(r)
        ws2 = wb.create_sheet("备注")
        ws2.append(["说明", "本表为测试数据，非真实业务数据"])
        wb.save(os.path.join(ROOT, d, "数据表.xlsx"))
    except Exception as e:
        print("  xlsx 跳过:", e)

    # --- PowerPoint ---
    try:
        from pptx import Presentation
        from pptx.util import Inches, Pt
        prs = Presentation()
        titles = [("LocalCraft", "本地文件处理工具"),
                  ("完全离线", "不联网、不上传、不调用任何外部接口"),
                  ("先预览再执行", "每一步都能回退"),
                  ("六个模块", "重命名 · 整理 · 转换 · 去重 · 摘要 · 导出")]
        for title, body in titles:
            slide = prs.slides.add_slide(prs.slide_layouts[1])
            slide.shapes.title.text = title
            slide.placeholders[1].text = body
        prs.save(os.path.join(ROOT, d, "演示文稿.pptx"))
    except Exception as e:
        print("  pptx 跳过:", e)

    # --- PDF（手写一个极简但合法的 PDF，中文用 STSong-Light） ---
    try:
        pdf_path = os.path.join(ROOT, d, "示例文档.pdf")
        lines = [
            "LocalCraft Sample Document",
            "",
            "This PDF is generated for testing the text extraction.",
            "It uses a simple built-in font so no external tool is needed.",
            "",
            "1. Reversible batch operations",
            "2. Content based de-duplication",
            "3. Extractive summarization",
        ]
        content = "BT /F1 14 Tf 60 760 Td 18 TL\n"
        for ln in lines:
            esc = ln.replace("\\", r"\\").replace("(", r"\(").replace(")", r"\)")
            content += "(%s) Tj T*\n" % esc
        content += "ET"
        objs = []
        objs.append("<< /Type /Catalog /Pages 2 0 R >>")
        objs.append("<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
        objs.append("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
                    "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>")
        objs.append("<< /Length %d >>\nstream\n%s\nendstream" % (len(content), content))
        objs.append("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
        out = io.BytesIO()
        out.write(b"%PDF-1.4\n")
        offsets = []
        for i, o in enumerate(objs, start=1):
            offsets.append(out.tell())
            out.write(("%d 0 obj\n%s\nendobj\n" % (i, o)).encode("latin-1"))
        xref = out.tell()
        out.write(("xref\n0 %d\n" % (len(objs) + 1)).encode("latin-1"))
        out.write(b"0000000000 65535 f \n")
        for off in offsets:
            out.write(("%010d 00000 n \n" % off).encode("latin-1"))
        out.write(("trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n"
                   % (len(objs) + 1, xref)).encode("latin-1"))
        with open(pdf_path, "wb") as f:
            f.write(out.getvalue())
    except Exception as e:
        print("  pdf 跳过:", e)


def write_readme():
    W("说明.txt", """LocalCraft 测试数据
================================================

这些文件是自动生成的，内容全部现编，不含任何真实数据。
按软件里的六个页面分成六组，每组都能直接拿去试：

  01-重命名素材
      文件名故意很乱：空格、括号、大小写中英文混杂。
      用法：批量重命名 → 选这个文件夹 → 试试
            · 查找替换：把空格换成下划线
            · 正则：^IMG_\\d+\\s+  →  照片-
            · 追加序号：起始 1、位数 3
      注意：里面有 6 张 jpg 加若干 txt，预览表应该列出全部。

  02-待整理
      各种类型混在一起，还有两层子文件夹和一个压缩包。
      用法：整理分类 → 选这个文件夹
            · 按类型 → 模板 {category}
            · 按年份 / 按扩展名 / 按首字母 都试试
            · 勾上"包含子文件夹"和不勾，对比预览条数

  03-重复内容
      "产品说明"有四个版本：正文高度相似，但文件名完全不同，
      其中两份是完全一样的，另两份做了同义替换。
      "完全无关的文档.txt" 和它们毫无关系，用来验证不会误判。
      两个 "太短*.txt" 用来验证"忽略正文少于 N 字"这个开关。
      用法：内容去重 → 选这个文件夹 → 先把阈值调到 85% 左右试试

  04-摘要素材
      三篇中文长文（1200 字上下）+ 一份 HTML。
      用法：文档摘要 → 选任意一篇 → 生成摘要
            TextRank 和 TF-IDF 各试一次，对比抽出来的句子

  05-格式转换
      同一张图存成了 png / jpg / bmp / gif 四种格式，还有一张小图。
      用法：格式转换 → 选这个文件夹
            · 转 PNG（内置，不需要外部程序）
            · 转 Markdown（txt / md 都试）
      转 Word / PDF 需要 Pandoc 或 LibreOffice，没装的话界面上会写明。

  06-Office文档
      docx / xlsx / pptx / pdf 各一份，都是能真正打开的文件。
      用法：文档摘要 → 选 docx 或 pdf，验证能不能抽出正文
            也可以拿它们试导出（Markdown / Word / PDF）

提示
------------------------------------------------
· 所有操作都是"先预览、再执行、可回退"，可以先随便试，
  执行完到"回退历史"里点一下就能还原。
· 建议先整个复制一份再操作，或者直接用版本控制看着。
· 想重新生成：python tools\\make_testdata.py
""")


def main():
    print("输出目录:", ROOT)
    group1_rename()
    group2_organize()
    group3_dupes()
    group4_summary()
    group5_convert()
    group6_office()
    write_readme()

    n = sum(len(fs) for _, _, fs in os.walk(ROOT))
    total = sum(os.path.getsize(os.path.join(r, f))
                for r, _, fs in os.walk(ROOT) for f in fs)
    print("完成：%d 个文件，共 %.1f MB" % (n, total / 1024 / 1024))


if __name__ == "__main__":
    main()
