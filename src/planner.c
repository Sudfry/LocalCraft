/* ==========================================================================
 * planner.c —— 把"规则算出来的粗结果"变成"可安全执行的计划"
 *
 *   1. 静态校验（源是否存在、名字是否合法、是否把目录移进自己内部……）
 *   2. 链式移动检测（A 的目标是 B 的源）—— 必须早于策略处理，
 *      否则 a→b、b→c 会被 RENAME 策略悄悄改成 a→"b (1)"，与用户意图不符
 *   3. 按策略解决"目标已存在"（跳过 / 覆盖 / 自动改名 / 问用户）
 *   4. 批次内重复目标检测
 *   5. 把每个 Action 拆成原子 Step 序列（含回收站步骤、建目录步骤）
 * ========================================================================== */
#include "lc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>
#include <time.h>

/* ============================================================ 基础 ====== */

Arena *plan_arena(Plan *p) { return p->arena; }

static char *new_plan_id(Arena *a) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d-%08x",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             (unsigned)(GetTickCount() & 0xffffffffu));
    return arena_strdup(a, buf);
}

void plan_init(Plan *p, Arena *a, const wchar_t *root, Policy policy,
               const wchar_t *category, const wchar_t *note) {
    memset(p, 0, sizeof(*p));
    p->arena = a;
    p->plan_id = new_plan_id(a);
    p->root = arena_wcsdup(a, root);
    p->policy = policy;
    p->category = arena_wcsdup(a, category ? category : L"batch");
    p->note = arena_wcsdup(a, note ? note : L"");
    p->created = lc_now_unix();
}

Action *plan_add(Plan *p, const wchar_t *category, const wchar_t *src,
                 const wchar_t *dst, const wchar_t *note) {
    if (p->n_actions + 1 > p->cap_actions) {
        size_t cap = p->cap_actions ? p->cap_actions * 2 : 64;
        Action *na = (Action *)arena_alloc(p->arena, cap * sizeof(Action));
        if (!na) return NULL;
        if (p->actions) memcpy(na, p->actions, p->n_actions * sizeof(Action));
        p->actions = na;
        p->cap_actions = cap;
    }
    Action *a = &p->actions[p->n_actions];
    memset(a, 0, sizeof(*a));

    char idbuf[32];
    snprintf(idbuf, sizeof(idbuf), "a%05u", (unsigned)p->n_actions);
    a->action_id = arena_strdup(p->arena, idbuf);
    a->category = arena_wcsdup(p->arena, category ? category : p->category);
    a->src = arena_wcsdup(p->arena, src);
    a->dst = arena_wcsdup(p->arena, dst);
    a->orig_src = a->src;
    a->orig_dst = a->dst;
    a->note = arena_wcsdup(p->arena, note ? note : L"");
    a->selected = 1;
    a->status = ST_PENDING;
    a->trash_seq = -1;
    p->n_actions++;
    return a;
}

/* ==================================================== 冲突与状态 ======= */

int action_blocked(const Action *a) {
    for (size_t i = 0; i < a->n_conflicts; ++i)
        if (a->conflicts[i].level == LEVEL_ERROR) return 1;
    return 0;
}

int action_changed(const Action *a) {
    return !lc_same_path_exact(a->src, a->dst);
}

int action_executable(const Action *a) {
    if (!a->selected || action_blocked(a)) return 0;
    return action_changed(a) || a->force_exec;
}

Level action_level(const Action *a) {
    Level best = LEVEL_OK;
    for (size_t i = 0; i < a->n_conflicts; ++i)
        if (a->conflicts[i].level > best) best = a->conflicts[i].level;
    return best;
}

void action_add_conflict(Action *a, ConflictCode code, Level level,
                         const wchar_t *fmt, ...) {
    if (a->n_conflicts + 1 > a->cap_conflicts) {
        size_t cap = a->cap_conflicts ? a->cap_conflicts * 2 : 4;
        Conflict *nc = (Conflict *)malloc(cap * sizeof(Conflict));
        if (!nc) return;
        if (a->conflicts) {
            memcpy(nc, a->conflicts, a->n_conflicts * sizeof(Conflict));
            free(a->conflicts);
        }
        a->conflicts = nc;
        a->cap_conflicts = cap;
    }
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 1023, fmt, ap);
    buf[1023] = 0;
    va_end(ap);

    Conflict *c = &a->conflicts[a->n_conflicts++];
    c->code = code;
    c->level = level;
    c->message = _wcsdup(buf);
}

static void action_clear_conflicts(Action *a) {
    for (size_t i = 0; i < a->n_conflicts; ++i) free(a->conflicts[i].message);
    free(a->conflicts);
    a->conflicts = NULL;
    a->n_conflicts = a->cap_conflicts = 0;
}

Step *action_push_step(Action *a, StepKind kind, const wchar_t *src,
                             const wchar_t *dst, const wchar_t *note) {
    if (a->n_steps + 1 > a->cap_steps) {
        size_t cap = a->cap_steps ? a->cap_steps * 2 : 4;
        Step *ns = (Step *)malloc(cap * sizeof(Step));
        if (!ns) return NULL;
        if (a->steps) {
            memcpy(ns, a->steps, a->n_steps * sizeof(Step));
            free(a->steps);
        }
        a->steps = ns;
        a->cap_steps = cap;
    }
    Step *s = &a->steps[a->n_steps++];
    memset(s, 0, sizeof(*s));
    s->kind = kind;
    s->src = src ? _wcsdup(src) : NULL;
    s->dst = dst ? _wcsdup(dst) : NULL;
    s->note = note ? _wcsdup(note) : NULL;
    s->ok = -1;
    return s;
}

/* ==================================================== 回收站路径 ======= */

wchar_t *plan_build_trash_path(Plan *p, Action *a, const wchar_t *name) {
    if (!a->has_trash_seq) {
        int n = 0;
        for (size_t i = 0; i < p->n_actions; ++i)
            if (p->actions[i].has_trash_seq) n++;
        a->trash_seq = n;
        a->has_trash_seq = 1;
    }
    wchar_t *trash = lc_trash_root();
    wchar_t *safe = lc_sanitize_component(name ? name : L"unnamed");
    wchar_t *pidw = lc_utf8_to_wcs(p->plan_id);

    WStr s;
    wstr_init(&s);
    wstr_puts(&s, trash);
    wstr_putc(&s, L'\\');
    wstr_puts(&s, pidw ? pidw : L"plan");
    wstr_putc(&s, L'\\');
    wstr_printf(&s, L"%04d__%ls", a->trash_seq, safe);

    free(trash); free(safe); free(pidw);
    return wstr_steal(&s);
}

/* ================================================== 集合化辅助 ========= */

typedef struct { wchar_t **keys; size_t n, cap; } KeySet;
typedef struct { wchar_t *key; int count; } KeyCount;

typedef struct { KeyCount *items; size_t n, cap; } KeyCounter;

static void keyset_init(KeySet *k) { k->keys = NULL; k->n = k->cap = 0; }
static void keyset_free(KeySet *k) {
    for (size_t i = 0; i < k->n; ++i) free(k->keys[i]);
    free(k->keys);
    k->keys = NULL; k->n = k->cap = 0;
}
static int keyset_add(KeySet *k, const wchar_t *key) {
    if (!key) return -1;
    for (size_t i = 0; i < k->n; ++i)
        if (wcscmp(k->keys[i], key) == 0) return 0;
    if (k->n + 1 > k->cap) {
        size_t cap = k->cap ? k->cap * 2 : 32;
        wchar_t **nk = (wchar_t **)realloc(k->keys, cap * sizeof(wchar_t *));
        if (!nk) return -1;
        k->keys = nk;
        k->cap = cap;
    }
    k->keys[k->n++] = _wcsdup(key);
    return 0;
}
static int keyset_has(KeySet *k, const wchar_t *key) {
    if (!key) return 0;
    for (size_t i = 0; i < k->n; ++i)
        if (wcscmp(k->keys[i], key) == 0) return 1;
    return 0;
}

static void counter_init(KeyCounter *c) { c->items = NULL; c->n = c->cap = 0; }
static void counter_free(KeyCounter *c) {
    for (size_t i = 0; i < c->n; ++i) free(c->items[i].key);
    free(c->items);
    c->items = NULL; c->n = c->cap = 0;
}
static void counter_add(KeyCounter *c, const wchar_t *key) {
    if (!key) return;
    for (size_t i = 0; i < c->n; ++i) {
        if (wcscmp(c->items[i].key, key) == 0) { c->items[i].count++; return; }
    }
    if (c->n + 1 > c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 32;
        KeyCount *ni = (KeyCount *)realloc(c->items, cap * sizeof(KeyCount));
        if (!ni) return;
        c->items = ni;
        c->cap = cap;
    }
    c->items[c->n].key = _wcsdup(key);
    c->items[c->n].count = 1;
    c->n++;
}
static int counter_get(KeyCounter *c, const wchar_t *key) {
    if (!key) return 0;
    for (size_t i = 0; i < c->n; ++i)
        if (wcscmp(c->items[i].key, key) == 0) return c->items[i].count;
    return 0;
}

/* ============================================== 1. 静态校验 =========== */

static void validate_static(Action *a, int check_source, int recheck_disk) {
    const wchar_t *src = a->src, *dst = a->dst;
    if (!src || !dst) {
        action_add_conflict(a, C_BAD_TARGET, LEVEL_ERROR, L"缺少源或目标路径");
        return;
    }
    wchar_t *ksrc = lc_norm_key(src);
    wchar_t *kdst = lc_norm_key(dst);
    int case_only = ksrc && kdst && wcscmp(ksrc, kdst) == 0;
    free(ksrc); free(kdst);

    if (a->no_source_check) check_source = 0;

    if (check_source && recheck_disk && !lc_exists(src))
        action_add_conflict(a, C_MISSING_SOURCE, LEVEL_ERROR,
                            L"源路径不存在：%ls", src);

    if (lc_same_path_exact(src, dst))
        action_add_conflict(a, C_NO_CHANGE, LEVEL_INFO,
                            L"新旧路径相同，无需操作");

    if (!a->no_source_check && lc_is_dir(src) && lc_is_within(dst, src))
        action_add_conflict(a, C_INTO_SELF, LEVEL_ERROR,
                            L"目标位于源文件夹内部，会造成递归移动");

    const wchar_t *base = lc_basename(dst);
    size_t blen = wcslen(base);
    while (blen > 0 && lc_is_sep(base[blen - 1])) blen--;
    if (blen == 0) {
        action_add_conflict(a, C_BAD_TARGET, LEVEL_ERROR,
                            L"目标缺少文件名：%ls", dst);
    } else {
        wchar_t *name = (wchar_t *)malloc((blen + 1) * sizeof(wchar_t));
        if (name) {
            memcpy(name, base, blen * sizeof(wchar_t));
            name[blen] = 0;
            if (!lc_is_valid_component(name))
                action_add_conflict(a, C_INVALID_NAME, LEVEL_ERROR,
                                    L"目标名含非法字符或是系统保留名：%ls", name);
            free(name);
        }
    }

    /* 目标是已存在的目录 —— 约定 dst 永远是完整文件路径，
       若那里是个目录，MoveFile 的语义完全不对，直接拦掉。 */
    if (recheck_disk && !case_only && lc_is_dir(dst) && !a->target_is_dir_ok)
        action_add_conflict(a, C_TARGET_IS_DIR, LEVEL_ERROR,
                            L"目标位置已存在同名文件夹：%ls", dst);

    if (lc_path_too_long(dst))
        action_add_conflict(a, C_LONG_PATH, LEVEL_WARN,
                            L"路径接近 260 字符上限，已自动启用 \\\\?\\ 长路径前缀");
}

/* ====================================== 2. 链式移动（必须先做） ====== */

typedef struct { wchar_t *key; Action **items; size_t n, cap; } SrcBucket;
typedef struct { SrcBucket *items; size_t n, cap; } SrcMap;

static SrcMap *srcmap_build(Plan *p) {
    SrcMap *m = (SrcMap *)calloc(1, sizeof(SrcMap));
    if (!m) return NULL;
    for (size_t i = 0; i < p->n_actions; ++i) {
        Action *a = &p->actions[i];
        if (!action_changed(a)) continue;
        wchar_t *k = lc_norm_key(a->src);
        if (!k) continue;
        size_t j = 0;
        for (; j < m->n; ++j) if (wcscmp(m->items[j].key, k) == 0) break;
        if (j == m->n) {
            if (m->n + 1 > m->cap) {
                size_t cap = m->cap ? m->cap * 2 : 16;
                SrcBucket *ni = (SrcBucket *)realloc(m->items, cap * sizeof(SrcBucket));
                if (!ni) { free(k); break; }
                m->items = ni;
                m->cap = cap;
            }
            m->items[m->n].key = k;
            m->items[m->n].items = NULL;
            m->items[m->n].n = m->items[m->n].cap = 0;
            j = m->n++;
        } else {
            free(k);
        }
        SrcBucket *b = &m->items[j];
        if (b->n + 1 > b->cap) {
            size_t cap = b->cap ? b->cap * 2 : 4;
            Action **ni = (Action **)realloc(b->items, cap * sizeof(Action *));
            if (!ni) break;
            b->items = ni;
            b->cap = cap;
        }
        b->items[b->n++] = a;
    }
    return m;
}

static void srcmap_free(SrcMap *m) {
    if (!m) return;
    for (size_t i = 0; i < m->n; ++i) {
        free(m->items[i].key);
        free(m->items[i].items);
    }
    free(m->items);
    free(m);
}

static SrcBucket *srcmap_find(SrcMap *m, const wchar_t *key) {
    if (!m || !key) return NULL;
    for (size_t i = 0; i < m->n; ++i)
        if (wcscmp(m->items[i].key, key) == 0) return &m->items[i];
    return NULL;
}

static void detect_chain_moves(Plan *p) {
    SrcMap *m = srcmap_build(p);
    if (!m) return;

    /* 会被整个搬走的目录（数量过多时跳过"目标落在被搬走目录内"的探测，
       避免 O(n·m) 拖慢大批次） */
    Action **moved_dirs = NULL;
    size_t n_dirs = 0, cap_dirs = 0;
    for (size_t i = 0; i < p->n_actions; ++i) {
        Action *a = &p->actions[i];
        if (!action_changed(a)) continue;
        if (!lc_is_dir(a->src)) continue;
        if (n_dirs + 1 > cap_dirs) {
            size_t cap = cap_dirs ? cap_dirs * 2 : 16;
            Action **ni = (Action **)realloc(moved_dirs, cap * sizeof(Action *));
            if (!ni) break;
            moved_dirs = ni;
            cap_dirs = cap;
        }
        moved_dirs[n_dirs++] = a;
    }
    int check_dirs = n_dirs <= 2000;

    for (size_t i = 0; i < p->n_actions; ++i) {
        Action *a = &p->actions[i];
        if (action_blocked(a) || !action_changed(a)) continue;
        if (a->is_copy || a->produce_only) continue;   /* 复制/生成不让源消失 */

        wchar_t *kdst = lc_norm_key(a->dst);
        SrcBucket *b = srcmap_find(m, kdst);
        if (b) {
            for (size_t j = 0; j < b->n; ++j) {
                if (b->items[j] == a) continue;
                action_add_conflict(a, C_CHAIN_MOVE, LEVEL_ERROR,
                    L"目标是本批次中另一操作的源路径，执行顺序不可控，已阻止");
                break;
            }
        }
        if (check_dirs) {
            for (size_t d = 0; d < n_dirs; ++d) {
                Action *other = moved_dirs[d];
                if (other == a) continue;
                if (lc_is_within(a->dst, other->src) &&
                    !lc_is_within(a->dst, a->src)) {
                    action_add_conflict(a, C_TARGET_INSIDE_MOVED_DIR, LEVEL_WARN,
                        L"目标位于本批次将被移动的目录「%ls」内部",
                        lc_basename(other->src));
                    break;
                }
            }
        }
        free(kdst);
    }

    free(moved_dirs);
    srcmap_free(m);
}

/* ================================== 3. 目标已存在 → 按策略处理 ======= */

static void resolve_existing(Plan *p, Action *a, KeyCounter *raw_counts,
                             KeySet *taken) {
    if (action_blocked(a)) return;

    wchar_t *kdst = lc_norm_key(a->dst);
    wchar_t *ksrc = lc_norm_key(a->src);
    if (!kdst) { free(ksrc); return; }

    int duplicated_in_batch = counter_get(raw_counts, kdst) > 1;

    int exists_on_disk = lc_exists(a->dst);
    if (exists_on_disk && ksrc && wcscmp(kdst, ksrc) == 0)
        exists_on_disk = 0;      /* 仅大小写改名：磁盘上那个就是它自己 */
    int exists_in_batch = keyset_has(taken, kdst);

    if (!exists_on_disk && !exists_in_batch) {
        keyset_add(taken, kdst);
        free(kdst); free(ksrc);
        return;
    }
    if (duplicated_in_batch && !exists_on_disk) {
        /* 同批撞车，不在这里改名，否则会掩盖真实冲突 */
        keyset_add(taken, kdst);
        free(kdst); free(ksrc);
        return;
    }

    const wchar_t *label = exists_on_disk ? L"磁盘上已存在" : L"本批次中已被占用";

    switch (p->policy) {
    case POLICY_SKIP:
        a->selected = 0;
        action_add_conflict(a, C_TARGET_EXISTS_SKIPPED, LEVEL_INFO,
                            L"目标%ls，按策略【跳过】", label);
        break;
    case POLICY_OVERWRITE:
        action_add_conflict(a, C_OVERWRITE, LEVEL_WARN,
                            L"目标%ls，执行时旧文件将移入回收站（可回退）", label);
        keyset_add(taken, kdst);
        break;
    case POLICY_RENAME: {
        /* 构造 taken（KeySet）的 Vec 视图 —— lc_unique_path 接受 Vec* */
        Vec view;
        vec_init(&view);
        for (size_t i = 0; i < taken->n; ++i) vec_push(&view, taken->keys[i]);
        wchar_t *nd = lc_unique_path(a->dst, &view);
        vec_free(&view);
        if (nd) {
            a->dst = arena_wcsdup(p->arena, nd);
            action_add_conflict(a, C_TARGET_EXISTS_RENAMED, LEVEL_INFO,
                                L"目标%ls，已自动改名为「%ls」",
                                label, lc_basename(a->dst));
            wchar_t *nk = lc_norm_key(a->dst);
            if (nk) { keyset_add(taken, nk); free(nk); }
            free(nd);
        }
        break;
    }
    case POLICY_ASK:
    default:
        action_add_conflict(a, C_TARGET_EXISTS, LEVEL_ERROR,
                            L"目标%ls，请在预览表中选择处理方式", label);
        break;
    }

    free(kdst);
    free(ksrc);
}

/* ====================================== 4. 批次内重复目标 =========== */

static void detect_duplicate_targets(Plan *p) {
    KeyCounter *c = (KeyCounter *)calloc(1, sizeof(KeyCounter));
    if (!c) return;
    counter_init(c);
    for (size_t i = 0; i < p->n_actions; ++i) {
        Action *a = &p->actions[i];
        if (action_blocked(a) || !action_changed(a)) continue;
        wchar_t *k = lc_norm_key(a->dst);
        counter_add(c, k);
        free(k);
    }
    for (size_t i = 0; i < p->n_actions; ++i) {
        Action *a = &p->actions[i];
        if (action_blocked(a) || !action_changed(a)) continue;
        wchar_t *k = lc_norm_key(a->dst);
        int n = counter_get(c, k);
        if (n > 1) {
            action_add_conflict(a, C_DUPLICATE_TARGET, LEVEL_ERROR,
                L"本批次有 %d 个操作指向同一目标（%ls），已阻止执行",
                n, lc_basename(a->dst));
        }
        free(k);
    }
    counter_free(c);
    free(c);
}

/* ============================================== 5. 拆解步骤 ========== */

typedef struct { wchar_t **keys; size_t n, cap; } DirSet;

static int dirset_has(DirSet *d, const wchar_t *k) {
    for (size_t i = 0; i < d->n; ++i) if (wcscmp(d->keys[i], k) == 0) return 1;
    return 0;
}
static void dirset_add(DirSet *d, const wchar_t *k) {
    if (d->n + 1 > d->cap) {
        size_t cap = d->cap ? d->cap * 2 : 16;
        wchar_t **nk = (wchar_t **)realloc(d->keys, cap * sizeof(wchar_t *));
        if (!nk) return;
        d->keys = nk;
        d->cap = cap;
    }
    d->keys[d->n++] = _wcsdup(k);
}
static void dirset_free(DirSet *d) {
    for (size_t i = 0; i < d->n; ++i) free(d->keys[i]);
    free(d->keys);
    d->keys = NULL; d->n = d->cap = 0;
}

static void add_mkdir_steps(Action *a, const wchar_t *dst_parent, DirSet *planned) {
    /* 从目标父目录往上找，直到遇到已存在的目录 */
    wchar_t *cur = _wcsdup(dst_parent);
    wchar_t **missing = NULL;
    size_t n_missing = 0, cap_missing = 0;
    int guard = 0;

    while (cur && guard++ < 64) {
        wchar_t *k = lc_norm_key(cur);
        if (!k) break;
        if (dirset_has(planned, k) || lc_is_dir(cur)) { free(k); break; }
        free(k);

        if (n_missing + 1 > cap_missing) {
            size_t cap = cap_missing ? cap_missing * 2 : 8;
            wchar_t **nm = (wchar_t **)realloc(missing, cap * sizeof(wchar_t *));
            if (!nm) break;
            missing = nm;
            cap_missing = cap;
        }
        missing[n_missing++] = _wcsdup(cur);

        wchar_t *parent = lc_dirname(cur);
        if (!parent || wcscmp(parent, cur) == 0) { free(parent); break; }
        free(cur);
        cur = parent;
    }
    free(cur);

    /* 自顶向下创建 */
    for (size_t i = n_missing; i > 0; --i) {
        wchar_t *k = lc_norm_key(missing[i - 1]);
        if (k) { dirset_add(planned, k); free(k); }
        action_push_step(a, STEP_MKDIR, NULL, missing[i - 1], L"创建目录");
    }
    for (size_t i = 0; i < n_missing; ++i) free(missing[i]);
    free(missing);
}

static void build_steps(Plan *p, Action *a, DirSet *planned) {
    wchar_t *parent = lc_dirname(a->dst);
    if (parent) {
        add_mkdir_steps(a, parent, planned);
        free(parent);
    }

    int is_overwrite = 0;
    for (size_t i = 0; i < a->n_conflicts; ++i)
        if (a->conflicts[i].code == C_OVERWRITE) is_overwrite = 1;

    if (lc_exists(a->dst) && is_overwrite) {
        wchar_t *tpath = plan_build_trash_path(p, a, lc_basename(a->dst));
        action_push_step(a, STEP_MOVE, a->dst, tpath, L"旧文件移入回收站");
        free(tpath);
    }

    if (a->produce_only) {
        /* src 必须带上：生成器（导出/转换）要靠它知道从哪个文件产出 */
        Step *st = action_push_step(a, STEP_CREATE, a->src, a->dst,
                                    a->producer ? a->producer : L"外部引擎产出");
        /* 产物由谁生成：executor 会在执行这一步时按规格现生成文件 */
        if (st && a->generator) st->generator = _wcsdup(a->generator);
    } else if (a->is_copy) {
        action_push_step(a, STEP_COPY, a->src, a->dst, L"复制");
    } else {
        action_push_step(a, STEP_MOVE, a->src, a->dst, NULL);
    }
}

/* ================================================== finalize ========= */

void plan_compute_stats(Plan *p) {
    p->st_total = p->st_selected = p->st_executable = 0;
    p->st_blocked = p->st_warned = p->st_overwrite = 0;
    for (size_t i = 0; i < p->n_actions; ++i) {
        Action *a = &p->actions[i];
        p->st_total++;
        if (a->selected) p->st_selected++;
        if (action_executable(a)) p->st_executable++;
        if (action_blocked(a)) p->st_blocked++;
        if (action_level(a) == LEVEL_WARN) p->st_warned++;
        for (size_t k = 0; k < a->n_conflicts; ++k)
            if (a->conflicts[k].code == C_OVERWRITE) { p->st_overwrite++; break; }
    }
}

void plan_finalize(Plan *p, int check_source, int recheck_disk) {
    /* 0) 复位（支持反复 finalize：用户来回切策略） */
    for (size_t i = 0; i < p->n_actions; ++i) {
        Action *a = &p->actions[i];
        action_clear_conflicts(a);
        for (size_t k = 0; k < a->n_steps; ++k) {
            free(a->steps[k].src);
            free(a->steps[k].dst);
            free(a->steps[k].note);
            free(a->steps[k].error);
            free(a->steps[k].generator);
        }
        free(a->steps);
        a->steps = NULL;
        a->n_steps = a->cap_steps = 0;
        a->status = ST_PENDING;
        if (a->error) { free(a->error); a->error = NULL; }
        a->dst = a->orig_dst;
        a->src = a->orig_src;
        a->trash_seq = -1;
        a->has_trash_seq = 0;
    }

    /* 1) 静态校验 */
    for (size_t i = 0; i < p->n_actions; ++i)
        validate_static(&p->actions[i], check_source, recheck_disk);

    /* 2) 链式移动 —— 必须早于策略处理 */
    detect_chain_moves(p);

    /* 3) 目标已存在 → 按策略处理 */
    KeyCounter raw_counts;
    counter_init(&raw_counts);
    for (size_t i = 0; i < p->n_actions; ++i) {
        wchar_t *k = lc_norm_key(p->actions[i].dst);
        counter_add(&raw_counts, k);
        free(k);
    }
    KeySet taken;
    keyset_init(&taken);
    for (size_t i = 0; i < p->n_actions; ++i)
        resolve_existing(p, &p->actions[i], &raw_counts, &taken);
    keyset_free(&taken);
    counter_free(&raw_counts);

    /* 4) 批次内重复目标 */
    detect_duplicate_targets(p);

    /* 5) 拆解步骤 */
    DirSet planned;
    memset(&planned, 0, sizeof(planned));
    for (size_t i = 0; i < p->n_actions; ++i) {
        Action *a = &p->actions[i];
        if (action_blocked(a) || !action_changed(a)) continue;
        build_steps(p, a, &planned);
    }
    dirset_free(&planned);

    plan_compute_stats(p);
}
