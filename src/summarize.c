/* ==========================================================================
 * summarize.c —— 模块 4：抽取式文档摘要（TextRank / TF-IDF）
 *
 * 完全离线、不依赖任何模型：
 *   · 分句 -> 每句分词（中文二元组）
 *   · TextRank：把句子当节点，句子间的词汇重叠当边权，跑 PageRank
 *   · TF-IDF  ：句子得分 = 句内词的 tf*idf 之和（idf 用"句子"当文档算）
 *   · 关键词  ：词在全部句子上的 tf*idf 排序
 *
 * 输出的是**原文字符区间**，所以界面上能直接跳到原文位置 —— 可追溯。
 * 复杂度与内存都被硬上限卡住，几十万字的文档也秒出。
 * ========================================================================== */
#include "lc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <wchar.h>

#define MAX_SENTENCES 1200      /* 超过这个句子数只取前 1200 句 */
#define TEXTRANK_ITER 40
#define TEXTRANK_DAMP 0.85

/* ==================================================== 词哈希 ========== */

static uint64_t tok_hash(const wchar_t *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; ++s) {
        h ^= (uint64_t)(*s & 0xFF); h *= 1099511628211ULL;
        h ^= (uint64_t)((*s >> 8) & 0xFF); h *= 1099511628211ULL;
    }
    return h;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* ---- 词 -> (df, tf) 表 ---- */

typedef struct {
    uint64_t       *hash;
    int            *df, *tf;
    const wchar_t **repr;      /* 每个槽的代表词（分词结果在 scratch arena 里，稳定） */
    size_t          cap, n;
} Vocab;

static int vocab_init(Vocab *v, size_t hint) {
    size_t cap = 256;
    while (cap < hint * 2) cap *= 2;
    v->hash = (uint64_t *)calloc(cap, sizeof(uint64_t));
    v->df = (int *)calloc(cap, sizeof(int));
    v->tf = (int *)calloc(cap, sizeof(int));
    v->repr = (const wchar_t **)calloc(cap, sizeof(wchar_t *));
    v->cap = cap;
    v->n = 0;
    return (v->hash && v->df && v->tf && v->repr) ? 0 : -1;
}
static void vocab_free(Vocab *v) {
    free(v->hash); free(v->df); free(v->tf); free(v->repr);
    v->hash = NULL; v->df = NULL; v->tf = NULL; v->repr = NULL;
    v->cap = v->n = 0;
}
static size_t vocab_slot(Vocab *v, uint64_t h) {
    size_t mask = v->cap - 1, i = (size_t)(h & mask);
    for (size_t probe = 0; probe < v->cap; ++probe) {
        size_t k = (i + probe) & mask;
        if (v->df[k] == 0) return k;
        if (v->hash[k] == h) return k;
    }
    return (size_t)(h & mask);
}

/* ==================================================== 摘要主流程 ===== */

int summarize_text(const wchar_t *text, int max_sentences, int use_textrank,
                   Summary *out, Arena *arena) {
    memset(out, 0, sizeof(*out));
    out->arena = arena;
    if (!text) return -1;

    size_t total = wcslen(text);
    if (total == 0) return -1;

    /* ---- 1) 分句 ---- */
    Arena *scratch = arena_new();
    if (!scratch) return -1;

    SpanList spans;
    spanlist_init(&spans, scratch);
    split_sentences(text, &spans);
    if (spans.n == 0) {
        spanlist_free(&spans);
        arena_free(scratch);
        return -1;
    }

    size_t n = spans.n;
    if (n > MAX_SENTENCES) n = MAX_SENTENCES;

    /* ---- 2) 每句分词，存成排序后的哈希数组 ---- */
    uint64_t **sent_tok = (uint64_t **)calloc(n, sizeof(uint64_t *));
    size_t    *sent_len = (size_t *)calloc(n, sizeof(size_t));
    Vocab      vocab;
    if (vocab_init(&vocab, 4096) != 0) {
        free(sent_tok); free(sent_len);
        spanlist_free(&spans); arena_free(scratch);
        return -1;
    }

    TokenList tl;
    tokens_init(&tl, scratch);

    for (size_t i = 0; i < n; ++i) {
        tl.n = 0;
        size_t st = spans.starts[i], en = spans.ends[i];
        /* 在句子区间内分词：临时复制一份（tokenize 需要 NUL 结尾） */
        wchar_t *seg = (wchar_t *)malloc((en - st + 1) * sizeof(wchar_t));
        if (!seg) continue;
        memcpy(seg, text + st, (en - st) * sizeof(wchar_t));
        seg[en - st] = 0;
        tokenize(seg, &tl);
        free(seg);

        if (tl.n == 0) { sent_tok[i] = NULL; sent_len[i] = 0; continue; }

        uint64_t *arr = (uint64_t *)malloc(tl.n * sizeof(uint64_t));
        if (!arr) { sent_tok[i] = NULL; continue; }
        for (size_t k = 0; k < tl.n; ++k) arr[k] = tok_hash(tl.items[k]);
        qsort(arr, tl.n, sizeof(uint64_t), cmp_u64);

        /* 去重后存 */
        size_t uniq = 0;
        for (size_t k = 0; k < tl.n; ++k)
            if (uniq == 0 || arr[k] != arr[uniq - 1]) arr[uniq++] = arr[k];
        sent_tok[i] = arr;
        sent_len[i] = uniq;

        /* 统计 df / tf */
        for (size_t k = 0; k < uniq; ++k) {
            size_t slot = vocab_slot(&vocab, arr[k]);
            if (vocab.df[slot] == 0) { vocab.hash[slot] = arr[k]; vocab.n++; }
            vocab.df[slot]++;
        }
        for (size_t k = 0; k < tl.n; ++k) {
            size_t slot = vocab_slot(&vocab, tok_hash(tl.items[k]));
            if (vocab.repr[slot] == NULL) vocab.repr[slot] = tl.items[k];
            vocab.tf[slot]++;
        }
    }
    tokens_free(&tl);

    size_t N = n > 0 ? n : 1;
    double *score = (double *)calloc(n, sizeof(double));
    if (!score) {
        for (size_t i = 0; i < n; ++i) free(sent_tok[i]);
        free(sent_tok); free(sent_len);
        vocab_free(&vocab); arena_free(scratch);
        return -1;
    }

    /* ---- 3) 打分 ---- */
    if (use_textrank) {
        /* 稀疏邻接：只保存非零相似度的边 */
        typedef struct { uint32_t j; double w; } Edge;
        Edge **adj = (Edge **)calloc(n, sizeof(Edge *));
        size_t *deg = (size_t *)calloc(n, sizeof(size_t));
        size_t *cap = (size_t *)calloc(n, sizeof(size_t));

        for (size_t i = 0; i < n; ++i) {
            if (!sent_tok[i]) continue;
            for (size_t j = i + 1; j < n; ++j) {
                if (!sent_tok[j]) continue;
                /* 有序数组求交集 */
                size_t a = 0, b = 0, common = 0;
                while (a < sent_len[i] && b < sent_len[j]) {
                    if (sent_tok[i][a] == sent_tok[j][b]) { common++; a++; b++; }
                    else if (sent_tok[i][a] < sent_tok[j][b]) a++;
                    else b++;
                }
                if (common == 0) continue;
                double w = (double)common /
                           (log((double)sent_len[i] + 1.0) +
                            log((double)sent_len[j] + 1.0));
                if (w <= 0) continue;
                /* 对称加边 */
                for (int dir = 0; dir < 2; ++dir) {
                    size_t x = dir ? j : i, y = dir ? i : j;
                    if (deg[x] + 1 > cap[x]) {
                        size_t nc = cap[x] ? cap[x] * 2 : 4;
                        Edge *ne = (Edge *)realloc(adj[x], nc * sizeof(Edge));
                        if (!ne) break;
                        adj[x] = ne; cap[x] = nc;
                    }
                    adj[x][deg[x]].j = (uint32_t)y;
                    adj[x][deg[x]].w = w;
                    deg[x]++;
                }
            }
        }

        double *wsum = (double *)calloc(n, sizeof(double));
        double *next = (double *)calloc(n, sizeof(double));
        for (size_t i = 0; i < n; ++i) {
            for (size_t k = 0; k < deg[i]; ++k) wsum[i] += adj[i][k].w;
            score[i] = 1.0;
        }

        for (int it = 0; it < TEXTRANK_ITER; ++it) {
            double diff = 0.0;
            for (size_t i = 0; i < n; ++i) {
                double sum = 0.0;
                for (size_t k = 0; k < deg[i]; ++k) {
                    size_t j = adj[i][k].j;
                    if (wsum[j] > 0) sum += adj[i][k].w / wsum[j] * score[j];
                }
                next[i] = (1.0 - TEXTRANK_DAMP) + TEXTRANK_DAMP * sum;
                diff += fabs(next[i] - score[i]);
            }
            memcpy(score, next, n * sizeof(double));
            if (diff < 1e-6) break;
        }

        /* 孤立句（没有任何边）用长度做一点补偿，否则永远是 0.15 */
        for (size_t i = 0; i < n; ++i)
            if (deg[i] == 0 && sent_len[i] > 0)
                score[i] += 0.02 * (double)sent_len[i] / 20.0;

        for (size_t i = 0; i < n; ++i) free(adj[i]);
        free(adj); free(deg); free(cap); free(wsum); free(next);
    } else {
        /* TF-IDF */
        for (size_t i = 0; i < n; ++i) {
            if (!sent_tok[i] || sent_len[i] == 0) { score[i] = 0; continue; }
            double s = 0.0;
            for (size_t k = 0; k < sent_len[i]; ++k) {
                size_t slot = vocab_slot(&vocab, sent_tok[i][k]);
                int df = vocab.df[slot] > 0 ? vocab.df[slot] : 1;
                int tf = vocab.tf[slot] > 0 ? vocab.tf[slot] : 1;
                double idf = log((double)N / (double)df) + 1.0;
                s += (1.0 + log((double)tf)) * idf;
            }
            score[i] = s / sqrt((double)sent_len[i]);
        }
    }

    /* ---- 4) 选前 k 句 ---- */
    int k = max_sentences;
    if (k <= 0) {
        /* 按正文长度自动决定 3~8 句 */
        k = (int)(N / 6);
        if (k < 3) k = 3;
        if (k > 8) k = 8;
        if ((size_t)k > n) k = (int)n;
    }
    if (k > (int)n) k = (int)n;
    if (k < 1) k = 1;

    /* 用下标做部分排序 */
    size_t *order = (size_t *)malloc(n * sizeof(size_t));
    for (size_t i = 0; i < n; ++i) order[i] = i;
    /* 稳定的选择排序（k 很小） */
    for (int a = 0; a < k; ++a) {
        size_t best = a;
        for (size_t b = a + 1; b < n; ++b)
            if (score[order[b]] > score[order[best]]) best = b;
        size_t t = order[a]; order[a] = order[best]; order[best] = t;
    }

    /* 选出来的按原文顺序排回去 */
    for (int a = 1; a < k; ++a) {
        size_t key = order[a];
        int b = a - 1;
        while (b >= 0 && order[b] > key) { order[b + 1] = order[b]; b--; }
        order[b + 1] = key;
    }

    out->sentences = (KeySentence *)arena_alloc(arena, (size_t)k * sizeof(KeySentence));
    if (!out->sentences) {
        free(order); free(score);
        for (size_t i = 0; i < n; ++i) free(sent_tok[i]);
        free(sent_tok); free(sent_len);
        vocab_free(&vocab); arena_free(scratch);
        return -1;
    }
    out->n_sentences = (size_t)k;
    for (int a = 0; a < k; ++a) {
        size_t i = order[a];
        out->sentences[a].start = (i < spans.n) ? spans.starts[i] : 0;
        out->sentences[a].end   = (i < spans.n) ? spans.ends[i]   : 0;
        out->sentences[a].score = score[i];
    }
    spanlist_free(&spans);

    /* ---- 5) 关键词（tf*idf 排序） ---- */
    {
        size_t want = 12;
        if (vocab.n < want) want = vocab.n;

        /* 收集所有词槽 */
        size_t *slots = (size_t *)malloc((vocab.n ? vocab.n : 1) * sizeof(size_t));
        size_t sn = 0;
        for (size_t s = 0; s < vocab.cap; ++s)
            if (vocab.df[s] > 0) slots[sn++] = s;

        double *ks = (double *)malloc((sn ? sn : 1) * sizeof(double));
        for (size_t i = 0; i < sn; ++i) {
            size_t s = slots[i];
            double idf = log((double)N / (double)vocab.df[s]) + 1.0;
            ks[i] = (1.0 + log((double)vocab.tf[s])) * idf;
        }
        /* 只选出前 want 个（词数可能很大，用部分选择） */
        for (size_t a = 0; a < sn && a < want; ++a) {
            size_t best = a;
            for (size_t b = a + 1; b < sn; ++b)
                if (ks[b] > ks[best]) best = b;
            if (best != a) {
                double t = ks[a]; ks[a] = ks[best]; ks[best] = t;
                size_t ts = slots[a]; slots[a] = slots[best]; slots[best] = ts;
            }
        }

        out->keywords = (wchar_t **)arena_alloc(arena, want * sizeof(wchar_t *));
        out->keyword_scores = (double *)arena_alloc(arena, want * sizeof(double));
        if (out->keywords && out->keyword_scores) {
            out->n_keywords = want;
            for (size_t a = 0; a < want; ++a) {
                const wchar_t *w = vocab.repr[slots[a]];
                out->keywords[a] = arena_wcsdup(arena, w ? w : L"");
                out->keyword_scores[a] = ks[a];
            }
        }
        free(slots);
        free(ks);
    }

    free(order);
    free(score);
    for (size_t i = 0; i < n; ++i) free(sent_tok[i]);
    free(sent_tok); free(sent_len);
    vocab_free(&vocab);
    arena_free(scratch);
    return 0;
}
