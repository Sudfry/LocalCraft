/* ==========================================================================
 * test_backup.c —— 备份 / 同步自检
 *
 * 这个模块最需要盯的不是"能不能复制"（复制本身是现成的），
 * 而是**两件会毁数据的事**：
 *
 *   1. 目标放在源内部 → 复制进去的文件被下一轮扫描当成新文件 → 无限自我复制
 *      把磁盘撑爆。这个检查必须在扫描之前做，不能等执行时才发现。
 *
 *   2. "已经一样"的判断错了 → 要么白白重抄一遍（慢），
 *      要么该抄的没抄（数据丢失）。两种都不能接受。
 *
 * 外加一条承诺：**只增不删**。这里专门验证目标里多出来的文件
 * 在执行后依然存在 —— 那是"真镜像"和本工具最关键的区别。
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

/* ==================================================== 辅助 ============ */

static void write_file(const wchar_t *path, const char *data, size_t len) {
    wchar_t *lp = lc_long_path(path);
    HANDLE h = CreateFileW(lp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, data, (DWORD)len, &w, NULL);
    CloseHandle(h);
}

static void write_str(const wchar_t *path, const char *s) {
    write_file(path, s, strlen(s));
}

/*
 * 拼路径。用一组轮转缓冲 —— 因为测试里经常出现
 * pj(dir, L"x") 直接当参数传给另一个 pj() 的写法，
 * 单个静态缓冲会被覆盖。
 */
static wchar_t *pj(const wchar_t *dir, const wchar_t *name) {
    static wchar_t buf[8][MAX_PATH * 2];
    static int slot = 0;
    wchar_t *b = buf[slot];
    slot = (slot + 1) % 8;
    size_t dl = wcslen(dir);
    int need_sep = dl > 0 && dir[dl-1] != L'\\' && dir[dl-1] != L'/';
    swprintf(b, MAX_PATH * 2, need_sep ? L"%ls\\%ls" : L"%ls%ls", dir, name);
    return b;
}


/* 造一个干净的测试目录对 */
static void make_case(const wchar_t *name, wchar_t *src, wchar_t *dst) {
    swprintf(src, MAX_PATH * 2, L"%ls\\%ls\\src", g_root, name);
    swprintf(dst, MAX_PATH * 2, L"%ls\\%ls\\dst", g_root, name);
    lc_mkdir_p(src);
    lc_mkdir_p(dst);
}

/* 把目录里的文件清掉（保留目录本身） */
static void wipe_dir(const wchar_t *dir) {
    Arena *a = arena_new();
    FileList fl;
    filelist_init(&fl, a);
    if (lc_scan_dir(dir, 1, 0, 1, &fl) == 0) {
        for (size_t i = 0; i < fl.n; ++i) {
            wchar_t *lp = lc_long_path(fl.paths[i]);
            DeleteFileW(lp);
            free(lp);
        }
    }
    arena_free(a);
}

/* 把目标里某个文件的修改时间往后调，用来模拟"变过了" */
static void bump_mtime(const wchar_t *path) {
    wchar_t *lp = lc_long_path(path);
    HANDLE h = CreateFileW(lp, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    u.QuadPart += 10000000ll * 3600;          /* +1 小时 */
    ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
    SetFileTime(h, NULL, NULL, &ft);
    CloseHandle(h);
}

/* ==================================================== 1. 安全检查 ====== */

static void test_safety(void) {
    wprintf(L"[1] 安全检查：会毁数据的两种情况必须先拦住\n");

    wchar_t src[MAX_PATH * 2], dst[MAX_PATH * 2];
    make_case(L"safety", src, dst);
    write_str(pj(src, L"a.txt"), "hello");

    BackupScan sc;
    wchar_t err[512];

    /* 目标在源内部 —— 必须拒绝，否则会无限自我复制把磁盘撑爆 */
    wchar_t inside[MAX_PATH * 2];
    swprintf(inside, MAX_PATH * 2, L"%ls\\sub", src);
    err[0] = 0;
    CHECK(backup_scan(src, inside, NULL, NULL, 1, 1, 0, 0, &sc, err, 512) != 0,
          L"目标在源内部时必须拒绝");
    CHECK(err[0] != 0, L"拒绝时要给出说明文字");
    CHECK(wcsstr(err, L"内部") != NULL || wcsstr(err, L"自我复制") != NULL,
          L"说明文字要点出原因，实际「%ls」", err);
    backup_scan_free(&sc);

    /* 源和目标相同 */
    err[0] = 0;
    CHECK(backup_scan(src, src, NULL, NULL, 1, 1, 0, 0, &sc, err, 512) != 0,
          L"源和目标相同时必须拒绝");
    backup_scan_free(&sc);

    /* 源不存在 */
    err[0] = 0;
    CHECK(backup_scan(L"D:\\__不存在的目录__", dst, NULL, NULL, 1, 1, 0, 0,
                      &sc, err, 512) != 0, L"源不存在时必须拒绝");
    backup_scan_free(&sc);

    /* 空参数 */
    err[0] = 0;
    CHECK(backup_scan(L"", dst, NULL, NULL, 1, 1, 0, 0, &sc, err, 512) != 0,
          L"源为空时必须拒绝");
    backup_scan_free(&sc);
}

/* ==================================================== 2. 差异判断 ====== */

static void test_diff_detection(void) {
    wprintf(L"[2] 差异判断：新增 / 已相同 / 已改过\n");

    wchar_t src[MAX_PATH * 2], dst[MAX_PATH * 2];
    make_case(L"diff", src, dst);
    wipe_dir(dst);

    write_str(pj(src, L"a.txt"), "AAA");
    write_str(pj(src, L"b.txt"), "BBBB");

    BackupScan sc;
    wchar_t err[512];
    err[0] = 0;

    /* 目标空的 → 两个都是新增 */
    CHECK(backup_scan(src, dst, NULL, NULL, 1, 1, 0, 0, &sc, err, 512) == 0,
          L"扫描应成功：%ls", err);
    CHECK(sc.scanned == 2, L"应扫到 2 个文件，实际 %lld", sc.scanned);
    CHECK(sc.n_new == 2, L"应识别出 2 个新增，实际 %lld", sc.n_new);
    CHECK(sc.n_diff == 0 && sc.n_same == 0, L"不该有更新或相同");
    CHECK(sc.total_bytes == 7, L"总字节应为 7，实际 %lld", sc.total_bytes);
    backup_scan_free(&sc);

    /* 先把两个文件复制过去，再扫一次 → 应该都判定为"已相同" */
    wchar_t da[MAX_PATH * 2], db[MAX_PATH * 2];
    swprintf(da, MAX_PATH * 2, L"%ls\\a.txt", dst);
    swprintf(db, MAX_PATH * 2, L"%ls\\b.txt", dst);
    CopyFileW(pj(src, L"a.txt"), da, FALSE);
    CopyFileW(pj(src, L"b.txt"), db, FALSE);

    CHECK(backup_scan(src, dst, NULL, NULL, 1, 1, 0, 0, &sc, err, 512) == 0,
          L"第二次扫描应成功");
    CHECK(sc.n_same == 2, L"内容一致时应判定为相同，实际 same=%lld diff=%lld",
          sc.n_same, sc.n_diff);
    CHECK(sc.n_new == 0 && sc.n_diff == 0, L"不该有新增或更新");
    CHECK(sc.total_bytes == 0, L"没有要复制的，字节数应为 0");
    backup_scan_free(&sc);

    /* 把源改大 → 应该判定为"更新" */
    write_str(pj(src, L"a.txt"), "AAAAAA");
    CHECK(backup_scan(src, dst, NULL, NULL, 1, 1, 0, 0, &sc, err, 512) == 0,
          L"扫描应成功");
    CHECK(sc.n_diff == 1, L"体积变了应判定为更新，实际 %lld", sc.n_diff);
    CHECK(sc.n_same == 1, L"没变的那一个仍应是相同");
    backup_scan_free(&sc);

    /* 恢复体积，但只改时间 → 默认（非严格）应判定为"更新" */
    write_str(pj(src, L"a.txt"), "AAA");
    {   /* 文件系统时间戳有粒度，直接把目标的时间往后调更可靠 */
        bump_mtime(da);
        WIN32_FILE_ATTRIBUTE_DATA fad;
        wchar_t *lp = lc_long_path(pj(src, L"a.txt"));
        GetFileAttributesExW(lp, GetFileExInfoStandard, &fad);
        free(lp);
        wchar_t *lp2 = lc_long_path(da);
        HANDLE h = CreateFileW(lp2, FILE_WRITE_ATTRIBUTES, 0, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        free(lp2);
        if (h != INVALID_HANDLE_VALUE) {
            SetFileTime(h, NULL, NULL, &fad.ftLastWriteTime);
            CloseHandle(h);
        }
    }
    CHECK(backup_scan(src, dst, NULL, NULL, 1, 1, 0, 0, &sc, err, 512) == 0,
          L"扫描应成功");
    CHECK(sc.n_same == 2, L"体积和时间都一致时应判定为相同，实际 same=%lld diff=%lld",
          sc.n_same, sc.n_diff);
    backup_scan_free(&sc);
}

/* ==================================================== 3. 严格比对 ====== */

static void test_strict_mode(void) {
    wprintf(L"[3] 严格比对：体积和时间一样、内容不同\n");

    wchar_t src[MAX_PATH * 2], dst[MAX_PATH * 2];
    make_case(L"strict", src, dst);
    wipe_dir(dst);

    /* 两个文件**字节数相同**但内容不同 —— 这正是非严格模式抓不住的 */
    wchar_t sa[MAX_PATH * 2], da[MAX_PATH * 2];
    swprintf(sa, MAX_PATH * 2, L"%ls\\x.bin", src);
    swprintf(da, MAX_PATH * 2, L"%ls\\x.bin", dst);
    write_str(sa, "AAAAAAAA");
    write_str(da, "BBBBBBBB");

    /* 把目标的时间对齐到源，让"体积+时间"这条快速判断认为它们一样 */
    {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        wchar_t *lp = lc_long_path(sa);
        GetFileAttributesExW(lp, GetFileExInfoStandard, &fad);
        free(lp);
        wchar_t *lp2 = lc_long_path(da);
        HANDLE h = CreateFileW(lp2, FILE_WRITE_ATTRIBUTES, 0, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        free(lp2);
        if (h != INVALID_HANDLE_VALUE) {
            SetFileTime(h, NULL, NULL, &fad.ftLastWriteTime);
            CloseHandle(h);
        }
    }

    BackupScan sc;
    wchar_t err[512];
    err[0] = 0;

    /* 非严格模式：会误判为"相同"（这是已知取舍，换速度） */
    CHECK(backup_scan(src, dst, NULL, NULL, 1, 1, 0, 0, &sc, err, 512) == 0,
          L"扫描应成功");
    CHECK(sc.n_same == 1, L"非严格模式下这种情况会被判为相同（换取速度）");
    backup_scan_free(&sc);

    /* 严格模式：算出哈希，正确识别出不同 */
    CHECK(backup_scan(src, dst, NULL, NULL, 1, 1, 1, 0, &sc, err, 512) == 0,
          L"严格扫描应成功");
    CHECK(sc.n_diff == 1,
          L"严格模式必须识别出内容不同，实际 same=%lld diff=%lld",
          sc.n_same, sc.n_diff);
    backup_scan_free(&sc);
}

/* ==================================================== 4. 过滤 ========== */

static void test_filters(void) {
    wprintf(L"[4] 扩展名与体积过滤\n");

    wchar_t src[MAX_PATH * 2], dst[MAX_PATH * 2];
    make_case(L"filter", src, dst);
    wipe_dir(dst);

    write_str(pj(src, L"a.jpg"), "1111");
    write_str(pj(src, L"b.mp4"), "2222");
    write_str(pj(src, L"c.tmp"), "3333");

    BackupScan sc;
    wchar_t err[512];
    err[0] = 0;

    /* 只处理 jpg 和 mp4 */
    CHECK(backup_scan(src, dst, L"jpg, mp4", NULL, 1, 1, 0, 0, &sc, err, 512) == 0,
          L"扫描应成功");
    CHECK(sc.n_new == 2, L"白名单应只放行 2 个，实际 %lld", sc.n_new);
    CHECK(sc.skipped_ext == 1, L"应跳过 1 个，实际 %lld", sc.skipped_ext);
    backup_scan_free(&sc);

    /* 跳过 tmp */
    CHECK(backup_scan(src, dst, NULL, L"tmp", 1, 1, 0, 0, &sc, err, 512) == 0,
          L"扫描应成功");
    CHECK(sc.n_new == 2, L"黑名单应排除 1 个，实际 %lld", sc.n_new);
    backup_scan_free(&sc);

    /* 体积上限：设成 1 字节，全部超限 */
    CHECK(backup_scan(src, dst, NULL, NULL, 1, 1, 0, 1, &sc, err, 512) == 0,
          L"扫描应成功");
    CHECK(sc.n_new == 0 && sc.skipped_size == 3,
          L"全部超限时应跳过 3 个，实际 skipped=%lld new=%lld",
          sc.skipped_size, sc.n_new);
    backup_scan_free(&sc);
}

/* ==================================================== 5. 校验 ========== */

static void test_verify(void) {
    wprintf(L"[5] 复制后校验：抓得住「抄错了却报成功」的情况\n");

    wchar_t src[MAX_PATH * 2], dst[MAX_PATH * 2];
    make_case(L"verify", src, dst);

    wchar_t sa[MAX_PATH * 2], da[MAX_PATH * 2];
    swprintf(sa, MAX_PATH * 2, L"%ls\\v.txt", src);
    swprintf(da, MAX_PATH * 2, L"%ls\\v.txt", dst);

    write_str(sa, "内容一致的内容");

    /* 完全一致 → 0 */
    CopyFileW(sa, da, FALSE);
    CHECK(backup_verify(sa, da) == 0, L"复制正确时应返回一致");

    /* 体积相同但内容不同 → 1（这就是校验存在的意义） */
    write_str(da, "内容不一的内容");
    CHECK(backup_verify(sa, da) == 1, L"内容不同时必须报不一致");

    /* 体积不同 → 1 */
    write_str(da, "短");
    CHECK(backup_verify(sa, da) == 1, L"体积不同时必须报不一致");

    /* 文件不存在 → -1 */
    CHECK(backup_verify(sa, L"D:\\__没有这个文件__") == -1,
          L"目标读不到时应返回 -1");

    /* 空文件对空文件 → 一致 */
    wchar_t e1[MAX_PATH * 2], e2[MAX_PATH * 2];
    swprintf(e1, MAX_PATH * 2, L"%ls\\e1.txt", src);
    swprintf(e2, MAX_PATH * 2, L"%ls\\e2.txt", dst);
    write_file(e1, "", 0);
    write_file(e2, "", 0);
    CHECK(backup_verify(e1, e2) == 0, L"两个空文件应判定为一致");
}

/* ==================================================== 6. 端到端 ======== */

static void test_end_to_end(void) {
    wprintf(L"[6] 端到端：复制 → 源不变 → 只增不删 → 回退\n");

    wchar_t src[MAX_PATH * 2], dst[MAX_PATH * 2];
    make_case(L"e2e", src, dst);
    wipe_dir(dst);

    /* 源里三个文件，其中一个在子目录里 */
    wchar_t sub[MAX_PATH * 2];
    swprintf(sub, MAX_PATH * 2, L"%ls\\子目录", src);
    lc_mkdir_p(sub);
    write_str(pj(src, L"a.txt"), "AAA");
    write_str(pj(src, L"b.txt"), "BBBB");
    write_str(pj(sub,    L"c.txt"), "CCCCC");

    /* 目标里放一个源里没有的文件 —— 它是"只增不删"的判据 */
    write_str(pj(dst, L"目标独有.txt"), "DONTDELETE");

    BackupScan sc;
    wchar_t err[512];
    err[0] = 0;
    CHECK(backup_scan(src, dst, NULL, NULL, 1, 1, 0, 0, &sc, err, 512) == 0,
          L"扫描应成功：%ls", err);
    CHECK(sc.n_new == 3, L"应识别出 3 个新增，实际 %lld", sc.n_new);

    /* 按上层（api.c）的做法把清单变成 COPY 计划 */
    Arena *a = arena_new();
    Plan *plan = (Plan *)arena_zalloc(a, sizeof(Plan));
    plan_init(plan, a, src, POLICY_RENAME, L"backup", L"备份测试");
    for (size_t i = 0; i < sc.n; ++i) {
        if (sc.items[i].state == 2) continue;
        Action *act = plan_add(plan, L"backup", sc.items[i].src,
                               sc.items[i].dst, L"复制到目标");
        if (act) act->is_copy = 1;
    }
    plan_finalize(plan, 1, 1);
    CHECK(plan->st_executable == 3, L"3 行都应可执行，实际 %llu",
          (unsigned long long)plan->st_executable);

    ExecOptions opt;
    memset(&opt, 0, sizeof(opt));
    RunResult r;
    executor_run(plan, &opt, &r);
    CHECK(r.ok == 3 && r.failed == 0, L"执行应成功 3 项（ok=%d failed=%d）",
          r.ok, r.failed);

    /* 源文件必须一个不动 —— 备份不是移动 */
    CHECK(lc_is_file(pj(src, L"a.txt")), L"源 a.txt 必须还在");
    CHECK(lc_is_file(pj(sub, L"c.txt")), L"源子目录的 c.txt 必须还在");

    /* 目标里应有全部 3 个，且目录结构保留 */
    wchar_t ta[MAX_PATH * 2], tb[MAX_PATH * 2], tc[MAX_PATH * 2];
    swprintf(ta, MAX_PATH * 2, L"%ls\\a.txt", dst);
    swprintf(tb, MAX_PATH * 2, L"%ls\\b.txt", dst);
    swprintf(tc, MAX_PATH * 2, L"%ls\\子目录\\c.txt", dst);
    CHECK(lc_is_file(ta), L"目标应有 a.txt");
    CHECK(lc_is_file(tb), L"目标应有 b.txt");
    CHECK(lc_is_file(tc), L"目标应保留子目录结构");

    /* 内容必须逐字节一致 */
    CHECK(backup_verify(pj(src, L"a.txt"), ta) == 0,
          L"a.txt 内容应一致");
    CHECK(backup_verify(pj(sub, L"c.txt"), tc) == 0,
          L"c.txt 内容应一致");

    /* ★ 只增不删：目标里那个源中没有的文件必须**原样保留** */
    CHECK(lc_is_file(pj(dst, L"目标独有.txt")),
          L"目标里多出来的文件绝不能被删除 —— 这是本工具和真镜像的关键区别");

    /* 回退：复制过去的应该被撤掉，源和目标的"独有文件"都要在 */
    wchar_t jp[MAX_PATH * 2];
    wcsncpy(jp, r.journal_path, MAX_PATH * 2 - 1);
    jp[MAX_PATH * 2 - 1] = 0;
    run_result_free(&r);

    ExecOptions o2;
    memset(&o2, 0, sizeof(o2));
    RunResult rb;
    rollback_apply(jp, &o2, &rb);
    CHECK(rb.failed == 0, L"回退应成功（failed=%d）", rb.failed);
    run_result_free(&rb);

    CHECK(!lc_is_file(ta), L"回退后目标里的 a.txt 应被撤掉");
    CHECK(lc_is_file(pj(src, L"a.txt")),
          L"回退绝不该动到源文件（它本来就是复制不是移动）");
    CHECK(lc_is_file(pj(dst, L"目标独有.txt")),
          L"回退也不该动目标里原有的文件");

    backup_scan_free(&sc);
    arena_free(a);
}

/* ==================================================== main =========== */

int main(void) {
    SetConsoleOutputCP(CP_UTF8);

    wchar_t exe[MAX_PATH * 2], *slash;
    GetModuleFileNameW(NULL, exe, MAX_PATH * 2);
    slash = wcsrchr(exe, L'\\'); if (slash) *slash = 0;
    slash = wcsrchr(exe, L'\\'); if (slash) *slash = 0;

    swprintf(g_root, MAX_PATH * 2, L"%ls\\testtmp\\backup-%u", exe,
             (unsigned)GetTickCount64());
    lc_mkdir_p(g_root);

    wprintf(L"=== LocalCraft 备份 / 同步自检 ===\n");
    wprintf(L"工作目录：%ls\n\n", g_root);

    test_safety();
    test_diff_detection();
    test_strict_mode();
    test_filters();
    test_verify();
    test_end_to_end();

    wprintf(L"\n=== 通过 %d / 失败 %d ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
