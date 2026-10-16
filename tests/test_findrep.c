/* ==========================================================================
 * test_findrep.c —— 文本内容批量查找替换自检
 *
 * 这个模块是**第一个改文件内容**的，风险比改文件名高一个量级：
 *   · 编码搞错 → 用户的 GBK 文档变成满屏乱码
 *   · 二进制防不住 → exe / jpg 被当成文本"替换"几个字节就损坏
 *   · 回退不到位 → 原内容永远找不回来
 *
 * 所以这里重点盯三件事：**编码保持、二进制防护、字节级回退**。
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

/* 编码常量与 findrep.c 里保持一致 */
enum { ENC_UTF8 = 0, ENC_UTF8_BOM, ENC_UTF16LE, ENC_UTF16BE, ENC_GBK };

static wchar_t g_root[MAX_PATH * 2];
static wchar_t g_home[MAX_PATH * 2];

/* ------------------------------------------------------- 文件读写 ---- */

static void write_bytes(const wchar_t *path, const void *data, size_t len) {
    wchar_t *lp = lc_long_path(path);
    HANDLE h = CreateFileW(lp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, data, (DWORD)len, &w, NULL);
    CloseHandle(h);
}

static unsigned char *read_bytes(const wchar_t *path, size_t *len_out) {
    *len_out = 0;
    wchar_t *lp = lc_long_path(path);
    HANDLE h = CreateFileW(lp, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0) { CloseHandle(h); return NULL; }
    size_t n = (size_t)sz.QuadPart;
    unsigned char *b = (unsigned char *)malloc(n + 1);
    if (!b) { CloseHandle(h); return NULL; }
    size_t got = 0;
    DWORD r = 0;
    if (n) ReadFile(h, b, (DWORD)n, &r, NULL);
    got = r;
    CloseHandle(h);
    b[got] = 0;
    *len_out = got;
    return b;
}

/* 把宽字符串按指定编码写成文件 */
static void write_enc(const wchar_t *path, const wchar_t *text, int enc) {
    UINT cp = (enc == ENC_GBK) ? 936u : CP_UTF8;
    int n = WideCharToMultiByte(cp, 0, text, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return;
    size_t body = (size_t)n - 1;
    size_t bom = (enc == ENC_UTF8_BOM) ? 3 : 0;
    unsigned char *b = (unsigned char *)malloc(body + bom + 1);
    if (!b) return;
    if (bom) { b[0] = 0xEF; b[1] = 0xBB; b[2] = 0xBF; }
    WideCharToMultiByte(cp, 0, text, -1, (char *)(b + bom), n, NULL, NULL);
    write_bytes(path, b, body + bom);
    free(b);
}


/* ------------------------------------------------------- 辅助 -------- */

/* 只建目录。main 里给的是本次运行独有的目录名（带 tick），所以不用清理旧内容。 */
static void reset_dir(const wchar_t *dir) { lc_mkdir_p(dir); }


/* ============================================== 1. 编码识别 ========== */

static void test_detect_encoding(void) {
    wprintf(L"[1] 编码识别与保持\n");

    wchar_t d[MAX_PATH * 2];
    swprintf(d, MAX_PATH * 2, L"%ls\\enc", g_root);
    reset_dir(d);

    const wchar_t *body = L"课程名称：高等数学\n课程名称：线性代数\n";

    wchar_t pu[MAX_PATH * 2], pg[MAX_PATH * 2], pb[MAX_PATH * 2];
    swprintf(pu, MAX_PATH * 2, L"%ls\\u8.txt", d);
    swprintf(pg, MAX_PATH * 2, L"%ls\\gbk.txt", d);
    swprintf(pb, MAX_PATH * 2, L"%ls\\bom.txt", d);
    write_enc(pu, body, ENC_UTF8);
    write_enc(pg, body, ENC_GBK);
    write_enc(pb, body, ENC_UTF8_BOM);

    FindRepOpts o;
    memset(&o, 0, sizeof(o));
    o.find = L"课程名称";
    o.replace = L"课程";
    o.recursive = 1;
    o.max_bytes = 64ll << 20;

    FindRepResult res;
    wchar_t err[512];
    err[0] = 0;
    int rc = findrep_scan(d, &o, &res, err, 512);
    CHECK(rc == 0, L"扫描应成功：%ls", err);
    CHECK(res.n_files == 3, L"应命中 3 个文件，实际 %llu",
          (unsigned long long)res.n_files);
    CHECK(res.total_matches == 6, L"应共 6 处匹配，实际 %lld", res.total_matches);

    int saw_utf8 = 0, saw_bom = 0, saw_gbk = 0;
    for (size_t i = 0; i < res.n_files; ++i) {
        const wchar_t *base = lc_basename(res.files[i].path);
        if (wcsstr(base, L"u8.txt"))  { saw_utf8 = res.files[i].encoding == ENC_UTF8;
                                        CHECK(res.files[i].matches == 2,
                                              L"u8.txt 应有 2 处匹配，实际 %d",
                                              res.files[i].matches); }
        if (wcsstr(base, L"bom.txt")) { saw_bom  = res.files[i].encoding == ENC_UTF8_BOM;
                                        CHECK(res.files[i].encoding == ENC_UTF8_BOM,
                                              L"带 BOM 的文件应识别为 ENC_UTF8_BOM，实际 %d",
                                              res.files[i].encoding); }
        if (wcsstr(base, L"gbk.txt")) { saw_gbk  = res.files[i].encoding == ENC_GBK;
                                        CHECK(res.files[i].encoding == ENC_GBK,
                                              L"GBK 文件应识别为 ENC_GBK，实际 %d",
                                              res.files[i].encoding); }
    }
    CHECK(saw_utf8, L"应识别出 UTF-8 文件");
    CHECK(saw_bom,  L"应识别出带 BOM 的 UTF-8 文件");
    CHECK(saw_gbk,  L"应识别出 GBK 文件");

    /*
     * 样例必须只包含命中的那一行。
     *
     * 判据是"里面不含换行" —— 这比"不含第 2 行的文字"准确：
     * 第 2 行的样例本来就该包含第 2 行的内容，用后者会把正常的也判成错。
     */
    int sample_ok = 1;
    for (size_t i = 0; i < res.n_hits; ++i) {
        const wchar_t *af = res.hits[i].after;
        if (!af) continue;
        if (wcslen(af) > 120) sample_ok = 0;                       /* 不该很长 */
        if (wcschr(af, L'\n') || wcschr(af, L'\r')) sample_ok = 0;  /* 不该跨行 */ 
    }
    CHECK(sample_ok, L"样例只应包含命中的那一行，不应带上后续内容");

    /* 再单独确认：第 1 行的样例里不该出现只有第 2 行才有的文字 */
    for (size_t i = 0; i < res.n_hits; ++i) {
        if (res.hits[i].line == 1 && res.hits[i].after) {
            CHECK(wcsstr(res.hits[i].after, L"线性代数") == NULL,
                  L"第 1 行的样例不应带上第 2 行的内容");
        }
    }

    findrep_free(&res);
}

/* ============================================== 2. 二进制防护 ======= */

static void test_binary_guard(void) {
    wprintf(L"[2] 二进制防护与扩展名过滤\n");

    wchar_t d[MAX_PATH * 2];
    swprintf(d, MAX_PATH * 2, L"%ls\\bin", g_root);
    reset_dir(d);

    /* 一个含 NUL 的 .txt —— 名字像文本，内容不是 */
    wchar_t pt[MAX_PATH * 2];
    swprintf(pt, MAX_PATH * 2, L"%ls\\fake.txt", d);
    unsigned char buf[512];
    for (size_t i = 0; i < sizeof(buf); ++i) buf[i] = (i % 5 == 0) ? 0 : (unsigned char)('A' + i % 26);
    write_bytes(pt, buf, sizeof(buf));

    /* 一个正常文本，用来确认扫描本身是通的 */
    wchar_t pg[MAX_PATH * 2];
    swprintf(pg, MAX_PATH * 2, L"%ls\\real.txt", d);
    write_enc(pg, L"课程名称：正常文件\n", ENC_UTF8);

    /* 一个非文本扩展名 */
    wchar_t pj[MAX_PATH * 2];
    swprintf(pj, MAX_PATH * 2, L"%ls\\note.numbers", d);
    write_enc(pj, L"课程名称：扩展名不在名单里\n", ENC_UTF8);

    FindRepOpts o;
    memset(&o, 0, sizeof(o));
    o.find = L"课程名称";
    o.replace = L"X";
    o.recursive = 1;
    o.max_bytes = 64ll << 20;

    FindRepResult res;
    wchar_t err[512];
    err[0] = 0;
    CHECK(findrep_scan(d, &o, &res, err, 512) == 0, L"扫描应成功");
    CHECK(res.skipped_binary == 1,
          L"含 NUL 的 .txt 应被判定为二进制并跳过，实际跳过 %lld", res.skipped_binary);
    CHECK(res.skipped_ext == 1,
          L"扩展名不在文本名单里的应被跳过，实际 %lld", res.skipped_ext);
    CHECK(res.n_files == 1, L"应只有 real.txt 命中，实际 %llu",
          (unsigned long long)res.n_files);
    findrep_free(&res);
}

/* ============================================== 3. 字面量与正则 ===== */

static void test_literal_and_regex(void) {
    wprintf(L"[3] 字面量与正则替换\n");

    wchar_t d[MAX_PATH * 2];
    swprintf(d, MAX_PATH * 2, L"%ls\\re", g_root);
    reset_dir(d);

    wchar_t p[MAX_PATH * 2];
    swprintf(p, MAX_PATH * 2, L"%ls\\a.txt", d);
    write_enc(p, L"a1 a2 a3\nA4 a5\n", ENC_UTF8);

    FindRepOpts o;
    memset(&o, 0, sizeof(o));
    o.find = L"a";
    o.replace = L"Z";
    o.recursive = 1;
    o.max_bytes = 64ll << 20;

    FindRepResult res;
    wchar_t err[512];
    err[0] = 0;

    /* 区分大小写：只有小写 a 被换掉 */
    CHECK(findrep_scan(d, &o, &res, err, 512) == 0, L"扫描应成功");
    CHECK(res.total_matches == 4, L"区分大小写应命中 4 个，实际 %lld", res.total_matches);
    findrep_free(&res);

    /* 忽略大小写：连大写 A 一起换掉 */
    o.ignore_case = 1;
    CHECK(findrep_scan(d, &o, &res, err, 512) == 0, L"扫描应成功");
    CHECK(res.total_matches == 5, L"忽略大小写应命中 5 个，实际 %lld", res.total_matches);
    findrep_free(&res);

    /* 正则：把数字抓出来 */
    memset(&o, 0, sizeof(o));
    o.find = L"\\d+";
    o.replace = L"#";
    o.use_regex = 1;
    o.recursive = 1;
    o.max_bytes = 64ll << 20;
    CHECK(findrep_scan(d, &o, &res, err, 512) == 0, L"正则扫描应成功：%ls", err);
    CHECK(res.total_matches == 5, L"正则 \\d+ 应命中 5 个，实际 %lld", res.total_matches);
    findrep_free(&res);

    /* 非法正则必须报错，而不是静默当成字面量 */
    o.find = L"([unclosed";
    wchar_t e2[512];
    e2[0] = 0;
    CHECK(findrep_scan(d, &o, &res, e2, 512) != 0, L"非法正则应当报错");
}

/* ============================================== 4. 生成器往返 ======= */

static void test_spec_roundtrip(void) {
    wprintf(L"[4] 生成器规格编解码（含冒号等特殊字符）\n");

    wchar_t d[MAX_PATH * 2];
    swprintf(d, MAX_PATH * 2, L"%ls\\spec", g_root);
    reset_dir(d);

    wchar_t src[MAX_PATH * 2], dst[MAX_PATH * 2];
    swprintf(src, MAX_PATH * 2, L"%ls\\in.txt", d);
    swprintf(dst, MAX_PATH * 2, L"%ls\\out.txt", d);

    /* 查找内容里带冒号 —— 这正是用十六进制编码的原因：
       如果规格是 "edit:flags:find:replace" 直接拼接，带冒号的 find 会把解析弄乱 */
    write_enc(src, L"地址是 http://a.com 和 http://b.com\n", ENC_UTF8);

    FindRepOpts o;
    memset(&o, 0, sizeof(o));
    o.find = L"http://";
    o.replace = L"https://";
    o.ignore_case = 1;

    wchar_t spec[4096];
    CHECK(findrep_make_spec(&o, spec, 4096) == 0, L"应能生成规格");
    CHECK(wcsncmp(spec, L"edit:", 5) == 0, L"规格应以 edit: 开头");

    wchar_t err[512];
    err[0] = 0;
    CHECK(findrep_apply(spec, src, dst, err, 512) == 0, L"执行替换应成功：%ls", err);

    size_t n = 0;
    unsigned char *b = read_bytes(dst, &n);
    CHECK(b != NULL, L"应能读回产物");
    if (b) {
        wchar_t *w = decode_text_bytes(b, n);
        CHECK(w != NULL, L"产物应能解码");
        if (w) {
            CHECK(wcsstr(w, L"https://a.com") != NULL, L"带冒号的查找内容应被正确替换：%ls", w);
            CHECK(wcsstr(w, L"https://b.com") != NULL, L"第二处也应被替换");
            CHECK(wcsstr(w, L"http://") == NULL, L"不应残留 http://");
            free(w);
        }
        free(b);
    }

    /* 规格损坏时应明确报错 */
    err[0] = 0;
    CHECK(findrep_apply(L"edit:0:xyz", src, dst, err, 512) != 0,
          L"畸形规格应报错而不是写出损坏文件");
}

/* ============================================== 5. 端到端可回退 ===== */

static void test_reversible_end_to_end(void) {
    wprintf(L"[5] 执行 → 回退 → 字节级还原\n");

    wchar_t d[MAX_PATH * 2];
    swprintf(d, MAX_PATH * 2, L"%ls\\rev", g_root);
    reset_dir(d);

    wchar_t pa[MAX_PATH * 2], pb[MAX_PATH * 2];
    swprintf(pa, MAX_PATH * 2, L"%ls\\a.txt", d);
    swprintf(pb, MAX_PATH * 2, L"%ls\\b.txt", d);
    write_enc(pa, L"课程名称：甲\n课程名称：乙\n", ENC_UTF8);
    write_enc(pb, L"课程名称：丙\n", ENC_GBK);        /* 关键：GBK 必须原样还原 */

    /* 记下原始字节，回退后要一字不差 */
    size_t na = 0, nb = 0;
    unsigned char *oa = read_bytes(pa, &na);
    unsigned char *ob = read_bytes(pb, &nb);
    CHECK(oa && ob, L"应能读到原始文件");

    FindRepOpts o;
    memset(&o, 0, sizeof(o));
    o.find = L"课程名称";
    o.replace = L"课程";
    o.recursive = 1;
    o.max_bytes = 64ll << 20;

    FindRepResult res;
    wchar_t err[512];
    err[0] = 0;
    CHECK(findrep_scan(d, &o, &res, err, 512) == 0, L"扫描应成功");
    CHECK(res.n_files == 2, L"应命中 2 个文件，实际 %llu",
          (unsigned long long)res.n_files);
    CHECK(res.total_matches == 3, L"应共 3 处匹配，实际 %lld", res.total_matches);

    wchar_t spec[4096];
    CHECK(findrep_make_spec(&o, spec, 4096) == 0, L"应能生成规格");

    /* 手工搭一个和 api.c 里一样的计划，验证"原地改内容"的步骤拆解 */
    Arena *a = arena_new();
    Plan *plan = (Plan *)arena_zalloc(a, sizeof(Plan));
    plan_init(plan, a, d, POLICY_RENAME, L"findrep", L"文本替换测试");
    for (size_t i = 0; i < res.n_files; ++i) {
        Action *act = plan_add(plan, L"findrep", res.files[i].path,
                               res.files[i].path, L"内容替换");
        if (!act) continue;
        act->produce_only = 1;
        act->inplace_edit = 1;
        act->force_exec   = 1;
        act->generator = arena_wcsdup(a, spec);
        act->producer  = arena_wcsdup(a, L"写入替换后的内容");
    }
    plan_finalize(plan, 1, 1);

    CHECK(plan->st_executable == 2, L"两行都应可执行，实际 %llu",
          (unsigned long long)plan->st_executable);

    /* 这一条是防回归的关键：拆解步骤的条件必须和执行判定一致。
       如果 planner 里漏掉 force_exec，两个动作会一条步骤都没有，
       执行时被当成"无事可做"跳过，而这里又不会报任何错。 */
    int total_steps = 0;
    for (size_t i = 0; i < plan->n_actions; ++i) total_steps += (int)plan->actions[i].n_steps;
    CHECK(total_steps == 4,
          L"每个原地替换动作应有 2 条步骤（MOVE + CREATE），共 4 条，实际 %d", total_steps);

    /* 预览阶段不应改动任何文件 */
    size_t t1 = 0;
    unsigned char *c1 = read_bytes(pa, &t1);
    CHECK(c1 && t1 == na, L"预览阶段源文件大小不应变化");
    free(c1);

    ExecOptions opt;
    memset(&opt, 0, sizeof(opt));
    RunResult r;
    executor_run(plan, &opt, &r);
    CHECK(r.ok == 2 && r.failed == 0 && r.skipped == 0,
          L"执行应成功 2 项（ok=%d failed=%d skipped=%d）", r.ok, r.failed, r.skipped);

    /* 内容应已替换，且编码不变 */
    {
        size_t n2 = 0;
        unsigned char *b2 = read_bytes(pb, &n2);
        CHECK(b2 != NULL, L"应能读回 GBK 文件");
        if (b2) {
            CHECK(n2 >= 2 && b2[0] != 0xEF,
                  L"GBK 文件不应被写成带 BOM 的 UTF-8");
            wchar_t *w = decode_text_bytes(b2, n2);
            if (w) {
                CHECK(wcsstr(w, L"课程：丙") != NULL, L"GBK 内容应被替换：%ls", w);
                CHECK(wcsstr(w, L"课程名称") == NULL, L"不应残留原文");
                free(w);
            }
            free(b2);
        }
    }

    wchar_t jp[MAX_PATH * 2];
    wcsncpy(jp, r.journal_path, MAX_PATH * 2 - 1);
    jp[MAX_PATH * 2 - 1] = 0;
    run_result_free(&r);

    /* ---- 回退 ---- */
    ExecOptions o2;
    memset(&o2, 0, sizeof(o2));
    RunResult rb;
    rollback_apply(jp, &o2, &rb);
    CHECK(rb.failed == 0, L"回退应成功（failed=%d）", rb.failed);
    run_result_free(&rb);

    /* ---- 字节级比对 ---- */
    size_t fa = 0, fb = 0;
    unsigned char *ra = read_bytes(pa, &fa);
    unsigned char *rbb = read_bytes(pb, &fb);
    CHECK(ra && fa == na && memcmp(ra, oa, na) == 0,
          L"UTF-8 文件回退后应与原字节完全一致（%llu vs %llu）",
          (unsigned long long)fa, (unsigned long long)na);
    CHECK(rbb && fb == nb && memcmp(rbb, ob, nb) == 0,
          L"GBK 文件回退后应与原字节完全一致（%llu vs %llu）",
          (unsigned long long)fb, (unsigned long long)nb);
    free(ra); free(rbb);

    free(oa); free(ob);
    findrep_free(&res);
    arena_free(a);
}

/* ==================================================== main =========== */

int main(void) {
    SetConsoleOutputCP(CP_UTF8);

    wchar_t exe[MAX_PATH * 2], *slash;
    GetModuleFileNameW(NULL, exe, MAX_PATH * 2);
    slash = wcsrchr(exe, L'\\'); if (slash) *slash = 0;
    slash = wcsrchr(exe, L'\\'); if (slash) *slash = 0;

    swprintf(g_home, MAX_PATH * 2, L"%ls\\testtmp\\findrep", exe);
    lc_mkdir_p(g_home);
    SetEnvironmentVariableW(L"LOCALCRAFT_HOME", g_home);

    swprintf(g_root, MAX_PATH * 2, L"%ls\\work-%u", g_home,
             (unsigned)GetTickCount64());
    lc_mkdir_p(g_root);

    wprintf(L"=== LocalCraft 文本批量查找替换自检 ===\n");
    wprintf(L"工作目录：%ls\n\n", g_root);

    test_detect_encoding();
    test_binary_guard();
    test_literal_and_regex();
    test_spec_roundtrip();
    test_reversible_end_to_end();

    wprintf(L"\n=== 通过 %d / 失败 %d ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
