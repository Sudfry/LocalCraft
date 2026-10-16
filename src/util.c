/* ==========================================================================
 * util.c —— Arena 分配器、宽字符串、编码转换、路径与文件系统工具
 * ========================================================================== */
#include "lc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>
#include <time.h>
#include <ctype.h>
#include <shlobj.h>
#include <shellapi.h>
#include <io.h>

/* =========================================================== Arena ======= */

#define ARENA_BLOCK_MIN (64 * 1024)

struct ArenaBlock {
    ArenaBlock *next;
    size_t      used;
    size_t      cap;
    /* 数据紧随其后 */
};

static size_t align16(size_t n) { return (n + 15u) & ~(size_t)15u; }

Arena *arena_new(void) {
    Arena *a = (Arena *)calloc(1, sizeof(Arena));
    return a;
}

void arena_free(Arena *a) {
    if (!a) return;
    ArenaBlock *b = a->head;
    while (b) {
        ArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    free(a);
}

static ArenaBlock *arena_new_block(size_t cap) {
    if (cap < ARENA_BLOCK_MIN) cap = ARENA_BLOCK_MIN;
    ArenaBlock *b = (ArenaBlock *)malloc(sizeof(ArenaBlock) + cap);
    if (!b) return NULL;
    b->next = NULL;
    b->used = 0;
    b->cap = cap;
    return b;
}

void *arena_alloc(Arena *a, size_t n) {
    if (!a || n == 0) return NULL;
    n = align16(n);
    ArenaBlock *b = a->head;
    if (!b || b->used + n > b->cap) {
        size_t cap = n > ARENA_BLOCK_MIN ? n : ARENA_BLOCK_MIN;
        ArenaBlock *nb = arena_new_block(cap);
        if (!nb) return NULL;
        nb->next = a->head;
        a->head = nb;
        b = nb;
    }
    void *p = (char *)(b + 1) + b->used;
    b->used += n;
    a->total += n;
    return p;
}

void *arena_zalloc(Arena *a, size_t n) {
    void *p = arena_alloc(a, n);
    if (p) memset(p, 0, n);
    return p;
}

wchar_t *arena_wcsdup(Arena *a, const wchar_t *s) {
    if (!s) return NULL;
    size_t n = wcslen(s);
    wchar_t *p = (wchar_t *)arena_alloc(a, (n + 1) * sizeof(wchar_t));
    if (!p) return NULL;
    memcpy(p, s, (n + 1) * sizeof(wchar_t));
    return p;
}

wchar_t *arena_wcsndup(Arena *a, const wchar_t *s, size_t n) {
    wchar_t *p = (wchar_t *)arena_alloc(a, (n + 1) * sizeof(wchar_t));
    if (!p) return NULL;
    memcpy(p, s, n * sizeof(wchar_t));
    p[n] = 0;
    return p;
}

char *arena_strdup(Arena *a, const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char *)arena_alloc(a, n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

/* ======================================================= 原始内存助手 ==== */

void *lc_alloc(size_t n) { return malloc(n ? n : 1); }
void *lc_realloc(void *p, size_t n) { return realloc(p, n ? n : 1); }
void lc_free(void *p) { free(p); }

/* ========================================================= WStr / Str ==== */

void wstr_init(WStr *s) { s->data = NULL; s->len = 0; s->cap = 0; }
void wstr_free(WStr *s) { free(s->data); s->data = NULL; s->len = s->cap = 0; }
void wstr_clear(WStr *s) { s->len = 0; if (s->data) s->data[0] = 0; }

int wstr_reserve(WStr *s, size_t need) {
    if (s->len + need + 1 <= s->cap) return 0;
    size_t cap = s->cap ? s->cap : 64;
    while (cap < s->len + need + 1) cap *= 2;
    wchar_t *p = (wchar_t *)realloc(s->data, cap * sizeof(wchar_t));
    if (!p) return -1;
    s->data = p;
    s->cap = cap;
    return 0;
}

int wstr_putc(WStr *s, wchar_t c) {
    if (wstr_reserve(s, 1) != 0) return -1;
    s->data[s->len++] = c;
    s->data[s->len] = 0;
    return 0;
}

int wstr_putn(WStr *s, const wchar_t *p, size_t n) {
    if (!p || !n) return 0;
    if (wstr_reserve(s, n) != 0) return -1;
    memcpy(s->data + s->len, p, n * sizeof(wchar_t));
    s->len += n;
    s->data[s->len] = 0;
    return 0;
}

int wstr_puts(WStr *s, const wchar_t *p) {
    return p ? wstr_putn(s, p, wcslen(p)) : 0;
}

int wstr_printf(WStr *s, const wchar_t *fmt, ...) {
    va_list ap;
    size_t cap = 256;
    wchar_t *buf = NULL;
    int n = -1;
    va_start(ap, fmt);
    for (;;) {
        wchar_t *nb = (wchar_t *)realloc(buf, cap * sizeof(wchar_t));
        if (!nb) break;
        buf = nb;
        va_list ap2;
        va_copy(ap2, ap);
        n = vswprintf(buf, cap, fmt, ap2);
        va_end(ap2);
        if (n >= 0) break;
        cap *= 2;
        if (cap > (1u << 22)) { n = -1; break; }
    }
    va_end(ap);
    if (n < 0) { free(buf); return -1; }
    int rc = wstr_putn(s, buf, (size_t)n);
    free(buf);
    return rc;
}

wchar_t *wstr_steal(WStr *s) {
    wchar_t *p = s->data;
    s->data = NULL;
    s->len = s->cap = 0;
    return p;
}

void str_init(Str *s) { s->data = NULL; s->len = 0; s->cap = 0; }
void str_free(Str *s) { free(s->data); s->data = NULL; s->len = s->cap = 0; }
void str_clear(Str *s) { s->len = 0; if (s->data) s->data[0] = 0; }

int str_reserve(Str *s, size_t need) {
    if (s->len + need + 1 <= s->cap) return 0;
    size_t cap = s->cap ? s->cap : 128;
    while (cap < s->len + need + 1) cap *= 2;
    char *p = (char *)realloc(s->data, cap);
    if (!p) return -1;
    s->data = p;
    s->cap = cap;
    return 0;
}

int str_putc(Str *s, char c) {
    if (str_reserve(s, 1) != 0) return -1;
    s->data[s->len++] = c;
    s->data[s->len] = 0;
    return 0;
}

int str_putn(Str *s, const char *p, size_t n) {
    if (!p || !n) return 0;
    if (str_reserve(s, n) != 0) return -1;
    memcpy(s->data + s->len, p, n);
    s->len += n;
    s->data[s->len] = 0;
    return 0;
}

int str_puts(Str *s, const char *p) { return p ? str_putn(s, p, strlen(p)) : 0; }

int str_printf(Str *s, const char *fmt, ...) {
    va_list ap;
    size_t cap = 256;
    char *buf = NULL;
    int n = -1;
    va_start(ap, fmt);
    for (;;) {
        char *nb = (char *)realloc(buf, cap);
        if (!nb) break;
        buf = nb;
        va_list ap2;
        va_copy(ap2, ap);
        n = vsnprintf(buf, cap, fmt, ap2);
        va_end(ap2);
        if (n >= 0 && (size_t)n < cap) break;
        cap *= 2;
        if (cap > (1u << 24)) { n = -1; break; }
    }
    va_end(ap);
    if (n < 0) { free(buf); return -1; }
    int rc = str_putn(s, buf, (size_t)n);
    free(buf);
    return rc;
}

char *str_steal(Str *s) {
    char *p = s->data;
    s->data = NULL;
    s->len = s->cap = 0;
    return p;
}

void vec_init(Vec *v) { v->items = NULL; v->len = 0; v->cap = 0; }
void vec_free(Vec *v) { free(v->items); v->items = NULL; v->len = v->cap = 0; }

int vec_push(Vec *v, void *item) {
    if (v->len + 1 > v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 16;
        void **p = (void **)realloc(v->items, cap * sizeof(void *));
        if (!p) return -1;
        v->items = p;
        v->cap = cap;
    }
    v->items[v->len++] = item;
    return 0;
}

void *vec_get(Vec *v, size_t i) { return i < v->len ? v->items[i] : NULL; }

/* ==================================================== 编码转换 =========== */

char *lc_wcs_to_utf8_n(const wchar_t *w, size_t n) {
    if (!w) return NULL;
    int need = WideCharToMultiByte(CP_UTF8, 0, w, (int)n, NULL, 0, NULL, NULL);
    if (need <= 0 && n > 0) return NULL;
    char *out = (char *)malloc((size_t)need + 1);
    if (!out) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, (int)n, out, need, NULL, NULL);
    out[need] = 0;
    return out;
}

char *lc_wcs_to_utf8(const wchar_t *w) {
    return w ? lc_wcs_to_utf8_n(w, wcslen(w)) : NULL;
}

wchar_t *lc_utf8_to_wcs(const char *u) {
    if (!u) return NULL;
    int need = MultiByteToWideChar(CP_UTF8, 0, u, -1, NULL, 0);
    if (need <= 0) {
        wchar_t *empty = (wchar_t *)malloc(sizeof(wchar_t));
        if (empty) empty[0] = 0;
        return empty;
    }
    wchar_t *out = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
    if (!out) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, u, -1, out, need);
    return out;
}

/* ====================================================== 路径工具 ========= */

int lc_is_sep(wchar_t c) { return c == L'\\' || c == L'/'; }
int lc_is_windows(void) { return 1; }

wchar_t *lc_display_path(const wchar_t *path) {
    if (!path) return NULL;
    size_t n = wcslen(path);
    if (n >= 4 && path[0] == L'\\' && path[1] == L'\\' && path[2] == L'?' && path[3] == L'\\') {
        if (n >= 8 && _wcsnicmp(path + 4, L"UNC\\", 4) == 0) {
            size_t m = n - 8;
            wchar_t *out = (wchar_t *)malloc((m + 3) * sizeof(wchar_t));
            if (!out) return NULL;
            out[0] = L'\\'; out[1] = L'\\';
            memcpy(out + 2, path + 8, (m + 1) * sizeof(wchar_t));
            return out;
        }
        return _wcsdup(path + 4);
    }
    return _wcsdup(path);
}

wchar_t *lc_abs_path(const wchar_t *path) {
    if (!path || !*path) return NULL;
    DWORD need = GetFullPathNameW(path, 0, NULL, NULL);
    if (need == 0) return NULL;
    wchar_t *buf = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
    if (!buf) return NULL;
    DWORD got = GetFullPathNameW(path, need, buf, NULL);
    if (got == 0 || got >= need) { free(buf); return NULL; }
    /* 去掉结尾多余的分隔符（保留根 "C:\"） */
    size_t n = wcslen(buf);
    while (n > 3 && lc_is_sep(buf[n - 1])) buf[--n] = 0;
    return buf;
}

wchar_t *lc_long_path(const wchar_t *path) {
    if (!path) return NULL;
    if (path[0] == L'\\' && path[1] == L'\\' && path[2] == L'?' && path[3] == L'\\')
        return _wcsdup(path);
    wchar_t *abs = lc_abs_path(path);
    if (!abs) return NULL;
    WStr s;
    wstr_init(&s);
    if (abs[0] == L'\\' && abs[1] == L'\\') {
        wstr_puts(&s, L"\\\\?\\UNC\\");
        wstr_puts(&s, abs + 2);
    } else {
        wstr_puts(&s, L"\\\\?\\");
        wstr_puts(&s, abs);
    }
    free(abs);
    return wstr_steal(&s);
}

wchar_t *lc_dirname(const wchar_t *path) {
    if (!path) return NULL;
    wchar_t *d = _wcsdup(path);
    if (!d) return NULL;
    size_t n = wcslen(d);
    while (n > 0 && lc_is_sep(d[n - 1])) d[--n] = 0;
    while (n > 0 && !lc_is_sep(d[n - 1])) d[--n] = 0;
    if (n == 0) { free(d); return _wcsdup(L"."); }
    if (n == 3 && d[1] == L':') { d[3] = 0; return d; }   /* C:\ */
    while (n > 1 && lc_is_sep(d[n - 1])) d[--n] = 0;
    return d;
}

const wchar_t *lc_basename(const wchar_t *path) {
    if (!path) return L"";
    const wchar_t *p = path, *last = path;
    for (; *p; ++p) if (lc_is_sep(*p)) last = p + 1;
    return last;
}

const wchar_t *lc_extname(const wchar_t *path) {
    const wchar_t *base = lc_basename(path);
    const wchar_t *dot = NULL;
    for (const wchar_t *p = base; *p; ++p) if (*p == L'.') dot = p;
    return dot ? dot : (base + wcslen(base));
}

wchar_t *lc_stem(const wchar_t *path) {
    const wchar_t *base = lc_basename(path);
    const wchar_t *ext = lc_extname(path);
    size_t n = (size_t)(ext - base);
    wchar_t *out = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    if (!out) return NULL;
    memcpy(out, base, n * sizeof(wchar_t));
    out[n] = 0;
    return out;
}

wchar_t *lc_norm_key(const wchar_t *path) {
    if (!path) return NULL;
    wchar_t *abs = lc_abs_path(path);
    if (!abs) return NULL;
    for (wchar_t *p = abs; *p; ++p) {
        if (*p == L'/') *p = L'\\';
        *p = (wchar_t)towlower(*p);
    }
    size_t n = wcslen(abs);
    while (n > 3 && lc_is_sep(abs[n - 1])) abs[--n] = 0;
    return abs;
}

int lc_same_path(const wchar_t *a, const wchar_t *b) {
    if (!a || !b) return 0;
    wchar_t *ka = lc_norm_key(a), *kb = lc_norm_key(b);
    int same = ka && kb && wcscmp(ka, kb) == 0;
    free(ka); free(kb);
    return same;
}

int lc_same_path_exact(const wchar_t *a, const wchar_t *b) {
    if (!a || !b) return 0;
    wchar_t *absa = lc_abs_path(a), *absb = lc_abs_path(b);
    if (!absa || !absb) { free(absa); free(absb); return 0; }
    for (wchar_t *p = absa; *p; ++p) if (*p == L'/') *p = L'\\';
    for (wchar_t *p = absb; *p; ++p) if (*p == L'/') *p = L'\\';
    int same = wcscmp(absa, absb) == 0;
    free(absa); free(absb);
    return same;
}

int lc_is_within(const wchar_t *child, const wchar_t *parent) {
    if (!child || !parent) return 0;
    wchar_t *kc = lc_norm_key(child), *kp = lc_norm_key(parent);
    if (!kc || !kp) { free(kc); free(kp); return 0; }
    size_t lc = wcslen(kc), lp = wcslen(kp);
    int result = 0;
    if (lc > lp && wcsncmp(kc, kp, lp) == 0 && lc_is_sep(kc[lp])) result = 1;
    free(kc); free(kp);
    return result;
}

/* -- 名字规整 -- */

static int is_reserved_name(const wchar_t *name) {
    static const wchar_t *R[] = {
        L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$", NULL
    };
    wchar_t base[16];
    size_t i = 0;
    for (; name[i] && name[i] != L'.' && i < 15; ++i) base[i] = (wchar_t)towupper(name[i]);
    base[i] = 0;
    for (int k = 0; R[k]; ++k) if (wcscmp(base, R[k]) == 0) return 1;
    if (i >= 3) {
        if ((wcsncmp(base, L"COM", 3) == 0 || wcsncmp(base, L"LPT", 3) == 0)
            && base[3] >= L'1' && base[3] <= L'9' && base[4] == 0)
            return 1;
    }
    return 0;
}

static int is_illegal_char(wchar_t c) {
    if (c < 32) return 1;
    switch (c) {
        case L'\\': case L'/': case L':': case L'*': case L'?':
        case L'"':  case L'<': case L'>': case L'|':
            return 1;
        default:
            return 0;
    }
}

wchar_t *lc_sanitize_component(const wchar_t *name) {
    if (!name) return _wcsdup(L"_");
    WStr s;
    wstr_init(&s);
    for (const wchar_t *p = name; *p; ++p)
        wstr_putc(&s, is_illegal_char(*p) ? L'_' : *p);

    /* 去掉结尾的点和空格 */
    while (s.len > 0 && (s.data[s.len - 1] == L'.' || s.data[s.len - 1] == L' '))
        s.data[--s.len] = 0;

    if (s.len == 0) {
        wstr_free(&s);
        return _wcsdup(L"_");
    }
    if (is_reserved_name(s.data)) {
        WStr t;
        wstr_init(&t);
        wstr_putc(&t, L'_');
        wstr_puts(&t, s.data);
        wstr_free(&s);
        s = t;
    }
    /* 截断到 255 个字符 */
    if (s.len > 255) s.data[s.len = 255] = 0;
    while (s.len > 0 && (s.data[s.len - 1] == L'.' || s.data[s.len - 1] == L' '))
        s.data[--s.len] = 0;
    if (s.len == 0) { wstr_free(&s); return _wcsdup(L"_"); }
    return wstr_steal(&s);
}

int lc_is_valid_component(const wchar_t *name) {
    if (!name || !*name) return 0;
    wchar_t *s = lc_sanitize_component(name);
    int ok = s && wcscmp(s, name) == 0;
    free(s);
    return ok;
}

static int unique_is_free(const wchar_t *full, Vec *taken_keys) {
    if (taken_keys) {
        wchar_t *k = lc_norm_key(full);
        if (k) {
            for (size_t i = 0; i < taken_keys->len; ++i) {
                if (wcscmp((const wchar_t *)taken_keys->items[i], k) == 0) {
                    free(k);
                    return 0;
                }
            }
            free(k);
        }
    }
    return !lc_exists(full);
}

wchar_t *lc_unique_path(const wchar_t *path, Vec *taken_keys) {
    if (!path) return NULL;
    wchar_t *dir = lc_dirname(path);
    const wchar_t *base = lc_basename(path);
    const wchar_t *ext = lc_extname(path);
    size_t stem_len = (size_t)(ext - base);

    WStr s;
    wstr_init(&s);
    if (dir && wcscmp(dir, L".") != 0) {
        wstr_puts(&s, dir);
        if (s.len && !lc_is_sep(s.data[s.len - 1])) wstr_putc(&s, L'\\');
    }
    wstr_putn(&s, base, stem_len);
    wstr_puts(&s, ext);

    if (unique_is_free(s.data, taken_keys)) { free(dir); return wstr_steal(&s); }

    for (int i = 1; i < 100000; ++i) {
        wstr_clear(&s);
        if (dir && wcscmp(dir, L".") != 0) {
            wstr_puts(&s, dir);
            if (s.len && !lc_is_sep(s.data[s.len - 1])) wstr_putc(&s, L'\\');
        }
        wstr_printf(&s, L"%.*ls (%d)%ls", (int)stem_len, base, i, ext);
        if (unique_is_free(s.data, taken_keys)) { free(dir); return wstr_steal(&s); }
    }
    free(dir);
    wstr_free(&s);
    return NULL;
}

int lc_path_too_long(const wchar_t *path) {
    return path && wcslen(path) >= 240;
}

/* ================================================== 字符串工具 =========== */

int lc_wcs_ieq(const wchar_t *a, const wchar_t *b) {
    if (!a || !b) return a == b;
    return _wcsicmp(a, b) == 0;
}

int lc_wcs_icmp(const wchar_t *a, const wchar_t *b) {
    if (!a) a = L"";
    if (!b) b = L"";
    return _wcsicmp(a, b);
}

int lc_wcs_starts_ci(const wchar_t *s, const wchar_t *prefix) {
    if (!s || !prefix) return 0;
    return _wcsnicmp(s, prefix, wcslen(prefix)) == 0;
}

int lc_wcs_ends_ci(const wchar_t *s, const wchar_t *suffix) {
    if (!s || !suffix) return 0;
    size_t ls = wcslen(s), lf = wcslen(suffix);
    if (lf > ls) return 0;
    return _wcsicmp(s + ls - lf, suffix) == 0;
}

int lc_wcs_ends(const wchar_t *s, const wchar_t *suffix) {
    if (!s || !suffix) return 0;
    size_t ls = wcslen(s), lf = wcslen(suffix);
    if (lf > ls) return 0;
    return wcscmp(s + ls - lf, suffix) == 0;
}

wchar_t *lc_wcs_replace_all(const wchar_t *s, const wchar_t *find,
                            const wchar_t *repl, int ignore_case) {
    if (!s || !find || !*find) return s ? _wcsdup(s) : NULL;
    WStr out;
    wstr_init(&out);
    size_t lf = wcslen(find);
    const wchar_t *p = s;
    while (*p) {
        int hit = ignore_case ? (_wcsnicmp(p, find, lf) == 0)
                              : (wcsncmp(p, find, lf) == 0);
        if (hit) {
            wstr_puts(&out, repl ? repl : L"");
            p += lf;
        } else {
            wstr_putc(&out, *p++);
        }
    }
    return wstr_steal(&out);
}

wchar_t *lc_wcs_trim(const wchar_t *s) {
    if (!s) return NULL;
    while (*s && (*s == L' ' || *s == L'\t' || *s == L'\r' || *s == L'\n')) ++s;
    size_t n = wcslen(s);
    while (n > 0 && (s[n-1] == L' ' || s[n-1] == L'\t' || s[n-1] == L'\r' || s[n-1] == L'\n')) --n;
    wchar_t *out = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    if (!out) return NULL;
    memcpy(out, s, n * sizeof(wchar_t));
    out[n] = 0;
    return out;
}

void lc_wcs_lower(wchar_t *s) { for (; s && *s; ++s) *s = (wchar_t)towlower(*s); }
void lc_wcs_upper(wchar_t *s) { for (; s && *s; ++s) *s = (wchar_t)towupper(*s); }

void lc_wcs_title(wchar_t *s) {
    int start = 1;
    for (; s && *s; ++s) {
        if (iswspace(*s) || *s == L'_' || *s == L'-') { start = 1; continue; }
        *s = start ? (wchar_t)towupper(*s) : (wchar_t)towlower(*s);
        start = 0;
    }
}

int lc_wcs_to_int(const wchar_t *s, int fallback) {
    if (!s) return fallback;
    while (*s == L' ') ++s;
    if (!*s) return fallback;
    wchar_t *end = NULL;
    long v = wcstol(s, &end, 10);
    if (end == s) return fallback;
    return (int)v;
}

/* =================================================== 文件系统 ============ */

int lc_exists(const wchar_t *path) {
    if (!path || !*path) return 0;
    wchar_t *lp = lc_long_path(path);
    DWORD attr = lp ? GetFileAttributesW(lp) : INVALID_FILE_ATTRIBUTES;
    free(lp);
    return attr != INVALID_FILE_ATTRIBUTES;
}

int lc_is_dir(const wchar_t *path) {
    if (!path || !*path) return 0;
    wchar_t *lp = lc_long_path(path);
    DWORD attr = lp ? GetFileAttributesW(lp) : INVALID_FILE_ATTRIBUTES;
    free(lp);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

int lc_is_file(const wchar_t *path) {
    if (!path || !*path) return 0;
    wchar_t *lp = lc_long_path(path);
    DWORD attr = lp ? GetFileAttributesW(lp) : INVALID_FILE_ATTRIBUTES;
    free(lp);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

int lc_mkdir_p(const wchar_t *path) {
    if (!path || !*path) return -1;
    if (lc_is_dir(path)) return 0;
    wchar_t *copy = _wcsdup(path);
    if (!copy) return -1;
    for (wchar_t *p = copy + 1; *p; ++p) {
        if (lc_is_sep(*p)) {
            wchar_t save = *p;
            *p = 0;
            wchar_t *lp = lc_long_path(copy);
            if (lp && !lc_is_dir(copy)) CreateDirectoryW(lp, NULL);
            free(lp);
            *p = save;
        }
    }
    wchar_t *lp = lc_long_path(copy);
    int rc = 0;
    if (lp && !lc_is_dir(copy)) {
        if (!CreateDirectoryW(lp, NULL)) {
            DWORD e = GetLastError();
            if (e != ERROR_ALREADY_EXISTS) rc = -1;
        }
    }
    free(lp);
    free(copy);
    return rc;
}

int lc_rmdir_empty(const wchar_t *path) {
    wchar_t *lp = lc_long_path(path);
    if (!lp) return -1;
    int rc = RemoveDirectoryW(lp) ? 0 : -1;
    free(lp);
    return rc;
}

int lc_dir_is_empty(const wchar_t *path) {
    WStr s;
    wstr_init(&s);
    wstr_puts(&s, path);
    if (s.len && !lc_is_sep(s.data[s.len - 1])) wstr_putc(&s, L'\\');
    wstr_puts(&s, L"*");
    wchar_t *lp = lc_long_path(s.data);
    wstr_free(&s);
    if (!lp) return 1;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(lp, &fd);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) return 1;
    int empty = 1;
    do {
        if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
            empty = 0;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return empty;
}

long long lc_size_of(const wchar_t *path) {
    if (lc_is_file(path)) {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        wchar_t *lp = lc_long_path(path);
        if (!lp) return 0;
        BOOL ok = GetFileAttributesExW(lp, GetFileExInfoStandard, &fad);
        free(lp);
        if (!ok) return 0;
        return ((long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    }
    if (lc_is_dir(path)) {
        long long total = 0;
        WStr s;
        wstr_init(&s);
        wstr_puts(&s, path);
        if (s.len && !lc_is_sep(s.data[s.len - 1])) wstr_putc(&s, L'\\');
        wstr_puts(&s, L"*");
        wchar_t *lp = lc_long_path(s.data);
        wstr_free(&s);
        if (!lp) return 0;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(lp, &fd);
        free(lp);
        if (h == INVALID_HANDLE_VALUE) return 0;
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;
            WStr c;
            wstr_init(&c);
            wstr_puts(&c, path);
            if (c.len && !lc_is_sep(c.data[c.len - 1])) wstr_putc(&c, L'\\');
            wstr_puts(&c, fd.cFileName);
            total += lc_size_of(c.data);
            wstr_free(&c);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return total;
    }
    return 0;
}

/* -- 目录扫描 -- */

void filelist_init(FileList *fl, Arena *a) {
    fl->paths = NULL; fl->n = 0; fl->cap = 0; fl->arena = a;
}

int filelist_push(FileList *fl, const wchar_t *path) {
    if (fl->n + 1 > fl->cap) {
        size_t cap = fl->cap ? fl->cap * 2 : 64;
        wchar_t **p = (wchar_t **)realloc(fl->paths, cap * sizeof(wchar_t *));
        if (!p) return -1;
        fl->paths = p;
        fl->cap = cap;
    }
    fl->paths[fl->n++] = arena_wcsdup(fl->arena, path);
    return 0;
}

typedef struct { wchar_t *dir; int depth; } ScanItem;

int lc_scan_dir(const wchar_t *root, int recursive, int skip_hidden,
                int files_only, FileList *out) {
    Vec stack;
    vec_init(&stack);
    ScanItem *first = (ScanItem *)malloc(sizeof(ScanItem));
    if (!first) { vec_free(&stack); return -1; }
    first->dir = _wcsdup(root);
    first->depth = 0;
    vec_push(&stack, first);

    while (stack.len > 0) {
        ScanItem *it = (ScanItem *)stack.items[--stack.len];
        wchar_t *dir = it->dir;
        int depth = it->depth;
        free(it);

        WStr pat;
        wstr_init(&pat);
        wstr_puts(&pat, dir);
        if (pat.len && !lc_is_sep(pat.data[pat.len - 1])) wstr_putc(&pat, L'\\');
        wstr_puts(&pat, L"*");
        wchar_t *lp = lc_long_path(pat.data);
        wstr_free(&pat);
        if (!lp) { free(dir); continue; }

        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(lp, &fd);
        free(lp);
        if (h == INVALID_HANDLE_VALUE) { free(dir); continue; }

        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;
            if (skip_hidden && fd.cFileName[0] == L'.') continue;
            if (skip_hidden && fd.cFileName[0] == L'~' && fd.cFileName[1] == L'$')
                continue;   /* Office 锁文件 */

            int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            WStr full;
            wstr_init(&full);
            wstr_puts(&full, dir);
            if (full.len && !lc_is_sep(full.data[full.len - 1])) wstr_putc(&full, L'\\');
            wstr_puts(&full, fd.cFileName);

            if (is_dir) {
                if (recursive) {
                    ScanItem *nx = (ScanItem *)malloc(sizeof(ScanItem));
                    if (nx) {
                        nx->dir = wstr_steal(&full);
                        nx->depth = depth + 1;
                        vec_push(&stack, nx);
                    } else {
                        wstr_free(&full);
                    }
                } else if (!files_only) {
                    filelist_push(out, full.data);
                    wstr_free(&full);
                } else {
                    wstr_free(&full);
                }
            } else {
                filelist_push(out, full.data);
                wstr_free(&full);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        free(dir);
    }
    vec_free(&stack);
    return 0;
}

/* -- 数据目录 -- */

wchar_t *lc_app_dir(void) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, buf, (DWORD)(sizeof(buf)/sizeof(buf[0])));
    if (n == 0) return _wcsdup(L".");
    wchar_t *dir = lc_dirname(buf);
    return dir ? dir : _wcsdup(L".");
}

int lc_is_portable(void) {
    wchar_t *dir = lc_app_dir();
    if (!dir) return 0;
    static const wchar_t *marks[] = { L"portable.marker", L"localcraft.portable", NULL };
    int found = 0;
    for (int i = 0; marks[i]; ++i) {
        WStr s;
        wstr_init(&s);
        wstr_puts(&s, dir);
        wstr_putc(&s, L'\\');
        wstr_puts(&s, marks[i]);
        if (lc_exists(s.data)) found = 1;
        wstr_free(&s);
        if (found) break;
    }
    free(dir);
    return found;
}

/* 把数据根目录定下来：优先 %LOCALAPPDATA%，不可写时退到 exe 旁边。
   结果缓存在 once 里，只算一次（这个函数会被多个线程频繁调用）。 */
static INIT_ONCE g_root_once = INIT_ONCE_STATIC_INIT;
static wchar_t  g_root_buf[MAX_PATH * 4];

static BOOL CALLBACK data_root_init(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once; (void)param; (void)ctx;

    /* 1) 环境变量优先（测试与便携场景用它把数据挪到指定位置） */
    DWORD n = GetEnvironmentVariableW(L"LOCALCRAFT_HOME", NULL, 0);
    if (n > 0 && n < MAX_PATH * 4) {
        GetEnvironmentVariableW(L"LOCALCRAFT_HOME", g_root_buf, MAX_PATH * 4);
        return TRUE;
    }

    /* 2) 便携模式：exe 旁边有 portable.marker */
    if (lc_is_portable()) {
        wchar_t *dir = lc_app_dir();
        if (dir) {
            swprintf(g_root_buf, MAX_PATH * 4, L"%ls\\.localcraft", dir);
            free(dir);
            return TRUE;
        }
    }

    /* 3) 常规：%LOCALAPPDATA%\LocalCraft
          必须**真的能建出来**才算数 —— 域环境里 LOCALAPPDATA 常被重定向到网络盘，
          或者被 Controlled Folder Access / 杀软挡住，这时要能自己退到别处，
          否则日志、回收站、界面配置全都无处可写。 */
    wchar_t base[MAX_PATH * 2];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base))) {
        wchar_t cand[MAX_PATH * 4];
        swprintf(cand, MAX_PATH * 4, L"%ls\\LocalCraft", base);
        if (lc_mkdir_p(cand) == 0 || lc_is_dir(cand)) {
            wcsncpy(g_root_buf, cand, MAX_PATH * 4 - 1);
            g_root_buf[MAX_PATH * 4 - 1] = 0;
            return TRUE;
        }
    }

    /* 4) 退路：exe 同级的 .localcraft（exe 跑起来了，这里至少是可写的） */
    {
        wchar_t *dir = lc_app_dir();
        if (dir) {
            wchar_t cand[MAX_PATH * 4];
            swprintf(cand, MAX_PATH * 4, L"%ls\\.localcraft", dir);
            if (lc_mkdir_p(cand) == 0 || lc_is_dir(cand)) {
                wcsncpy(g_root_buf, cand, MAX_PATH * 4 - 1);
                g_root_buf[MAX_PATH * 4 - 1] = 0;
                free(dir);
                return TRUE;
            }
            free(dir);
        }
    }

    /* 5) 最后兜底：当前目录 */
    wcsncpy(g_root_buf, L".\\LocalCraft", MAX_PATH * 4 - 1);
    g_root_buf[MAX_PATH * 4 - 1] = 0;
    return TRUE;
}

wchar_t *lc_data_root(void) {
    InitOnceExecuteOnce(&g_root_once, data_root_init, NULL, NULL);
    return _wcsdup(g_root_buf);
}

static wchar_t *subdir_of_root(const wchar_t *sub) {
    wchar_t *root = lc_data_root();
    WStr s;
    wstr_init(&s);
    wstr_puts(&s, root);
    wstr_putc(&s, L'\\');
    wstr_puts(&s, sub);
    free(root);
    return wstr_steal(&s);
}

wchar_t *lc_journal_dir(void)  { return subdir_of_root(L"journal"); }
wchar_t *lc_trash_root(void)   { return subdir_of_root(L"trash"); }
wchar_t *lc_cache_dir(void)    { return subdir_of_root(L"cache"); }

wchar_t *lc_tools_dir(void) {
    wchar_t *dir = lc_app_dir();
    WStr s;
    wstr_init(&s);
    wstr_puts(&s, dir);
    wstr_puts(&s, L"\\tools");
    free(dir);
    return wstr_steal(&s);
}

int lc_ensure_dir(const wchar_t *path) { return lc_mkdir_p(path); }

int lc_shell_open(const wchar_t *path) {
    if (!path) return -1;
    HINSTANCE r = ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
    return ((INT_PTR)r > 32) ? 0 : -1;
}

int lc_find_browser(wchar_t *out, size_t cch) {
    static const wchar_t *cands[] = {
        L"%ProgramFiles(x86)%\\Microsoft\\Edge\\Application\\msedge.exe",
        L"%ProgramFiles%\\Microsoft\\Edge\\Application\\msedge.exe",
        L"%LOCALAPPDATA%\\Microsoft\\Edge\\Application\\msedge.exe",
        L"%ProgramFiles%\\Google\\Chrome\\Application\\chrome.exe",
        L"%ProgramFiles(x86)%\\Google\\Chrome\\Application\\chrome.exe",
        L"%LOCALAPPDATA%\\Google\\Chrome\\Application\\chrome.exe",
        NULL
    };
    for (int i = 0; cands[i]; ++i) {
        wchar_t expanded[MAX_PATH * 2];
        DWORD n = ExpandEnvironmentStringsW(cands[i], expanded,
                                            (DWORD)(sizeof(expanded)/sizeof(expanded[0])));
        if (n == 0 || n > sizeof(expanded)/sizeof(expanded[0])) continue;
        if (lc_is_file(expanded)) {
            wcsncpy(out, expanded, cch - 1);
            out[cch - 1] = 0;
            return 0;
        }
    }
    return -1;
}

/* ================================================== 权限检测 ============ */

/*
 * 当前进程是否在管理员权限下运行。
 *
 * 为什么必须检测：如果 LocalCraft 是管理员启动的，它 CreateProcess 出来的
 * 浏览器也会继承管理员权限。而桌面上通常已经开着一个普通权限的浏览器，
 * 两者不允许共存 —— Edge 会弹"现有实例正在以提升的权限运行"，
 * 页面就永远加载不出来。
 */
int lc_is_elevated(void) {
    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return 0;
    TOKEN_ELEVATION el;
    DWORD n = 0;
    int result = 0;
    if (GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &n))
        result = el.TokenIsElevated ? 1 : 0;
    CloseHandle(tok);
    return result;
}

/* ================================================== 启动信息 ============ */

/*
 * 把本次启动的网址和结果写成一份纯文本。
 *
 * 为什么要这么做：程序依赖 WebView2/Edge 来显示界面，万一浏览器没起来、
 * 或者 --app 模式被策略挡住，用户看到的就只是"什么都没发生"。
 * 有了这个文件，至少有一个地方能查到确实开过服务、地址是什么。
 */
void lc_write_startup_note(const wchar_t *url, int ok, const wchar_t *how) {
    wchar_t *root = lc_data_root();
    if (!root) return;
    lc_mkdir_p(root);

    WStr path;
    wstr_init(&path);
    wstr_puts(&path, root);
    wstr_puts(&path, L"\\启动信息.txt");

    WStr body;
    wstr_init(&body);
    wstr_puts(&body, L"LocalCraft 启动信息\r\n");
    wstr_puts(&body, L"========================================\r\n\r\n");
    wstr_puts(&body, L"界面地址：\r\n  ");
    wstr_puts(&body, url ? url : L"(无)");
    wstr_puts(&body, L"\r\n\r\n");
    wstr_puts(&body, L"窗口是否已打开：");
    wstr_puts(&body, ok ? L"是" : L"否");
    wstr_puts(&body, L"\r\n");
    if (!ok) {
        wstr_puts(&body,
            L"\r\n如果浏览器没有自动弹出，把上面的地址整行复制到浏览器地址栏\r\n"
            L"（Edge / Chrome / Firefox 都行）再回车即可。\r\n"
            L"注意要连 token= 后面那一长串一起复制，令牌每次启动都会变。\r\n");
    }
    wstr_puts(&body, L"\r\n打开方式：");
    wstr_puts(&body, how && how[0] ? how : L"(未知)");

    /* 权限很关键：管理员启动会让浏览器也带上管理员权限，
       与桌面上已有的普通权限实例冲突，页面就出不来 */
    int elev = lc_is_elevated();
    wstr_puts(&body, L"\r\n运行权限：");
    wstr_puts(&body, elev ? L"管理员（提升）" : L"普通用户");
    if (elev) {
        wstr_puts(&body,
            L"\r\n\r\n提示：本程序不需要管理员权限。以管理员身份运行会让浏览器也带上\r\n"
            L"管理员权限，与桌面上已经打开的浏览器冲突，可能出现\r\n"
            L"「现有实例正在以提升的权限运行」而打不开页面。\r\n"
            L"建议直接双击运行（不要用「以管理员身份运行」）。\r\n"
            L"程序已经把浏览器降到普通权限启动，但你自己也要留意这一点。");
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t ts[64];
    swprintf(ts, 64, L"\r\n\r\n时间：%04d-%02d-%02d %02d:%02d:%02d\r\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    wstr_puts(&body, ts);

    char *u8 = lc_wcs_to_utf8(body.data);
    if (u8) {
        wchar_t *lp = lc_long_path(path.data);
        if (lp) {
            HANDLE h = CreateFileW(lp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                /* 带 BOM：记事本打开中文才不乱码 */
                DWORD w = 0;
                const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
                WriteFile(h, bom, 3, &w, NULL);
                WriteFile(h, u8, (DWORD)strlen(u8), &w, NULL);
                CloseHandle(h);
            }
            free(lp);
        }
        free(u8);
    }

    wstr_free(&body);
    wstr_free(&path);
    free(root);
}

/* ================================================== 格式化 ============== */

void lc_human_size(long long bytes, wchar_t *buf, size_t cch) {
    static const wchar_t *U[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double v = (double)bytes;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    if (i == 0) swprintf(buf, cch, L"%lld B", bytes);
    else        swprintf(buf, cch, L"%.2f %ls", v, U[i]);
}

void lc_format_time(int64_t unix_seconds, wchar_t *buf, size_t cch) {
    time_t t = (time_t)unix_seconds;
    struct tm tmv;
    if (localtime_s(&tmv, &t) != 0) { swprintf(buf, cch, L"?"); return; }
    swprintf(buf, cch, L"%04d-%02d-%02d %02d:%02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

int64_t lc_now_unix(void) { return (int64_t)time(NULL); }

/* ============================================ 追加写 + fsync ============ */

int lc_append_line_fsync(HANDLE h, const char *utf8_line) {
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD len = (DWORD)strlen(utf8_line);
    DWORD written = 0;
    if (!WriteFile(h, utf8_line, len, &written, NULL) || written != len)
        return -1;
    const char nl = '\n';
    if (!WriteFile(h, &nl, 1, &written, NULL) || written != 1)
        return -1;
    /* 关键：每一步都落盘。断电也不会丢已完成步骤的回退信息。 */
    FlushFileBuffers(h);
    return 0;
}
/* ================================================== 剪贴板 ============== */

int lc_copy_to_clipboard(const wchar_t *text) {
    if (!text) return -1;
    size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) return -1;
    void *p = GlobalLock(h);
    if (!p) { GlobalFree(h); return -1; }
    memcpy(p, text, bytes);
    GlobalUnlock(h);

    if (!OpenClipboard(NULL)) { GlobalFree(h); return -1; }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, h)) {
        CloseClipboard();
        GlobalFree(h);
        return -1;
    }
    /* 所有权已交给剪贴板，不能再 GlobalFree */
    CloseClipboard();
    return 0;
}
/* ================================================================== 图标 */

/*
 * 取应用图标。
 * 图标以 RT_GROUP_ICON / ID 101 的形式嵌在 exe 里（见 src/app.rc），
 * 这里按需要的尺寸把它取出来。
 *
 * small = 0 取大图标（标题栏、Alt+Tab），small = 1 取小图标（任务栏、窗口左上角）。
 * 取不到就退回系统默认图标 —— 宁可图标不好看，也不能让窗口没有图标。
 */
HICON lc_app_icon(int small) {
    HINSTANCE hi = GetModuleHandleW(NULL);
    int cx = GetSystemMetrics(small ? SM_CXSMICON : SM_CXICON);
    int cy = GetSystemMetrics(small ? SM_CYSMICON : SM_CYICON);

    HICON ic = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                 cx, cy, LR_DEFAULTCOLOR);
    if (!ic) {
        ic = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(101), IMAGE_ICON,
                               0, 0, LR_DEFAULTSIZE | LR_DEFAULTCOLOR);
    }
    if (!ic) ic = LoadIconW(NULL, small ? IDI_APPLICATION : IDI_APPLICATION);
    return ic;
}