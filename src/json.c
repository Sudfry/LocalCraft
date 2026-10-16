/* ==========================================================================
 * LocalCraft — src/json.c
 * 自包含的 JSON 读写实现（C11，无第三方依赖）
 *
 * 设计要点
 * --------
 *  1. 解析结果全部从 Arena 分配；json_free 只是兼容用的空操作。
 *  2. 解析过程不复制整份输入：只用一个游标在输入缓冲上移动，
 *     并且每次读字符前都检查是否越界 —— 绝不读过缓冲区末尾。
 *  3. 数组/对象的中间结果用 malloc 的增长数组收集（几何增长，均摊 O(n)），
 *     结束时一次性按真实大小拷进 Arena —— 既没有二次复杂度，也不浪费 Arena。
 *  4. 对象（键数 >= 8）在解析时建立开放寻址哈希索引，键重复时"后来者覆盖"，
 *     查询 json_get 为 O(1)；小对象直接线性比较。
 *  5. 最大嵌套深度 200，超过返回 NULL（不会因为深递归而爆栈）。
 *  6. 写出去的时候不转义非 ASCII —— 中文原样输出为 UTF-8
 *     （等价于 Python 的 ensure_ascii=False）。
 * ========================================================================== */
#include "lc.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>

/* ======================================================= 内部数据结构 ==== */

#define JSON_MAX_DEPTH 200   /* 最大嵌套深度 */
#define OBJ_INDEX_MIN  8     /* 键数达到该值开始建哈希索引 */

typedef enum {
    JSON_T_NULL = 0,
    JSON_T_BOOL,
    JSON_T_NUM,
    JSON_T_STR,
    JSON_T_ARR,
    JSON_T_OBJ
} JsonKind;

typedef struct {
    char  *p;    /* Arena 里的 UTF-8 缓冲，NUL 结尾 */
    size_t n;    /* 真实字节数（可能含内嵌 NUL） */
} JsonStr;

typedef struct {
    JsonValue **items;
    size_t      n;
} JsonArr;

typedef struct {
    char       **keys;    /* 键：Arena 里的 UTF-8 缓冲（NUL 结尾） */
    size_t      *klens;   /* 键的真实字节数 */
    JsonValue  **vals;
    size_t       n;
    size_t      *idx;     /* 开放寻址表：存 槽位号+1，0 表示空槽；NULL=未建 */
    size_t       idxcap;  /* 表容量，2 的幂 */
} JsonObj;

struct JsonValue {
    JsonKind kind;
    union {
        int b;
        struct {
            double    d;
            long long i;
            int       is_int;   /* i 是否可用（整数记号且在 long long 范围内） */
        } num;
        JsonStr  s;
        JsonArr  arr;
        JsonObj  obj;
    } u;
};

/* 解析器状态 */
typedef struct {
    const char *p;
    const char *end;    /* 输入末尾（指向 NUL 之前） */
    Arena      *a;
    int         depth;
    int         err;
} JsonP;

/* 对象构建期（malloc 增长数组） */
typedef struct {
    char      **keys;
    size_t     *klens;
    JsonValue **vals;
    size_t      n, cap;
    size_t     *idx;
    size_t      idxcap;
} ObjBuild;

/* 数组构建期 */
typedef struct {
    JsonValue **items;
    size_t      n, cap;
} ArrBuild;

/* =========================================================== 小工具 ==== */

static int pj_isdigit(char c) { return c >= '0' && c <= '9'; }

static void pj_ws(JsonP *P) {
    while (P->p < P->end) {
        char c = *P->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') P->p++;
        else break;
    }
}

/* 把码点写成 UTF-8 追加到 s。
   落单代理（U+D800..U+DFFF）按 3 字节原样编码（WTF-8），保证可逆。 */
static void pj_utf8_put(Str *s, unsigned cp) {
    if (cp < 0x80u) {
        str_putc(s, (char)cp);
    } else if (cp < 0x800u) {
        str_putc(s, (char)(0xC0u | (cp >> 6)));
        str_putc(s, (char)(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        str_putc(s, (char)(0xE0u | (cp >> 12)));
        str_putc(s, (char)(0x80u | ((cp >> 6) & 0x3Fu)));
        str_putc(s, (char)(0x80u | (cp & 0x3Fu)));
    } else {
        str_putc(s, (char)(0xF0u | (cp >> 18)));
        str_putc(s, (char)(0x80u | ((cp >> 12) & 0x3Fu)));
        str_putc(s, (char)(0x80u | ((cp >> 6) & 0x3Fu)));
        str_putc(s, (char)(0x80u | (cp & 0x3Fu)));
    }
}

/* ============================================================== 写 ==== */

static void pj_put_u_escape(Str *out, unsigned cp) {
    static const char hex[] = "0123456789abcdef";
    str_putn(out, "\\u", 2);
    str_putc(out, hex[(cp >> 12) & 0xFu]);
    str_putc(out, hex[(cp >> 8) & 0xFu]);
    str_putc(out, hex[(cp >> 4) & 0xFu]);
    str_putc(out, hex[cp & 0xFu]);
}

/* 单个字节：只转义 ASCII 的控制字符、引号、反斜杠。
   >= 0x80 的字节（UTF-8 连续字节）原样输出。 */
static void pj_escape_byte(Str *out, unsigned char c) {
    switch (c) {
    case '"':  str_putn(out, "\\\"", 2); break;
    case '\\': str_putn(out, "\\\\", 2); break;
    case '\b': str_putn(out, "\\b", 2);  break;
    case '\f': str_putn(out, "\\f", 2);  break;
    case '\n': str_putn(out, "\\n", 2);  break;
    case '\r': str_putn(out, "\\r", 2);  break;
    case '\t': str_putn(out, "\\t", 2);  break;
    default:
        if (c < 0x20u) pj_put_u_escape(out, c);
        else str_putc(out, (char)c);
        break;
    }
}

/* 单个码点：ASCII 走转义表，代理按 \uXXXX，其余原样 UTF-8 */
static void pj_escape_cp(Str *out, unsigned cp) {
    if (cp < 0x80u) {
        pj_escape_byte(out, (unsigned char)cp);
    } else if (cp >= 0xD800u && cp <= 0xDFFFu) {
        pj_put_u_escape(out, cp);
    } else {
        pj_utf8_put(out, cp);
    }
}

void json_escape_char_to(Str *out, char c) {
    if (!out) return;
    str_putc(out, '"');
    pj_escape_byte(out, (unsigned char)c);
    str_putc(out, '"');
}

void json_escape_wchar_to(Str *out, wchar_t c) {
    if (!out) return;
    str_putc(out, '"');
    pj_escape_cp(out, (unsigned)c);
    str_putc(out, '"');
}

void json_escape_utf8_to(Str *out, const char *u) {
    if (!out) return;
    str_putc(out, '"');
    if (u) {
        const unsigned char *p = (const unsigned char *)u;
        while (*p) pj_escape_byte(out, *p++);
    }
    str_putc(out, '"');
}

void json_escape_to(Str *out, const wchar_t *w) {
    if (!out) return;
    str_putc(out, '"');
    if (w) {
        while (*w) {
            unsigned cp = (unsigned)(*w);
            /* UTF-16 代理对合并成一个码点 */
            if (cp >= 0xD800u && cp <= 0xDBFFu &&
                (unsigned)w[1] >= 0xDC00u && (unsigned)w[1] <= 0xDFFFu) {
                cp = 0x10000u + ((cp - 0xD800u) << 10) +
                     ((unsigned)w[1] - 0xDC00u);
                pj_escape_cp(out, cp);
                w += 2;
                continue;
            }
            pj_escape_cp(out, cp);
            w++;
        }
    }
    str_putc(out, '"');
}

/* ============================================================== 读 ==== */

/* 读取 4 位十六进制；成功返回 0 */
static int pj_hex4(const char *p, unsigned *out) {
    unsigned v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        char c = p[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else return -1;
        v = (v << 4) | d;
    }
    *out = v;
    return 0;
}

/* 词法解析一个字符串，把解码后的 UTF-8 追加到 out。
   调用时 *P->p == '"'；成功返回 0。 */
static int pj_raw_string(JsonP *P, Str *out) {
    P->p++; /* 跳过开引号 */
    for (;;) {
        unsigned char c;
        if (P->p >= P->end) return -1;      /* 未闭合 */
        c = (unsigned char)*P->p++;
        if (c == '"') return 0;
        if (c == '\\') {
            char e;
            if (P->p >= P->end) return -1;
            e = *P->p++;
            switch (e) {
            case '"':  str_putc(out, '"');  break;
            case '\\': str_putc(out, '\\'); break;
            case '/':  str_putc(out, '/');  break;
            case 'b':  str_putc(out, '\b'); break;
            case 'f':  str_putc(out, '\f'); break;
            case 'n':  str_putc(out, '\n'); break;
            case 'r':  str_putc(out, '\r'); break;
            case 't':  str_putc(out, '\t'); break;
            case 'u': {
                unsigned cp;
                if (P->end - P->p < 4) return -1;
                if (pj_hex4(P->p, &cp) != 0) return -1;
                P->p += 4;
                if (cp >= 0xD800u && cp <= 0xDBFFu) {
                    /* 高代理：尝试吃后面的低代理组成代理对 */
                    if (P->end - P->p >= 6 &&
                        P->p[0] == '\\' && P->p[1] == 'u') {
                        unsigned lo = 0;
                        if (pj_hex4(P->p + 2, &lo) == 0 &&
                            lo >= 0xDC00u && lo <= 0xDFFFu) {
                            P->p += 6;
                            pj_utf8_put(out, 0x10000u +
                                             ((cp - 0xD800u) << 10) +
                                             (lo - 0xDC00u));
                            break;
                        }
                    }
                    pj_utf8_put(out, cp);   /* 落单：原样保留，可逆 */
                } else {
                    pj_utf8_put(out, cp);
                }
                break;
            }
            default:
                return -1;                  /* 非法转义 */
            }
        } else if (c < 0x20u) {
            return -1;                      /* 字符串里不许出现裸控制字符 */
        } else {
            str_putc(out, (char)c);
        }
    }
}

static JsonValue *pj_value(JsonP *P);

static JsonValue *pj_new(JsonP *P, JsonKind k) {
    JsonValue *v = (JsonValue *)arena_zalloc(P->a, sizeof(JsonValue));
    if (!v) { P->err = 1; return NULL; }
    v->kind = k;
    return v;
}

static JsonValue *pj_simple(JsonP *P, JsonKind k, int b) {
    JsonValue *v = pj_new(P, k);
    if (v) v->u.b = b;
    return v;
}

static JsonValue *pj_string(JsonP *P) {
    Str s;
    JsonValue *v;
    char *buf;
    str_init(&s);
    if (pj_raw_string(P, &s) != 0) {
        str_free(&s);
        P->err = 1;
        return NULL;
    }
    v = pj_new(P, JSON_T_STR);
    if (!v) { str_free(&s); return NULL; }
    buf = (char *)arena_alloc(P->a, s.len + 1);
    if (!buf) { str_free(&s); P->err = 1; return NULL; }
    if (s.len) memcpy(buf, s.data, s.len);
    buf[s.len] = 0;
    v->u.s.p = buf;
    v->u.s.n = s.len;
    str_free(&s);
    return v;
}

static JsonValue *pj_number(JsonP *P) {
    const char *start = P->p;
    const char *q = P->p;
    int is_int = 1;
    size_t len;
    char stackbuf[64];
    char *tmp = stackbuf;
    double d;
    long long iv = 0;
    int has_i = 0;
    JsonValue *v;

    if (q < P->end && *q == '-') q++;
    if (q >= P->end || !pj_isdigit(*q)) { P->err = 1; return NULL; }
    if (*q == '0') {
        q++;
        if (q < P->end && pj_isdigit(*q)) { P->err = 1; return NULL; } /* 01 */
    } else {
        while (q < P->end && pj_isdigit(*q)) q++;
    }
    if (q < P->end && *q == '.') {
        is_int = 0;
        q++;
        if (q >= P->end || !pj_isdigit(*q)) { P->err = 1; return NULL; }
        while (q < P->end && pj_isdigit(*q)) q++;
    }
    if (q < P->end && (*q == 'e' || *q == 'E')) {
        is_int = 0;
        q++;
        if (q < P->end && (*q == '+' || *q == '-')) q++;
        if (q >= P->end || !pj_isdigit(*q)) { P->err = 1; return NULL; }
        while (q < P->end && pj_isdigit(*q)) q++;
    }

    len = (size_t)(q - start);
    if (len + 1 > sizeof(stackbuf)) {
        tmp = (char *)malloc(len + 1);
        if (!tmp) { P->err = 1; return NULL; }
    }
    memcpy(tmp, start, len);
    tmp[len] = 0;

    d = strtod(tmp, NULL);
    if (is_int) {
        char *vend = NULL;
        errno = 0;
        iv = strtoll(tmp, &vend, 10);
        if (errno != ERANGE && vend && *vend == 0) has_i = 1;
    }
    if (tmp != stackbuf) free(tmp);
    P->p = q;

    v = pj_new(P, JSON_T_NUM);
    if (!v) return NULL;
    v->u.num.d = d;
    v->u.num.i = iv;
    v->u.num.is_int = has_i;
    return v;
}

/* ---- 数组 ---- */

static int pj_arr_push(ArrBuild *b, JsonValue *v) {
    if (b->n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 8;
        JsonValue **p = (JsonValue **)realloc(b->items, cap * sizeof(JsonValue *));
        if (!p) return -1;
        b->items = p;
        b->cap = cap;
    }
    b->items[b->n++] = v;
    return 0;
}

static JsonValue *pj_array(JsonP *P) {
    ArrBuild b;
    JsonValue *v = NULL;
    memset(&b, 0, sizeof(b));

    P->p++; /* '[' */
    if (++P->depth > JSON_MAX_DEPTH) { P->err = 1; return NULL; }

    pj_ws(P);
    if (P->p < P->end && *P->p == ']') {
        P->p++;
    } else {
        for (;;) {
            JsonValue *item = pj_value(P);
            if (!item) goto fail;
            if (pj_arr_push(&b, item) != 0) { P->err = 1; goto fail; }
            pj_ws(P);
            if (P->p >= P->end) { P->err = 1; goto fail; }
            if (*P->p == ',') { P->p++; pj_ws(P); continue; }
            if (*P->p == ']') { P->p++; break; }
            P->err = 1;      /* 缺逗号 / 缺右括号 */
            goto fail;
        }
    }
    P->depth--;

    v = pj_new(P, JSON_T_ARR);
    if (!v) goto fail;
    if (b.n) {
        JsonValue **items = (JsonValue **)arena_alloc(P->a, b.n * sizeof(JsonValue *));
        if (!items) { P->err = 1; v = NULL; goto fail; }
        memcpy(items, b.items, b.n * sizeof(JsonValue *));
        v->u.arr.items = items;
        v->u.arr.n = b.n;
    }
    free(b.items);
    return v;

fail:
    free(b.items);
    return NULL;
}

/* ---- 对象（带键索引，重复键后者胜） ---- */

static size_t pj_obj_reserve(ObjBuild *b, size_t need) {
    if (need <= b->cap) return 0;
    {
        size_t cap = b->cap ? b->cap * 2 : 8;
        char **nk;
        size_t *nl;
        JsonValue **nv;
        while (cap < need) cap *= 2;
        nk = (char **)realloc(b->keys, cap * sizeof(char *));
        if (!nk) return 1;
        b->keys = nk;
        nl = (size_t *)realloc(b->klens, cap * sizeof(size_t));
        if (!nl) return 1;
        b->klens = nl;
        nv = (JsonValue **)realloc(b->vals, cap * sizeof(JsonValue *));
        if (!nv) return 1;
        b->vals = nv;
        b->cap = cap;
    }
    return 0;
}

static unsigned pj_hash(const char *k, size_t n) {
    unsigned h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= (unsigned char)k[i];
        h *= 16777619u;
    }
    h ^= (unsigned) n;
    h *= 16777619u;
    return h;
}

/* 建立/重建索引表（容量必须是 2 的幂） */
static int pj_obj_rehash(ObjBuild *b, size_t newcap) {
    size_t *ni = (size_t *)calloc(newcap, sizeof(size_t));
    size_t i;
    if (!ni) return -1;
    for (i = 0; i < b->n; i++) {
        size_t m = newcap - 1;
        size_t j = (size_t)pj_hash(b->keys[i], b->klens[i]) & m;
        while (ni[j] != 0) j = (j + 1) & m;
        ni[j] = i + 1;
    }
    free(b->idx);
    b->idx = ni;
    b->idxcap = newcap;
    return 0;
}

static int pj_obj_put(ObjBuild *b, char *key, size_t klen, JsonValue *v) {
    if (!b->idx && b->n + 1 >= OBJ_INDEX_MIN) {
        if (pj_obj_rehash(b, 16) != 0) return -1;
    }
    if (b->idx && (b->n + 1) * 4 >= b->idxcap * 3) {
        if (pj_obj_rehash(b, b->idxcap * 2) != 0) return -1;
    }
    if (b->idx) {
        size_t m = b->idxcap - 1;
        size_t j = (size_t)pj_hash(key, klen) & m;
        while (b->idx[j] != 0) {
            size_t slot = b->idx[j] - 1;
            if (b->klens[slot] == klen &&
                memcmp(b->keys[slot], key, klen) == 0) {
                b->vals[slot] = v;   /* 重复键：后来的覆盖先前的 */
                return 0;
            }
            j = (j + 1) & m;
        }
        if (pj_obj_reserve(b, b->n + 1) != 0) return -1;
        b->keys[b->n] = key;
        b->klens[b->n] = klen;
        b->vals[b->n] = v;
        b->idx[j] = b->n + 1;
        b->n++;
        return 0;
    }
    {
        size_t i;
        for (i = 0; i < b->n; i++) {
            if (b->klens[i] == klen && memcmp(b->keys[i], key, klen) == 0) {
                b->vals[i] = v;
                return 0;
            }
        }
    }
    if (pj_obj_reserve(b, b->n + 1) != 0) return -1;
    b->keys[b->n] = key;
    b->klens[b->n] = klen;
    b->vals[b->n] = v;
    b->n++;
    return 0;
}

static JsonValue *pj_object(JsonP *P) {
    ObjBuild b;
    JsonValue *v = NULL;
    memset(&b, 0, sizeof(b));

    P->p++; /* '{' */
    if (++P->depth > JSON_MAX_DEPTH) { P->err = 1; return NULL; }

    pj_ws(P);
    if (P->p < P->end && *P->p == '}') {
        P->p++;
    } else {
        for (;;) {
            Str key;
            char *kp;
            size_t klen;
            JsonValue *val;

            if (P->p >= P->end || *P->p != '"') { P->err = 1; goto fail; }
            str_init(&key);
            if (pj_raw_string(P, &key) != 0) {
                str_free(&key);
                P->err = 1;
                goto fail;
            }
            klen = key.len;
            kp = (char *)arena_alloc(P->a, klen + 1);
            if (!kp) {
                str_free(&key);
                P->err = 1;
                goto fail;
            }
            if (klen) memcpy(kp, key.data, klen);
            kp[klen] = 0;
            str_free(&key);

            pj_ws(P);
            if (P->p >= P->end || *P->p != ':') { P->err = 1; goto fail; }
            P->p++;

            val = pj_value(P);
            if (!val) goto fail;
            if (pj_obj_put(&b, kp, klen, val) != 0) { P->err = 1; goto fail; }

            pj_ws(P);
            if (P->p >= P->end) { P->err = 1; goto fail; }
            if (*P->p == ',') { P->p++; pj_ws(P); continue; }
            if (*P->p == '}') { P->p++; break; }
            P->err = 1;      /* 缺逗号 / 缺右花括号 */
            goto fail;
        }
    }
    P->depth--;

    v = pj_new(P, JSON_T_OBJ);
    if (!v) goto fail;
    v->u.obj.n = b.n;
    if (b.n) {
        char **keys = (char **)arena_alloc(P->a, b.n * sizeof(char *));
        size_t *klens = (size_t *)arena_alloc(P->a, b.n * sizeof(size_t));
        JsonValue **vals = (JsonValue **)arena_alloc(P->a, b.n * sizeof(JsonValue *));
        if (!keys || !klens || !vals) { P->err = 1; v = NULL; goto fail; }
        memcpy(keys, b.keys, b.n * sizeof(char *));
        memcpy(klens, b.klens, b.n * sizeof(size_t));
        memcpy(vals, b.vals, b.n * sizeof(JsonValue *));
        v->u.obj.keys = keys;
        v->u.obj.klens = klens;
        v->u.obj.vals = vals;
    }
    if (b.idx) {
        size_t *ix = (size_t *)arena_alloc(P->a, b.idxcap * sizeof(size_t));
        if (!ix) { P->err = 1; v = NULL; goto fail; }
        memcpy(ix, b.idx, b.idxcap * sizeof(size_t));
        v->u.obj.idx = ix;
        v->u.obj.idxcap = b.idxcap;
    }
    free(b.keys); free(b.klens); free(b.vals); free(b.idx);
    return v;

fail:
    free(b.keys); free(b.klens); free(b.vals); free(b.idx);
    return NULL;
}

static JsonValue *pj_value(JsonP *P) {
    char c;
    if (P->err) return NULL;
    pj_ws(P);
    if (P->p >= P->end) { P->err = 1; return NULL; }
    c = *P->p;
    switch (c) {
    case '{': return pj_object(P);
    case '[': return pj_array(P);
    case '"': return pj_string(P);
    case 't':
        if (P->end - P->p >= 4 && memcmp(P->p, "true", 4) == 0) {
            P->p += 4;
            return pj_simple(P, JSON_T_BOOL, 1);
        }
        P->err = 1;
        return NULL;
    case 'f':
        if (P->end - P->p >= 5 && memcmp(P->p, "false", 5) == 0) {
            P->p += 5;
            return pj_simple(P, JSON_T_BOOL, 0);
        }
        P->err = 1;
        return NULL;
    case 'n':
        if (P->end - P->p >= 4 && memcmp(P->p, "null", 4) == 0) {
            P->p += 4;
            return pj_simple(P, JSON_T_NULL, 0);
        }
        P->err = 1;
        return NULL;
    default:
        if (c == '-' || pj_isdigit(c)) return pj_number(P);
        P->err = 1;
        return NULL;
    }
}

JsonValue *json_parse(const char *utf8, Arena *a) {
    JsonP P;
    JsonValue *v;

    if (!utf8 || !a) return NULL;

    P.p = utf8;
    P.end = utf8 + strlen(utf8);
    P.a = a;
    P.depth = 0;
    P.err = 0;

    /* 跳过 UTF-8 BOM */
    if (P.end - P.p >= 3 &&
        (unsigned char)P.p[0] == 0xEF &&
        (unsigned char)P.p[1] == 0xBB &&
        (unsigned char)P.p[2] == 0xBF) {
        P.p += 3;
    }

    v = pj_value(&P);
    if (!v) return NULL;
    pj_ws(&P);
    if (P.p != P.end) return NULL;   /* 尾部垃圾 */
    return v;
}

void json_free(JsonValue *v) {
    (void)v;   /* 内存归 Arena 所有，这里什么都不用做 */
}

/* =========================================================== 访问器 ==== */

int json_is_null(const JsonValue *v)   { return v && v->kind == JSON_T_NULL; }
int json_is_object(const JsonValue *v) { return v && v->kind == JSON_T_OBJ; }
int json_is_array(const JsonValue *v)  { return v && v->kind == JSON_T_ARR; }

const char *json_string(const JsonValue *v, const char *fallback) {
    if (!v || v->kind != JSON_T_STR || !v->u.s.p) return fallback;
    return v->u.s.p;
}

int json_bool(const JsonValue *v, int fallback) {
    if (!v) return fallback;
    if (v->kind == JSON_T_BOOL) return v->u.b;
    if (v->kind == JSON_T_NUM) return v->u.num.d != 0.0;
    return fallback;
}

/* 数字/数字串 -> long long；失败返回 0 */
static int pj_to_ll(const JsonValue *v, long long *out) {
    if (!v) return 0;
    if (v->kind == JSON_T_NUM) {
        if (v->u.num.is_int) { *out = v->u.num.i; return 1; }
        {
            double d = v->u.num.d;
            if (!(d == d)) return 0;                       /* NaN */
            if (d >= 9223372036854775808.0 || d < -9223372036854775808.0) return 0;
            *out = (long long)d;                           /* 截断小数部分 */
        }
        return 1;
    }
    if (v->kind == JSON_T_STR && v->u.s.p) {
        const char *s = v->u.s.p;
        char *end = NULL;
        long long t;
        errno = 0;
        t = strtoll(s, &end, 10);
        if (end != s && errno != ERANGE) {
            const char *r = end;
            while (*r == ' ' || *r == '\t' || *r == '\n' || *r == '\r') r++;
            if (*r == 0) { *out = t; return 1; }
        }
        /* 退一步：允许 "12.5" / "1e3" 这类数字串，截断取整 */
        end = NULL;
        {
            double d = strtod(s, &end);
            if (end != s && end) {
                const char *r = end;
                while (*r == ' ' || *r == '\t' || *r == '\n' || *r == '\r') r++;
                if (*r == 0 && d == d &&
                    d < 9223372036854775808.0 && d >= -9223372036854775808.0) {
                    *out = (long long)d;
                    return 1;
                }
            }
        }
    }
    if (v->kind == JSON_T_BOOL) { *out = v->u.b ? 1 : 0; return 1; }
    return 0;
}

long long json_llong(const JsonValue *v, long long fallback) {
    long long x;
    if (!pj_to_ll(v, &x)) return fallback;
    return x;
}

int json_int(const JsonValue *v, int fallback) {
    long long x;
    if (!pj_to_ll(v, &x)) return fallback;
    if (x > (long long)INT_MAX) return INT_MAX;
    if (x < (long long)INT_MIN) return INT_MIN;
    return (int)x;
}

double json_double(const JsonValue *v, double fallback) {
    if (!v) return fallback;
    if (v->kind == JSON_T_NUM) return v->u.num.d;
    if (v->kind == JSON_T_STR && v->u.s.p) {
        char *end = NULL;
        double d = strtod(v->u.s.p, &end);
        if (end != v->u.s.p && end) {
            const char *r = end;
            while (*r == ' ' || *r == '\t' || *r == '\n' || *r == '\r') r++;
            if (*r == 0) return d;
        }
    }
    return fallback;
}

JsonValue *json_get(const JsonValue *obj, const char *key) {
    const JsonObj *o;
    size_t klen;
    size_t i;

    if (!obj || obj->kind != JSON_T_OBJ || !key) return NULL;
    o = &obj->u.obj;
    klen = strlen(key);

    if (o->idx) {
        size_t m = o->idxcap - 1;
        size_t j = (size_t)pj_hash(key, klen) & m;
        while (o->idx[j] != 0) {
            size_t slot = o->idx[j] - 1;
            if (o->klens[slot] == klen &&
                memcmp(o->keys[slot], key, klen) == 0) {
                return o->vals[slot];
            }
            j = (j + 1) & m;
        }
        return NULL;
    }
    for (i = 0; i < o->n; i++) {
        if (o->klens[i] == klen && memcmp(o->keys[i], key, klen) == 0) {
            return o->vals[i];
        }
    }
    return NULL;
}

size_t json_array_len(const JsonValue *v) {
    if (!v || v->kind != JSON_T_ARR) return 0;
    return v->u.arr.n;
}

JsonValue *json_at(const JsonValue *v, size_t i) {
    if (!v || v->kind != JSON_T_ARR || i >= v->u.arr.n) return NULL;
    return v->u.arr.items[i];
}
