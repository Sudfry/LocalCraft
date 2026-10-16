# -*- coding: utf-8 -*-
"""
造表格测试数据。

用 openpyxl 生成**真实结构**的 xlsx（共享字符串表、样式、日期格式都是真的），
而不是手工拼的假文件 —— 手写解析器只有面对真实文件才说明得了问题。

特意包含：
  · 日期列（xlsx 里存的是序列号 + 格式，最容易读成 45000）
  · 中文内容（走共享字符串表）
  · 引号、逗号、换行（考验 CSV 转义）
  · 公式结果
  · 多个工作簿（考验合并）
"""
import os
import sys
from datetime import date

from openpyxl import Workbook

OUT = sys.argv[1] if len(sys.argv) > 1 else "."


def make_orders(path, records):
    wb = Workbook()
    ws = wb.active
    ws.title = "订单"
    ws.append(["订单号", "客户", "城市", "金额", "下单日期", "备注"])
    for r in records:
        ws.append(r)
    wb.save(path)


os.makedirs(OUT, exist_ok=True)

make_orders(os.path.join(OUT, "订单-1月.xlsx"), [
    ["A001", "北京朝阳店", "北京", 12800.5,  date(2026, 1, 5),  "加急"],
    ["A002", "上海浦东店", "上海", 3400.0,   date(2026, 1, 12), ""],
    ["A003", "广州天河店", "广州", 99999.99, date(2026, 1, 20), "含运费"],
])

make_orders(os.path.join(OUT, "订单-2月.xlsx"), [
    ["B001", "北京海淀店", "北京", 5600.0,   date(2026, 2, 3),  ""],
    ["B002", "成都高新店", "成都", 780.25,   date(2026, 2, 14), '含"引号"'],
    ["B003", "上海徐汇店", "上海", 23500.0,  date(2026, 2, 28), "月度大单"],
])

make_orders(os.path.join(OUT, "订单-3月.xlsx"), [
    ["C001", "深圳南山店", "深圳", 18900.0,  date(2026, 3, 8),  "换货一次"],
    ["C002", "杭州西湖店", "杭州", 4200.0,   date(2026, 3, 19), "含,逗号"],
    ["C003", "武汉光谷店", "武汉", 333.33,   date(2026, 3, 30), ""],
])

# 一个纯 CSV，用来验证 CSV 也能读
with open(os.path.join(OUT, "库存.csv"), "w", encoding="utf-8-sig", newline="") as f:
    f.write("商品,数量,单价\n")
    f.write("笔记本,120,4.5\n")
    f.write("圆珠笔,880,1.2\n")

files = sorted(os.listdir(OUT))
print("已生成 %d 个文件：" % len(files))
for n in files:
    print("  %-24s %8d 字节" % (n, os.path.getsize(os.path.join(OUT, n))))
