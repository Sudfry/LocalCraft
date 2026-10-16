/* ==========================================================================
 * test_category.c —— 用户自定义分类自检
 *
 * 这个模块的坑集中在两处：
 *
 *   1. **默认不能改变既有行为**。首次生成的模板里如果示例规则是生效的，
 *      所有人装上之后 PDF 就全被归进「发票」了 —— 内置的「文档」被悄悄顶掉，
 *      用户还不知道是自己没写的那段配置干的。测试里专门盯这一条。
 *
 *   2. **配置是手写的**，出错要有用。整份作废是最差的做法：
 *      用户只看到"分类不起作用了"，完全不知道错在第几行。
 *      所以这里也验证"跳过坏行、报出行号"。
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

static wchar_t g_home[MAX_PATH * 2];

/* 用指定编码写文本文件 */
static void write_enc(const wchar_t *path, const wchar_t *text, int enc) {
    UINT cp = (enc == 936) ? 936u : CP_UTF8;
    int n = WideCharToMultiByte(cp, 0, text, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return;
    size_t body = (size_t)n - 1;
    size_t bom = (enc == 65001) ? 3 : 0;
    unsigned char *b = (unsigned char *)malloc(body + bom + 1);
    if (!b) return;
    if (bom) { b[0] = 0xEF; b[1] = 0xBB; b[2] = 0xBF; }
    WideCharToMultiByte(cp, 0, text, -1, (char *)(b + bom), n, NULL, NULL);

    wchar_t *lp = lc_long_path(path);
    HANDLE h = CreateFileW(lp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) { free(b); return; }
    DWORD w = 0;
    WriteFile(h, b, (DWORD)(body + bom), &w, NULL);
    CloseHandle(h);
    free(b);
}

static void set_config(const wchar_t *text, int enc) {
    write_enc(categories_config_path(), text, enc);
    wchar_t e[512], w[512];
    e[0] = 0; w[0] = 0;
    int created = 0;
    categories_reload(e, 512, w, 512, &created);
}

/* ============================================== 1. 首次生成模板 ======= */

static void test_first_run_creates_template(void) {
    wprintf(L"[1] 首次运行生成模板，且模板不得改变默认行为\n");

    const wchar_t *cfg = categories_config_path();
    /* 先删掉，模拟全新安装 */
    DeleteFileW(cfg);
    CHECK(!lc_is_file(cfg), L"前置条件：配置文件应不存在");

    /* 重新载入会触发创建 */
    wchar_t e[512], w[512];
    e[0] = 0; w[0] = 0;
    int created = 0;
    CHECK(categories_reload(e, 512, w, 512, &created) == 0, L"载入应成功");
    CHECK(created == 1, L"应报告「已生成模板」");
    CHECK(lc_is_file(cfg), L"模板文件应被创建出来");

    /* ★ 关键回归：模板里的示例规则必须是注释状态。
       如果哪天有人把示例的注释去掉，这一条会立刻红。 */
    CHECK(categories_count() == 0,
          L"全新安装时不应有任何生效的自定义分类，实际 %llu 条",
          (unsigned long long)categories_count());

    /* 更要紧的是：内置分类一条都不能被顶掉 */
    CHECK(wcscmp(organize_category_of(L"x.pdf"),  L"文档") == 0,
          L"pdf 应仍归入内置「文档」，实际「%ls」", organize_category_of(L"x.pdf"));
    CHECK(wcscmp(organize_category_of(L"a.jpg"),  L"图片") == 0,
          L"jpg 应仍归入内置「图片」");
    CHECK(wcscmp(organize_category_of(L"n.docx"), L"文档") == 0,
          L"docx 应仍归入内置「文档」");
    CHECK(wcscmp(organize_category_of(L"t.xlsx"), L"表格") == 0,
          L"xlsx 应仍归入内置「表格」");

    /*
     * psd / ai / sketch 这些设计稿格式内置表里没有，会落到「其他」。
     * 这正好是自定义分类要解决的问题，所以专门确认一下这个现状 ——
     * 如果哪天内置表补上了它们，这一条会红，提醒我们同步改掉。
     */
    CHECK(wcscmp(organize_category_of(L"s.psd"), L"其他") == 0,
          L"psd 目前不在内置表里，应归入「其他」，实际「%ls」",
          organize_category_of(L"s.psd"));
}

/* ============================================== 2. 各种写法 =========== */

static void test_rule_forms(void) {
    wprintf(L"[2] 扩展名 / 关键词 / 正则三种写法\n");

    set_config(
        L"; 测试配置\n"
        L"[发票]\n"
        L"ext   = pdf, ofd\n"
        L"name  = 发票, 增值税\n"
        L"order = 10\n"
        L"\n"
        L"[合同]\n"
        L"name  = 合同, 协议\n"
        L"order = 20\n"
        L"\n"
        L"[截图]\n"
        L"name_regex = (截图|screenshot)\n"
        L"order = 30\n", 65001);

    CHECK(categories_count() == 3, L"应载入 3 个分类，实际 %llu",
          (unsigned long long)categories_count());

    /* 扩展名匹配（大小写不敏感、配置里不写点也行） */
    CHECK(wcscmp(categories_match(L"D:\\x\\a.pdf"), L"发票") == 0, L"pdf 应命中「发票」");
    CHECK(wcscmp(categories_match(L"D:\\x\\a.PDF"), L"发票") == 0, L"扩展名匹配应忽略大小写");
    CHECK(wcscmp(categories_match(L"D:\\x\\a.ofd"), L"发票") == 0, L"ofd 应命中「发票」");

    /* 关键词匹配 */
    CHECK(wcscmp(categories_match(L"D:\\x\\2026年3月发票.pdf"), L"发票") == 0,
          L"文件名含「发票」应命中");
    CHECK(wcscmp(categories_match(L"D:\\x\\采购合同.docx"), L"合同") == 0,
          L"文件名含「合同」应命中");

    /* 正则匹配 */
    CHECK(wcscmp(categories_match(L"D:\\x\\微信截图2026.png"), L"截图") == 0,
          L"正则应命中「截图」");
    CHECK(wcscmp(categories_match(L"D:\\x\\my_screenshot_1.png"), L"截图") == 0,
          L"正则应命中 screenshot");

    /* 都不命中 → 返回 NULL，交给内置规则 */
    CHECK(categories_match(L"D:\\x\\随便什么.txt") == NULL, L"都不命中时不应返回分类");

    /* 自定义优先于内置 */
    CHECK(wcscmp(organize_category_of(L"D:\\x\\a.pdf"), L"发票") == 0,
          L"自定义分类应优先于内置「文档」");
    CHECK(wcscmp(organize_category_of(L"D:\\x\\a.txt"), L"文档") == 0,
          L"没被自定义覆盖的仍走内置");

    /*
     * 自定义分类真正的价值：内置表覆盖不到的格式。
     * psd / sketch 这些设计稿默认都落到「其他」，用一条自定义规则就能收拢起来。
     */
    set_config(
        L"[设计稿]\n"
        L"ext   = psd, ai, sketch, fig\n"
        L"order = 30\n", 65001);
    CHECK(wcscmp(organize_category_of(L"D:\\x\\banner.psd"), L"设计稿") == 0,
          L"自定义规则应能收拢内置表没覆盖的格式");
    CHECK(wcscmp(organize_category_of(L"D:\\x\\logo.ai"), L"设计稿") == 0,
          L"ai 也应命中");
    CHECK(wcscmp(organize_category_of(L"D:\\x\\a.jpg"), L"图片") == 0,
          L"没写在自定义规则里的仍走内置");
}

/* ============================================== 3. order 优先级 ======= */

static void test_order_priority(void) {
    wprintf(L"[3] order 决定谁先命中\n");

    /* 故意把「报销单」写在后面，但 order 更小，应该它先命中 */
    set_config(
        L"[发票]\n"
        L"name  = 报销\n"
        L"order = 50\n"
        L"\n"
        L"[报销单]\n"
        L"name  = 报销\n"
        L"order = 5\n", 65001);

    CHECK(wcscmp(categories_match(L"3月报销记录.pdf"), L"报销单") == 0,
          L"order 小的应先命中，实际「%ls」", categories_match(L"3月报销记录.pdf"));

    /* order 相同时按声明顺序 */
    set_config(
        L"[甲]\nname = 测试\norder = 7\n"
        L"[乙]\nname = 测试\norder = 7\n", 65001);
    CHECK(wcscmp(categories_match(L"测试文件.txt"), L"甲") == 0,
          L"order 相同时应按声明顺序，实际「%ls」", categories_match(L"测试文件.txt"));
}

/* ============================================== 4. 出错处理 =========== */

static void test_error_handling(void) {
    wprintf(L"[4] 坏行跳过并报行号，不整份作废\n");

    set_config(
        L"[好分类]\n"
        L"ext = abc\n"
        L"\n"
        L"[坏分类]\n"
        L"这一行没有等号\n"
        L"ext = xyz\n", 65001);

    wchar_t e[512], w[512];
    e[0] = 0; w[0] = 0;
    int created = 0;
    categories_reload(e, 512, w, 512, &created);

    /* 坏行被跳过，但两个分类都还在（坏的那条只是少了一条规则） */
    CHECK(categories_count() == 2, L"两个分类都应保留，实际 %llu",
          (unsigned long long)categories_count());
    CHECK(wcscmp(categories_match(L"a.abc"), L"好分类") == 0, L"好分类应正常工作");
    CHECK(wcscmp(categories_match(L"a.xyz"), L"坏分类") == 0,
          L"坏分类里合法的那条规则仍应生效");
    CHECK(w[0] != 0, L"应给出警告信息（哪一行有问题）");

    /* 非法分类名（含不能用于文件夹的字符）应被拒绝 */
    set_config(
        L"[好]\next = aaa\n"
        L"[坏/名]\next = bbb\n", 65001);
    CHECK(categories_count() == 1, L"含非法字符的分类名应被拒绝，实际 %llu",
          (unsigned long long)categories_count());
    CHECK(wcscmp(categories_match(L"x.aaa"), L"好") == 0, L"合法分类应保留");

    /* 空分类名 */
    set_config(L"[]\next = aaa\n", 65001);
    CHECK(categories_count() == 0, L"空分类名应被拒绝");

    /* 不认识的键 */
    set_config(L"[甲]\nfrobnicate = 1\next = zzz\n", 65001);
    CHECK(categories_count() == 1, L"未知键不应导致分类失效");
    CHECK(wcscmp(categories_match(L"q.zzz"), L"甲") == 0, L"同段落里的合法规则应生效");

    /* 正则写错了：分类还在，只是这条正则不生效，而且要报出来 */
    set_config(L"[甲]\nname_regex = ([未闭合\next = yyy\n", 65001);
    e[0] = 0; w[0] = 0;
    categories_reload(e, 512, w, 512, &created);
    CHECK(w[0] != 0, L"正则编译失败应给出警告");
    CHECK(wcscmp(categories_match(L"q.yyy"), L"甲") == 0,
          L"正则坏掉不应影响同一段落的扩展名规则");

    /* 整份空配置 = 退回内置 */
    set_config(L"; 只有注释\n; 什么规则都没有\n", 65001);
    CHECK(categories_count() == 0, L"空配置应没有任何分类");
    CHECK(wcscmp(organize_category_of(L"x.pdf"), L"文档") == 0, L"空配置应退回内置规则");
}

/* ============================================== 5. 编码 =============== */

static void test_config_encoding(void) {
    wprintf(L"[5] 配置文件编码：笔记本另存为 ANSI 也要能读\n");

    /* 同一个配置，分别用 UTF-8(带BOM) 和 GBK 各写一遍，结果应一致 */
    const wchar_t *cfg_utf8 =
        L"[发票]\nname = 发票\norder = 1\n";
    set_config(cfg_utf8, 65001);
    CHECK(categories_count() == 1, L"UTF-8 配置应能载入");
    CHECK(wcscmp(categories_match(L"3月发票.pdf"), L"发票") == 0,
          L"UTF-8 里的中文关键词应能匹配");

    set_config(cfg_utf8, 936);
    CHECK(categories_count() == 1, L"GBK 配置应能载入");
    CHECK(wcscmp(categories_match(L"3月发票.pdf"), L"发票") == 0,
          L"GBK 里的中文关键词应能匹配（记事本另存为 ANSI 是常见情况）");

    set_config(cfg_utf8, 65001);   /* 还原成带 BOM 的 */
    CHECK(categories_count() == 1, L"带 BOM 的 UTF-8 应能载入");
    CHECK(wcscmp(categories_match(L"3月发票.pdf"), L"发票") == 0,
          L"带 BOM 时首行不应把分类名读脏");
}

/* ==================================================== main =========== */

int main(void) {
    SetConsoleOutputCP(CP_UTF8);

    wchar_t exe[MAX_PATH * 2], *slash;
    GetModuleFileNameW(NULL, exe, MAX_PATH * 2);
    slash = wcsrchr(exe, L'\\'); if (slash) *slash = 0;
    slash = wcsrchr(exe, L'\\'); if (slash) *slash = 0;

    swprintf(g_home, MAX_PATH * 2, L"%ls\\testtmp\\category-%u", exe,
             (unsigned)GetTickCount64());
    lc_mkdir_p(g_home);
    SetEnvironmentVariableW(L"LOCALCRAFT_HOME", g_home);

    wprintf(L"=== LocalCraft 自定义分类自检 ===\n");
    wprintf(L"工作目录：%ls\n\n", g_home);

    test_first_run_creates_template();
    test_rule_forms();
    test_order_priority();
    test_error_handling();
    test_config_encoding();

    wprintf(L"\n=== 通过 %d / 失败 %d ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
