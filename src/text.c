/* ==========================================================================
 * text.c —— 分词、分句、编码识别
 *
 * 中文分词的取舍
 * --------------
 * 正规做法要带词典（jieba 之类，几十 MB），与本项目"0.5 MB 单文件"的目标冲突。
 * 这里用**相邻二元组（bigram）**：
 *     "本地文件处理" -> 本地 地文 文件 件处 处理
 * 这是信息检索里的经典做法，不需要词典，召回率足够高，配合 SimHash / TF-IDF
 * 效果与分词几乎等价（多出来的噪声由停用词和权重自动压下去）。
 *
 * 英文/数字按词切分并小写化；其余符号当分隔符。
 * ========================================================================== */
#include "lc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>

/* ==================================================== 字符分类 ========= */

/* CJK 统一表意文字（含扩展 A）与常用兼容区 */
static int is_cjk(wchar_t c) {
    return (c >= 0x4E00 && c <= 0x9FFF)     /* 基本区 */
        || (c >= 0x3400 && c <= 0x4DBF)     /* 扩展 A */
        || (c >= 0xF900 && c <= 0xFAFF)     /* 兼容表意 */
        || (c >= 0x3040 && c <= 0x30FF)     /* 日文假名 */
        || (c >= 0xAC00 && c <= 0xD7AF);    /* 韩文谚文 */
}

static int is_word_char(wchar_t c) {
    if (is_cjk(c)) return 0;                /* CJK 单独处理 */
    if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')) return 1;
    if (c >= L'0' && c <= L'9') return 1;
    if (c == L'_' || c == L'\'' || c == L'-') return 1;
    if (c >= 0x00C0 && c <= 0x024F) return 1;   /* 拉丁扩展：é ü ñ 等 */
    return 0;
}

static int is_sentence_end(wchar_t c) {
    switch (c) {
        case L'。': case L'！': case L'？': case L'；':
        case L'…': case L'\n': case L'\r':
        case L'.': case L'!': case L'?': case L';':
            return 1;
        default:
            return 0;
    }
}

static int is_space(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\u3000' || c == L'\u00A0';
}

/* ==================================================== 停用词 =========== */

/* 中文虚词 + 英文停用词。它们对区分文档内容几乎没有贡献。 */
static const wchar_t *STOP_ZH[] = {
    L"的", L"了", L"和", L"是", L"就", L"都", L"而", L"及", L"与", L"着",
    L"或", L"一个", L"没有", L"我们", L"你们", L"他们", L"这个", L"那个",
    L"这些", L"那些", L"可以", L"因为", L"所以", L"但是", L"如果", L"虽然",
    L"并且", L"然后", L"以及", L"对于", L"关于", L"通过", L"进行", L"已经",
    L"还是", L"只是", L"不是", L"就是", L"也是", L"在", L"有", L"我", L"你",
    L"他", L"她", L"它", L"这", L"那", L"之", L"于", L"以", L"为", L"被",
    L"把", L"让", L"从", L"到", L"对", L"给", L"会", L"能", L"要", L"很",
    L"更", L"最", L"也", L"还", L"又", L"再", L"才", L"只", L"等", L"各",
    NULL
};

static const wchar_t *STOP_EN[] = {
    L"the", L"a", L"an", L"and", L"or", L"but", L"if", L"of", L"to", L"in",
    L"on", L"at", L"by", L"for", L"with", L"is", L"are", L"was", L"were",
    L"be", L"been", L"being", L"it", L"its", L"this", L"that", L"these",
    L"those", L"as", L"from", L"have", L"has", L"had", L"do", L"does", L"did",
    L"will", L"would", L"can", L"could", L"should", L"may", L"might", L"not",
    L"no", L"so", L"than", L"then", L"there", L"here", L"we", L"you", L"they",
    L"he", L"she", L"i", L"me", L"my", L"our", L"their", L"all", L"any",
    NULL
};

static int is_stopword(const wchar_t *w) {
    for (int i = 0; STOP_ZH[i]; ++i)
        if (wcscmp(w, STOP_ZH[i]) == 0) return 1;
    for (int i = 0; STOP_EN[i]; ++i)
        if (wcscmp(w, STOP_EN[i]) == 0) return 1;
    return 0;
}

/* ==================================================== Token 列表 ======= */

void tokens_init(TokenList *t, Arena *a) {
    t->items = NULL; t->n = 0; t->cap = 0; t->arena = a;
}

void tokens_free(TokenList *t) {
    if (!t) return;
    free(t->items);          /* 词本身在 arena 里 */
    t->items = NULL; t->n = t->cap = 0;
}

static int tokens_push(TokenList *t, const wchar_t *w, size_t len) {
    if (len == 0) return 0;
    if (t->n + 1 > t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 256;
        wchar_t **ni = (wchar_t **)realloc(t->items, cap * sizeof(wchar_t *));
        if (!ni) return -1;
        t->items = ni;
        t->cap = cap;
    }
    wchar_t *w2 = arena_wcsndup(t->arena, w, len);
    if (!w2) return -1;
    lc_wcs_lower(w2);
    if (is_stopword(w2)) return 0;
    t->items[t->n++] = w2;
    return 0;
}

int tokenize(const wchar_t *text, TokenList *out) {
    if (!text) return 0;

    size_t i = 0;
    while (text[i]) {
        wchar_t c = text[i];

        if (is_cjk(c)) {
            /* 收集整段连续 CJK，再切二元组 */
            size_t start = i;
            while (text[i] && is_cjk(text[i])) ++i;
            size_t run = i - start;
            if (run == 1) {
                tokens_push(out, text + start, 1);
            } else {
                for (size_t k = 0; k + 1 < run; ++k)
                    tokens_push(out, text + start + k, 2);
            }
            continue;
        }

        if (is_word_char(c)) {
            size_t start = i;
            while (text[i] && is_word_char(text[i])) ++i;
            tokens_push(out, text + start, i - start);
            continue;
        }

        ++i;   /* 其余字符是分隔符 */
    }
    return 0;
}

/* ==================================================== 分句 ============= */

void spanlist_init(SpanList *s, Arena *a) {
    s->starts = NULL; s->ends = NULL; s->n = 0; s->cap = 0; s->arena = a;
}

void spanlist_free(SpanList *s) {
    if (!s) return;
    free(s->starts);
    free(s->ends);
    s->starts = s->ends = NULL;
    s->n = s->cap = 0;
}

static int spanlist_push(SpanList *s, size_t start, size_t end) {
    if (end <= start) return 0;
    if (s->n + 1 > s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 64;
        size_t *ns = (size_t *)realloc(s->starts, cap * sizeof(size_t));
        if (!ns) return -1;
        s->starts = ns;
        size_t *ne = (size_t *)realloc(s->ends, cap * sizeof(size_t));
        if (!ne) return -1;
        s->ends = ne;
        s->cap = cap;
    }
    s->starts[s->n] = start;
    s->ends[s->n] = end;
    s->n++;
    return 0;
}

/* 去掉句子首尾空白，太短的句子（<2 字符）丢弃 */
static void push_trimmed(SpanList *s, const wchar_t *text, size_t start, size_t end) {
    while (start < end && (is_space(text[start]) || text[start] == L'\n' ||
                           text[start] == L'\r')) start++;
    while (end > start && (is_space(text[end - 1]) || text[end - 1] == L'\n' ||
                           text[end - 1] == L'\r')) end--;
    if (end - start < 2) return;
    spanlist_push(s, start, end);
}

void split_sentences(const wchar_t *text, SpanList *out) {
    if (!text) return;
    size_t start = 0, i = 0;
    while (text[i]) {
        wchar_t c = text[i];
        if (is_sentence_end(c)) {
            /* 连续的分句符一起吃掉，并把标点包含进句子区间 ——
               摘要回显时读起来才完整 */
            size_t pend = i + 1;
            while (text[pend] && is_sentence_end(text[pend])) ++pend;
            push_trimmed(out, text, start, pend);
            i = pend;
            start = i;
            continue;
        }
        /* 段落分隔也算句子边界 */
        if (c == 0x3000 && text[i + 1] == 0x3000) {
            push_trimmed(out, text, start, i + 1);
            start = i + 1;
        }
        ++i;
    }
    push_trimmed(out, text, start, i);
}

/* ==================================================== 编码识别 ========= */

wchar_t *decode_text_bytes(const unsigned char *data, size_t len) {
    if (!data) return NULL;

    /* UTF-16 LE BOM */
    if (len >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
        size_t chars = (len - 2) / 2;
        wchar_t *out = (wchar_t *)malloc((chars + 1) * sizeof(wchar_t));
        if (!out) return NULL;
        memcpy(out, data + 2, chars * sizeof(wchar_t));
        out[chars] = 0;
        return out;
    }
    /* UTF-16 BE BOM */
    if (len >= 2 && data[0] == 0xFE && data[1] == 0xFF) {
        size_t chars = (len - 2) / 2;
        wchar_t *out = (wchar_t *)malloc((chars + 1) * sizeof(wchar_t));
        if (!out) return NULL;
        for (size_t i = 0; i < chars; ++i)
            out[i] = (wchar_t)((data[2 + i*2] << 8) | data[3 + i*2]);
        out[chars] = 0;
        return out;
    }
    /* UTF-8 BOM */
    if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        data += 3;
        len  -= 3;
    }

    /* 先按 UTF-8 试。严格校验，失败就退回 GBK。 */
    {
        int ok = 1;
        for (size_t i = 0; i < len && ok; ) {
            unsigned char b = data[i];
            int extra;
            if      (b < 0x80)      extra = 0;
            else if ((b & 0xE0) == 0xC0) extra = 1;
            else if ((b & 0xF0) == 0xE0) extra = 2;
            else if ((b & 0xF8) == 0xF0) extra = 3;
            else { ok = 0; break; }
            if (i + (size_t)extra >= len) { ok = 0; break; }
            for (int k = 1; k <= extra; ++k)
                if ((data[i + k] & 0xC0) != 0x80) { ok = 0; break; }
            i += (size_t)extra + 1;
        }
        if (ok) {
            int need = MultiByteToWideChar(CP_UTF8, 0, (const char *)data,
                                           (int)len, NULL, 0);
            if (need > 0) {
                wchar_t *out = (wchar_t *)malloc(((size_t)need + 1) * sizeof(wchar_t));
                if (!out) return NULL;
                MultiByteToWideChar(CP_UTF8, 0, (const char *)data, (int)len,
                                    out, need);
                out[need] = 0;
                return out;
            }
        }
    }

    /* GBK / 系统 ANSI 兜底 */
    {
        int need = MultiByteToWideChar(936, 0, (const char *)data, (int)len, NULL, 0);
        if (need <= 0)
            need = MultiByteToWideChar(CP_ACP, 0, (const char *)data, (int)len, NULL, 0);
        if (need <= 0) return NULL;
        wchar_t *out = (wchar_t *)malloc(((size_t)need + 1) * sizeof(wchar_t));
        if (!out) return NULL;
        MultiByteToWideChar(936, 0, (const char *)data, (int)len, out, need);
        out[need] = 0;
        return out;
    }
}
