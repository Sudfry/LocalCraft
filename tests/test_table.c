/* ==========================================================================
 * test_table.c —— 表格处理自检
 *
 * 最要紧的一点：**测试里的 xlsx 是用 zip 写出器现造的**，不是外部工具生成的。
 * 这样测试自包含（不依赖 Python、不依赖任何外部命令），而且走的是和真实文件
 * 完全一样的解析路径 —— 共享字符串表、内联字符串、日期样式、行号不连续，
 * 全都覆盖到。
 *
 * 日期是重点中的重点：xlsx 里日期根本不存在，是"数字 + 样式"。
 * 少读一天、或多减一天，出来的年份月份都对、只有日子差 1，
 * 不看具体数据根本发现不了。所以这里逐一比对到日。
 * ========================================================================== */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "lc.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (cond) { g_pass++; }                                                 \
        else {                                                                  \
            g_fail++;                                                           \
            wprintf(L"  FAIL  ");                                               \
            wprintf(__VA_ARGS__);                                               \
            wprintf(L"   [line %d]\n", __LINE__);                               \
        }                                                                       \
    } while (0)

static wchar_t g_root[MAX_PATH * 2];

/* ==================================================== 造 xlsx ========= */

/*
 * 拼一个最小但结构真实的 xlsx。
 *
 * 这里刻意包含几种真实软件会写出来的不同形式：
 *   · 共享字符串表（Excel 的做法）
 *   · 内联字符串 t="inlineStr"（openpyxl 的做法）
 *   · 数字显式带 t="n"（openpyxl 写，Excel 通常省略）
 *   · 自定义日期格式（numFmtId=164，而不是内置的 14）
 */
static int make_xlsx(const wchar_t *path, const char *sheet_xml,
                     const char *styles_xml, const char *shared_xml) {
    LcZipWriter *w = zip_writer_create();
    if (!w) return -1;

    const char *ct =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "</Types>";
    zip_writer_add(w, "[Content_Types].xml", (const unsigned char *)ct, strlen(ct));

    const char *rels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"/>";
    zip_writer_add(w, "_rels/.rels", (const unsigned char *)rels, strlen(rels));

    zip_writer_add(w, "xl/worksheets/sheet1.xml",
                   (const unsigned char *)sheet_xml, strlen(sheet_xml));
    if (styles_xml)
        zip_writer_add(w, "xl/styles.xml",
                       (const unsigned char *)styles_xml, strlen(styles_xml));
    if (shared_xml)
        zip_writer_add(w, "xl/sharedStrings.xml",
                       (const unsigned char *)shared_xml, strlen(shared_xml));

    int rc = zip_writer_save(w, path);
    zip_writer_free(w);
    return rc;
}

/* 样式表：cellXfs 的第 1 个 xf 用自定义日期格式 164 */
static const char *STYLES_XML =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
    "<numFmts count=\"1\">"
    "<numFmt numFmtId=\"164\" formatCode=\"yyyy\\-mm\\-dd\"/>"
    "</numFmts>"
    "<cellXfs count=\"2\">"
    "<xf numFmtId=\"0\" fontId=\"0\"/>"
    "<xf numFmtId=\"164\" fontId=\"0\"/>"
    "</cellXfs>"
    "</styleSheet>";

/* 共享字符串：0=姓名 1=城市 2=北京 3=上海 —— 故意乱序，验证索引解析 */
static const char *SHARED_XML =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" count=\"5\">"
    "<si><t>姓名</t></si>"
    "<si><t>城市</t></si>"
    "<si><t>北京</t></si>"
    "<si><t>上海</t></si>"
    "<si><t>张&amp;三</t></si>"     /* 实体转义 */
    "</sst>";

/* 表：A=共享字符串, B=内联字符串, C=日期(共享样式), D=数字 */
static const char *SHEET_XML =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
    "<sheetData>"
    /* 第 1 行：前两个用共享字符串，后两个用内联 */
    "<row r=\"1\">"
    "<c r=\"A1\" t=\"s\"><v>0</v></c>"
    "<c r=\"B1\" t=\"s\"><v>1</v></c>"
    "<c r=\"C1\" t=\"inlineStr\"><is><t>入职日期</t></is></c>"
    "<c r=\"D1\" t=\"inlineStr\"><is><t>工号</t></is></c>"
    "</row>"
    /* 第 2 行：日期用自定义样式 s=1，数字显式 t="n" */
    "<row r=\"2\">"
    "<c r=\"A2\" t=\"s\"><v>2</v></c>"
    "<c r=\"B2\" t=\"inlineStr\"><is><t>海淀</t></is></c>"
    "<c r=\"C2\" s=\"1\" t=\"n\"><v>46027</v></c>"
    "<c r=\"D2\" t=\"n\"><v>1001</v></c>"
    "</row>"
    /* 第 3 行 */
    "<row r=\"3\">"
    "<c r=\"A3\" t=\"s\"><v>3</v></c>"
    "<c r=\"B3\" t=\"inlineStr\"><is><t>徐汇</t></is></c>"
    "<c r=\"C3\" s=\"1\" t=\"n\"><v>46031</v></c>"
    "<c r=\"D3\" t=\"n\"><v>1002</v></c>"
    "</row>"
    /* 第 4 行：数字**不带 t 属性**（Excel 的写法），同样要走日期换算 */
    "<row r=\"4\">"
    "<c r=\"A4\" t=\"s\"><v>4</v></c>"
    "<c r=\"C4\" s=\"1\"><v>46388</v></c>"
    "<c r=\"D4\"><v>1003</v></c>"
    "</row>"
    "</sheetData>"
    "</worksheet>";

/* ==================================================== 1. 读 xlsx ======= */

static void test_read_xlsx(void) {
    wprintf(L"[1] 读 xlsx：共享字符串 / 内联字符串 / 日期 / 跳行\n");

    wchar_t p[MAX_PATH * 2];
    swprintf(p, MAX_PATH * 2, L"%ls\\t1.xlsx", g_root);
    CHECK(make_xlsx(p, SHEET_XML, STYLES_XML, SHARED_XML) == 0, L"造 xlsx 应成功");
    CHECK(lc_is_file(p), L"文件应存在");

    Table t;
    wchar_t err[512];
    err[0] = 0;
    CHECK(table_read_xlsx(p, &t, err, 512) == 0, L"读取应成功：%ls", err);

    /* 行号跳了（1,2,5,6），但表里应该还是 4 行，且顺序不乱 */
    CHECK(t.nrows == 4, L"应有 4 行，实际 %llu",
          (unsigned long long)t.nrows);
    CHECK(t.ncols == 4, L"应有 4 列，实际 %llu", (unsigned long long)t.ncols);

    /* 表头：前两列来自共享字符串，后两列来自内联字符串 */
    CHECK(wcscmp(table_get(&t, 0, 0), L"姓名") == 0,
          L"A1 应为「姓名」，实际「%ls」", table_get(&t, 0, 0));
    CHECK(wcscmp(table_get(&t, 0, 1), L"城市") == 0,
          L"B1 应为「城市」，实际「%ls」", table_get(&t, 0, 1));
    CHECK(wcscmp(table_get(&t, 0, 2), L"入职日期") == 0,
          L"C1 应为「入职日期」，实际「%ls」", table_get(&t, 0, 2));

    /* 共享字符串索引要真的按表取，而不是按出现顺序 */
    CHECK(wcscmp(table_get(&t, 1, 0), L"北京") == 0,
          L"第 2 行姓名应为「北京」，实际「%ls」", table_get(&t, 1, 0));
    CHECK(wcscmp(table_get(&t, 1, 1), L"海淀") == 0,
          L"第 2 行内联字符串应为「海淀」，实际「%ls」", table_get(&t, 1, 1));

    /*
     * 日期。序列号 46027 对应 2026-01-05。
     *
     * 这是本测试里最重要的一条断言：差一天的话年份月份都对，
     * 只有日子不对，肉眼几乎看不出来。
     */
    CHECK(wcscmp(table_get(&t, 1, 2), L"2026-01-05") == 0,
          L"46027 应换成 2026-01-05，实际「%ls」", table_get(&t, 1, 2));
    CHECK(wcscmp(table_get(&t, 2, 2), L"2026-01-09") == 0,
          L"46031 应换成 2026-01-09，实际「%ls」", table_get(&t, 2, 2));
    /* 46388 = 2027-01-01，跨年，验证闰年推进没错 */
    CHECK(wcscmp(table_get(&t, 3, 2), L"2027-01-01") == 0,
          L"46388 应换成 2027-01-01，实际「%ls」", table_get(&t, 3, 2));

    /* 数字列不该被日期化（样式是 0，不是日期） */
    CHECK(wcscmp(table_get(&t, 1, 3), L"1001") == 0,
          L"工号应是原样数字，实际「%ls」", table_get(&t, 1, 3));

    /* 第 6 行缺了 B 列，补空串而不是错位 */
    CHECK(table_get(&t, 3, 1)[0] == 0, L"缺失的单元格应为空串，而不是错位");
    CHECK(wcscmp(table_get(&t, 3, 3), L"1003") == 0,
          L"缺列后的 D 列仍应对齐，实际「%ls」", table_get(&t, 3, 3));

    table_free(&t);
}

/* ==================================================== 2. 日期边界 ===== */

static void test_date_base(void) {
    wprintf(L"[2] 日期基准：1900 年那个「假闰年」的分界\n");

    /* 序列号 1 = 1900-01-01；59 = 1900-02-28；61 = 1900-03-01 */
    static const struct { const char *v; const wchar_t *want; } CASE[] = {
        { "1",  L"1900-01-01" },
        { "59", L"1900-02-28" },
        { "61", L"1900-03-01" },
        { "45000", L"2023-03-15" },   /* 常见范围 */
        { "46027", L"2026-01-05" },
    };

    for (int i = 0; i < 5; ++i) {
        char xml[2048];
        snprintf(xml, sizeof(xml),
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
            "<sheetData><row r=\"1\">"
            "<c r=\"A1\" s=\"1\" t=\"n\"><v>%s</v></c>"
            "</row></sheetData></worksheet>", CASE[i].v);

        wchar_t p[MAX_PATH * 2];
        swprintf(p, MAX_PATH * 2, L"%ls\\d%s.xlsx", g_root,
                 (const wchar_t[]){ (wchar_t)CASE[i].v[0], 0 });
        make_xlsx(p, xml, STYLES_XML, NULL);

        Table t;
        wchar_t err[256];
        err[0] = 0;
        if (table_read_xlsx(p, &t, err, 256) != 0) {
            CHECK(0, L"序列号 %hs 读取失败：%ls", CASE[i].v, err);
            continue;
        }
        CHECK(wcscmp(table_get(&t, 0, 0), CASE[i].want) == 0,
              L"序列号 %hs 应为 %ls，实际「%ls」", CASE[i].v, CASE[i].want,
              table_get(&t, 0, 0));
        table_free(&t);
    }
}

/* ==================================================== 3. CSV 往返 ===== */

static void test_csv_roundtrip(void) {
    wprintf(L"[3] CSV：引号 / 逗号 / 换行的转义与还原\n");

    Table t;
    table_init(&t);
    table_set(&t, 0, 0, L"名称");
    table_set(&t, 0, 1, L"备注");
    table_set(&t, 1, 0, L"普通");
    table_set(&t, 1, 1, L"简单");
    table_set(&t, 2, 0, L"含,逗号");
    table_set(&t, 2, 1, L"含\"引号\"");
    table_set(&t, 3, 0, L"含\n换行");
    table_set(&t, 3, 1, L"中文内容");

    wchar_t p[MAX_PATH * 2];
    swprintf(p, MAX_PATH * 2, L"%ls\\r.csv", g_root);
    wchar_t err[512];
    err[0] = 0;
    CHECK(table_write_csv(&t, p, 1, err, 512) == 0, L"写 CSV 应成功：%ls", err);

    /* 前 3 字节必须是 UTF-8 BOM，否则中文 Excel 打开是乱码 */
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(h != INVALID_HANDLE_VALUE, L"应能打开产物");
    if (h != INVALID_HANDLE_VALUE) {
        unsigned char b[8] = {0};
        DWORD got = 0;
        ReadFile(h, b, 8, &got, NULL);
        CloseHandle(h);
        CHECK(got >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF,
              L"应写 UTF-8 BOM（%02x %02x %02x）", b[0], b[1], b[2]);
    }

    /* 读回来必须完全一致 —— 引号、逗号、换行都要能原样还原 */
    Table r;
    err[0] = 0;
    CHECK(table_read_csv(p, &r, err, 512) == 0, L"读回 CSV 应成功：%ls", err);
    CHECK(r.nrows == 4, L"应有 4 行，实际 %llu", (unsigned long long)r.nrows);
    CHECK(wcscmp(table_get(&r, 2, 0), L"含,逗号") == 0,
          L"含逗号的字段应还原，实际「%ls」", table_get(&r, 2, 0));
    CHECK(wcscmp(table_get(&r, 2, 1), L"含\"引号\"") == 0,
          L"含引号的字段应还原，实际「%ls」", table_get(&r, 2, 1));
    CHECK(wcscmp(table_get(&r, 3, 0), L"含\n换行") == 0,
          L"含换行的字段应还原，实际「%ls」", table_get(&r, 3, 0));
    CHECK(wcscmp(table_get(&r, 3, 1), L"中文内容") == 0, L"中文应还原");

    table_free(&r);
    table_free(&t);
}

/* ==================================================== 4. 筛选 / 排序 == */

static void test_filter_sort(void) {
    wprintf(L"[4] 筛选与排序\n");

    /* 造 6 行数据（含表头） */
    Table t;
    table_init(&t);
    const wchar_t *ROWS[][3] = {
        { L"城市",  L"金额", L"备注" },
        { L"北京",  L"300",  L"甲" },
        { L"上海",  L"100",  L"乙" },
        { L"北京",  L"200",  L"丙" },
        { L"广州",  L"500",  L"丁" },
        { L"上海",  L"400",  L"戊" },
    };
    for (int r = 0; r < 6; ++r)
        for (int c = 0; c < 3; ++c) table_set(&t, (size_t)r, (size_t)c, ROWS[r][c]);

    /* 筛选：只看北京 */
    Table f;
    table_init(&f);
    for (size_t r = 0; r < t.nrows; ++r)
        for (size_t c = 0; c < t.ncols; ++c)
            table_set(&f, r, c, table_get(&t, r, c));

    CHECK(table_filter(&f, 0, L"contains", L"北京", 1) == 0, L"筛选应成功");
    CHECK(f.nrows == 3, L"北京应有 2 条 + 表头，实际 %llu",
          (unsigned long long)f.nrows);
    CHECK(wcscmp(table_get(&f, 0, 0), L"城市") == 0, L"表头应保留");
    CHECK(wcscmp(table_get(&f, 1, 0), L"北京") == 0, L"第 1 条应是北京");
    CHECK(wcscmp(table_get(&f, 2, 0), L"北京") == 0, L"第 2 条应是北京");
    /* 原来的 A 条与 C 条都还在，不能重复也不能丢 */
    CHECK(wcscmp(table_get(&f, 1, 2), L"甲") == 0,
          L"第 1 条应是「甲」，实际「%ls」", table_get(&f, 1, 2));
    CHECK(wcscmp(table_get(&f, 2, 2), L"丙") == 0,
          L"第 2 条应是「丙」，实际「%ls」", table_get(&f, 2, 2));
    table_free(&f);

    /* 数值升序 */
    Table s;
    table_init(&s);
    for (size_t r = 0; r < t.nrows; ++r)
        for (size_t c = 0; c < t.ncols; ++c)
            table_set(&s, r, c, table_get(&t, r, c));
    CHECK(table_sort(&s, 1, 1, 0, 1) == 0, L"排序应成功");
    CHECK(wcscmp(table_get(&s, 0, 1), L"金额") == 0, L"表头应留在最前");
    CHECK(wcscmp(table_get(&s, 1, 1), L"100") == 0,
          L"升序第 1 个应是 100，实际「%ls」", table_get(&s, 1, 1));
    CHECK(wcscmp(table_get(&s, 5, 1), L"500") == 0,
          L"升序最后应是 500，实际「%ls」", table_get(&s, 5, 1));

    /* 数值降序 */
    CHECK(table_sort(&s, 1, 1, 1, 1) == 0, L"降序应成功");
    CHECK(wcscmp(table_get(&s, 1, 1), L"500") == 0,
          L"降序第 1 个应是 500，实际「%ls」", table_get(&s, 1, 1));
    table_free(&s);

    /* 选列：只留第 2、0 列（顺序也要跟着走） */
    Table k;
    table_init(&k);
    for (size_t r = 0; r < t.nrows; ++r)
        for (size_t c = 0; c < t.ncols; ++c)
            table_set(&k, r, c, table_get(&t, r, c));
    int cols[2] = { 2, 0 };
    CHECK(table_select_cols(&k, cols, 2) == 0, L"选列应成功");
    CHECK(k.ncols == 2, L"应只剩 2 列，实际 %llu", (unsigned long long)k.ncols);
    CHECK(wcscmp(table_get(&k, 0, 0), L"备注") == 0,
          L"第 1 列应是备注，实际「%ls」", table_get(&k, 0, 0));
    CHECK(wcscmp(table_get(&k, 0, 1), L"城市") == 0,
          L"第 2 列应是城市，实际「%ls」", table_get(&k, 0, 1));
    table_free(&k);

    table_free(&t);
}

/* ==================================================== 5. 合并 ========= */

static void test_append(void) {
    wprintf(L"[5] 合并：表头只留一份\n");

    Table a, b;
    table_init(&a);
    table_init(&b);

    table_set(&a, 0, 0, L"编号");
    table_set(&a, 0, 1, L"名称");
    table_set(&a, 1, 0, L"1");
    table_set(&a, 1, 1, L"甲");

    table_set(&b, 0, 0, L"编号");     /* b 的表头不该重复进来 */
    table_set(&b, 0, 1, L"名称");
    table_set(&b, 1, 0, L"2");
    table_set(&b, 1, 1, L"乙");

    CHECK(table_append(&a, &b, 1, 1) == 0, L"追加应成功");
    CHECK(a.nrows == 3, L"合并后应有 3 行（1 表头 + 2 数据），实际 %llu",
          (unsigned long long)a.nrows);
    CHECK(wcscmp(table_get(&a, 2, 1), L"乙") == 0,
          L"第 3 行应是「乙」，实际「%ls」", table_get(&a, 2, 1));

    table_free(&a);
    table_free(&b);
}

/* ==================================================== main =========== */

int main(void) {
    SetConsoleOutputCP(CP_UTF8);

    wchar_t exe[MAX_PATH * 2], *slash;
    GetModuleFileNameW(NULL, exe, MAX_PATH * 2);
    slash = wcsrchr(exe, L'\\'); if (slash) *slash = 0;
    slash = wcsrchr(exe, L'\\'); if (slash) *slash = 0;

    swprintf(g_root, MAX_PATH * 2, L"%ls\\testtmp\\table-%u", exe,
             (unsigned)GetTickCount64());
    lc_mkdir_p(g_root);

    wprintf(L"=== LocalCraft 表格处理自检 ===\n");
    wprintf(L"工作目录：%ls\n\n", g_root);

    test_read_xlsx();
    test_date_base();
    test_csv_roundtrip();
    test_filter_sort();
    test_append();

    wprintf(L"\n=== 通过 %d / 失败 %d ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
