/* ==========================================================================
 * executor.c —— 顺序执行 Plan，逐步把事实写进日志
 *
 * 安全约定：
 *   · 目标已存在时**绝不覆盖**（除非计划里显式带了回收站步骤）。
 *     宁可这一步失败，也不丢用户文件。
 *   · 单个动作失败不影响后续动作（除非 stop_on_error）。
 *   · 失败本身也记进日志，回退时一并处理。
 * ========================================================================== */
#include "lc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ================================================== 错误消息 ========== */

static void win_error(DWORD code, wchar_t *buf, size_t cch) {
    wchar_t *msg = NULL;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                             FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0,
                             (LPWSTR)&msg, 0, NULL);
    if (n && msg) {
        while (n > 0 && (msg[n-1] == L'\r' || msg[n-1] == L'\n' || msg[n-1] == L' '))
            msg[--n] = 0;
        swprintf(buf, cch, L"%ls（错误码 %lu）", msg, (unsigned long)code);
        LocalFree(msg);
    } else {
        swprintf(buf, cch, L"系统错误码 %lu", (unsigned long)code);
    }
}

/* ================================================== 底层操作 ========== */

static long long size_of(const wchar_t *path);

/* 递归复制目录 */
static int copy_tree(const wchar_t *src, const wchar_t *dst,
                     wchar_t *err, size_t errcch) {
    if (lc_mkdir_p(dst) != 0 && !lc_is_dir(dst)) {
        swprintf(err, errcch, L"无法创建目录：%ls", dst);
        return -1;
    }
    WStr pat;
    wstr_init(&pat);
    wstr_puts(&pat, src);
    if (pat.len && !lc_is_sep(pat.data[pat.len - 1])) wstr_putc(&pat, L'\\');
    wstr_puts(&pat, L"*");
    wchar_t *lp = lc_long_path(pat.data);
    wstr_free(&pat);
    if (!lp) return -1;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(lp, &fd);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) return 0;

    int rc = 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        WStr s, d;
        wstr_init(&s); wstr_init(&d);
        wstr_puts(&s, src); wstr_putc(&s, L'\\'); wstr_puts(&s, fd.cFileName);
        wstr_puts(&d, dst); wstr_putc(&d, L'\\'); wstr_puts(&d, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (copy_tree(s.data, d.data, err, errcch) != 0) rc = -1;
        } else {
            wchar_t *ls = lc_long_path(s.data), *ld = lc_long_path(d.data);
            if (!ls || !ld || !CopyFileW(ls, ld, TRUE)) {
                win_error(GetLastError(), err, errcch);
                rc = -1;
            }
            free(ls); free(ld);
        }
        wstr_free(&s); wstr_free(&d);
        if (rc != 0) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return rc;
}

static int do_move(const wchar_t *src, const wchar_t *dst,
                   wchar_t *err, size_t errcch, long long *size_out) {
    if (!lc_exists(src)) {
        swprintf(err, errcch, L"源不存在：%ls", src);
        return -1;
    }
    int case_only = lc_same_path(src, dst);
    if (lc_exists(dst) && !case_only) {
        swprintf(err, errcch, L"目标已存在，拒绝覆盖：%ls", dst);
        return -1;
    }

    long long sz = size_of(src);
    wchar_t *parent = lc_dirname(dst);
    if (parent) {
        if (!lc_is_dir(parent)) lc_mkdir_p(parent);
        free(parent);
    }

    wchar_t *ls = lc_long_path(src);
    wchar_t *ld = lc_long_path(dst);
    if (!ls || !ld) { free(ls); free(ld); swprintf(err, errcch, L"路径过长或非法"); return -1; }

    /* MOVEFILE_COPY_ALLOWED：跨盘时自动退化为复制+删除 */
    if (!MoveFileExW(ls, ld, MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING)) {
        DWORD e = GetLastError();
        if (case_only && e == ERROR_ALREADY_EXISTS) {
            /* 仅大小写改名：先改成一个临时名字，再改成目标 */
            WStr tmp;
            wstr_init(&tmp);
            wstr_puts(&tmp, dst);
            wstr_puts(&tmp, L".~lc_tmp~");
            wchar_t *lt = lc_long_path(tmp.data);
            wstr_free(&tmp);
            if (lt && MoveFileExW(ls, lt, MOVEFILE_REPLACE_EXISTING) &&
                MoveFileExW(lt, ld, MOVEFILE_REPLACE_EXISTING)) {
                free(lt); free(ls); free(ld);
                if (size_out) *size_out = sz;
                return 0;
            }
            free(lt);
            e = GetLastError();
        }
        win_error(e, err, errcch);
        free(ls); free(ld);
        return -1;
    }
    free(ls); free(ld);
    if (size_out) *size_out = sz;
    return 0;
}

static int do_copy(const wchar_t *src, const wchar_t *dst,
                   wchar_t *err, size_t errcch, long long *size_out) {
    if (!lc_exists(src)) {
        swprintf(err, errcch, L"源不存在：%ls", src);
        return -1;
    }
    if (lc_exists(dst)) {
        swprintf(err, errcch, L"目标已存在，拒绝覆盖：%ls", dst);
        return -1;
    }
    wchar_t *parent = lc_dirname(dst);
    if (parent) {
        if (!lc_is_dir(parent)) lc_mkdir_p(parent);
        free(parent);
    }

    int rc = 0;
    if (lc_is_dir(src)) {
        rc = copy_tree(src, dst, err, errcch);
    } else {
        wchar_t *ls = lc_long_path(src), *ld = lc_long_path(dst);
        if (!ls || !ld || !CopyFileW(ls, ld, TRUE)) {
            win_error(GetLastError(), err, errcch);
            rc = -1;
        }
        free(ls); free(ld);
    }
    if (rc == 0 && size_out) *size_out = size_of(dst);
    return rc;
}

static long long size_of(const wchar_t *path) { return lc_size_of(path); }

/* MoveFileExW 的长路径包装（RemoveDirectoryW 需要 \\?\ 前缀） */
static int remove_dir_long(const wchar_t *path) {
    wchar_t *lp = lc_long_path(path);
    if (!lp) return 0;
    BOOL ok = RemoveDirectoryW(lp);
    free(lp);
    return ok ? 1 : 0;
}

/* ================================================== 单步执行 ========== */

static int run_step(Step *s) {
    wchar_t err[1024];
    err[0] = 0;

    switch (s->kind) {
    case STEP_MKDIR:
        if (!lc_is_dir(s->dst)) {
            if (lc_mkdir_p(s->dst) != 0 && !lc_is_dir(s->dst)) {
                win_error(GetLastError(), err, 1024);
                s->error = _wcsdup(err);
                return -1;
            }
        }
        return 0;

    case STEP_RMDIR:
        if (!lc_is_dir(s->dst)) {
            if (s->lenient) { s->skipped = 1; return 0; }
            s->error = _wcsdup(L"目录不存在");
            return -1;
        }
        if (!remove_dir_long(s->dst)) {
            DWORD e = GetLastError();
            if (s->lenient) {
                s->skipped = 1;
                return 0;
            }
            win_error(e, err, 1024);
            s->error = _wcsdup(err);
            return -1;
        }
        return 0;

    case STEP_MOVE:
        if (do_move(s->src, s->dst, err, 1024, &s->size) != 0) {
            s->error = _wcsdup(err);
            return -1;
        }
        return 0;

    case STEP_COPY:
        if (do_copy(s->src, s->dst, err, 1024, &s->size) != 0) {
            s->error = _wcsdup(err);
            return -1;
        }
        return 0;

    case STEP_CREATE:
        /* 有生成规格就现在生成（导出 / 内置格式转换都走这里） */
        if (s->generator && s->generator[0]) {
            if (lc_generate_artifact(s->generator, s->src, s->dst, err, 1024) != 0) {
                if (!err[0])
                    swprintf(err, 1024, L"生成失败：%ls", s->generator);
                s->error = _wcsdup(err);
                return -1;
            }
        }
        if (!lc_exists(s->dst)) {
            swprintf(err, 1024, L"预期产出的文件不存在：%ls", s->dst);
            s->error = _wcsdup(err);
            return -1;
        }
        s->size = size_of(s->dst);
        return 0;
    }
    s->error = _wcsdup(L"未知步骤类型");
    return -1;
}

/* ================================================== RunResult ========= */

void run_result_init(RunResult *r) { memset(r, 0, sizeof(*r)); }

void run_result_free(RunResult *r) {
    if (!r) return;
    for (size_t i = 0; i < r->n_errors; ++i) free(r->errors[i]);
    free(r->errors);
    r->errors = NULL;
    r->n_errors = 0;
    if (r->journal_path) { free(r->journal_path); r->journal_path = NULL; }
}

static void result_add_error(RunResult *r, const wchar_t *msg) {
    if (!msg) return;
    wchar_t **ne = (wchar_t **)realloc(r->errors, (r->n_errors + 1) * sizeof(wchar_t *));
    if (!ne) return;
    r->errors = ne;
    r->errors[r->n_errors++] = _wcsdup(msg);
}

/* ================================================== 执行主循环 ======== */

static int run_action(Plan *p, Action *a, JournalWriter *jw) {
    a->status = ST_PENDING;
    if (a->error) { free(a->error); a->error = NULL; }
    journal_action_begin(jw, a);

    int done_any = 0;
    for (size_t seq = 0; seq < a->n_steps; ++seq) {
        Step *s = &a->steps[seq];
        s->ok = -1;
        s->skipped = 0;
        if (s->error) { free(s->error); s->error = NULL; }

        if (run_step(s) != 0) {
            s->ok = 0;
            a->status = done_any ? ST_PARTIAL : ST_FAILED;
            a->error = s->error ? _wcsdup(s->error) : _wcsdup(L"未知错误");
            journal_step(jw, a, (int)seq, s);
            journal_action_end(jw, a);
            return -1;
        }
        if (s->skipped) {
            /* 该步不适用（如回退时目录已非空）。不写日志 ——
               日志里只放"真正发生过的事"，否则重做会多做一次。 */
            continue;
        }
        s->ok = 1;
        done_any = 1;
        journal_step(jw, a, (int)seq, s);
    }
    a->status = done_any ? ST_DONE : ST_SKIPPED;
    journal_action_end(jw, a);
    return done_any ? 0 : 1;
}

/* 预演：只读校验，不碰任何文件 */
static void dry_run(Plan *p, RunResult *r) {
    for (size_t i = 0; i < p->n_actions; ++i) {
        Action *a = &p->actions[i];
        if (!action_executable(a)) { r->skipped++; continue; }
        wchar_t problem[1024];
        problem[0] = 0;

        for (size_t k = 0; k < a->n_steps; ++k) {
            Step *s = &a->steps[k];
            if (s->kind == STEP_MOVE || s->kind == STEP_COPY) {
                if (!lc_exists(s->src)) {
                    swprintf(problem, 1024, L"源不存在：%ls", s->src);
                    break;
                }
                int is_trash = s->note && wcsstr(s->note, L"回收站") != NULL;
                if (lc_exists(s->dst) && !is_trash &&
                    !lc_same_path(s->src, s->dst)) {
                    swprintf(problem, 1024, L"目标已被占用：%ls", s->dst);
                    break;
                }
            } else if (s->kind == STEP_CREATE && lc_exists(s->dst)) {
                swprintf(problem, 1024, L"输出已存在：%ls", s->dst);
                break;
            }
        }
        if (problem[0]) {
            a->status = ST_SKIPPED;
            if (a->error) free(a->error);
            a->error = _wcsdup(problem);
            result_add_error(r, problem);
            r->skipped++;
        } else {
            a->status = ST_DONE;
            r->ok++;
        }
    }
    r->total = (int)p->n_actions;
}

int executor_run(Plan *plan, const ExecOptions *opt, RunResult *result) {
    run_result_init(result);
    ULONGLONG t0 = GetTickCount64();

    if (opt->dry_run) {
        dry_run(plan, result);
        result->seconds = (double)(GetTickCount64() - t0) / 1000.0;
        return 0;
    }

    wchar_t *jpath = NULL;
    if (opt->journal_path) {
        jpath = _wcsdup(opt->journal_path);
    } else {
        wchar_t *pidw = lc_utf8_to_wcs(plan->plan_id);
        jpath = journal_path_for(pidw ? pidw : L"plan", opt->is_rollback);
        free(pidw);
    }
    result->journal_path = _wcsdup(jpath);

    JournalWriter jw;
    memset(&jw, 0, sizeof(jw));
    jw.path = jpath;
    jw.is_rollback_hint = opt->is_rollback;
    jw.plan_id = plan->plan_id;
    jw.file = journal_open(jpath);
    if (jw.file == INVALID_HANDLE_VALUE) {
        wchar_t msg[512];
        swprintf(msg, 512, L"无法写入日志文件：%ls", jpath);
        result_add_error(result, msg);
        result->failed = 1;
        result->total = (int)plan->n_actions;
        free(jpath);
        return -1;
    }

    journal_write_header(&jw, plan);

    int total_todo = 0;
    for (size_t i = 0; i < plan->n_actions; ++i)
        if (action_executable(&plan->actions[i])) total_todo++;

    int done = 0;
    for (size_t i = 0; i < plan->n_actions; ++i) {
        Action *a = &plan->actions[i];
        if (!action_executable(a)) { result->skipped++; continue; }

        if (opt->cancel_check && opt->cancel_check(opt->cancel_ud)) {
            result->cancelled = 1;
            break;
        }
        if (opt->progress)
            opt->progress(opt->progress_ud, done, total_todo, a->src);

        int rc = run_action(plan, a, &jw);
        if (rc == 0) result->ok++;
        else if (rc < 0) {
            result->failed++;
            if (a->error) result_add_error(result, a->error);
            if (opt->stop_on_error) {
                result->cancelled = 1;
                done++;
                break;
            }
        } else {
            result->skipped++;
        }
        done++;
        if (opt->progress)
            opt->progress(opt->progress_ud, done, total_todo, L"");
    }

    const wchar_t *status = result->cancelled ? L"cancelled"
                          : result->failed ? L"completed_with_errors"
                          : L"completed";
    journal_footer(&jw, status, result->ok, result->failed, result->skipped, L"");
    journal_close(&jw);

    result->total = (int)plan->n_actions;
    result->seconds = (double)(GetTickCount64() - t0) / 1000.0;
    free(jpath);
    return result->failed ? -1 : 0;
}
