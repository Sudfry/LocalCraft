/* ==========================================================================
 * test_export.c —— 模块 5（导出）与模块 2（格式转换）自检
 *
 * 重点验证中文场景下真正会出问题的地方：
 *   · docx 的标题层级（Heading 样式 + outlineLvl）在不在
 *   · 每个 run 有没有声明东亚字体（否则 Word 里中文会变方块/忽大忽小）
 *   · 上下标、粗斜体有没有写进去
 *   · 导出的 docx 能不能被自己的抽取器读回来（闭环验证）
 *   · 导出产物是否真的进了回退体系（执行 → 回退 → 产物消失）
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
static wchar_t g_home[MAX_PATH * 2];


static void write_u8(const wchar_t *path, const char *data, size_t len) {
    wchar_t *lp = lc_long_path(path);
    HANDLE h = CreateFileW(lp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, data, (DWORD)len, &w, NULL);
    CloseHandle(h);
}

static char *read_u8(const wchar_t *path, size_t *len_out) {
    wchar_t *lp = lc_long_path(path);
    HANDLE h = CreateFileW(lp, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (200 << 20)) {
        CloseHandle(h);
        return NULL;
    }
    size_t n = (size_t)sz.QuadPart;
    char *buf = (char *)malloc(n + 1);
    if (!buf) { CloseHandle(h); return NULL; }
    DWORD got = 0;
    if (!ReadFile(h, buf, (DWORD)n, &got, NULL)) got = 0;
    CloseHandle(h);
    buf[got] = 0;
    if (len_out) *len_out = got;
    return buf;
}

/* 在字节缓冲里找子串 */
static int contains(const char *hay, size_t hlen, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0 || nl > hlen) return 0;
    for (size_t i = 0; i + nl <= hlen; ++i)
        if (memcmp(hay + i, needle, nl) == 0) return 1;
    return 0;
}

/* ==================================================== 测试用 Markdown == */

static const wchar_t *SAMPLE_MD =
L"# 本地文件处理工具设计说明\n"
L"\n"
L"这是一段普通正文，用来验证**粗体**、*斜体*、`行内代码` 与 ~~删除线~~ 是否保留。\n"
L"\n"
L"## 一、总体架构\n"
L"\n"
L"程序分为四层：界面层、服务层、核心层与工具层。\n"
L"\n"
L"### 1.1 核心层\n"
L"\n"
L"- 计划构建：负责冲突分析\n"
L"- 执行器：负责逐步落日志\n"
L"- 回退引擎：负责逆序取反\n"
L"\n"
L"#### 1.1.1 更深的层级\n"
L"\n"
L"深度到四级标题，验证 Heading 4 也能正确生成。\n"
L"\n"
L"##### 1.1.1.1 第五级\n"
L"\n"
L"###### 1.1.1.1.1 第六级\n"
L"\n"
L"## 二、数学与上下标\n"
L"\n"
L"水的化学式是 H~2~O，质能方程是 E=mc^2^，也可以用 <sub>下标</sub> 和 <sup>上标</sup> 写。\n"
L"\n"
L"1. 第一步：选择文件夹\n"
L"2. 第二步：设置规则\n"
L"3. 第三步：预览并执行\n"
L"\n"
L"> 引用：所有操作都必须可回退。\n"
L"\n"
L"---\n"
L"\n"
L"```\n"
L"localcraft --headless --port 8080\n"
L"```\n";

/* ==================================================== 导出测试 ========= */

static void test_export_markdown(void) {
    wprintf(L"[1] 导出 Markdown\n");
    wchar_t out[MAX_PATH * 2];
    swprintf(out, MAX_PATH * 2, L"%ls\\out.md", g_root);

    wchar_t err[512];
    err[0] = 0;
    int rc = export_markdown_file(SAMPLE_MD, out, err, 512);
    CHECK(rc == 0, L"导出应成功：%ls", err);
    CHECK(lc_is_file(out), L"应生成 .md 文件");

    size_t len = 0;
    char *data = read_u8(out, &len);
    CHECK(data != NULL, L"应能读回文件");
    if (data) {
        /* 标题层级必须原样保留 */
        CHECK(contains(data, len, "# \xE6\x9C\xAC\xE5\x9C\xB0\xE6\x96\x87\xE4\xBB\xB6"),
              L"一级标题应原样保留");
        CHECK(contains(data, len, "### 1.1"), L"三级标题应原样保留");
        CHECK(contains(data, len, "###### 1.1.1.1.1"), L"六级标题应原样保留");
        CHECK(contains(data, len, "**"), L"粗体标记应保留");
        CHECK(contains(data, len, "H~2~O"), L"下标标记应保留");
        free(data);
    }
}

static void test_export_docx(void) {
    wprintf(L"[2] 导出 Word（中文标题层级是重点）\n");
    wchar_t out[MAX_PATH * 2];
    swprintf(out, MAX_PATH * 2, L"%ls\\out.docx", g_root);

    wchar_t err[512];
    err[0] = 0;
    int rc = export_docx_file(SAMPLE_MD, out, err, 512);
    CHECK(rc == 0, L"导出 docx 应成功：%ls", err);
    CHECK(lc_is_file(out), L"应生成 .docx 文件");

    /* docx 必须是合法 zip，且包含关键部件 */
    LcZip *z = zip_open(out);
    CHECK(z != NULL, L"docx 应当是一个合法的 zip 包");
    if (!z) return;

    unsigned char *doc = NULL;
    size_t doclen = 0;
    CHECK(zip_read_by_name(z, "word/document.xml", &doc, &doclen) == 0,
          L"应包含 word/document.xml");
    CHECK(zip_read_by_name(z, "[Content_Types].xml", &doc, &doclen) == 0,
          L"应包含 [Content_Types].xml");

    unsigned char *styles = NULL;
    size_t slen = 0;
    CHECK(zip_read_by_name(z, "word/styles.xml", &styles, &slen) == 0,
          L"应包含 word/styles.xml");
    if (styles) {
        const char *s = (const char *)styles;
        /* 关键是 outlineLvl：没有它，Word 导航窗格和目录认不出层级 */
        CHECK(contains(s, slen, "w:outlineLvl"), L"标题样式必须带 outlineLvl");
        CHECK(contains(s, slen, "w:styleId=\"Heading1\""), L"应有 Heading1 样式");
        CHECK(contains(s, slen, "w:styleId=\"Heading6\""), L"应有 Heading6 样式");
        CHECK(contains(s, slen, "w:eastAsia="), L"必须声明东亚字体，否则中文会变方块");
        free(styles);
    }

    /* 正文里应出现各级标题引用与上下标 */
    if (doc) {
        CHECK(zip_read_by_name(z, "word/document.xml", &doc, &doclen) == 0,
              L"重读 document.xml");
        const char *s = (const char *)doc;
        CHECK(contains(s, doclen, "w:pStyle w:val=\"Heading1\""),
              L"正文里应使用 Heading1");
        CHECK(contains(s, doclen, "w:pStyle w:val=\"Heading4\""),
              L"四级标题应映射到 Heading4");
        CHECK(contains(s, doclen, "w:pStyle w:val=\"Heading6\""),
              L"六级标题应映射到 Heading6");
        CHECK(contains(s, doclen, "w:vertAlign w:val=\"subscript\""),
              L"下标应映射到 vertAlign=subscript");
        CHECK(contains(s, doclen, "w:vertAlign w:val=\"superscript\""),
              L"上标应映射到 vertAlign=superscript");
        CHECK(contains(s, doclen, "w:b/"), L"粗体应写入");
        CHECK(contains(s, doclen, "w:i/"), L"斜体应写入");
        CHECK(contains(s, doclen, "w:strike/"), L"删除线应写入");
        CHECK(contains(s, doclen, "w:eastAsia="), L"每个 run 都要带东亚字体");
        CHECK(contains(s, doclen, "F2F3F5"), L"行内代码应有底纹");
        free(doc);
    }
    zip_close(z);

    /* 闭环：用自己的抽取器把 docx 读回来，中文必须一字不差 */
    {
        wchar_t e[512];
        e[0] = 0;
        wchar_t *text = extract_text(out, 1u << 20, e, 512);
        CHECK(text != NULL, L"应能把刚导出的 docx 读回来：%ls", e);
        if (text) {
            CHECK(wcsstr(text, L"本地文件处理工具设计说明") != NULL,
                  L"读回的中文标题应完全一致");
            CHECK(wcsstr(text, L"总体架构") != NULL, L"二级标题文字应读得到");
            CHECK(wcsstr(text, L"水的化学式是 H2O") != NULL ||
                  wcsstr(text, L"H2O") != NULL,
                  L"下标内容应保留为 H2O");
            CHECK(wcsstr(text, L"E=mc2") != NULL, L"上标内容应保留为 mc2");
            wprintf(L"      读回正文片段：%.60ls…\n", text);
            free(text);
        }
    }
}

static void test_export_pdf(void) {
    wprintf(L"[3] 导出 PDF\n");
    wchar_t out[MAX_PATH * 2];
    swprintf(out, MAX_PATH * 2, L"%ls\\out.pdf", g_root);

    wchar_t err[512];
    err[0] = 0;
    int rc = export_pdf_file(SAMPLE_MD, out, err, 512);
    CHECK(rc == 0, L"导出 PDF 应成功：%ls", err);

    size_t len = 0;
    char *data = read_u8(out, &len);
    CHECK(data != NULL && len > 400, L"PDF 应有实际内容");
    if (data) {
        CHECK(memcmp(data, "%PDF-1.4", 8) == 0, L"应以内嵌 %PDF 头开始");
        CHECK(contains(data, len, "%%EOF"), L"应有 %%EOF 结束标记");
        CHECK(contains(data, len, "/Type /Catalog"), L"应有 Catalog");
        CHECK(contains(data, len, "/Type /Pages"), L"应有 Pages");
        CHECK(contains(data, len, "UniGB-UCS2-H"),
              L"中文必须用 UniGB-UCS2-H 编码，否则是乱码");
        CHECK(contains(data, len, "STSong-Light"), L"应引用标准中文宋体");
        CHECK(contains(data, len, "xref"), L"应有交叉引用表");
        CHECK(contains(data, len, "startxref"), L"应有 startxref");

        /* 中文应当以 UCS-2 大端十六进制写进内容流：
            "本" = U+672C，"地" = U+5730 */
        CHECK(contains(data, len, "672C") || contains(data, len, "672c"),
              L"中文应按 UCS-2 十六进制写入");
        free(data);
    }
}

/* ==================================================== 转换测试 ========= */

static void test_convert_text(void) {
    wprintf(L"[4] 格式转换（内置）\n");

    /* Markdown -> 纯文本：标记应被剥掉 */
    wchar_t src[MAX_PATH * 2], dst[MAX_PATH * 2];
    swprintf(src, MAX_PATH * 2, L"%ls\\note.md", g_root);
    swprintf(dst, MAX_PATH * 2, L"%ls\\note.txt", g_root);
    write_u8(src, "# \xE6\xA0\x87\xE9\xA2\x98\n\n\xE6\xAD\xA3\xE6\x96\x87**\xE5\x8A\xA0\xE7\xB2\x97**\xE5\x86\x85\xE5\xAE\xB9\n", 40);

    CHECK(convert_builtin_supported(src, dst), L"md -> txt 应被判定为内置支持");
    wchar_t err[512];
    err[0] = 0;
    CHECK(lc_generate_artifact(L"convert:txt", src, dst, err, 512) == 0,
          L"md -> txt 应成功：%ls", err);
    CHECK(lc_is_file(dst), L"应生成 txt");

    {
        size_t len = 0;
        char *d = read_u8(dst, &len);
        if (d) {
            CHECK(!contains(d, len, "#"), L"转成纯文本后不应再有 # 标记");
            CHECK(!contains(d, len, "**"), L"转成纯文本后不应再有 ** 标记");
            CHECK(contains(d, len, "\xE6\xA0\x87\xE9\xA2\x98"), L"中文文字应保留");
            free(d);
        }
    }

    /* Markdown -> HTML：应有正确的结构与 charset */
    wchar_t html[MAX_PATH * 2];
    swprintf(html, MAX_PATH * 2, L"%ls\\note.html", g_root);
    err[0] = 0;
    CHECK(lc_generate_artifact(L"convert:html", src, html, err, 512) == 0,
          L"md -> html 应成功：%ls", err);
    {
        size_t len = 0;
        char *d = read_u8(html, &len);
        if (d) {
            CHECK(contains(d, len, "charset=\"utf-8\""), L"HTML 应声明 UTF-8");
            CHECK(contains(d, len, "<h1>"), L"应生成 h1");
            CHECK(contains(d, len, "</html>"), L"HTML 应完整闭合");
            free(d);
        }
    }

    /* txt -> md */
    wchar_t t2[MAX_PATH * 2], m2[MAX_PATH * 2];
    swprintf(t2, MAX_PATH * 2, L"%ls\\plain.txt", g_root);
    swprintf(m2, MAX_PATH * 2, L"%ls\\plain.md", g_root);
    write_u8(t2, "\xE7\xAC\xAC\xE4\xB8\x80\xE6\xAE\xB5\n\xE7\xAC\xAC\xE4\xBA\x8C\xE6\xAE\xB5\n", 13);
    err[0] = 0;
    CHECK(lc_generate_artifact(L"convert:md", t2, m2, err, 512) == 0,
          L"txt -> md 应成功：%ls", err);
    CHECK(lc_is_file(m2), L"应生成 md");
}

/* 构造一个最小 BMP（4x3 红色），测 GDI+ 图片互转 */
static void make_bmp(const wchar_t *path) {
    const int W = 4, H = 3;
    const int row = W * 3;                 /* 每行字节数，已是 4 的倍数 */
    const int datasz = row * H;
    const int filesz = 54 + datasz;
    unsigned char *b = (unsigned char *)calloc(1, filesz);
    if (!b) return;
    b[0] = 'B'; b[1] = 'M';
    b[2] = (unsigned char)(filesz); b[3] = (unsigned char)(filesz >> 8);
    b[10] = 54;                            /* 数据偏移 */
    b[14] = 40;                            /* BITMAPINFOHEADER */
    b[18] = (unsigned char)W; b[22] = (unsigned char)H;
    b[26] = 1;                             /* planes */
    b[28] = 24;                            /* 位深 */
    b[34] = (unsigned char)datasz;
    for (int i = 0; i < datasz; i += 3) { b[54+i] = 0; b[54+i+1] = 0; b[54+i+2] = 255; }
    write_u8(path, (const char *)b, (size_t)filesz);
    free(b);
}

/*
 * 转换可行性判断。
 *
 * 这条测试的来历：界面上选"转成 PNG"时，同一文件夹里的 .txt 也被列进了
 * 预览表，还标成"正常、可执行"。原因是 api.c 里算出了判断结果却没有用它：
 *     if (!convert_builtin_supported(src, dst)) { }   // 花括号是空的
 * 用测试数据一跑就露出来了。这里把各种格式组合钉死，防止再退化。
 */
static void test_convert_feasibility(void) {
    wprintf(L"[6] 转换可行性判断（哪些组合根本做不到）\n");

    /* 内置就能做：图片互转 */
    CHECK(convert_feasibility(L"a.bmp", L"b.png") == 2, L"bmp -> png 应判为内置可完成");
    CHECK(convert_feasibility(L"a.jpg", L"b.gif") == 2, L"jpg -> gif 应判为内置可完成");
    /* 内置就能做：文档抽成文本类 */
    CHECK(convert_feasibility(L"a.docx", L"b.md") == 2, L"docx -> md 应判为内置可完成");
    CHECK(convert_feasibility(L"a.pptx", L"b.txt") == 2, L"pptx -> txt 应判为内置可完成");

    /* 内置做不了但引擎可能行 */
    CHECK(convert_feasibility(L"a.docx", L"b.pdf") == 1, L"docx -> pdf 应判为需要外部引擎");
    CHECK(convert_feasibility(L"a.md", L"b.docx") == 1, L"md -> docx 应判为需要外部引擎");

    /* 根本做不到 —— 这几条正是之前放过去的 */
    CHECK(convert_feasibility(L"a.txt", L"b.png") == 0, L"txt -> png 必须判为做不到");
    CHECK(convert_feasibility(L"a.md", L"b.png") == 0, L"md -> png 必须判为做不到");
    CHECK(convert_feasibility(L"a.txt", L"b.jpg") == 0, L"txt -> jpg 必须判为做不到");
    CHECK(convert_feasibility(L"a.docx", L"b.png") == 0, L"docx -> png 必须判为做不到");
    /* 图片 -> 文本需要 OCR，本项目不做 */
    CHECK(convert_feasibility(L"a.png", L"b.txt") == 0, L"png -> txt 需要 OCR，应判为做不到");
    CHECK(convert_feasibility(L"a.jpg", L"b.md") == 0, L"jpg -> md 需要 OCR，应判为做不到");
}

static void test_convert_image(void) {
    wprintf(L"[5] 图片转换（GDI+，无需任何外部组件）\n");
    wchar_t bmp[MAX_PATH * 2], png[MAX_PATH * 2], jpg[MAX_PATH * 2];
    swprintf(bmp, MAX_PATH * 2, L"%ls\\pic.bmp", g_root);
    swprintf(png, MAX_PATH * 2, L"%ls\\pic.png", g_root);
    swprintf(jpg, MAX_PATH * 2, L"%ls\\pic.jpg", g_root);

    make_bmp(bmp);
    CHECK(lc_is_file(bmp), L"测试用 BMP 应生成成功");
    CHECK(convert_builtin_supported(bmp, png), L"bmp -> png 应被判定为内置支持");

    wchar_t err[512];
    err[0] = 0;
    int rc = lc_generate_artifact(L"convert:png", bmp, png, err, 512);
    CHECK(rc == 0, L"bmp -> png 应成功：%ls", err);
    CHECK(lc_is_file(png), L"应生成 png");

    size_t len = 0;
    char *d = read_u8(png, &len);
    if (d) {
        static const unsigned char PNG_SIG[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
        CHECK(len > 8 && memcmp(d, PNG_SIG, 8) == 0, L"PNG 文件头应正确");
        free(d);
    }

    err[0] = 0;
    rc = lc_generate_artifact(L"convert:jpg", bmp, jpg, err, 512);
    CHECK(rc == 0, L"bmp -> jpg 应成功：%ls", err);
    d = read_u8(jpg, &len);
    if (d) {
        CHECK(len > 3 && (unsigned char)d[0] == 0xFF && (unsigned char)d[1] == 0xD8,
              L"JPEG 应以 FFD8 开始");
        free(d);
    }

    /* 反向：png -> bmp */
    wchar_t back[MAX_PATH * 2];
    swprintf(back, MAX_PATH * 2, L"%ls\\back.bmp", g_root);
    err[0] = 0;
    rc = lc_generate_artifact(L"convert:bmp", png, back, err, 512);
    CHECK(rc == 0, L"png -> bmp 应成功：%ls", err);
    d = read_u8(back, &len);
    if (d) {
        CHECK(len > 2 && d[0] == 'B' && d[1] == 'M', L"BMP 文件头应正确");
        free(d);
    }
}

/* ==================================================== 可回退性 ========= */

static void test_export_is_reversible(void) {
    wprintf(L"[6] 导出产物进回退体系\n");

    wchar_t src[MAX_PATH * 2];
    swprintf(src, MAX_PATH * 2, L"%ls\\rev.md", g_root);
    write_u8(src, "# \xE5\x9B\x9E\xE9\x80\x80\xE6\xB5\x8B\xE8\xAF\x95\n\n\xE6\xAD\xA3\xE6\x96\x87\n", 20);

    wchar_t target[MAX_PATH * 2];
    swprintf(target, MAX_PATH * 2, L"%ls\\rev.docx", g_root);

    Arena *a = arena_new();
    Plan *plan = (Plan *)arena_zalloc(a, sizeof(Plan));
    plan_init(plan, a, g_root, POLICY_RENAME, L"export", L"导出测试");

    Action *act = plan_add(plan, L"export", src, target, L"导出为 Word");
    CHECK(act != NULL, L"应能加入导出动作");
    if (act) {
        act->produce_only = 1;
        act->generator = arena_wcsdup(a, L"export:docx");
    }
    plan_finalize(plan, 1, 1);

    CHECK(plan->st_executable == 1, L"导出动作应可执行，实际 %llu",
          (unsigned long long)plan->st_executable);
    CHECK(!lc_is_file(target), L"预览阶段不应生成任何文件");

    ExecOptions opt;
    memset(&opt, 0, sizeof(opt));
    RunResult r;
    executor_run(plan, &opt, &r);
    CHECK(r.ok == 1 && r.failed == 0, L"执行应成功（ok=%d failed=%d）", r.ok, r.failed);
    CHECK(lc_is_file(target), L"执行后应生成 docx");
    CHECK(lc_is_file(src), L"源文件必须原样保留（导出不是移动）");

    wchar_t jp[MAX_PATH * 2];
    wcsncpy(jp, r.journal_path, MAX_PATH * 2 - 1);
    run_result_free(&r);

    ExecOptions o2;
    memset(&o2, 0, sizeof(o2));
    RunResult rb;
    rollback_apply(jp, &o2, &rb);
    CHECK(rb.failed == 0, L"导出回退应成功（failed=%d）", rb.failed);
    CHECK(!lc_is_file(target), L"回退后产物应被移走");
    CHECK(lc_is_file(src), L"回退不应动到源文件");

    run_result_free(&rb);
    arena_free(a);
}

/* ==================================================== main ============= */

int main(void) {
    SetConsoleOutputCP(CP_UTF8);

    wchar_t exe[MAX_PATH * 2], *slash;
    GetModuleFileNameW(NULL, exe, MAX_PATH * 2);
    slash = wcsrchr(exe, L'\\'); if (slash) *slash = 0;
    slash = wcsrchr(exe, L'\\'); if (slash) *slash = 0;

    swprintf(g_home, MAX_PATH * 2, L"%ls\\testtmp\\export", exe);
    lc_mkdir_p(g_home);
    SetEnvironmentVariableW(L"LOCALCRAFT_HOME", g_home);

    swprintf(g_root, MAX_PATH * 2, L"%ls\\work-%u", g_home, (unsigned)GetTickCount());
    lc_mkdir_p(g_root);

    wprintf(L"=== LocalCraft 导出与转换自检 ===\n");
    wprintf(L"工作目录：%ls\n\n", g_root);

    test_export_markdown();
    test_export_docx();
    test_export_pdf();
    test_convert_text();
    test_convert_image();
    test_convert_feasibility();
    test_export_is_reversible();

    wprintf(L"\n=== 通过 %d / 失败 %d ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
