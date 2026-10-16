/* ==========================================================================
 * LocalCraft — 正则引擎（完全自研，不依赖第三方库 / 不需要 regex.h）
 * regex.c
 *
 * 设计
 * ----
 *  1. 解析：递归下降把模式编译成 AST。递归深度只跟"模式结构"（括号嵌套）
 *     有关，跟输入长度无关。解析时限制嵌套层数，绝不因为长输入爆栈。
 *
 *  2. 执行：AST 编译成线性字节码，用 **显式回溯栈** 运行
 *     （Russ Cox 式 backtracking VM）。匹配过程零递归 —— 2MB 输入、
 *     极深的量词嵌套都不会撑爆 C 栈，因为栈全在堆上。
 *
 *     - `SPLIT x` ：把 (x, 当前输入位置) 压入回溯栈，然后**继续往下走**
 *                   （下一条指令就是"优先分支"）。
 *     - `JMP x`   ：无条件跳转。
 *     - `LOOP x`  ：进入循环体 x，并在循环状态里记下当前位置。
 *     - `GUARD`   ：循环体结束。位置前进了就跳回 SPLIT 再来一轮；
 *                   位置没动（空匹配）就结束循环，避免 `(a*)*` 死循环。
 *     - `SAVE n`  ：把当前位置写进捕获槽 n。
 *     回溯时用"路径记录"撤销所有槽位改动（捕获 + 循环状态），
 *     保证栈回退到哪个时刻，状态就是哪个时刻的状态。
 *
 *  3. 安全性：全局步数上限 LC_STEP_LIMIT（2,000,000）。超限立刻停止，
 *     返回已经得到的结果 —— 绝不挂死、绝不崩溃。`(a+)+b` 这类灾难性
 *     回溯只会慢一小会儿，然后放弃。
 *
 *  4. 内存：标准 malloc / free（不用 Arena），regex_free 全部回收。
 *
 * ---------------------------------------------------------------------------
 * 支持的语法
 *   字面量 · . · ^ · $ · | · ( ... ) · (?: ... )
 *   * + ? 以及懒惰形式 *? +? ??
 *   {n} {n,} {n,m} 以及它们的懒惰形式
 *   [abc] [^abc] [a-z] 以及类内的 \d \w \s \D \W \S
 *   转义 \d \w \s \D \W \S \n \t \r \f \v \0 · 以及 \. \* \\ 等任意转义字面量
 *   ignore_case 大小写不敏感匹配（wide 字符，中文按单字符处理）
 *   替换 repl 支持 \0 \& \1..\9 与 \\ ，其余字符原样输出
 *
 * 有意不实现
 *   模式内的反向引用、前后瞻 (?= (?! (?<= (?<!、命名组 (?<name>)、
 *   \b \B 单词边界、POSIX 字符集 [[:alpha:]]、类内区间运算 [a-z&&[^aeiou]]、
 *   多行模式（^ $ 只认整个字符串的首尾）、\p{...}、UTF-16 代理对拆分。
 * ========================================================================== */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600     /* 需要 GetTickCount64（Windows Vista+） */
#endif
#include "lc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

/* ======================================================== 限制常量 ==== */

/* ---------------------------------------------------------------- 两道闸
 *
 * 目的：任何模式 + 任何输入都必须"有界时间"内返回，绝不挂死 UI。
 *
 * 闸 1 —— 步数预算（第一道，也是主要的）：
 *   每次 regex_matches / regex_replace_all 调用会算出一个预算
 *       budget = LC_STEP_LIMIT + len(输入) * LC_STEPS_PER_CHAR
 *   单次"从某个起点开始"的匹配尝试最多走这么多步，超了立即放弃。
 *   它随输入长度线性放大，所以 2MB 文本上的线性匹配不会被误杀；
 *   而指数级的灾难性回溯（(a+)+b 之类）会在几步内撞上限。
 *
 * 闸 2 —— 墙钟（第二道，兜底）：
 *   整个调用还有 LC_TIME_BUDGET_MS 毫秒的上限，循环里定期查表。
 *   超时就停止，把已经产生的结果返回（regex_replace_all 返回 0
 *   并写出部分结果，regex_matches 返回"不匹配"）。这样即使遇到
 *   某些模式结构让步数估算失准，也绝不会无限跑下去。
 * ---------------------------------------------------------------------- */

/* 单次匹配尝试的基础步数预算 */
#define LC_STEP_LIMIT     2000000u
/* 每个输入字符额外允许的步数（线性匹配留的余量，贪心量词约 4~5 步/字符） */
#define LC_STEPS_PER_CHAR 8u
/* 一次调用的墙钟上限（毫秒） */
#define LC_TIME_BUDGET_MS 2000u
#define LC_MAX_PATTERN    1000000     /* 模式长度上限 */
#define LC_MAX_REPEAT     100000      /* {n,m} 展开上限 */
#define LC_MAX_LOOP_ITER  100000      /* 单个循环最多重复次数 */
#define LC_MAX_DEPTH      150         /* 括号嵌套上限（限制递归深度，防爆栈） */
/*: 允许的最大捕获组编号。替换里用 \1..\31 引用；
    \10 及以上只在"这个组确实存在"时才按两位解析，否则按单数字语义。 */
#define LC_MAX_GROUPS     31
/*: 捕获槽数组长度。每个组占两个槽（起点/终点），再加整体匹配的 slot 0。
    必须是 2*(LC_MAX_GROUPS+1)，否则组号稍大就会出现"能编译但引用不到"。 */
#define LC_NCAPS          (2 * (LC_MAX_GROUPS + 1))
#define LC_FOLD_CACHE     256         /* 大小写折叠缓存 */
#define LC_FOLD_NONE      ((wchar_t)0xFFFF)

/* 单调毫秒时钟（Vista+ 用 64 位；老系统退化为 32 位并做回绕修正） */
static ULONGLONG lc_now_ms(void) { return (ULONGLONG)GetTickCount64(); }

/* ============================================================ 语法树 ==== */

typedef struct { wchar_t lo, hi; } LC_Range;   /* 闭区间 */

typedef enum {
    AST_EMPTY = 0,   /* 空（如 a| 的空支） */
    AST_CHAR,        /* 字面量 */
    AST_ANY,         /* . */
    AST_BOL,         /* ^ */
    AST_EOL,         /* $ */
    AST_CLASS,       /* [ ... ] */
    AST_SEQ,         /* 顺序 a+b */
    AST_ALT,         /* 分支 a|b */
    AST_QUANT,       /* 量词 */
    AST_GROUP        /* ( ... ) */
} AstType;

typedef struct AstNode AstNode;
struct AstNode {
    AstType   type;
    wchar_t   ch;        /* AST_CHAR */
    int       neg;       /* AST_CLASS 取反 */
    LC_Range *ranges;    /* AST_CLASS */
    size_t    nr;        /* AST_CLASS 区间数 */
    AstNode  *a, *b;     /* SEQ: a+b  ALT: a|b  QUANT/GROUP: body */
    int       min, max;  /* AST_QUANT：max < 0 表示无上限 */
    int       lazy;      /* AST_QUANT 懒惰 */
    int       group;     /* AST_GROUP 捕获组号，0 = 非捕获 */
};

static AstNode *ast_new(AstType t) {
    AstNode *n = (AstNode *)calloc(1, sizeof(AstNode));
    if (n) { n->type = t; n->max = -1; }
    return n;
}

static void ast_free(AstNode *n) {
    if (!n) return;
    ast_free(n->a);
    ast_free(n->b);
    free(n->ranges);
    free(n);
}

/* ===================================================== 区间集合 ==== */

typedef struct {
    LC_Range *v;
    size_t    n, cap;
    int       oom;
} RangeVec;

static void rv_init(RangeVec *rv) { rv->v = NULL; rv->n = rv->cap = 0; rv->oom = 0; }
static void rv_free(RangeVec *rv) { free(rv->v); rv_init(rv); }

static void rv_push(RangeVec *rv, wchar_t lo, wchar_t hi) {
    if (lo > hi) { wchar_t t = lo; lo = hi; hi = t; }
    if (rv->n == rv->cap) {
        size_t c = rv->cap ? rv->cap * 2 : 8;
        LC_Range *p = (LC_Range *)realloc(rv->v, c * sizeof(LC_Range));
        if (!p) { rv->oom = 1; return; }
        rv->v = p; rv->cap = c;
    }
    rv->v[rv->n].lo = lo;
    rv->v[rv->n].hi = hi;
    rv->n++;
}

/* ASCII 语义的预定义集合（不依赖 locale，跨平台稳定） */
static void rv_add_digit(RangeVec *rv) { rv_push(rv, L'0', L'9'); }
static void rv_add_word(RangeVec *rv) {
    rv_push(rv, L'0', L'9');
    rv_push(rv, L'A', L'Z');
    rv_push(rv, L'_', L'_');
    rv_push(rv, L'a', L'z');
}
static void rv_add_space(RangeVec *rv) {
    rv_push(rv, L'\t', L'\r');
    rv_push(rv, L' ',  L' ');
}
/* 全字符集，供取反兜底使用 */
static void rv_add_all(RangeVec *rv) { rv_push(rv, (wchar_t)0, (wchar_t)0xFFFF); }

/* ========================================================== 解析器 ==== */

typedef struct {
    const wchar_t *p;
    size_t         pos, len;
    int            ngroups;
    int            depth;
    int            failed;
    wchar_t       *err;
    size_t         errcch;
    unsigned       guard;
} Parser;

static void perr(Parser *P, const wchar_t *msg) {
    if (P->failed) return;
    P->failed = 1;
    if (P->err && P->errcch) {
        wcsncpy(P->err, msg, P->errcch - 1);
        P->err[P->errcch - 1] = 0;
    }
}

static wchar_t peek(Parser *P)  { return (P->pos < P->len) ? P->p[P->pos] : 0; }
static wchar_t peek2(Parser *P) { return (P->pos + 1 < P->len) ? P->p[P->pos + 1] : 0; }

static int is_digit_w(wchar_t c) { return c >= L'0' && c <= L'9'; }
static int is_hex_w(wchar_t c) {
    return is_digit_w(c) || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}
static int hexval_w(wchar_t c) {
    if (c >= L'0' && c <= L'9') return (int)(c - L'0');
    if (c >= L'a' && c <= L'f') return (int)(c - L'a') + 10;
    return (int)(c - L'A') + 10;
}

static AstNode *parse_expr(Parser *P);

/*
 * 解析一个转义（P->pos 指向 '\\'）。
 * 返回 1：结果是"预定义字符集"，区间累加到 rv，取反标志放入 *neg。
 * 返回 0：结果是单个字面量，放入 *ch。
 * 返回 -1：语法错误。
 */
static int parse_escape(Parser *P, wchar_t *ch, RangeVec *rv, int *neg)
{
    wchar_t c;
    if (P->pos >= P->len) { perr(P, L"模式以反斜杠 \\ 结尾，转义不完整"); return -1; }
    P->pos++;                       /* 吃掉 '\\' */
    if (P->pos >= P->len) { perr(P, L"模式以反斜杠 \\ 结尾，转义不完整"); return -1; }
    c = P->p[P->pos++];

    switch (c) {
    case L'd': rv_add_digit(rv); *neg = 0; return 1;
    case L'D': rv_add_digit(rv); *neg = 1; return 1;
    case L'w': rv_add_word(rv);  *neg = 0; return 1;
    case L'W': rv_add_word(rv);  *neg = 1; return 1;
    case L's': rv_add_space(rv); *neg = 0; return 1;
    case L'S': rv_add_space(rv); *neg = 1; return 1;
    case L'n': *ch = L'\n'; return 0;
    case L't': *ch = L'\t'; return 0;
    case L'r': *ch = L'\r'; return 0;
    case L'f': *ch = L'\f'; return 0;
    case L'v': *ch = L'\v'; return 0;
    case L'0': *ch = L'\0'; return 0;
    case L'x': {
        unsigned v = 0; int i;
        for (i = 0; i < 2 && is_hex_w(peek(P)); i++)
            v = v * 16u + (unsigned)hexval_w(P->p[P->pos++]);
        if (i == 0) { *ch = L'x'; return 0; }
        *ch = (wchar_t)v;
        return 0;
    }
    default:
        *ch = c;                    /* \. \* \\ \| \( \[ 等 */
        return 0;
    }
}

/*
 * 字符类 [...]，P->pos 停在 '[' 上。
 * 约定：'\d' 等正转义直接把区间并进集合；'\D' 等负转义等价于类取反。
 *       '-' 在开头或结尾是字面量，'a-z' 是区间。
 */
static AstNode *parse_class(Parser *P)
{
    AstNode  *node;
    RangeVec  rv;
    int       neg = 0, first = 1;

    rv_init(&rv);
    P->pos++;                                   /* 吃掉 '[' */
    if (peek(P) == L'^') { neg = 1; P->pos++; }

    for (;;) {
        wchar_t c, lo = 0;
        int     neg_esc = 0, is_set = 0;

        if (P->pos >= P->len) {
            perr(P, L"字符类 [ 没有对应的 ]");
            rv_free(&rv);
            return NULL;
        }
        c = P->p[P->pos];
        if (c == L']' && !first) { P->pos++; break; }   /* 类结束 */
        first = 0;

        if (c == L'\\') {
            RangeVec tmp;
            rv_init(&tmp);
            is_set = 0; neg_esc = 0;
            if (parse_escape(P, &lo, &tmp, &neg_esc) < 0) {
                rv_free(&tmp); rv_free(&rv); return NULL;
            }
            if (is_set || (P->failed == 0 && tmp.n > 0)) {
                /* 预定义集合 */
                size_t i;
                if (neg_esc) neg = 1;
                for (i = 0; i < tmp.n; i++) rv_push(&rv, tmp.v[i].lo, tmp.v[i].hi);
                rv_free(&tmp);
                if (rv.oom) { perr(P, L"内存不足"); rv_free(&rv); return NULL; }
                continue;
            }
            rv_free(&tmp);
        } else {
            lo = c;
            P->pos++;
        }

        /* 看看后面是不是区间端点 */
        if (peek(P) == L'-' && P->pos + 1 < P->len && P->p[P->pos + 1] != L']') {
            wchar_t hi = 0;
            P->pos++;                       /* 吃掉 '-' */
            if (peek(P) == L'\\') {
                RangeVec t2; int n2 = 0;
                rv_init(&t2);
                if (parse_escape(P, &hi, &t2, &n2) != 0) {
                    rv_free(&t2);
                    perr(P, L"字符类的区间端点不能是 \\d \\w \\s 这类集合");
                    rv_free(&rv);
                    return NULL;
                }
                rv_free(&t2);
            } else {
                hi = P->p[P->pos++];
            }
            if (lo > hi) {
                perr(P, L"字符类的区间顺序颠倒（例如 [z-a]）");
                rv_free(&rv);
                return NULL;
            }
            rv_push(&rv, lo, hi);
        } else {
            rv_push(&rv, lo, lo);
        }
        if (rv.oom) { perr(P, L"内存不足"); rv_free(&rv); return NULL; }
    }

    if (rv.n == 0 && !neg) {
        perr(P, L"空的字符类 [] 没有意义");
        rv_free(&rv);
        return NULL;
    }
    if (rv.n == 0) rv_add_all(&rv);         /* 取反类兜底 */

    node = ast_new(AST_CLASS);
    if (!node) { perr(P, L"内存不足"); rv_free(&rv); return NULL; }
    node->ranges = rv.v;
    node->nr     = rv.n;
    node->neg    = neg;
    return node;
}

/*
 * {n} {n,} {n,m}，P->pos 停在 '{' 上。
 * 合法则前进并返回 1；不是合法量词返回 0（调用方按字面量 '{' 处理）；
 * 是"看起来像量词但非法"则报错并返回 -1。
 */
static int parse_brace(Parser *P, int *min_out, int *max_out)
{
    size_t save = P->pos;
    size_t q = save + 1;
    long n1 = 0, n2 = 0;
    int  has_n1 = 0, has_n2 = 0, has_comma = 0;

    while (q < P->len && is_digit_w(P->p[q])) {
        n1 = n1 * 10 + (long)(P->p[q] - L'0');
        if (n1 > (long)LC_MAX_REPEAT * 10L) { P->pos = save; return 0; }
        has_n1 = 1; q++;
    }
    if (q < P->len && P->p[q] == L',') {
        has_comma = 1; q++;
        while (q < P->len && is_digit_w(P->p[q])) {
            n2 = n2 * 10 + (long)(P->p[q] - L'0');
            if (n2 > (long)LC_MAX_REPEAT * 10L) { P->pos = save; return 0; }
            has_n2 = 1; q++;
        }
    }
    if (q >= P->len || P->p[q] != L'}') { P->pos = save; return 0; }
    /* 必须有第一个数字：{n} / {n,} / {n,m}。
     * "{}"、"{,3}"、"{,}" 都不是量词，按普通字符处理。 */
    if (!has_n1) { P->pos = save; return 0; }

    if (!has_comma)                { *min_out = (int)n1; *max_out = (int)n1; }
    else if (!has_n2)              { *min_out = (int)n1; *max_out = -1; }
    else                           { *min_out = (int)n1; *max_out = (int)n2; }

    if (*min_out > LC_MAX_REPEAT || (*max_out >= 0 && *max_out > LC_MAX_REPEAT)) {
        perr(P, L"重复次数 {n,m} 太大（上限 100000）");
        return -1;
    }
    if (*max_out >= 0 && *max_out < *min_out) {
        perr(P, L"重复次数区间无效：{n,m} 必须满足 m >= n");
        return -1;
    }
    P->pos = q + 1;
    return 1;
}

/* 原子（不含量词）。返回节点、NULL（结尾 / '|' / ')'），失败时 NULL + failed。 */
static AstNode *parse_atom(Parser *P)
{
    wchar_t c;

    if (P->pos >= P->len) return NULL;
    c = P->p[P->pos];
    if (c == L')' || c == L'|') return NULL;

    if (++P->guard > (unsigned)LC_MAX_PATTERN * 4u + 4096u) {
        perr(P, L"正则表达式过于复杂");
        return NULL;
    }

    if (c == L'*' || c == L'+' || c == L'?') {
        perr(P, L"量词位置错误：* + ? 前面必须有可重复的表达式");
        return NULL;
    }

    if (c == L'.') { P->pos++; return ast_new(AST_ANY); }
    if (c == L'^') { P->pos++; return ast_new(AST_BOL); }
    if (c == L'$') { P->pos++; return ast_new(AST_EOL); }
    if (c == L'[') return parse_class(P);

    if (c == L'(') {
        AstNode *sub, *g;
        int      cap = 1;

        P->pos++;
        if (peek(P) == L'?' && peek2(P) == L':') { P->pos += 2; cap = 0; }
        else if (peek(P) == L'?') {
            perr(P, L"不支持的组语法（只支持 (?: ... )，不支持 (?= (?< (?i) 等）");
            return NULL;
        }

        if (P->depth >= LC_MAX_DEPTH) {
            perr(P, L"括号嵌套层数过多");
            return NULL;
        }
        P->depth++;
        sub = parse_expr(P);
        P->depth--;

        if (P->failed) { ast_free(sub); return NULL; }
        if (peek(P) != L')') {
            ast_free(sub);
            perr(P, L"括号 ( 没有对应的 )");
            return NULL;
        }
        P->pos++;

        g = ast_new(AST_GROUP);
        if (!g) { ast_free(sub); perr(P, L"内存不足"); return NULL; }
        if (cap) {
            if (P->ngroups >= LC_MAX_GROUPS) {
                ast_free(sub);
                perr(P, L"捕获组过多（最多 31 组）");
                return NULL;
            }
            g->group = ++P->ngroups;
        }
        g->a = sub;
        return g;
    }

    if (c == L'\\') {
        wchar_t   ch = 0;
        RangeVec  rv;
        int       neg = 0;
        int       rc;
        rv_init(&rv);
        rc = parse_escape(P, &ch, &rv, &neg);
        if (rc < 0) { rv_free(&rv); return NULL; }
        if (rc == 1) {
            AstNode *n = ast_new(AST_CLASS);
            if (!n) { rv_free(&rv); perr(P, L"内存不足"); return NULL; }
            n->ranges = rv.v;
            n->nr     = rv.n;
            n->neg    = neg;
            return n;
        }
        rv_free(&rv);
        {
            AstNode *n = ast_new(AST_CHAR);
            if (!n) { perr(P, L"内存不足"); return NULL; }
            n->ch = ch;
            return n;
        }
    }

    P->pos++;
    {
        AstNode *n = ast_new(AST_CHAR);
        if (!n) { perr(P, L"内存不足"); return NULL; }
        n->ch = c;
        return n;
    }
}

/* 一串原子 + 量词，直到 ')' / '|' / 结尾 */
static AstNode *parse_seq(Parser *P)
{
    AstNode *result = NULL;

    while (!P->failed) {
        wchar_t  c;
        AstNode *atom;
        int      min = 1, max = 1, lazy = 0, has_q = 0;

        if (P->pos >= P->len) break;
        c = P->p[P->pos];
        if (c == L')' || c == L'|') break;

        if (++P->guard > (unsigned)LC_MAX_PATTERN * 4u + 4096u) {
            perr(P, L"正则表达式过于复杂");
            break;
        }

        atom = parse_atom(P);
        if (P->failed) { ast_free(atom); ast_free(result); return NULL; }
        if (!atom) break;

        if (peek(P) == L'*')      { P->pos++; min = 0; max = -1; has_q = 1; }
        else if (peek(P) == L'+') { P->pos++; min = 1; max = -1; has_q = 1; }
        else if (peek(P) == L'?') { P->pos++; min = 0; max = 1;  has_q = 1; }
        else if (peek(P) == L'{') {
            int bmin = 0, bmax = 0, br = parse_brace(P, &bmin, &bmax);
            if (br < 0) { ast_free(atom); ast_free(result); return NULL; }
            if (br == 1) { min = bmin; max = bmax; has_q = 1; }
            /* br == 0：'{' 是普通字面量，已由 parse_atom 之外的原样处理 */
        }

        if (has_q) {
            if (peek(P) == L'?') { P->pos++; lazy = 1; }
            if (!(min == 1 && max == 1)) {
                AstNode *q = ast_new(AST_QUANT);
                if (!q) {
                    ast_free(atom); ast_free(result);
                    perr(P, L"内存不足"); return NULL;
                }
                q->a = atom; q->min = min; q->max = max; q->lazy = lazy;
                atom = q;
            }
            if (peek(P) == L'*' || peek(P) == L'+' || peek(P) == L'?' ||
                peek(P) == L'{') {
                perr(P, L"量词重复：* + ? { } 不能连续出现");
                ast_free(atom); ast_free(result);
                return NULL;
            }
        }

        if (!result) {
            result = atom;
        } else {
            AstNode *s = ast_new(AST_SEQ);
            if (!s) { ast_free(atom); ast_free(result); perr(P, L"内存不足"); return NULL; }
            s->a = result;
            s->b = atom;
            result = s;
        }
    }

    if (P->failed) { ast_free(result); return NULL; }
    if (!result) {
        AstNode *e = ast_new(AST_EMPTY);
        if (!e) { perr(P, L"内存不足"); return NULL; }
        return e;
    }
    return result;
}

/* 分支 a|b|c */
static AstNode *parse_alt(Parser *P)
{
    AstNode *left = parse_seq(P);
    if (P->failed) { ast_free(left); return NULL; }

    while (peek(P) == L'|') {
        AstNode *right, *n;
        P->pos++;
        right = parse_seq(P);
        if (P->failed) { ast_free(left); ast_free(right); return NULL; }
        n = ast_new(AST_ALT);
        if (!n) { ast_free(left); ast_free(right); perr(P, L"内存不足"); return NULL; }
        n->a = left;
        n->b = right;
        left = n;
    }
    return left;
}

static AstNode *parse_expr(Parser *P) { return parse_alt(P); }

/* ============================================================ 字节码 ==== */

enum {
    OP_CHAR = 0,   /* 匹配字面量 ch                      */
    OP_ANY,        /* 匹配任意非 \n 字符                  */
    OP_CLASS,      /* 匹配字符类  a=区间数 p1=区间数组 ch=取反 */
    OP_BOL,        /* ^                                   */
    OP_EOL,        /* $                                   */
    OP_SAVE,       /* 捕获槽 a <- 当前位置                */
    OP_SPLIT,      /* 压入回溯点 a，继续执行下一条         */
    OP_JMP,        /* 跳到 a                              */
    OP_LOOP,       /* 进入循环 a（循环号 p2）            */
    OP_GUARD,      /* 循环体出口：前进->a 重来，没前进->b 退出 */
    OP_END         /* 程序正常结束（匹配成功）。放在最后一条，
                      保证"下一条 pc"永远在程序内部，不会误判成功 */
};

typedef struct {
    int       op;
    wchar_t   ch;       /* OP_CHAR：字面量；OP_CLASS：是否取反 */
    int       a, b;     /* 跳转目标 / 参数 */
    int       p2;       /* 循环号（OP_LOOP / OP_GUARD / 真循环的 OP_SPLIT） */
    int       p3;       /* 循环计数器槽号（OP_SPLIT/OP_LOOP：-1 表示不是循环） */
    int       p4;       /* 最少重复次数（仅 OP_GUARD 用） */
    LC_Range *ranges;   /* OP_CLASS 的区间数组（自有内存） */
} Inst;

struct LcRegex {
    Inst   *code;
    size_t  n, cap;
    size_t  nloops;
    int     ngroups;
    int     ignore_case;
    wchar_t fold_cache[LC_FOLD_CACHE];
};

typedef struct {
    LcRegex *re;
    int      failed;
    wchar_t *err;
    size_t   errcch;
    int      depth;
    unsigned guard;
} Ctx;

static void cerr(Ctx *c, const wchar_t *msg) {
    if (c->failed) return;
    c->failed = 1;
    if (c->err && c->errcch) {
        wcsncpy(c->err, msg, c->errcch - 1);
        c->err[c->errcch - 1] = 0;
    }
}

static int emit(Ctx *c, int op, int a, int b, int p2, wchar_t ch) {
    LcRegex *re = c->re;
    int idx;
    if (c->failed) return -1;
    if (re->n == re->cap) {
        size_t nc = re->cap ? re->cap * 2 : 64;
        Inst *p = (Inst *)realloc(re->code, nc * sizeof(Inst));
        if (!p) { cerr(c, L"内存不足"); return -1; }
        re->code = p;
        re->cap  = nc;
    }
    idx = (int)re->n++;
    re->code[idx].op     = op;
    re->code[idx].ch     = ch;
    re->code[idx].a      = a;
    re->code[idx].b      = b;
    re->code[idx].p2     = p2;
    re->code[idx].p3     = -1;
    re->code[idx].p4     = 0;
    re->code[idx].ranges = NULL;
    return idx;
}

static int emit_split(Ctx *c, int target) {
    return emit(c, OP_SPLIT, target, 0, 0, 0);
}

static int compile_node(Ctx *c, const AstNode *n);

/*
 * 把 body 复制 count 份。每份之间是"顺序"，不需要额外跳转。
 */
static int emit_copies(Ctx *c, const AstNode *body, int count)
{
    int i;
    for (i = 0; i < count; i++) {
        if (c->failed) return -1;
        if (compile_node(c, body) < 0) return -1;
    }
    return 0;
}

/*
 * 收集量词区域的"出口占位符"：
 *   - exits[]  : SPLIT 指令号（它的回溯目标 b 应该指向区域之后）
 *   - jmps[]   : JMP 指令号（它应该跳到区域之后）
 * 区域生成完之后统一 patch。
 */
typedef struct {
    int *v; size_t n, cap; int oom;
} IntVec;

static void iv_init(IntVec *x) { x->v = NULL; x->n = x->cap = 0; x->oom = 0; }
static void iv_free(IntVec *x) { free(x->v); iv_init(x); }
static void iv_push(IntVec *x, int v) {
    if (x->n == x->cap) {
        size_t c = x->cap ? x->cap * 2 : 16;
        int *p = (int *)realloc(x->v, c * sizeof(int));
        if (!p) { x->oom = 1; return; }
        x->v = p; x->cap = c;
    }
    x->v[x->n++] = v;
}

typedef struct {
    IntVec splits;     /* SPLIT 指令号 */
    IntVec bodies;     /* 与 splits 一一对应：该 SPLIT 的"循环体/可选体入口" */
    IntVec lazy_jmps;  /* 与 splits 一一对应：懒惰 SPLIT 之后那条 JMP 指令号
                          （需要 patch 成区域出口；贪婪时为 -1） */
    IntVec jmps;       /* 需要跳到区域之后的 JMP */
    IntVec guards;     /* 需要把 b 设成区域之后的 GUARD */
    int    oom;
} QuantPatch;

static void qp_push(QuantPatch *q, int split, int body, int lazy_jmp) {
    iv_push(&q->splits, split);
    iv_push(&q->bodies, body);
    iv_push(&q->lazy_jmps, lazy_jmp);
}

/*
 * 通用量词生成。
 *
 * 指令语义回顾：SPLIT 是"优先执行下一条指令，把 a 压成回溯点"。
 *
 *   {n}    : B B B
 *   {0,1}  : SPLIT(体 / 回溯 after) ; B ; JMP after
 *   {0,1}? : SPLIT(体 / 回溯 after) ; JMP after ; B
 *   {n,m}  : {n} + {0,m-n}
 *   {n,}   : {n} + LOOP L ; L: SPLIT(体 / after) ; B ; JMP L ; GUARD(L / after)
 *
 * 贪婪与懒惰只差"优先路径"：
 *   贪婪 -> SPLIT 的优先路径就是紧接着的体（体排在 SPLIT 之后）
 *   懒惰 -> SPLIT 之后先放一条 JMP after，把优先路径引向"退出"
 * 两种情况下 SPLIT.a（回溯目标）都是"体入口"，所以退出与进入都能正确回溯。
 */
static int emit_quant_region(Ctx *c, const AstNode *body, int min, int max,
                             int lazy, QuantPatch *q)
{
    LcRegex *re = c->re;

    if (max == 0) return 0;                     /* 匹配 0 次 */

    if (max < 0) {                              /* {n,} */
        int li, ci, loop_i, split, bs, j, guard, lj = -1;
        if (emit_copies(c, body, min) < 0) return -1;

        li     = (int)re->nloops++;
        ci     = (int)re->nloops++;             /* 循环计数器槽 */
        loop_i = emit(c, OP_LOOP, 0, 0, li, 0);
        if (loop_i < 0) return -1;
        re->code[loop_i].p3 = ci;
        re->code[loop_i].p4 = min;              /* 最少重复次数 */

        split  = emit_split(c, 0);
        if (split < 0) return -1;
        re->code[split].p2 = li;                /* 标记"这是真循环的 SPLIT" */
        re->code[split].p3 = ci;

        if (lazy) {
            lj = emit(c, OP_JMP, 0, 0, 0, 0);   /* 优先退出；目标稍后 patch */
            if (lj < 0) return -1;
        }
        bs = (int)re->n;
        if (compile_node(c, body) < 0) return -1;
        guard = emit(c, OP_GUARD, split, 0, li, 0);
        if (guard < 0) return -1;
        /* 循环体结束必须回到 GUARD（不是回到 SPLIT！）。
         * GUARD 负责判断"这一轮有没有前进"：没前进就退出循环，
         * 否则再跳回 SPLIT 开始下一轮。回到 SPLIT 会绕过 GUARD，
         * 空匹配时就会无限循环。 */
        j = emit(c, OP_JMP, guard, 0, 0, 0);
        if (j < 0) return -1;
        re->code[guard].p3 = ci;
        re->code[guard].p4 = min;

        re->code[loop_i].a = split;   /* 进入循环要先经过 SPLIT */
        qp_push(q, split, bs, lj);
        /* 循环体末尾那条 JMP 的目标已经写成 guard，不需要再 patch */
        iv_push(&q->guards, guard);
        return 0;
    }

    if (min >= max) return emit_copies(c, body, min);   /* {n}：正好 n 份 */

    /* 有界重复 {n,m}：先做 min 份必需的，再用一条"可选副本链"做 m-n 份。
     * 链的构造（opt>0 时）：
     *      SPLIT  -> b=body_entry（回溯时进入本份）
     *      可选副本体（懒惰时前面先放一条 JMP 到区域出口）
     *      然后递归处理 opt-1；贪婪时副本体之后补一条 JMP 到区域出口
     * opt 每次减 1，所以一定终止。 */
    if (min > 0) {
        if (emit_copies(c, body, min) < 0) return -1;
    }

    /* 可选副本链 */
    if (!lazy) {
        int split = emit_split(c, 0);
        int bs, lj = -1, j;
        if (split < 0) return -1;
        bs = (int)re->n;
        if (compile_node(c, body) < 0) return -1;
        qp_push(q, split, bs, lj);
        if (max - min - 1 > 0) {
            if (emit_quant_region(c, body, 0, max - min - 1, 0, q) < 0) return -1;
        }
        j = emit(c, OP_JMP, 0, 0, 0, 0);
        if (j < 0) return -1;
        iv_push(&q->jmps, j);
        return 0;
    }

    /* 懒惰链：SPLIT ; JMP(exit) ; body ; ... 优先走退出 */
    {
        int split = emit_split(c, 0);
        int bs, lj, j;
        if (split < 0) return -1;
        lj = emit(c, OP_JMP, 0, 0, 0, 0);
        if (lj < 0) return -1;
        bs = (int)re->n;
        if (compile_node(c, body) < 0) return -1;
        qp_push(q, split, bs, lj);
        if (max - min - 1 > 0) {
            if (emit_quant_region(c, body, 0, max - min - 1, 1, q) < 0) return -1;
        }
        j = emit(c, OP_JMP, 0, 0, 0, 0);
        if (j < 0) return -1;
        iv_push(&q->jmps, j);
        return 0;
    }
}

static int compile_quant(Ctx *c, const AstNode *n)
{
    LcRegex *re = c->re;
    QuantPatch q;
    size_t i;
    int    after;

    iv_init(&q.splits);
    iv_init(&q.bodies);
    iv_init(&q.lazy_jmps);
    iv_init(&q.jmps);
    iv_init(&q.guards);
    q.oom = 0;

    if (emit_quant_region(c, n->a, n->min, n->max, n->lazy, &q) < 0)
        goto bad;
    if (q.splits.oom || q.bodies.oom || q.lazy_jmps.oom ||
        q.jmps.oom || q.guards.oom) {
        cerr(c, L"内存不足");
        goto bad;
    }

    after = (int)re->n;

    /* SPLIT：优先分支 = 下一条指令（编译器保证它是对的），
     * 回溯点 b = "另一条路"：
     *   贪婪 -> 直接跳到区域出口（after）
     *   懒惰 -> 跳到循环体入口（bodies[i]），退出由后面那条 JMP 负责
     * a 字段保留体入口，仅作调试信息。 */
    for (i = 0; i < q.splits.n; i++) {
        int si = q.splits.v[i];
        int bi = (i < q.bodies.n) ? q.bodies.v[i] : 0;
        int lj = (i < q.lazy_jmps.n) ? q.lazy_jmps.v[i] : -1;
        re->code[si].a = bi;
        re->code[si].b = (lj >= 0) ? bi : after;
        if (lj >= 0) re->code[lj].a = after;
    }
    for (i = 0; i < q.jmps.n; i++)   re->code[q.jmps.v[i]].a   = after;
    for (i = 0; i < q.guards.n; i++) re->code[q.guards.v[i]].b = after;

    iv_free(&q.splits); iv_free(&q.bodies); iv_free(&q.lazy_jmps);
    iv_free(&q.jmps);   iv_free(&q.guards);
    return 0;

bad:
    iv_free(&q.splits); iv_free(&q.bodies); iv_free(&q.lazy_jmps);
    iv_free(&q.jmps);   iv_free(&q.guards);
    return -1;
}

static int compile_node(Ctx *c, const AstNode *n)
{
    if (c->failed || !n) return -1;

    if (++c->guard > (unsigned)LC_MAX_PATTERN * 8u + 8192u) {
        cerr(c, L"正则表达式过于复杂");
        return -1;
    }
    if (c->depth > LC_MAX_DEPTH + 16) {
        cerr(c, L"模式嵌套过深");
        return -1;
    }

    switch (n->type) {
    case AST_EMPTY:
        return 0;

    case AST_CHAR:
        return emit(c, OP_CHAR, 0, 0, 0, n->ch) < 0 ? -1 : 0;

    case AST_ANY:
        return emit(c, OP_ANY, 0, 0, 0, 0) < 0 ? -1 : 0;

    case AST_BOL:
        return emit(c, OP_BOL, 0, 0, 0, 0) < 0 ? -1 : 0;

    case AST_EOL:
        return emit(c, OP_EOL, 0, 0, 0, 0) < 0 ? -1 : 0;

    case AST_CLASS: {
        int idx = emit(c, OP_CLASS, (int)n->nr, 0, 0,
                       (wchar_t)(n->neg ? 1 : 0));
        LC_Range *r;
        if (idx < 0) return -1;
        r = (LC_Range *)malloc(n->nr * sizeof(LC_Range));
        if (!r) { cerr(c, L"内存不足"); return -1; }
        memcpy(r, n->ranges, n->nr * sizeof(LC_Range));
        c->re->code[idx].ranges = r;
        return 0;
    }

    case AST_GROUP:
        /* 0 号捕获组（整体匹配）由 regex_compile 的序言/尾声统一处理，
           这里只处理用户可见的 \1..\9，避免重复写槽 0/1 */
        if (n->group && emit(c, OP_SAVE, n->group * 2, 0, 0, 0) < 0) return -1;
        c->depth++;
        if (compile_node(c, n->a) < 0) { c->depth--; return -1; }
        c->depth--;
        if (n->group && emit(c, OP_SAVE, n->group * 2 + 1, 0, 0, 0) < 0) return -1;
        return 0;

    case AST_SEQ:
        if (compile_node(c, n->a) < 0) return -1;
        return compile_node(c, n->b);

    case AST_ALT: {
        /* 形状：SPLIT ; <左支> ; JMP after ; <右支> ; after:
         * 优先路径 = SPLIT 的下一条（左支）；回溯点 b = 右支入口。 */
        int split = emit_split(c, 0);
        int right, j;
        if (split < 0) return -1;
        if (compile_node(c, n->a) < 0) return -1;
        j = emit(c, OP_JMP, 0, 0, 0, 0);
        if (j < 0) return -1;
        right = (int)c->re->n;
        if (compile_node(c, n->b) < 0) return -1;
        c->re->code[split].a = right;   /* 调试信息 / 体入口 */
        c->re->code[split].b = right;   /* 回溯 -> 右支 */
        c->re->code[j].a     = (int)c->re->n;   /* 左支成功 -> 跳过右支 */
        return 0;
    }

    case AST_QUANT:
        c->depth++;
        if (compile_quant(c, n) < 0) { c->depth--; return -1; }
        c->depth--;
        return 0;

    default:
        cerr(c, L"内部错误：未知的语法树节点");
        return -1;
    }
}

/* ======================================================= 匹配执行器 ==== */

/* 路径记录：记下"某槽位被改写前的旧值"，回溯时撤销。
 *   idx >= 0 ：捕获槽 idx
 *   idx <  0 ：循环状态槽 (-idx-1)
 */
typedef struct { int idx; int old; } Trail;

/* 回溯点 */
typedef struct { size_t pc; size_t pos; size_t trail; } Marker;

/* 循环状态：lpos[循环号] = 最近一次进入循环体时的位置，供 GUARD 判断是否前进。
 * 它同样通过路径记录参与回溯，所以不需要额外的栈。 */

typedef struct {
    Trail  *trail; size_t nt, tcap;
    Marker *bt;    size_t nbt, bcap;
    int    *lpos;  size_t nl, lcap;   /* 循环状态：位置 + 迭代计数 */
    ULONGLONG deadline;               /* 墙钟截止时间（GetTickCount64） */
    unsigned  tick;                   /* 距上次查时钟的步数 */
} Vm;

static void vm_free(Vm *v) {
    free(v->trail); free(v->bt); free(v->lpos);
    memset(v, 0, sizeof(*v));
}

/* 路径记录的编码：
 *   idx >= 0   -> 捕获槽 idx
 *   idx <  0   -> 循环状态槽 (-idx-1)（位置或迭代计数），old 是旧值 */
#define TR_LOOP(li)  (-((int)(li)) - 1)
#define TR_IS_LOOP(i) ((i) < 0)

static int t_push(Vm *v, int idx, int old) {
    if (v->nt == v->tcap) {
        size_t c = v->tcap ? v->tcap * 2 : 256;
        Trail *p = (Trail *)realloc(v->trail, c * sizeof(Trail));
        if (!p) return -1;
        v->trail = p; v->tcap = c;
    }
    v->trail[v->nt].idx = idx;
    v->trail[v->nt].old = old;
    v->nt++;
    return 0;
}

static int b_push(Vm *v, size_t pc, size_t pos, size_t trail) {
    if (v->nbt == v->bcap) {
        size_t c = v->bcap ? v->bcap * 2 : 256;
        Marker *p = (Marker *)realloc(v->bt, c * sizeof(Marker));
        if (!p) return -1;
        v->bt = p; v->bcap = c;
    }
    v->bt[v->nbt].pc    = pc;
    v->bt[v->nbt].pos   = pos;
    v->bt[v->nbt].trail = trail;
    v->nbt++;
    return 0;
}

/* 保证循环状态槽存在并返回它的指针（不写路径记录） */
static int *l_cell(Vm *v, int li) {
    if (li < 0) return NULL;
    if ((size_t)li >= v->nl) {
        size_t c = (size_t)li + 8;
        int *p = (int *)realloc(v->lpos, c * sizeof(int));
        if (!p) return NULL;
        v->lpos = p;
        while (v->nl < c) v->lpos[v->nl++] = 0;
        v->lcap = c;
    }
    return &v->lpos[li];
}

/* 设置 lpos[li] = val，并把旧值记进路径记录 */
static int l_set(Vm *v, int li, int val) {
    int *p = l_cell(v, li);
    if (!p) return -1;
    if (t_push(v, TR_LOOP(li), *p) < 0) return -1;
    *p = val;
    return 0;
}

static int l_get(const Vm *v, int li) {
    if ((size_t)li >= v->nl) return -1;
    return v->lpos[li];
}

/* 回到 mark：撤销 mark 之后的所有改动（捕获槽 + 循环状态） */
static void vm_rewind(Vm *v, const LcRegex *re, int *caps, size_t mark) {
    (void)re;
    while (v->nt > mark) {
        Trail *t = &v->trail[--v->nt];
        if (TR_IS_LOOP(t->idx)) {
            int li = -(t->idx) - 1;
            if ((size_t)li < v->nl) v->lpos[li] = t->old;
        } else {
            caps[t->idx] = t->old;
        }
    }
}

/* 大小写折叠（带缓存；缓存让长文本上的折叠几乎零成本） */
static wchar_t fold(LcRegex *re, wchar_t c) {
    if ((unsigned)c < LC_FOLD_CACHE) {
        wchar_t r = re->fold_cache[(unsigned)c];
        if (r != LC_FOLD_NONE) return r;
        r = (wchar_t)towlower(c);
        re->fold_cache[(unsigned)c] = r;
        return r;
    }
    return (wchar_t)towlower(c);
}

static int chr_eq(LcRegex *re, wchar_t a, wchar_t b) {
    if (a == b) return 1;
    if (!re->ignore_case) return 0;
    return fold(re, a) == fold(re, b);
}

/* c 是否落在区间 r 里（大小写不敏感时同时看折叠形式） */
static int class_hit_range(LcRegex *re, const LC_Range *r, wchar_t c) {
    if (c >= r->lo && c <= r->hi) return 1;
    if (re->ignore_case) {
        wchar_t lc = fold(re, c);
        wchar_t uc = (wchar_t)towupper(c);
        if (lc >= r->lo && lc <= r->hi) return 1;
        if (uc >= r->lo && uc <= r->hi) return 1;
    }
    return 0;
}

/* 从 start 位置开始尝试匹配。返回 1 成功（caps 填好）、0 失败/放弃、-1 OOM。
 * deadline 为 0 表示不做墙钟限制（仍然受步数预算保护）。
 * step_budget 是本轮允许的最大步数（由调用方按输入长度算好）。 */
static int vm_match_at(LcRegex *re, const wchar_t *s, size_t len,
                       size_t start, int *caps, ULONGLONG deadline,
                       unsigned step_budget)
{
    const Inst *code = re->code;
    size_t      pc = 0, pos = start;
    Vm          v;
    unsigned    steps = 0;
    size_t      i;
    int         rc = 0;

    memset(&v, 0, sizeof(v));
    v.deadline = deadline;
    for (i = 0; i < LC_NCAPS; i++) caps[i] = -1;

    for (;;) {
        const Inst *in;

        /* 闸 1：步数预算（随输入长度放大，绝不无限） */
        if (++steps > step_budget) { rc = 0; goto done; }
        /* 闸 2：墙钟。每 4096 步查一次，避免频繁调用系统 API */
        if (v.deadline && ((++v.tick & 0xFFFu) == 0) &&
            lc_now_ms() >= v.deadline) {
            rc = 0;
            goto done;
        }
        if (pc >= re->n) { rc = 1; goto done; }     /* 正常结束（兜底） */

        in = &code[pc];
        switch (in->op) {
        case OP_CHAR:
            if (pos < len && chr_eq(re, s[pos], in->ch)) { pos++; pc++; continue; }
            break;

        case OP_ANY:
            if (pos < len && s[pos] != L'\n') { pos++; pc++; continue; }
            break;

        case OP_CLASS: {
            /* 注意：越界时直接失败，绝不能翻转 neg 之后当成功 ——
             * 否则取反类会"匹配"字符串结尾，然后无限吃下去。 */
            if (pos < len) {
                int hit = 0;
                wchar_t c = s[pos];
                size_t  k, n = (size_t)in->a;
                for (k = 0; k < n; k++) {
                    if (class_hit_range(re, &in->ranges[k], c)) { hit = 1; break; }
                }
                if (in->ch) hit = !hit;
                if (hit) { pos++; pc++; continue; }
            }
            break;
        }

        case OP_BOL:
            if (pos == 0) { pc++; continue; }
            break;

        case OP_EOL:
            if (pos == len) { pc++; continue; }
            break;

        case OP_SAVE:
            if (in->a >= 0 && in->a < LC_NCAPS) {
                if (t_push(&v, in->a, caps[in->a]) < 0) { rc = -1; goto done; }
                caps[in->a] = (int)pos;
            }
            pc++;
            continue;

        case OP_JMP:
            pc = (size_t)in->a;
            continue;

        case OP_SPLIT: {
            int cnt = in->p3;      /* 循环计数器槽；-1 表示普通分支 */
            if (cnt >= 0) {
                /* 真循环的 SPLIT：记录"本轮起点"，并把迭代计数 +1。
                 * 两者都走路径记录，所以回溯时会自动恢复。 */
                int *cptr = l_cell(&v, cnt);
                int *pptr = l_cell(&v, in->p2);
                if (!cptr || !pptr) { rc = -1; goto done; }
                if (t_push(&v, TR_LOOP(cnt), *cptr) < 0) { rc = -1; goto done; }
                *cptr = *cptr + 1;
                if (t_push(&v, TR_LOOP(in->p2), *pptr) < 0) { rc = -1; goto done; }
                *pptr = (int)pos;
            }
            /* 优先分支 = 下一条指令（继续执行）；
             * 回溯点 = b（"另一条路"：贪婪时是退出，懒惰时是进入循环体）。
             * 编译器会把"优先走的那条"摆在 SPLIT 后面。 */
            if (b_push(&v, (size_t)in->b, pos, v.nt) < 0) { rc = -1; goto done; }
            pc++;
            continue;
        }

        case OP_LOOP: {
            int *cptr = l_cell(&v, in->p3);
            if (!cptr) { rc = -1; goto done; }
            *cptr = 0;                          /* 迭代计数清零 */
            if (l_set(&v, in->p2, (int)pos) < 0) { rc = -1; goto done; }
            pc = (size_t)in->a;
            continue;
        }

        case OP_GUARD: {
            /* 循环体出口。相对"本轮 SPLIT 时的位置"前进了就再来一轮；
             * 位置没动（空匹配）就结束循环 —— `(a*)*` 不会死循环。
             * 最少次数由编译器在循环前面生成的"必需副本"保证，
             * 这里不需要再判一次（多判反而会破坏回溯）。 */
            if (l_get(&v, in->p2) != (int)pos) { pc = (size_t)in->a; continue; }
            pc = (size_t)in->b;
            continue;
        }

        case OP_END:
            rc = 1; goto done;

        default:
            break;
        }

        /* ---------- 失败：回溯 ---------- */
        if (v.nbt == 0) { rc = 0; goto done; }
        v.nbt--;
        pc  = v.bt[v.nbt].pc;
        pos = v.bt[v.nbt].pos;
        vm_rewind(&v, re, caps, v.bt[v.nbt].trail);
    }

done:
    vm_free(&v);
    return rc;
}

/* 在 s[from..] 中寻找第一个匹配。
 * 预算按输入长度线性放大，保证大文本上的线性匹配不被误杀。 */
static int vm_search_ex(LcRegex *re, const wchar_t *s, size_t len, size_t from,
                        int *caps, ULONGLONG deadline)
{
    size_t   i;
    unsigned budget = LC_STEP_LIMIT;

    if (len <= (size_t)(~(unsigned)0) / LC_STEPS_PER_CHAR)
        budget += (unsigned)(len * LC_STEPS_PER_CHAR);

    for (i = from; i <= len; i++) {
        int rc;
        /* 每个起点之前查一次墙钟，保证"整体"有界 */
        if (deadline && lc_now_ms() >= deadline) return 0;
        rc = vm_match_at(re, s, len, i, caps, deadline, budget);
        if (rc != 0) return rc;
    }
    return 0;
}

static int vm_search(LcRegex *re, const wchar_t *s, size_t len, size_t from,
                     int *caps)
{
    return vm_search_ex(re, s, len, from, caps,
                        lc_now_ms() + LC_TIME_BUDGET_MS);
}

/* ============================================================ 公开接口 ==== */

LcRegex *regex_compile(const wchar_t *pattern, int ignore_case, wchar_t *err,
                       size_t errcch)
{
    Parser   P;
    AstNode *root = NULL;
    LcRegex *re;
    Ctx      c;
    int      i;

    if (err && errcch) err[0] = 0;
    if (!pattern) {
        if (err && errcch) {
            wcsncpy(err, L"正则表达式为空", errcch - 1);
            err[errcch - 1] = 0;
        }
        return NULL;
    }

    P.p       = pattern;
    P.pos     = 0;
    P.len     = wcslen(pattern);
    P.ngroups = 0;
    P.depth   = 0;
    P.failed  = 0;
    P.err     = err;
    P.errcch  = errcch;
    P.guard   = 0;

    if (P.len > LC_MAX_PATTERN) {
        perr(&P, L"正则表达式过长");
        return NULL;
    }

    root = parse_expr(&P);
    if (P.failed) { ast_free(root); return NULL; }
    if (P.pos != P.len) {
        ast_free(root);
        if (pattern[P.pos] == L')') perr(&P, L"多余的右括号 )");
        else                        perr(&P, L"正则表达式语法错误");
        return NULL;
    }

    re = (LcRegex *)calloc(1, sizeof(LcRegex));
    if (!re) { ast_free(root); perr(&P, L"内存不足"); return NULL; }
    for (i = 0; i < LC_FOLD_CACHE; i++) re->fold_cache[i] = LC_FOLD_NONE;
    re->ignore_case = ignore_case ? 1 : 0;
    re->ngroups     = P.ngroups;
    re->nloops      = 0;

    c.re     = re;
    c.failed = 0;
    c.err    = err;
    c.errcch = errcch;
    c.depth  = 0;
    c.guard  = 0;

    if (emit(&c, OP_SAVE, 0, 0, 0, 0) < 0)      goto bad;   /* 整体匹配起点 */
    if (compile_node(&c, root) < 0)              goto bad;
    if (emit(&c, OP_SAVE, 1, 0, 0, 0) < 0)       goto bad;   /* 整体匹配终点 */
    if (emit(&c, OP_END, 0, 0, 0, 0) < 0)        goto bad;   /* 正常结束 */

    ast_free(root);
    return re;

bad:
    ast_free(root);
    regex_free(re);
    return NULL;
}

void regex_free(LcRegex *re) {
    size_t i;
    if (!re) return;
    if (re->code) {
        for (i = 0; i < re->n; i++) free(re->code[i].ranges);
        free(re->code);
    }
    free(re);
}

int regex_matches(LcRegex *re, const wchar_t *input) {
    int caps[LC_NCAPS];
    int rc;
    if (!re || !input) return 0;
    rc = vm_search(re, input, wcslen(input), 0, caps);
    return rc == 1 ? 1 : 0;
}

/* 把 repl 追加到 out，展开 \0 \& \1..\9（以及组存在时的 \10..\31）与 \\ */
static int append_repl(WStr *out, const wchar_t *repl, const wchar_t *input,
                       const int *caps, int ngroups)
{
    const wchar_t *p;

    if (!repl) return 0;
    for (p = repl; *p; p++) {
        if (*p != L'\\') { if (wstr_putc(out, *p) != 0) return -1; continue; }
        p++;
        if (!*p) {                                  /* 结尾孤立的反斜杠 */
            if (wstr_putc(out, L'\\') != 0) return -1;
            break;
        }
        if (*p == L'\\')      { if (wstr_putc(out, L'\\') != 0) return -1; }
        else if (*p == L'0' || *p == L'&') {         /* 整体匹配 */
            if (caps[0] >= 0 && caps[1] >= caps[0]) {
                if (wstr_putn(out, input + caps[0],
                              (size_t)(caps[1] - caps[0])) != 0) return -1;
            }
        } else if (*p >= L'1' && *p <= L'9') {
            int g = (int)(*p - L'0');
            /* 两位数组号：只有当那个组确实存在时才按两位解析，
               否则保持标准单数字语义（\1 后跟字面字符 '2'）。 */
            if (p[1] >= L'0' && p[1] <= L'9') {
                int g2 = g * 10 + (int)(p[1] - L'0');
                if (g2 > 9 && g2 <= ngroups && g2 <= LC_MAX_GROUPS) {
                    g = g2;
                    p++;
                }
            }
            if (2 * g + 1 < LC_NCAPS && caps[2 * g] >= 0 &&
                caps[2 * g + 1] >= caps[2 * g]) {
                if (wstr_putn(out, input + caps[2 * g],
                              (size_t)(caps[2 * g + 1] - caps[2 * g])) != 0)
                    return -1;
            }
            /* 组不存在 / 没参与匹配 -> 替换成空串 */
        } else {
            /* \x 之类：保留反斜杠与原字符（与多数工具一致） */
            if (wstr_putc(out, L'\\') != 0) return -1;
            if (wstr_putc(out, *p) != 0) return -1;
        }
    }
    return 0;
}

int regex_replace_all(LcRegex *re, const wchar_t *input, const wchar_t *repl,
                      WStr *out)
{
    int    caps[LC_NCAPS];
    size_t len, pos;
    size_t from;
    ULONGLONG deadline;

    if (!re || !out) return -1;
    if (!input) input = L"";

    len  = wcslen(input);
    pos  = 0;    /* 已经搬运到 out 的输入位置 */
    from = 0;    /* 下一轮扫描的起点 */

    /* 整个替换调用的墙钟上限：超时就停止，写出已产生的部分结果并返回 0。
     * 这样即使模式极端复杂，调用方（例如 UI 线程）也绝不会被卡住。 */
    deadline = lc_now_ms() + LC_TIME_BUDGET_MS;

    while (from <= len) {
        int rc;
        int made_progress = 0;

        if (lc_now_ms() >= deadline) break;     /* 时间到，输出已产生的部分 */
        rc = vm_search_ex(re, input, len, from, caps, deadline);
        if (rc < 0) return -1;                       /* 内存不足 */
        if (rc == 0) break;                          /* 不再有匹配（或预算耗尽） */

        if (caps[0] < 0) caps[0] = (int)from;
        if (caps[1] < caps[0]) caps[1] = (int)from;

        /* 夹紧到合法范围，防止意外越界 */
        if ((size_t)caps[0] > len) caps[0] = (int)len;
        if ((size_t)caps[1] > len) caps[1] = (int)len;
        if (caps[1] < caps[0])     caps[1] = caps[0];

        /* 匹配之前的部分原样照抄 */
        if ((size_t)caps[0] > pos) {
            if (wstr_putn(out, input + pos, (size_t)caps[0] - pos) != 0) return -1;
        }
        if (append_repl(out, repl, input, caps, re->ngroups) != 0) return -1;

        if (caps[1] > caps[0]) {
            /* 非空匹配：正常前进 */
            pos  = (size_t)caps[1];
            from = pos;
            made_progress = 1;
        } else {
            /* 空匹配：必须手工吃一个字符，否则死循环。
             * 已经到串尾就结束 —— 尾部的空匹配只替换一次。 */
            if ((size_t)caps[0] < len) {
                if (wstr_putc(out, input[caps[0]]) != 0) return -1;
                pos  = (size_t)caps[0] + 1u;
                from = pos;
                made_progress = 1;
            } else {
                pos  = len;
                from = len + 1u;
            }
        }
        if (!made_progress) break;
    }

    if (pos < len) {
        if (wstr_putn(out, input + pos, len - pos) != 0) return -1;
    }
    return 0;
}
