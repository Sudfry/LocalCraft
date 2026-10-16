/* ==========================================================================
 * rollback.c —— 回退引擎：读日志 → 逆序取反 → 生成回退计划 → 预览 → 执行
 *
 * 核心思想：**回退不是"撤销栈"，而是对日志的一次重新演算。**
 *   · 日志记录的是"实际发生过的步骤"（失败步骤之前的全部成功步骤）
 *   · 把成功步骤倒过来，每个步骤取逆操作，就得到回退计划
 *   · 回退本身也写一份新日志 → 回退可以再被回退（即"重做"）
 *   · 不依赖操作系统撤销功能，也不受时间限制
 *
 * 逆操作对照：
 *   MOVE(src → dst)  ⟹  MOVE(dst → src)
 *   COPY(src → dst)  ⟹  MOVE(dst → 回收站)
 *   MKDIR(dst)       ⟹  RMDIR(dst)      （非空则跳过，绝不递归删）
 *   RMDIR(dst)       ⟹  MKDIR(dst)
 *   CREATE(dst)      ⟹  MOVE(dst → 回收站)
 * ========================================================================== */
#include "lc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* --------------------------------------------------- 已占用 / 已让出 --- */

typedef struct { wchar_t **keys; size_t n, cap; } KeyVec;

static void kv_init(KeyVec *k) { k->keys = NULL; k->n = k->cap = 0; }
static void kv_free(KeyVec *k) {
    for (size_t i = 0; i < k->n; ++i) free(k->keys[i]);
    free(k->keys);
    k->keys = NULL; k->n = k->cap = 0;
}
static int kv_has(KeyVec *k, const wchar_t *key) {
    if (!key) return 0;
    for (size_t i = 0; i < k->n; ++i)
        if (wcscmp(k->keys[i], key) == 0) return 1;
    return 0;
}
static void kv_add(KeyVec *k, const wchar_t *key) {
    if (!key) return;
    if (k->n + 1 > k->cap) {
        size_t cap = k->cap ? k->cap * 2 : 16;
        wchar_t **nk = (wchar_t **)realloc(k->keys, cap * sizeof(wchar_t *));
        if (!nk) return;
        k->keys = nk;
        k->cap = cap;
    }
    k->keys[k->n++] = _wcsdup(key);
}

/* ------------------------------------------------------- 逆操作判定 --- */

static const wchar_t *describe_step(StepKind kind, const wchar_t *src,
                                    const wchar_t *dst) {
    (void)src; (void)dst;
    switch (kind) {
        case STEP_MOVE:   return L"移动";
        case STEP_COPY:   return L"复制";
        case STEP_MKDIR:  return L"新建目录";
        case STEP_RMDIR:  return L"删除空目录";
        case STEP_CREATE: return L"生成文件";
    }
    return L"操作";
}

/* 为一条成功的日志步骤构造逆向 Action（含冲突判定） */
static void build_inverse(Plan *plan, const JournalStep *rec, size_t idx,
                          KeyVec *occupied, KeyVec *freed,
                          const wchar_t *trash_base) {
    wchar_t note[512];
    swprintf(note, 512, L"撤销：%ls", describe_step(rec->kind, rec->src, rec->dst));

    Action *a = plan_add(plan, L"rollback", L"", L"", note);
    if (!a) return;
    a->force_exec = 1;

    switch (rec->kind) {
    case STEP_MOVE: {
        wchar_t *dir = lc_dirname(rec->dst ? rec->dst : L"");
        (void)dir; free(dir);
        a->src = arena_wcsdup(plan->arena, rec->dst);
        a->dst = arena_wcsdup(plan->arena, rec->src);
        action_push_step(a, STEP_MOVE, a->src, a->dst, L"回退：移回原位");

        if (!lc_exists(a->src)) {
            a->selected = 0;
            action_add_conflict(a, C_ROLLBACK_SOURCE_MISSING, LEVEL_INFO,
                                L"当前位置已无此文件，跳过：%ls", a->src);
            return;
        }
        int same_file = lc_same_path(a->dst, a->src);
        wchar_t *kdst = lc_norm_key(a->dst);
        if (lc_exists(a->dst) && !kv_has(freed, kdst) && !same_file) {
            a->selected = 0;
            action_add_conflict(a, C_ROLLBACK_TARGET_OCCUPIED, LEVEL_WARN,
                                L"原位置已被占用，请手工处理：%ls", a->dst);
            free(kdst);
            return;
        }
        free(kdst);

        if (rec->size > 0 && lc_is_file(a->src)) {
            long long now = lc_size_of(a->src);
            if (now != rec->size) {
                action_add_conflict(a, C_ROLLBACK_SIZE_MISMATCH, LEVEL_WARN,
                    L"文件大小与执行时不一致（%lld → %lld 字节），请确认后再回退",
                    rec->size, now);
            }
        }
        break;
    }

    case STEP_COPY: {
        WStr land;
        wstr_init(&land);
        wstr_puts(&land, trash_base);
        wstr_printf(&land, L"\\%04u__", (unsigned)(idx + 1));
        wchar_t *safe = lc_sanitize_component(lc_basename(rec->dst ? rec->dst : L""));
        wstr_puts(&land, safe);
        free(safe);

        a->src = arena_wcsdup(plan->arena, rec->dst);
        a->dst = arena_wcsdup(plan->arena, land.data);
        wstr_free(&land);
        action_push_step(a, STEP_MOVE, a->src, a->dst, L"回退：副本移入回收站");
        wstr_clear(&land);
        swprintf(note, 512, L"撤销：删除执行时复制的文件 %ls", rec->dst);
        a->note = arena_wcsdup(plan->arena, note);

        if (!lc_exists(a->src)) {
            a->selected = 0;
            action_add_conflict(a, C_ROLLBACK_SOURCE_MISSING, LEVEL_INFO,
                                L"副本已不存在，跳过：%ls", a->src);
            return;
        }
        break;
    }

    case STEP_MKDIR: {
        a->src = arena_wcsdup(plan->arena, rec->dst);
        a->dst = arena_wcsdup(plan->arena, rec->dst);
        action_push_step(a, STEP_RMDIR, NULL, a->dst, L"回退：删除执行时新建的目录");
        a->steps[0].lenient = 1;
        swprintf(note, 512, L"撤销：删除执行时新建的目录 %ls", rec->dst);
        a->note = arena_wcsdup(plan->arena, note);

        if (!lc_is_dir(a->dst)) {
            a->selected = 0;
            action_add_conflict(a, C_ROLLBACK_SOURCE_MISSING, LEVEL_INFO,
                                L"目录已不存在，跳过：%ls", a->dst);
            return;
        }
        /* 目录现在多半非空（文件还没搬回去）。回退时会先移回其中的文件，
           届时若仍非空，RMDIR 会按 lenient 自动跳过 —— 绝不递归删。 */
        if (!lc_dir_is_empty(a->dst))
            action_add_conflict(a, C_ROLLBACK_DIR_NOT_EMPTY, LEVEL_INFO,
                L"目录当前非空；回退会先移回其中的文件，若届时仍非空则自动跳过删除");
        break;
    }

    case STEP_RMDIR: {
        a->src = arena_wcsdup(plan->arena, rec->dst);
        a->dst = arena_wcsdup(plan->arena, rec->dst);
        action_push_step(a, STEP_MKDIR, NULL, a->dst, L"回退：恢复被删除的空目录");
        swprintf(note, 512, L"撤销：恢复被删除的空目录 %ls", rec->dst);
        a->note = arena_wcsdup(plan->arena, note);
        if (lc_exists(a->dst)) {
            a->selected = 0;
            action_add_conflict(a, C_ROLLBACK_TARGET_EXISTS, LEVEL_INFO,
                                L"目录已存在，无需恢复：%ls", a->dst);
            return;
        }
        break;
    }

    case STEP_CREATE: {
        WStr land;
        wstr_init(&land);
        wstr_puts(&land, trash_base);
        wstr_printf(&land, L"\\%04u__", (unsigned)(idx + 1));
        wchar_t *safe = lc_sanitize_component(lc_basename(rec->dst ? rec->dst : L""));
        wstr_puts(&land, safe);
        free(safe);

        a->src = arena_wcsdup(plan->arena, rec->dst);
        a->dst = arena_wcsdup(plan->arena, land.data);
        wstr_free(&land);
        action_push_step(a, STEP_MOVE, a->src, a->dst, L"回退：生成物移入回收站");
        swprintf(note, 512, L"撤销：删除执行时生成的文件 %ls", rec->dst);
        a->note = arena_wcsdup(plan->arena, note);

        if (!lc_exists(a->src)) {
            a->selected = 0;
            action_add_conflict(a, C_ROLLBACK_SOURCE_MISSING, LEVEL_INFO,
                                L"生成物已不存在，跳过：%ls", a->src);
            return;
        }
        break;
    }
    }
}

/* ========================================================= 预览 ======= */

int rollback_preview(const wchar_t *journal_path, Arena *a,
                     RollbackPreview *out) {
    memset(out, 0, sizeof(*out));
    out->arena = a;

    JournalSteps steps;
    if (journal_load_steps(journal_path, &steps) != 0) return -1;

    /* 把日志概要**深拷贝**进调用方的 arena。
       steps.arena 马上就释放了，直接赋指针会留下悬垂引用。 */
    out->doc.path = arena_wcsdup(a, journal_path);
    out->doc.plan_id = steps.doc.plan_id ? arena_strdup(a, steps.doc.plan_id) : NULL;
    out->doc.root = steps.doc.root ? arena_wcsdup(a, steps.doc.root) : NULL;
    out->doc.category = steps.doc.category ? arena_wcsdup(a, steps.doc.category) : NULL;
    out->doc.footer_status = steps.doc.footer_status
                             ? arena_wcsdup(a, steps.doc.footer_status) : NULL;
    out->doc.policy = steps.doc.policy;
    out->doc.created = steps.doc.created;
    out->doc.n_steps = steps.doc.n_steps;
    out->doc.n_records = steps.doc.n_records;
    out->doc.complete = steps.doc.complete;
    out->doc.status_ok = steps.doc.status_ok;

    Plan *plan = (Plan *)arena_zalloc(a, sizeof(Plan));
    out->plan = plan;

    wchar_t note[512];
    swprintf(note, 512, L"回退 %ls", lc_basename(journal_path));
    plan_init(plan, a, out->doc.root, POLICY_ASK, L"rollback", note);

    wchar_t *trash = lc_trash_root();
    wchar_t *pidw = lc_utf8_to_wcs(plan->plan_id);
    WStr tbase;
    wstr_init(&tbase);
    wstr_puts(&tbase, trash);
    wstr_puts(&tbase, L"\\rb-");
    wstr_puts(&tbase, pidw ? pidw : L"plan");
    free(trash); free(pidw);

    if (steps.n == 0) {
        wstr_free(&tbase);
        free(steps.items);
        arena_free(steps.arena);
        plan_compute_stats(plan);
        return 0;
    }

    KeyVec occupied, freed;
    kv_init(&occupied);
    kv_init(&freed);

    /* 关键：倒序处理 */
    for (size_t i = steps.n; i > 0; --i) {
        const JournalStep *rec = &steps.items[i - 1];
        size_t idx = steps.n - i;
        build_inverse(plan, rec, idx, &occupied, &freed, tbase.data);

        Action *a2 = &plan->actions[plan->n_actions - 1];
        wchar_t *kd = lc_norm_key(a2->dst);
        kv_add(&occupied, kd);
        free(kd);
        wchar_t *ks = lc_norm_key(a2->src);
        kv_add(&freed, ks);
        free(ks);
    }

    kv_free(&occupied);
    kv_free(&freed);
    wstr_free(&tbase);
    free(steps.items);
    arena_free(steps.arena);

    plan_compute_stats(plan);
    return 0;
}

/* ========================================================= 执行 ======= */

int rollback_apply(const wchar_t *journal_path, const ExecOptions *opt,
                   RunResult *result) {
    run_result_init(result);

    Arena *a = arena_new();
    RollbackPreview preview;
    if (rollback_preview(journal_path, a, &preview) != 0) {
        arena_free(a);
        result->failed = 1;
        return -1;
    }

    /* 没有任何可回退步骤 → 直接返回 */
    int n_exec = 0;
    for (size_t i = 0; i < preview.plan->n_actions; ++i)
        if (action_executable(&preview.plan->actions[i])) n_exec++;
    if (n_exec == 0) {
        result->skipped = (int)preview.plan->n_actions;
        result->total = (int)preview.plan->n_actions;
        arena_free(a);
        return 0;
    }

    /* 回退日志：rollback-<新计划id>_undo_<原计划id>.jsonl
       —— 文件名里带上原 id，界面才能判断"这份日志已被回退过"。 */
    const char *orig_pid = preview.doc.plan_id ? preview.doc.plan_id : "plan";
    wchar_t *new_w = lc_utf8_to_wcs(preview.plan->plan_id);
    wchar_t *orig_w = lc_utf8_to_wcs(orig_pid);
    wchar_t *jdir = lc_journal_dir();

    WStr jp;
    wstr_init(&jp);
    wstr_puts(&jp, jdir);
    wstr_puts(&jp, L"\\rollback-");
    wstr_puts(&jp, new_w ? new_w : L"plan");
    wstr_puts(&jp, L"_undo_");
    wstr_puts(&jp, orig_w ? orig_w : L"plan");
    wstr_puts(&jp, L".jsonl");

    ExecOptions ro = *opt;
    ro.is_rollback = 1;
    ro.journal_path = jp.data;

    int rc = executor_run(preview.plan, &ro, result);

    wstr_free(&jp);
    free(new_w); free(orig_w); free(jdir);
    arena_free(a);
    return rc;
}

