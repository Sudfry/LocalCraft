/* ==========================================================================
 * test_analysis.c —— 分词 / SimHash 去重 / 抽取式摘要 的自检
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

static void write_w(const wchar_t *path, const wchar_t *content) {
    char *u8 = lc_wcs_to_utf8(content);
    if (!u8) return;
    wchar_t *lp = lc_long_path(path);
    HANDLE h = CreateFileW(lp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) { free(u8); return; }
    DWORD w = 0;
    WriteFile(h, u8, (DWORD)strlen(u8), &w, NULL);
    CloseHandle(h);
    free(u8);
}

static wchar_t *wp(const wchar_t *name) {
    static wchar_t buf[8][MAX_PATH * 2];
    static int slot = 0;
    slot = (slot + 1) % 8;
    swprintf(buf[slot], MAX_PATH * 2, L"%ls\\%ls", g_root, name);
    return buf[slot];
}

/* ====================================================== 分词 =========== */

static void test_tokenize(void) {
    wprintf(L"[1] 分词与分句\n");
    Arena *a = arena_new();
    TokenList t;
    tokens_init(&t, a);

    tokenize(L"本地文件处理工具", &t);
    /* "本地文件处理工具" 8 个汉字 -> 7 个二元组 */
    CHECK(t.n == 7, L"8 个汉字应切出 7 个二元组，实际 %llu",
          (unsigned long long)t.n);
    if (t.n >= 7) {
        CHECK(wcscmp(t.items[0], L"本地") == 0, L"第一组应是「本地」，实际 %ls", t.items[0]);
        CHECK(wcscmp(t.items[1], L"地文") == 0, L"第二组应是「地文」");
        CHECK(wcscmp(t.items[6], L"工具") == 0, L"最后一组应是「工具」");
    }

    t.n = 0;
    tokenize(L"LocalCraft 1.0 版本", &t);
    int has_local = 0, has_ver = 0;
    for (size_t i = 0; i < t.n; ++i) {
        if (wcscmp(t.items[i], L"localcraft") == 0) has_local = 1;  /* 应小写化 */
        if (wcscmp(t.items[i], L"版本") == 0) has_ver = 1;
    }
    CHECK(has_local, L"英文词应被切成小写的 localcraft");
    CHECK(has_ver, L"中文部分应切出「版本」");

    /* 停用词应被丢弃 */
    t.n = 0;
    tokenize(L"的 了 是 the a an", &t);
    CHECK(t.n == 0, L"停用词应全部丢弃，实际剩 %llu", (unsigned long long)t.n);

    /* 标点当分隔符 */
    t.n = 0;
    tokenize(L"文件，处理。工具！", &t);
    CHECK(t.n == 3, L"标点应分隔出 3 段，实际 %llu", (unsigned long long)t.n);

    tokens_free(&t);
    arena_free(a);

    /* 分句 */
    a = arena_new();
    SpanList sp;
    spanlist_init(&sp, a);
    const wchar_t *text = L"第一句。第二句！第三句？";
    split_sentences(text, &sp);
    CHECK(sp.n == 3, L"应切出 3 句，实际 %llu", (unsigned long long)sp.n);
    if (sp.n == 3) {
        CHECK(sp.ends[0] - sp.starts[0] == 4, L"第一句长度应为 4（含句号）");
        CHECK(text[sp.starts[1]] == L'第', L"第二句起点应对齐");
    }
    spanlist_free(&sp);
    arena_free(a);
}

/* ====================================================== 编码 =========== */

static void test_decode(void) {
    wprintf(L"[2] 编码识别\n");
    /* UTF-8 */
    const char *u8 = "\xE4\xB8\xAD\xE6\x96\x87";   /* 中文 */
    wchar_t *w = decode_text_bytes((const unsigned char *)u8, strlen(u8));
    CHECK(w && wcscmp(w, L"中文") == 0, L"UTF-8 解码失败");
    free(w);

    /* 带 BOM 的 UTF-8 */
    const char *bom = "\xEF\xBB\xBF\xE4\xB8\xAD";
    w = decode_text_bytes((const unsigned char *)bom, strlen(bom));
    CHECK(w && wcscmp(w, L"中") == 0, L"UTF-8 BOM 未剥掉");
    free(w);

    /* UTF-16 LE */
    unsigned char u16[] = {0xFF, 0xFE, 0x2D, 0x4E, 0x87, 0x65, 0x00, 0x00};
    w = decode_text_bytes(u16, 8);
    CHECK(w && wcscmp(w, L"中文") == 0, L"UTF-16LE 解码失败（得到 %ls）",
          w ? w : L"(null)");
    free(w);

    /* GBK 编码的 "中文" = D6 D0 CE C4 */
    unsigned char gbk[] = {0xD6, 0xD0, 0xCE, 0xC4, 0x00};
    w = decode_text_bytes(gbk, 4);
    CHECK(w && wcscmp(w, L"中文") == 0, L"GBK 回退解码失败（得到 %ls）",
          w ? w : L"(null)");
    free(w);
}

/* ====================================================== SimHash ======== */

static void test_simhash(void) {
    wprintf(L"[3] SimHash 指纹\n");

    const wchar_t *t1 =
        L"本地文件处理工具需要支持批量重命名、格式转换和内容去重。"
        L"所有操作都应该先预览再执行，并且能够一键回退。"
        L"回退依赖日志文件，记录操作前后的完整映射关系。";

    /* 内容基本一致，只是措辞略有不同、文件名不同 */
    const wchar_t *t2 =
        L"本地文件处理工具需要支持批量重命名、格式转换以及内容去重。"
        L"所有操作都应当先预览再执行，并且可以一键回退。"
        L"回退依赖日志文件，记录操作前后的完整映射关系。";

    /* 完全不同的内容 */
    const wchar_t *t3 =
        L"今天的天气很好，适合出门散步。"
        L"公园里的樱花开了，吸引了很多游客拍照留念。"
        L"晚上的气温会下降，记得多穿一件外套。";

    SimHash h1 = simhash_text(t1);
    SimHash h2 = simhash_text(t2);
    SimHash h3 = simhash_text(t3);

    CHECK(h1.tokens > 0 && h2.tokens > 0 && h3.tokens > 0, L"应都能算出指纹");
    CHECK(h1.bits != 0, L"指纹不应为 0");

    double s12 = simhash_similarity(h1, h2);
    double s13 = simhash_similarity(h1, h3);
    wprintf(L"      相似1-2 = %.3f   相似1-3 = %.3f\n", s12, s13);

    CHECK(s12 > 0.80, L"相近文本相似度应 > 0.80，实际 %.3f", s12);
    CHECK(s13 < s12, L"不同文本的相似度应明显更低");
    CHECK(simhash_distance(h1, h1) == 0, L"与自己距离应为 0");
    CHECK(simhash_similarity(h1, h1) == 1.0, L"与自己相似度应为 1.0");

    /* 空文本不应崩 */
    SimHash he = simhash_text(L"");
    CHECK(he.bits == 0 && he.tokens == 0, L"空文本指纹应为 0");
}

/* ====================================================== 摘要 =========== */

static void test_summarize(void) {
    wprintf(L"[4] 抽取式摘要\n");

    const wchar_t *doc =
        L"LocalCraft 是一款完全离线的本地文件处理工具。"
        L"它不会发起任何网络请求，也不会把文件上传到任何地方。"
        L"所有批量操作都遵循先预览、再执行、可回退的流程。"
        L"今天午饭吃的是番茄炒蛋和米饭，味道还不错。"
        L"预览阶段是只读的，会把每个文件的旧状态和新状态列成对照表。"
        L"执行过程会逐步写入回退日志，之后任何时候都能一键还原。"
        L"回退不依赖系统撤销功能，也不受时间限制。"
        L"楼下那只橘猫又在晒太阳了，看起来非常惬意。"
        L"程序使用纯 C 语言实现，静态链接后只有一个可执行文件。"
        L"体积只有五百多 KB，不需要安装任何运行库。";

    Arena *a = arena_new();
    Summary s;
    int rc = summarize_text(doc, 5, 1, &s, a);
    CHECK(rc == 0, L"TextRank 摘要应成功");
    CHECK(s.n_sentences == 5, L"应抽出 5 句，实际 %llu",
          (unsigned long long)s.n_sentences);

    /* 抽出的句子必须能定位回原文，且文本合理 */
    int all_valid = 1;
    for (size_t i = 0; i < s.n_sentences; ++i) {
        if (s.sentences[i].end <= s.sentences[i].start ||
            s.sentences[i].end > wcslen(doc)) { all_valid = 0; break; }
    }
    CHECK(all_valid, L"所有摘要句子都应能定位到原文区间");

    /* 相邻两句的区间应当递增（按原文顺序输出） */
    int ordered = 1;
    for (size_t i = 1; i < s.n_sentences; ++i)
        if (s.sentences[i].start < s.sentences[i - 1].start) ordered = 0;
    CHECK(ordered, L"摘要应按原文顺序排列");

    /* 主题句应当入选，"午饭吃的是番茄炒蛋" 这种离题句不该入选 */
    int hit_topic = 0, hit_offtopic = 0;
    for (size_t i = 0; i < s.n_sentences; ++i) {
        size_t st = s.sentences[i].start, en = s.sentences[i].end;
        size_t len = en - st;
        wchar_t buf[512];
        if (len >= 512) len = 511;
        memcpy(buf, doc + st, len * sizeof(wchar_t));
        buf[len] = 0;
        if (wcsstr(buf, L"预览") || wcsstr(buf, L"回退") ||
            wcsstr(buf, L"离线") || wcsstr(buf, L"纯 C")) hit_topic++;
        if (wcsstr(buf, L"番茄炒蛋") || wcsstr(buf, L"橘猫")) hit_offtopic++;
    }
    CHECK(hit_topic >= 2, L"主题句应至少入选 2 句，实际 %d", hit_topic);
    CHECK(hit_offtopic == 0, L"离题句不应入选，实际入选 %d 句", hit_offtopic);

    /* 关键词 */
    CHECK(s.n_keywords > 0, L"应抽出关键词");
    for (size_t i = 0; i < s.n_keywords; ++i)
        (void)s.keywords[i];
    wprintf(L"      关键词 %llu 个，首个 = %ls\n",
            (unsigned long long)s.n_keywords,
            s.n_keywords ? s.keywords[0] : L"(无)");
    CHECK(s.keywords[0] && s.keywords[0][0] != 0, L"首个关键词不应为空");

    wprintf(L"      摘要首句 = ");
    if (s.n_sentences) {
        size_t len = s.sentences[0].end - s.sentences[0].start;
        for (size_t k = 0; k < len; ++k) wprintf(L"%lc", doc[s.sentences[0].start + k]);
    }
    wprintf(L"\n");

    /* TF-IDF 路径也要能跑 */
    Summary s2;
    rc = summarize_text(doc, 3, 0, &s2, a);
    CHECK(rc == 0 && s2.n_sentences == 3, L"TF-IDF 摘要应抽出 3 句，实际 %llu",
          (unsigned long long)s2.n_sentences);

    /* 自动句数 */
    Summary s3;
    rc = summarize_text(doc, 0, 1, &s3, a);
    CHECK(rc == 0 && s3.n_sentences >= 3 && s3.n_sentences <= 8,
          L"自动句数应在 3~8 之间，实际 %llu", (unsigned long long)s3.n_sentences);

    /* 空文本与单句不应崩 */
    Summary s4;
    summarize_text(L"", 3, 1, &s4, a);
    Summary s5;
    summarize_text(L"只有一句话。", 3, 1, &s5, a);
    CHECK(s5.n_sentences <= 1, L"单句文档最多抽出 1 句");

    arena_free(a);
}

/* ====================================================== 去重 ========== */

static void test_dedup(void) {
    wprintf(L"[5] 内容级去重（按内容，不看文件名）\n");

    wchar_t *home = lc_data_root();
    (void)home; free(home);

    /* 造三份"内容雷同但文件名毫不相干"的文档，外加两份无关文档 */
    const wchar_t *common =
        L"季度经营分析报告\n"
        L"本季度营业收入较上季度增长百分之十二，主要来自华东区域的渠道扩张。\n"
        L"毛利率维持在百分之三十八左右，与去年同期基本持平。\n"
        L"费用方面，销售费用增长百分之九，管理费用下降百分之三。\n"
        L"下季度计划继续加大在华南和西南的投入，同时控制人员规模。\n"
        L"风险提示：原材料价格波动可能对毛利率造成两个百分点的影响。\n";

    write_w(wp(L"report_A.txt"), common);
    /* 第二份：改几句话，模拟"同一份文档的另一个版本" */
    write_w(wp(L"会议纪要_最终版.txt"),
            L"季度经营分析报告\n"
            L"本季度营业收入较上季度增长百分之十二，主要来自华东区域的渠道扩张。\n"
            L"毛利率维持在百分之三十八左右，与去年同期基本持平。\n"
            L"费用方面，销售费用增长百分之九，管理费用下降了百分之三。\n"
            L"下季度计划继续加大在华南和西南地区的投入，同时控制人员规模。\n"
            L"风险提示：原材料价格波动可能对毛利率造成两个百分点的影响。\n");
    write_w(wp(L"z_副本(1).txt"), common);

    write_w(wp(L"完全无关_天气.txt"),
            L"今天天气晴朗，气温二十度左右，适合外出活动。\n"
            L"傍晚有轻微北风，空气质量良好，适合开窗通风。\n"
            L"明天预计有小雨，出行请记得携带雨具，注意路面湿滑。\n"
            L"周末气温会进一步回升，可以安排户外活动或者短途旅行。\n");
    write_w(wp(L"完全无关_菜谱.txt"),
            L"番茄炒蛋的做法：先把鸡蛋打散，加少许盐搅拌均匀备用。\n"
            L"番茄切块，热锅冷油，倒入蛋液炒至凝固后盛出备用。\n"
            L"锅中留底油，下番茄翻炒出汁，加糖和盐调味，再倒入鸡蛋翻匀。\n"
            L"出锅前撒上葱花即可，全程大约需要十分钟，非常简单快捷。\n");

    Arena *a = arena_new();
    DedupRule rule;
    dedup_rule_init(&rule, a);
    rule.threshold = 0.85;
    rule.min_chars = 20;
    rule.recursive = 1;

    DedupResult res;
    wchar_t err[512];
    int rc = dedup_scan(g_root, &rule, &res, err, 512);
    CHECK(rc == 0, L"扫描应成功：%ls", err);

    wprintf(L"      扫描 %llu 个文档，%llu 组重复，跳过 %d 个（距离上限 %d）\n",
            (unsigned long long)res.n, (unsigned long long)res.n_groups,
            res.skipped, res.max_distance);

    CHECK(res.n == 5, L"应扫描到 5 个文档，实际 %llu", (unsigned long long)res.n);
    CHECK(res.n_groups >= 1, L"应至少发现 1 组重复，实际 %llu",
          (unsigned long long)res.n_groups);

    /* 找出重复组：report_A / 会议纪要_最终版 / z_副本(1) 三者应在同一组 */
    int in_group_report = -1, in_group_meeting = -1, in_group_copy = -1;
    int weather_group = -1, recipe_group = -1;
    for (size_t i = 0; i < res.n; ++i) {
        const wchar_t *nm = lc_basename(res.items[i].path);
        if (wcsstr(nm, L"report_A"))       in_group_report  = res.items[i].group;
        if (wcsstr(nm, L"会议纪要"))        in_group_meeting = res.items[i].group;
        if (wcsstr(nm, L"z_副本"))          in_group_copy    = res.items[i].group;
        if (wcsstr(nm, L"天气"))            weather_group    = res.items[i].group;
        if (wcsstr(nm, L"菜谱"))            recipe_group     = res.items[i].group;
    }

    CHECK(in_group_report > 0, L"report_A 应被判为重复（实际 group=%d）", in_group_report);
    CHECK(in_group_meeting > 0, L"会议纪要_最终版 应被判为重复（实际 group=%d）",
          in_group_meeting);
    CHECK(in_group_copy > 0, L"z_副本(1) 应被判为重复");
    CHECK(in_group_report == in_group_meeting &&
          in_group_report == in_group_copy,
          L"三份内容雷同但文件名不同的文档应归入同一组（%d / %d / %d）",
          in_group_report, in_group_meeting, in_group_copy);
    CHECK(weather_group == -1, L"无关的天气文档不应被判为重复");
    CHECK(recipe_group == -1, L"无关的菜谱文档不应被判为重复");

    /* 生成计划 */
    Plan *plan = dedup_build_plan(a, g_root, &res, &rule, POLICY_RENAME, NULL);
    CHECK(plan != NULL, L"应能生成去重计划");
    if (plan) {
        CHECK(plan->st_total == 2, L"3 份重复里应移走 2 份（保留 1 份），实际 %llu",
              (unsigned long long)plan->st_total);
        CHECK(plan->st_executable == 2, L"两条动作都应可执行");
        /* 每一条都应当是「（可能先建目录）+ 移动进 _重复文件」 */
        int shape_ok = 1;
        for (size_t i = 0; i < plan->n_actions; ++i) {
            Action *act = &plan->actions[i];
            if (act->n_steps < 1) { shape_ok = 0; break; }
            if (act->steps[act->n_steps - 1].kind != STEP_MOVE) shape_ok = 0;
            for (size_t k = 0; k + 1 < act->n_steps; ++k)
                if (act->steps[k].kind != STEP_MKDIR) shape_ok = 0;
            if (act->dst && !wcsstr(act->dst, L"_重复文件")) shape_ok = 0;
        }
        CHECK(shape_ok, L"去重动作应当是「（按需建目录）+ 移动到 _重复文件」");
    }

    /* 真跑一遍，再回退 */
    if (plan) {
        ExecOptions opt;
        memset(&opt, 0, sizeof(opt));
        RunResult r;
        executor_run(plan, &opt, &r);
        CHECK(r.ok == 2 && r.failed == 0, L"去重执行应成功 2 项（ok=%d failed=%d）",
              r.ok, r.failed);
        CHECK(lc_is_file(wp(L"_重复文件\\z_副本(1).txt")) ||
              lc_is_file(wp(L"_重复文件\\会议纪要_最终版.txt")),
              L"应有文件被移入 _重复文件");

        wchar_t jp[MAX_PATH * 2];
        wcsncpy(jp, r.journal_path, MAX_PATH * 2 - 1);
        run_result_free(&r);

        ExecOptions o2;
        memset(&o2, 0, sizeof(o2));
        RunResult rb;
        rollback_apply(jp, &o2, &rb);
        CHECK(rb.failed == 0, L"去重回退应成功（failed=%d）", rb.failed);
        CHECK(lc_is_file(wp(L"z_副本(1).txt")),
              L"回退后 z_副本(1).txt 应回到原位");
    }

    dedup_result_free(&res);
    arena_free(a);
}

/* ====================================================== main =========== */

int main(void) {
    SetConsoleOutputCP(CP_UTF8);

    wchar_t exe[MAX_PATH * 2], *slash;
    GetModuleFileNameW(NULL, exe, MAX_PATH * 2);
    slash = wcsrchr(exe, L'\\'); if (slash) *slash = 0;
    slash = wcsrchr(exe, L'\\'); if (slash) *slash = 0;

    swprintf(g_home, MAX_PATH * 2, L"%ls\\testtmp\\analysis", exe);
    lc_mkdir_p(g_home);
    SetEnvironmentVariableW(L"LOCALCRAFT_HOME", g_home);

    swprintf(g_root, MAX_PATH * 2, L"%ls\\work-%u", g_home, (unsigned)GetTickCount());
    lc_mkdir_p(g_root);

    wprintf(L"=== LocalCraft 文本分析自检 ===\n");
    wprintf(L"工作目录：%ls\n\n", g_root);

    test_tokenize();
    test_decode();
    test_simhash();
    test_summarize();
    test_dedup();

    wprintf(L"\n=== 通过 %d / 失败 %d ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
