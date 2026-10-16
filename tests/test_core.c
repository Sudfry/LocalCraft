/* ==========================================================================
 * test_core.c —— 模块 1 核心（计划 / 执行 / 回退 / 重命名 / 分类）端到端测试
 *
 * 用真实的文件系统跑，不打桩 —— 这才叫验证"能回退"。
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
            wprintf(L"  \x2717 FAIL  ");                                        \
            wprintf(__VA_ARGS__);                                               \
            wprintf(L"   [第 %d 行]\n", __LINE__);                              \
        }                                                                       \
    } while (0)

static wchar_t g_root[MAX_PATH * 2];
static wchar_t g_home[MAX_PATH * 2];

/* -------------------------------------------------------- 小工具 ------ */

static void path_join(wchar_t *out, size_t cch, const wchar_t *a, const wchar_t *b) {
    swprintf(out, cch, L"%ls\\%ls", a, b);
}

static void write_file(const wchar_t *path, const char *content) {
    wchar_t *lp = lc_long_path(path);
    HANDLE h = CreateFileW(lp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, content, (DWORD)strlen(content), &w, NULL);
    CloseHandle(h);
}

static void read_file(const wchar_t *path, char *buf, size_t cch) {
    buf[0] = 0;
    wchar_t *lp = lc_long_path(path);
    HANDLE h = CreateFileW(lp, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD got = 0;
    if (ReadFile(h, buf, (DWORD)(cch - 1), &got, NULL)) buf[got] = 0;
    CloseHandle(h);
}

static int is_file(const wchar_t *path) { return lc_is_file(path); }

static wchar_t *wp(const wchar_t *name) {
    static wchar_t buf[8][MAX_PATH * 2];
    static int slot = 0;
    slot = (slot + 1) % 8;
    path_join(buf[slot], MAX_PATH * 2, g_root, name);
    return buf[slot];
}

static void fresh_root(const wchar_t *tag) {
    wchar_t base[MAX_PATH * 2];
    swprintf(base, MAX_PATH * 2, L"%ls\\lc-test-%ls", g_home, tag);
    if (lc_is_dir(base)) {
        /* 直接换个新目录，避免递归删除的复杂度 */
    }
    swprintf(base, MAX_PATH * 2, L"%ls\\lc-test-%ls-%u", g_home, tag,
             (unsigned)GetTickCount());
    lc_mkdir_p(base);
    wcsncpy(g_root, base, MAX_PATH * 2 - 1);
}

/* ====================================================== 用例 =========== */

static void test_planner_conflicts(void) {
    wprintf(L"[1] 计划与冲突分析\n");
    Arena *a;

    /* --- 基本重命名 --- */
    fresh_root(L"plan1");
    write_file(wp(L"a.txt"), "AAA");
    write_file(wp(L"b.txt"), "BBB");
    a = arena_new();
    {
        RenameRule r;
        rename_rule_init(&r, a);
        r.prefix = arena_wcsdup(a, L"X_");
        Plan *p = rename_build_plan(a, g_root, &r, POLICY_RENAME, NULL);
        CHECK(p->st_total == 2, L"应识别 2 个文件，实际 %llu",
              (unsigned long long)p->st_total);
        CHECK(p->st_executable == 2, L"应可执行 2 项，实际 %llu",
              (unsigned long long)p->st_executable);
        CHECK(p->st_blocked == 0, L"不应有冲突");
        for (size_t i = 0; i < p->n_actions; ++i) {
            CHECK(p->actions[i].n_steps == 1 &&
                  p->actions[i].steps[0].kind == STEP_MOVE,
                  L"普通重命名应为单个 MOVE 步骤");
        }
        rename_rule_free(&r);
    }
    arena_free(a);

    /* --- 源缺失 --- */
    fresh_root(L"plan2");
    a = arena_new();
    {
        Plan *p = (Plan *)arena_zalloc(a, sizeof(Plan));
        plan_init(p, a, g_root, POLICY_RENAME, L"rename", L"");
        plan_add(p, L"rename", wp(L"ghost.txt"), wp(L"x.txt"), L"");
        plan_finalize(p, 1, 1);
        CHECK(p->st_blocked == 1, L"源缺失应被阻止");
        CHECK(p->actions[0].conflicts[0].code == C_MISSING_SOURCE,
              L"冲突码应为 missing_source");
    }
    arena_free(a);

    /* --- 目标已存在：三种策略 --- */
    fresh_root(L"plan3");
    write_file(wp(L"a.txt"), "AAA");
    write_file(wp(L"x.txt"), "OLD");
    {
        Policy pols[3] = {POLICY_RENAME, POLICY_SKIP, POLICY_OVERWRITE};
        for (int k = 0; k < 3; ++k) {
            a = arena_new();
            Plan *p = (Plan *)arena_zalloc(a, sizeof(Plan));
            plan_init(p, a, g_root, pols[k], L"rename", L"");
            plan_add(p, L"rename", wp(L"a.txt"), wp(L"x.txt"), L"");
            plan_finalize(p, 1, 1);
            Action *act = &p->actions[0];
            if (pols[k] == POLICY_RENAME) {
                CHECK(wcscmp(lc_basename(act->dst), L"x (1).txt") == 0,
                      L"rename policy: got [%ls]\n", lc_basename(act->dst));
                CHECK(act->n_conflicts == 1 &&
                      act->conflicts[0].code == C_TARGET_EXISTS_RENAMED,
                      L"应记录自动改名提示");
            } else if (pols[k] == POLICY_SKIP) {
                CHECK(act->selected == 0, L"skip 策略应取消勾选");
                CHECK(!action_executable(act), L"skip 后不应可执行");
            } else {
                CHECK(act->n_steps == 2, L"覆盖应先入回收站再就位，实际 %llu 步",
                      (unsigned long long)act->n_steps);
                CHECK(act->steps[0].kind == STEP_MOVE &&
                      act->steps[0].dst && wcsstr(act->steps[0].dst, L"trash"),
                      L"第一步应是移动到回收站");
                CHECK(act->n_conflicts >= 1 && act->conflicts[0].code == C_OVERWRITE,
                      L"应记录覆盖警告");
            }
            arena_free(a);
        }
    }

    /* --- 同批重复目标 --- */
    fresh_root(L"plan4");
    write_file(wp(L"a.txt"), "A");
    write_file(wp(L"b.txt"), "B");
    a = arena_new();
    {
        Plan *p = (Plan *)arena_zalloc(a, sizeof(Plan));
        plan_init(p, a, g_root, POLICY_RENAME, L"rename", L"");
        plan_add(p, L"rename", wp(L"a.txt"), wp(L"same.txt"), L"");
        plan_add(p, L"rename", wp(L"b.txt"), wp(L"same.txt"), L"");
        plan_finalize(p, 1, 1);
        CHECK(p->st_blocked == 2, L"两个目标撞车应都被阻止，实际 %llu",
              (unsigned long long)p->st_blocked);
        CHECK(p->actions[0].conflicts[0].code == C_DUPLICATE_TARGET,
              L"冲突码应为 duplicate_target");
    }
    arena_free(a);

    /* --- 链式移动 a→b, b→c --- */
    fresh_root(L"plan5");
    write_file(wp(L"a.txt"), "A");
    write_file(wp(L"b.txt"), "B");
    a = arena_new();
    {
        Plan *p = (Plan *)arena_zalloc(a, sizeof(Plan));
        plan_init(p, a, g_root, POLICY_RENAME, L"rename", L"");
        plan_add(p, L"rename", wp(L"a.txt"), wp(L"b.txt"), L"");
        plan_add(p, L"rename", wp(L"b.txt"), wp(L"c.txt"), L"");
        plan_finalize(p, 1, 1);
        int found = 0;
        for (size_t i = 0; i < p->actions[0].n_conflicts; ++i)
            if (p->actions[0].conflicts[i].code == C_CHAIN_MOVE) found = 1;
        CHECK(found, L"链式移动应被检测出来，且不能被自动改名掩盖");
    }
    arena_free(a);

    /* --- 目录移进自己内部 --- */
    fresh_root(L"plan6");
    lc_mkdir_p(wp(L"folder"));
    a = arena_new();
    {
        Plan *p = (Plan *)arena_zalloc(a, sizeof(Plan));
        plan_init(p, a, g_root, POLICY_RENAME, L"rename", L"");
        plan_add(p, L"rename", wp(L"folder"), wp(L"folder\\sub"), L"");
        plan_finalize(p, 1, 1);
        CHECK(p->actions[0].conflicts[0].code == C_INTO_SELF,
              L"应检测到目录移入自身");
        CHECK(action_blocked(&p->actions[0]), L"应被阻止");
    }
    arena_free(a);

    /* --- 非法目标名 --- */
    fresh_root(L"plan7");
    write_file(wp(L"a.txt"), "A");
    a = arena_new();
    {
        Plan *p = (Plan *)arena_zalloc(a, sizeof(Plan));
        plan_init(p, a, g_root, POLICY_RENAME, L"rename", L"");
        plan_add(p, L"rename", wp(L"a.txt"), wp(L"bad<>name.txt"), L"");
        plan_finalize(p, 1, 1);
        CHECK(p->actions[0].conflicts[0].code == C_INVALID_NAME,
              L"非法字符应被拦下");
    }
    arena_free(a);

    /* --- 反复 finalize 不应让目标名漂移 --- */
    fresh_root(L"plan8");
    write_file(wp(L"a.txt"), "A");
    write_file(wp(L"x.txt"), "OLD");
    a = arena_new();
    {
        Plan *p = (Plan *)arena_zalloc(a, sizeof(Plan));
        plan_init(p, a, g_root, POLICY_RENAME, L"rename", L"");
        plan_add(p, L"rename", wp(L"a.txt"), wp(L"x.txt"), L"");
        plan_finalize(p, 1, 1);
        wchar_t first[MAX_PATH * 2];
        wcsncpy(first, lc_basename(p->actions[0].dst), MAX_PATH - 1);
        Policy seq[4] = {POLICY_ASK, POLICY_RENAME, POLICY_SKIP, POLICY_RENAME};
        for (int i = 0; i < 4; ++i) {
            p->policy = seq[i];
            plan_finalize(p, 1, 1);
        }
        CHECK(wcscmp(first, lc_basename(p->actions[0].dst)) == 0,
              L"反复切换策略后目标名漂移了：%ls -> %ls", first,
              lc_basename(p->actions[0].dst));
    }
    arena_free(a);
}

static void test_rename_rules(void) {
    wprintf(L"[2] 重命名规则\n");
    fresh_root(L"rule1");
    write_file(wp(L"photo.jpg"), "x");
    write_file(wp(L"note.txt"), "x");

    Arena *a = arena_new();
    WStr out;
    wstr_init(&out);

    /* 前缀后缀 */
    {
        RenameRule r; rename_rule_init(&r, a);
        r.prefix = arena_wcsdup(a, L"P_");
        r.suffix = arena_wcsdup(a, L"_end");
        CHECK(rename_new_name(wp(L"photo.jpg"), 1, &r, &out, NULL, 0) == 0, L"计算失败");
        CHECK(wcscmp(out.data, L"P_photo_end.jpg") == 0,
              L"前后缀结果错：%ls", out.data);
        rename_rule_free(&r);
    }
    /* 字面替换 */
    {
        RenameRule r; rename_rule_init(&r, a);
        r.find = arena_wcsdup(a, L"photo");
        r.replace = arena_wcsdup(a, L"照片");
        rename_new_name(wp(L"photo.jpg"), 1, &r, &out, NULL, 0);
        CHECK(wcscmp(out.data, L"照片.jpg") == 0, L"字面替换错：%ls", out.data);
        rename_rule_free(&r);
    }
    /* 正则 + 反向引用 */
    {
        RenameRule r; rename_rule_init(&r, a);
        r.find = arena_wcsdup(a, L"photo");
        r.replace = arena_wcsdup(a, L"pic");
        r.use_regex = 1;
        write_file(wp(L"IMG_0001.JPG"), "x");
        r.find = arena_wcsdup(a, L"IMG_(\\d+)");
        r.replace = arena_wcsdup(a, L"P\\1");
        rename_new_name(wp(L"IMG_0001.JPG"), 1, &r, &out, NULL, 0);
        CHECK(wcscmp(out.data, L"P0001.JPG") == 0, L"正则反向引用错：%ls", out.data);
        rename_rule_free(&r);
    }
    /* 模板 + 序号 */
    {
        RenameRule r; rename_rule_init(&r, a);
        r.template_ = arena_wcsdup(a, L"旅行_{n:03d}{ext}");
        rename_new_name(wp(L"photo.jpg"), 7, &r, &out, NULL, 0);
        CHECK(wcscmp(out.data, L"旅行_007.jpg") == 0, L"模板序号错：%ls", out.data);
        rename_rule_free(&r);
    }
    /* 模板不含 {ext} → 自动保留扩展名 */
    {
        RenameRule r; rename_rule_init(&r, a);
        r.template_ = arena_wcsdup(a, L"照片_{n:02d}");
        rename_new_name(wp(L"photo.jpg"), 3, &r, &out, NULL, 0);
        CHECK(wcscmp(out.data, L"照片_03.jpg") == 0, L"模板保留扩展名错：%ls", out.data);
        rename_rule_free(&r);
    }
    /* 非法模板变量应报错 */
    {
        RenameRule r; rename_rule_init(&r, a);
        r.template_ = arena_wcsdup(a, L"{nope}");
        wchar_t err[256];
        int rc = rename_new_name(wp(L"photo.jpg"), 1, &r, &out, err, 256);
        CHECK(rc != 0, L"非法模板变量应返回错误");
        CHECK(err[0] != 0, L"应给出错误说明");
        rename_rule_free(&r);
    }
    /* 大小写 */
    {
        RenameRule r; rename_rule_init(&r, a);
        r.case_mode = arena_wcsdup(a, L"lower");
        write_file(wp(L"MyFile.TXT"), "x");
        rename_new_name(wp(L"MyFile.TXT"), 1, &r, &out, NULL, 0);
        CHECK(wcscmp(out.data, L"myfile.TXT") == 0, L"小写转换错：%ls", out.data);
        rename_rule_free(&r);
    }
    /* 保留名/非法字符应被规整 */
    {
        RenameRule r; rename_rule_init(&r, a);
        r.prefix = arena_wcsdup(a, L"CON");
        r.suffix = arena_wcsdup(a, L"<bad>");
        rename_new_name(wp(L"photo.jpg"), 1, &r, &out, NULL, 0);
        CHECK(wcsstr(out.data, L"<") == NULL, L"非法字符未被规整：%ls", out.data);
        rename_rule_free(&r);
    }

    wstr_free(&out);
    arena_free(a);
}

static void test_execute_and_rollback(void) {
    wprintf(L"[3] 执行与一键回退（模块 1 的核心验收项）\n");
    fresh_root(L"rb1");
    write_file(wp(L"a.txt"), "AAA");
    write_file(wp(L"b.txt"), "BBB");

    Arena *a = arena_new();
    RenameRule r;
    rename_rule_init(&r, a);
    r.prefix = arena_wcsdup(a, L"X_");
    Plan *p = rename_build_plan(a, g_root, &r, POLICY_RENAME, NULL);

    ExecOptions opt;
    memset(&opt, 0, sizeof(opt));
    RunResult res;
    int rc = executor_run(p, &opt, &res);
    CHECK(rc == 0, L"执行应成功");
    CHECK(res.ok == 2, L"成功数应为 2，实际 %d", res.ok);
    CHECK(res.failed == 0, L"不应有失败");
    CHECK(res.journal_path != NULL, L"应生成回退日志");
    CHECK(is_file(wp(L"X_a.txt")), L"应存在 X_a.txt");
    CHECK(is_file(wp(L"X_b.txt")), L"应存在 X_b.txt");
    CHECK(!is_file(wp(L"a.txt")), L"a.txt 应已被改名");
    {
        char buf[64];
        read_file(wp(L"X_a.txt"), buf, sizeof(buf));
        CHECK(strcmp(buf, "AAA") == 0, L"内容应保持 AAA，实际 %s", buf);
    }

    /* 回退前先验证日志可读 */
    {
        JournalSteps js;
        CHECK(journal_load_steps(res.journal_path, &js) == 0, L"日志应可读");
        CHECK(js.n == 2, L"日志应有 2 条成功步骤，实际 %llu",
              (unsigned long long)js.n);
        CHECK(js.items[0].kind == STEP_MOVE, L"步骤类型应是 move");
        arena_free(js.arena);
    }

    /* 回退 */
    wchar_t jpath[MAX_PATH * 2];
    wcsncpy(jpath, res.journal_path, MAX_PATH * 2 - 1);
    run_result_free(&res);

    ExecOptions opt2;
    memset(&opt2, 0, sizeof(opt2));
    RunResult rb;
    rc = rollback_apply(jpath, &opt2, &rb);
    CHECK(rc == 0, L"回退应成功");
    CHECK(rb.failed == 0, L"回退不应有失败，实际 %d", rb.failed);
    CHECK(is_file(wp(L"a.txt")), L"回退后应恢复 a.txt");
    CHECK(is_file(wp(L"b.txt")), L"回退后应恢复 b.txt");
    CHECK(!is_file(wp(L"X_a.txt")), L"回退后 X_a.txt 应消失");
    {
        char buf[64];
        read_file(wp(L"a.txt"), buf, sizeof(buf));
        CHECK(strcmp(buf, "AAA") == 0, L"回退后内容应完好，实际 %s", buf);
    }

    /* 回退日志本身也能再回退 —— 即"重做" */
    wchar_t rbpath[MAX_PATH * 2];
    wcsncpy(rbpath, rb.journal_path, MAX_PATH * 2 - 1);
    run_result_free(&rb);

    ExecOptions opt3;
    memset(&opt3, 0, sizeof(opt3));
    RunResult redo;
    rc = rollback_apply(rbpath, &opt3, &redo);
    CHECK(rc == 0, L"重做应成功");
    CHECK(redo.failed == 0, L"重做不应有失败");
    CHECK(is_file(wp(L"X_a.txt")), L"重做后应回到 X_a.txt");
    run_result_free(&redo);

    rename_rule_free(&r);
    arena_free(a);
}

static void test_overwrite_roundtrip(void) {
    wprintf(L"[4] 覆盖式操作也可完整回退\n");
    fresh_root(L"ow1");
    write_file(wp(L"new.txt"), "NEW");
    write_file(wp(L"old.txt"), "OLD");

    Arena *a = arena_new();
    Plan *p = (Plan *)arena_zalloc(a, sizeof(Plan));
    plan_init(p, a, g_root, POLICY_OVERWRITE, L"rename", L"");
    plan_add(p, L"rename", wp(L"new.txt"), wp(L"old.txt"), L"");
    plan_finalize(p, 1, 1);
    CHECK(p->st_executable == 1, L"覆盖计划应可执行");
    CHECK(p->actions[0].n_steps == 2, L"覆盖应为 2 步");

    ExecOptions opt;
    memset(&opt, 0, sizeof(opt));
    RunResult res;
    executor_run(p, &opt, &res);
    CHECK(res.failed == 0, L"覆盖执行应成功");
    {
        char buf[64];
        read_file(wp(L"old.txt"), buf, sizeof(buf));
        CHECK(strcmp(buf, "NEW") == 0, L"覆盖后应是 NEW，实际 %s", buf);
    }

    wchar_t jpath[MAX_PATH * 2];
    wcsncpy(jpath, res.journal_path, MAX_PATH * 2 - 1);
    run_result_free(&res);

    ExecOptions opt2;
    memset(&opt2, 0, sizeof(opt2));
    RunResult rb;
    rollback_apply(jpath, &opt2, &rb);
    CHECK(rb.failed == 0, L"覆盖回退应成功，失败 %d", rb.failed);
    {
        char buf[64];
        read_file(wp(L"old.txt"), buf, sizeof(buf));
        CHECK(strcmp(buf, "OLD") == 0,
              L"回退后旧内容必须还原，实际 %s", buf);
    }
    CHECK(is_file(wp(L"new.txt")), L"回退后 new.txt 应回来了");
    run_result_free(&rb);
    arena_free(a);
}

static void test_rollback_safety(void) {
    wprintf(L"[5] 回退的安全边界（绝不覆盖用户的文件）\n");
    fresh_root(L"safe1");
    write_file(wp(L"a.txt"), "AAA");

    Arena *a = arena_new();
    RenameRule r;
    rename_rule_init(&r, a);
    r.prefix = arena_wcsdup(a, L"X_");
    Plan *p = rename_build_plan(a, g_root, &r, POLICY_RENAME, NULL);

    ExecOptions opt;
    memset(&opt, 0, sizeof(opt));
    RunResult res;
    executor_run(p, &opt, &res);

    /* 用户手工又造了一个 a.txt */
    write_file(wp(L"a.txt"), "手工文件");

    ExecOptions opt2;
    memset(&opt2, 0, sizeof(opt2));
    RunResult rb;
    rollback_apply(res.journal_path, &opt2, &rb);

    {
        char buf[64];
        read_file(wp(L"a.txt"), buf, sizeof(buf));
        CHECK(strcmp(buf, "手工文件") == 0,
              L"回退绝不能覆盖用户后来创建的文件！实际 %s", buf);
    }
    CHECK(is_file(wp(L"X_a.txt")), L"被占用的那一项应被跳过，原件留在原地");
    CHECK(rb.ok == 0, L"应跳过而不是执行，实际 ok=%d", rb.ok);

    run_result_free(&rb);
    run_result_free(&res);
    rename_rule_free(&r);
    arena_free(a);
}

static void test_nested_dir_cleanup(void) {
    wprintf(L"[6] 执行时新建的目录会被回退清理\n");
    fresh_root(L"dir1");
    write_file(wp(L"a.txt"), "AAA");

    Arena *a = arena_new();
    Plan *p = (Plan *)arena_zalloc(a, sizeof(Plan));
    plan_init(p, a, g_root, POLICY_RENAME, L"rename", L"");
    plan_add(p, L"rename", wp(L"a.txt"), wp(L"deep\\sub\\a.txt"), L"");
    plan_finalize(p, 1, 1);
    CHECK(p->actions[0].n_steps == 3, L"应生成 2 个建目录 + 1 个移动，实际 %llu",
          (unsigned long long)p->actions[0].n_steps);

    ExecOptions opt;
    memset(&opt, 0, sizeof(opt));
    RunResult res;
    executor_run(p, &opt, &res);
    CHECK(is_file(wp(L"deep\\sub\\a.txt")), L"文件应移入深层目录");

    wchar_t jpath[MAX_PATH * 2];
    wcsncpy(jpath, res.journal_path, MAX_PATH * 2 - 1);
    run_result_free(&res);

    ExecOptions opt2;
    memset(&opt2, 0, sizeof(opt2));
    RunResult rb;
    rollback_apply(jpath, &opt2, &rb);
    CHECK(rb.failed == 0, L"回退应成功，失败 %d", rb.failed);
    CHECK(is_file(wp(L"a.txt")), L"文件应回到原处");
    CHECK(!lc_is_dir(wp(L"deep")), L"执行时新建的目录应被清理掉");
    run_result_free(&rb);
    arena_free(a);
}

static void test_organize_roundtrip(void) {
    wprintf(L"[7] 整理分类与回退\n");
    fresh_root(L"org1");
    write_file(wp(L"a.jpg"), "IMG");
    write_file(wp(L"b.pdf"), "DOC");
    write_file(wp(L"c.txt"), "TXT");

    Arena *a = arena_new();
    OrganizeRule orule;
    organize_rule_init(&orule, a);
    orule.template_ = arena_wcsdup(a, L"{category}");

    Plan *p = organize_build_plan(a, g_root, &orule, POLICY_RENAME, NULL);
    CHECK(p->st_total == 3, L"应识别 3 个文件，实际 %llu",
          (unsigned long long)p->st_total);
    CHECK(p->st_executable == 3, L"应可执行 3 项");

    ExecOptions opt;
    memset(&opt, 0, sizeof(opt));
    RunResult res;
    executor_run(p, &opt, &res);
    CHECK(res.failed == 0, L"分类执行应成功");
    CHECK(is_file(wp(L"图片\\a.jpg")), L"a.jpg 应归入 图片\\");
    CHECK(is_file(wp(L"文档\\b.pdf")), L"b.pdf 应归入 文档\\");
    CHECK(is_file(wp(L"文档\\c.txt")), L"c.txt 应归入 文档\\");

    wchar_t jpath[MAX_PATH * 2];
    wcsncpy(jpath, res.journal_path, MAX_PATH * 2 - 1);
    run_result_free(&res);

    ExecOptions opt2;
    memset(&opt2, 0, sizeof(opt2));
    RunResult rb;
    rollback_apply(jpath, &opt2, &rb);
    CHECK(rb.failed == 0, L"分类回退应成功，失败 %d", rb.failed);
    CHECK(is_file(wp(L"a.jpg")), L"a.jpg 应回到根目录");
    CHECK(is_file(wp(L"b.pdf")), L"b.pdf 应回到根目录");
    {
        char buf[64];
        read_file(wp(L"a.jpg"), buf, sizeof(buf));
        CHECK(strcmp(buf, "IMG") == 0, L"内容应完好");
    }
    CHECK(!lc_is_dir(wp(L"图片")), L"新建的分类目录应被清理");

    run_result_free(&rb);
    organize_rule_free(&orule);
    arena_free(a);
}

static void test_category_lookup(void) {
    wprintf(L"[8] 分类判定\n");
    CHECK(wcscmp(organize_category_of(L"a.JPG"), L"图片") == 0, L"JPG 应归图片");
    CHECK(wcscmp(organize_category_of(L"a.cr2"), L"图片") == 0, L"CR2 应归图片");
    CHECK(wcscmp(organize_category_of(L"a.docx"), L"文档") == 0, L"docx 应归文档");
    CHECK(wcscmp(organize_category_of(L"a.mp4"), L"视频") == 0, L"mp4 应归视频");
    CHECK(wcscmp(organize_category_of(L"a.xyz"), L"其他") == 0, L"未知扩展名归其他");

    fresh_root(L"org2");
    write_file(wp(L"a.jpg"), "x");
    write_file(wp(L"b.jpg"), "x");
    write_file(wp(L"c.pdf"), "x");
    Arena *a = arena_new();
    CategoryStat *stats = NULL;
    size_t n = 0;
    int total = 0;
    long long size = 0;
    CHECK(organize_category_stats(g_root, 1, a, &stats, &n, &total, &size) == 0,
          L"统计应成功");
    CHECK(total == 3, L"总数应为 3，实际 %d", total);
    CHECK(n == 2, L"应有 2 个类别，实际 %llu", (unsigned long long)n);
    CHECK(wcscmp(stats[0].category, L"图片") == 0, L"数量最多的应是图片");
    CHECK(stats[0].count == 2, L"图片应有 2 个");
    for (size_t i = 0; i < n; ++i) free(stats[i].category);
    free(stats);
    arena_free(a);
}

static void test_no_partial_data_loss(void) {
    wprintf(L"[9] 单个失败不影响其它项，且整体仍可回退\n");
    fresh_root(L"mix1");
    write_file(wp(L"a.txt"), "A");
    write_file(wp(L"b.txt"), "B");
    write_file(wp(L"c.txt"), "C");

    Arena *a = arena_new();
    RenameRule r;
    rename_rule_init(&r, a);
    r.prefix = arena_wcsdup(a, L"R_");
    Plan *p = rename_build_plan(a, g_root, &r, POLICY_RENAME, NULL);

    /* 预览之后、执行之前，b.txt 被别的程序删掉了 */
    wchar_t *bp = lc_long_path(wp(L"b.txt"));
    DeleteFileW(bp);
    free(bp);

    ExecOptions opt;
    memset(&opt, 0, sizeof(opt));
    RunResult res;
    executor_run(p, &opt, &res);
    CHECK(res.ok == 2, L"其余 2 项应成功，实际 %d", res.ok);
    CHECK(res.failed == 1, L"b.txt 应失败，实际 %d", res.failed);
    CHECK(is_file(wp(L"R_a.txt")) && is_file(wp(L"R_c.txt")),
          L"成功的两项应已改名");

    wchar_t jpath[MAX_PATH * 2];
    wcsncpy(jpath, res.journal_path, MAX_PATH * 2 - 1);
    run_result_free(&res);

    ExecOptions opt2;
    memset(&opt2, 0, sizeof(opt2));
    RunResult rb;
    rollback_apply(jpath, &opt2, &rb);
    CHECK(rb.failed == 0, L"部分执行后照样能回退，失败 %d", rb.failed);
    CHECK(is_file(wp(L"a.txt")) && is_file(wp(L"c.txt")),
          L"回退后 a/c 应恢复原名");
    run_result_free(&rb);
    rename_rule_free(&r);
    arena_free(a);
}

/* ====================================================== main =========== */

int main(void) {
    SetConsoleOutputCP(CP_UTF8);

    /* 隔离数据目录，别污染真实用户的日志。
       放在 exe 同级的 testtmp 下 —— 有些受限环境不允许写 %TEMP%。 */
    wchar_t exe[MAX_PATH * 2], *slash;
    GetModuleFileNameW(NULL, exe, MAX_PATH * 2);
    slash = wcsrchr(exe, L'\\');
    if (slash) *slash = 0;                       /* ...\build\obj */
    slash = wcsrchr(exe, L'\\');
    if (slash) *slash = 0;                       /* ...\build */

    swprintf(g_home, MAX_PATH * 2, L"%ls\\testtmp", exe);
    lc_mkdir_p(g_home);
    if (!lc_is_dir(g_home)) {
        wprintf(L"无法创建测试目录：%ls\n", g_home);
        return 1;
    }
    SetEnvironmentVariableW(L"LOCALCRAFT_HOME", g_home);

    wprintf(L"=== LocalCraft 核心自检 ===\n");
    wprintf(L"测试目录：%ls\n\n", g_home);

    test_planner_conflicts();
    test_rename_rules();
    test_execute_and_rollback();
    test_overwrite_roundtrip();
    test_rollback_safety();
    test_nested_dir_cleanup();
    test_organize_roundtrip();
    test_category_lookup();
    test_no_partial_data_loss();

    wprintf(L"\n=== 通过 %d / 失败 %d ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
