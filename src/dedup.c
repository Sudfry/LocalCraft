/* ==========================================================================
 * dedup.c —— 模块 3：内容级去重
 *
 * 思路
 * ----
 *   1. 抽取每个文档的纯文本（extract.c）
 *   2. 分词（中文二元组 / 英文词），算 64 位 SimHash 指纹
 *   3. 用**分带 LSH**找候选对（把 64 位切成 4 段 16 位，同段同值才可能相近），
 *      再用真实汉明距离验证 —— 这样 10 万文档也不用 O(n²) 两两比较
 *   4. 并查集把相似文档聚成组
 *
 * SimHash 的性质：内容相近 -> 指纹汉明距离小。所以"文件名不同但内容雷同"
 * 的文档会落进同一组，而这是按文件名做不出来的。
 *
 * 处理动作同样是一个普通 MOVE（移进一个子文件夹），
 * 因此天然进入"预览 → 执行 → 可回退"的体系。
 * ========================================================================== */
#include "lc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ==================================================== 哈希 ============ */

static uint64_t fnv1a_w(const wchar_t *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; ++s) {
        wchar_t c = *s;
        h ^= (uint64_t)(c & 0xFF);
        h *= 1099511628211ULL;
        h ^= (uint64_t)((c >> 8) & 0xFF);
        h *= 1099511628211ULL;
    }
    return h;
}

int simhash_distance(SimHash a, SimHash b) {
    uint64_t x = a.bits ^ b.bits;
    int n = 0;
    while (x) { x &= (x - 1); ++n; }
    return n;
}

double simhash_similarity(SimHash a, SimHash b) {
    return 1.0 - (double)simhash_distance(a, b) / 64.0;
}

/* ---- 词频表（开放寻址），用于给 SimHash 加权 ---- */

typedef struct {
    uint64_t *hash;
    int      *count;
    size_t    cap, n;
} FreqTab;

static int freq_init(FreqTab *f, size_t hint) {
    size_t cap = 64;
    while (cap < hint * 2) cap *= 2;
    f->hash = (uint64_t *)calloc(cap, sizeof(uint64_t));
    f->count = (int *)calloc(cap, sizeof(int));
    f->cap = cap;
    f->n = 0;
    return f->hash && f->count ? 0 : -1;
}

static void freq_free(FreqTab *f) {
    free(f->hash); free(f->count);
    f->hash = NULL; f->count = NULL; f->cap = f->n = 0;
}

static void freq_add(FreqTab *f, uint64_t h) {
    size_t mask = f->cap - 1;
    size_t i = (size_t)(h & mask);
    for (size_t probe = 0; probe < f->cap; ++probe) {
        size_t k = (i + probe) & mask;
        if (f->count[k] == 0) {
            f->hash[k] = h;
            f->count[k] = 1;
            f->n++;
            return;
        }
        if (f->hash[k] == h) { f->count[k]++; return; }
    }
}

SimHash simhash_tokens(const TokenList *t) {
    SimHash out;
    memset(&out, 0, sizeof(out));
    if (!t || t->n == 0) return out;

    FreqTab f;
    if (freq_init(&f, t->n) != 0) return out;
    for (size_t i = 0; i < t->n; ++i)
        freq_add(&f, fnv1a_w(t->items[i]));

    int64_t v[64];
    memset(v, 0, sizeof(v));

    for (size_t k = 0; k < f.cap; ++k) {
        if (f.count[k] == 0) continue;
        int w = f.count[k];
        if (w > 8) w = 8;                 /* 抑制极端高频词 */
        uint64_t h = f.hash[k];
        for (int b = 0; b < 64; ++b)
            v[b] += ((h >> b) & 1ULL) ? w : -w;
    }
    freq_free(&f);

    for (int b = 0; b < 64; ++b)
        if (v[b] > 0) out.bits |= (1ULL << b);
    out.tokens = (int)t->n;
    return out;
}

SimHash simhash_text(const wchar_t *text) {
    SimHash out;
    memset(&out, 0, sizeof(out));
    if (!text) return out;

    Arena *a = arena_new();
    if (!a) return out;
    TokenList t;
    tokens_init(&t, a);
    tokenize(text, &t);
    out = simhash_tokens(&t);
    tokens_free(&t);
    arena_free(a);
    return out;
}

/* ==================================================== 扫描 ============ */

typedef struct {
    wchar_t  *path;
    SimHash   hash;
    int       tokens;
    long long size;
} DItem;

typedef struct {
    DItem  *items;
    size_t  n, cap;
    Arena  *arena;
} DVec;

static int dvec_push(DVec *v, const wchar_t *path, SimHash h, int tokens,
                     long long size) {
    if (v->n + 1 > v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 64;
        DItem *ni = (DItem *)realloc(v->items, cap * sizeof(DItem));
        if (!ni) return -1;
        v->items = ni;
        v->cap = cap;
    }
    v->items[v->n].path = arena_wcsdup(v->arena, path);
    v->items[v->n].hash = h;
    v->items[v->n].tokens = tokens;
    v->items[v->n].size = size;
    v->n++;
    return 0;
}

/* ---- 并查集 ---- */

static size_t uf_find(size_t *parent, size_t x) {
    while (parent[x] != x) {
        parent[x] = parent[parent[x]];
        x = parent[x];
    }
    return x;
}

/* ---- 分带 LSH 表 ---- */

typedef struct {
    uint16_t band;
    uint32_t idx;
} BandEnt;

typedef struct {
    BandEnt *ents;
    size_t   n, cap;
} BandTable;

static int band_cmp(const void *a, const void *b) {
    const BandEnt *x = (const BandEnt *)a, *y = (const BandEnt *)b;
    if (x->band != y->band) return x->band < y->band ? -1 : 1;
    if (x->idx != y->idx) return x->idx < y->idx ? -1 : 1;
    return 0;
}

/* ==================================================== 规则 ============ */

void dedup_rule_init(DedupRule *r, Arena *a) {
    memset(r, 0, sizeof(*r));
    r->threshold = 0.92;
    r->recursive = 1;
    r->min_chars = 40;
    r->action = 0;
    r->target_dir = arena_wcsdup(a, L"_重复文件");
    vec_init(&r->include_ext);
}

const wchar_t *dedup_action_name(int action) {
    switch (action) {
        case 1:  return L"移入回收站";
        default: return L"移入重复文件夹";
    }
}

/* 扫描 + 抽取 + 指纹 + 分组。返回 0 成功。 */
int dedup_scan(const wchar_t *root, const DedupRule *r, DedupResult *out,
               wchar_t *err, size_t errcch) {
    memset(out, 0, sizeof(*out));

    if (!lc_is_dir(root)) {
        swprintf(err, errcch, L"文件夹不存在：%ls", root);
        return -1;
    }

    Arena *a = arena_new();
    if (!a) { swprintf(err, errcch, L"内存不足"); return -1; }

    FileList fl;
    filelist_init(&fl, a);
    if (lc_scan_dir(root, r->recursive, 1, 1, &fl) != 0) {
        arena_free(a);
        swprintf(err, errcch, L"无法读取目录：%ls", root);
        return -1;
    }

    DVec vec;
    memset(&vec, 0, sizeof(vec));
    vec.arena = a;

    int skipped = 0;
    wchar_t first_err[512];
    first_err[0] = 0;

    for (size_t i = 0; i < fl.n; ++i) {
        const wchar_t *path = fl.paths[i];
        if (!extract_supported(path)) { skipped++; continue; }

        wchar_t e[512];
        e[0] = 0;
        wchar_t *text = extract_text(path, 2u << 20, e, 512);
        if (!text) {
            if (!first_err[0] && e[0]) wcsncpy(first_err, e, 511);
            skipped++;
            continue;
        }
        size_t tlen = wcslen(text);
        if (tlen < (size_t)r->min_chars) { free(text); skipped++; continue; }

        SimHash h = simhash_text(text);
        free(text);
        if (h.tokens == 0) { skipped++; continue; }

        dvec_push(&vec, path, h, h.tokens, lc_size_of(path));
    }

    /* ---- 用分带 LSH 找候选对，再验证 ---- */
    size_t n = vec.n;
    size_t *parent = (size_t *)malloc((n ? n : 1) * sizeof(size_t));
    for (size_t i = 0; i < n; ++i) parent[i] = i;

    int max_dist = (int)((1.0 - r->threshold) * 64.0 + 0.5);
    if (max_dist < 0) max_dist = 0;
    if (max_dist > 32) max_dist = 32;

    if (n > 1) {
        for (int band = 0; band < 4; ++band) {
            BandTable bt;
            bt.n = 0; bt.cap = n;
            bt.ents = (BandEnt *)malloc(n * sizeof(BandEnt));
            if (!bt.ents) break;
            for (size_t i = 0; i < n; ++i) {
                uint16_t v = (uint16_t)((vec.items[i].hash.bits >> (band * 16)) & 0xFFFF);
                bt.ents[bt.n].band = v;
                bt.ents[bt.n].idx  = (uint32_t)i;
                bt.n++;
            }
            qsort(bt.ents, bt.n, sizeof(BandEnt), band_cmp);

            size_t s = 0;
            while (s < bt.n) {
                size_t e2 = s;
                while (e2 + 1 < bt.n && bt.ents[e2 + 1].band == bt.ents[s].band) ++e2;
                /* 同一段值相同的都在 [s, e2]，两两验证 */
                for (size_t x = s; x <= e2; ++x) {
                    for (size_t y = x + 1; y <= e2; ++y) {
                        size_t i = bt.ents[x].idx, j = bt.ents[y].idx;
                        if (vec.items[i].hash.bits == vec.items[j].hash.bits ||
                            simhash_distance(vec.items[i].hash, vec.items[j].hash) <= max_dist) {
                            size_t ri = uf_find(parent, i), rj = uf_find(parent, j);
                            if (ri != rj) parent[ri] = rj;
                        }
                    }
                }
                s = e2 + 1;
            }
            free(bt.ents);
        }
    }

    /* ---- 收组 ---- */
    size_t *group_of = (size_t *)malloc((n ? n : 1) * sizeof(size_t));
    memset(group_of, 0xff, (n ? n : 1) * sizeof(size_t));
    size_t n_groups = 0;
    for (size_t i = 0; i < n; ++i) {
        size_t root_i = uf_find(parent, i);
        /* 统计这个根有多少成员 */
        size_t cnt = 0;
        for (size_t k = 0; k < n; ++k)
            if (uf_find(parent, k) == root_i) cnt++;
        if (cnt < 2) continue;      /* 只有自己的不算重复组 */
        if (group_of[root_i] == (size_t)-1) group_of[root_i] = n_groups++;
    }

    out->items = (DedupItem *)calloc(n ? n : 1, sizeof(DedupItem));
    out->n = 0;
    for (size_t i = 0; i < n; ++i) {
        size_t root_i = uf_find(parent, i);
        if (group_of[root_i] == (size_t)-1) {
            /* 不重复的文档也报出来，但标记为 -1，界面可以隐藏 */
            out->items[out->n].path = vec.items[i].path;
            out->items[out->n].hash = vec.items[i].hash;
            out->items[out->n].tokens = vec.items[i].tokens;
            out->items[out->n].size = vec.items[i].size;
            out->items[out->n].group = -1;
            out->n++;
            continue;
        }
        out->items[out->n].path = vec.items[i].path;
        out->items[out->n].hash = vec.items[i].hash;
        out->items[out->n].tokens = vec.items[i].tokens;
        out->items[out->n].size = vec.items[i].size;
        out->items[out->n].group = (int)group_of[root_i] + 1;
        out->n++;
    }
    out->n_groups = n_groups;
    out->skipped = skipped;
    out->max_distance = max_dist;
    out->arena = a;
    if (first_err[0]) wcsncpy(out->first_error, first_err, 511);

    free(group_of);
    free(parent);
    free(vec.items);
    free(fl.paths);
    return 0;
}

void dedup_result_free(DedupResult *res) {
    if (!res) return;
    free(res->items);
    res->items = NULL;
    res->n = 0;
    if (res->arena) { arena_free(res->arena); res->arena = NULL; }
}

/* ==================================================== 生成计划 ======== */

/* 组内第一条（按路径序）作为保留项，其余为重复项 */
Plan *dedup_build_plan(Arena *a, const wchar_t *root, const DedupResult *res,
                       const DedupRule *r, Policy policy,
                       const wchar_t *trash_root) {
    (void)trash_root;

    Plan *plan = (Plan *)arena_zalloc(a, sizeof(Plan));
    if (!plan) return NULL;

    WStr desc;
    wstr_init(&desc);
    wstr_printf(&desc, L"内容去重（相似度阈值 %.0f%%，%llu 组重复）",
                r->threshold * 100.0, (unsigned long long)res->n_groups);
    plan_init(plan, a, root, policy, L"dedup", desc.data);
    wstr_free(&desc);

    for (int g = 1; g <= (int)res->n_groups; ++g) {
        /* 组内按扫描顺序的第一条作为保留项 */
        size_t keeper = (size_t)-1;
        for (size_t i = 0; i < res->n; ++i) {
            if (res->items[i].group == g) { keeper = i; break; }
        }
        if (keeper == (size_t)-1) continue;

        for (size_t i = keeper + 1; i < res->n; ++i) {
            if (res->items[i].group != g) continue;

            const wchar_t *name = lc_basename(res->items[i].path);
            WStr dst;
            wstr_init(&dst);
            if (r->action == 1) {
                wchar_t *tr = lc_trash_root();
                wstr_puts(&dst, tr);
                wstr_puts(&dst, L"\\dedup\\");
                wstr_puts(&dst, lc_basename(root));
                wstr_putc(&dst, L'\\');
                wstr_puts(&dst, name);
                free(tr);
            } else {
                wstr_puts(&dst, root);
                if (dst.len && !lc_is_sep(dst.data[dst.len - 1])) wstr_putc(&dst, L'\\');
                wstr_puts(&dst, (r->target_dir && r->target_dir[0])
                                ? r->target_dir : L"_重复文件");
                wstr_putc(&dst, L'\\');
                wstr_puts(&dst, name);
            }

            double sim = simhash_similarity(res->items[i].hash,
                                            res->items[keeper].hash);
            WStr note;
            wstr_init(&note);
            wstr_printf(&note, L"与「%ls」内容相似 %.0f%%（%d 词）",
                        lc_basename(res->items[keeper].path), sim * 100.0,
                        res->items[i].tokens);

            Action *act = plan_add(plan, L"dedup", res->items[i].path,
                                   dst.data, note.data);
            if (act) act->rule_desc = arena_wcsdup(a, note.data);

            wstr_free(&note);
            wstr_free(&dst);
        }
    }

    plan_finalize(plan, 1, 1);
    return plan;
}
