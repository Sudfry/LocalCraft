/* ==========================================================================
 * category.c —— 用户自定义分类
 *
 * 为什么用 INI 而不是 JSON
 * ------------------------
 * 这个文件是**给用户手改的**。JSON 虽然程序好解析，但让人手写就是灾难：
 * 少一个逗号整份配置作废、中文要转义、不能写注释、出错信息只会说"第 1 行第 37 列
 * 附近有意外字符"。INI 没有这些毛病：一行一条、可以随便写注释、
 * 格式错了一眼就能看出来是哪儿。
 *
 * 配置文件放在数据目录下的「分类规则.ini」，首次运行会自动生成一份带注释的模板，
 * 用户照着改就行。界面上有按钮可以直接打开它。
 *
 * 匹配规则
 * --------
 * 每个 [分类名] 段落可以写这几种条件，**满足任意一条**就算命中：
 *
 *     ext        = pdf, ofd              按扩展名（逗号分隔，不用写点）
 *     name       = 发票, 增值税, 报销     文件名里包含这些关键词
 *     name_regex = ^\d{4}-\d{2}-\d{2}   文件名匹配这条正则
 *     order      = 10                    优先级，数字小的先试（默认 100）
 *
 * 一个文件只会归到一个分类：按 order 从小到大试，第一个命中的生效。
 * 全都没命中时，退回内置规则（图片 / 文档 / 音视频 / 压缩包…）。
 *
 * 为什么"一个文件只归一类"
 * ------------------------
 * 如果允许一个文件同时属于「发票」和「PDF」，那按 {category} 归类时它该放哪个目录？
 * 预览表里会出现同一个文件对应多个目标路径，用户根本没法确认。
 * 用 order 把优先级说清楚，比让用户去猜要诚实得多。
 * ========================================================================== */
#include "lc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ==================================================== 数据结构 ========= */

typedef struct {
    wchar_t *name;
    Vec      exts;       /* wchar_t*，存不带点的小写形式 */
    Vec      keywords;   /* wchar_t*，文件名关键词 */
    wchar_t *regex_src;  /* 原始正则字符串（可为空） */
    void    *regex;      /* 编译后的 LcRegex*，第一次用到时才编 */
    int      order;
    int      decl_seq;   /* 声明顺序，用来在同 order 时稳定排序 */
} UserCategory;

struct CategorySet {
    UserCategory *items;
    size_t        n, cap;
    wchar_t      *config_path;
};

/* 全局单例。配置只在用户显式重载时才变，读操作很轻。
   organize_category_of() 是按文件调用的，没有上下文参数可传，
   所以这里用单例是务实的选择。 */
static CategorySet      *g_set  = NULL;
static CRITICAL_SECTION  g_lock;
static int               g_lock_ready = 0;

static void ensure_lock(void) {
    if (!g_lock_ready) {
        InitializeCriticalSection(&g_lock);
        g_lock_ready = 1;
    }
}

/* ==================================================== 字符串小工具 ==== */

static wchar_t *dup_trim_n(const wchar_t *s, size_t n) {
    while (n > 0 && (s[0] == L' ' || s[0] == L'\t')) { ++s; --n; }
    while (n > 0 && (s[n-1] == L' ' || s[n-1] == L'\t' ||
                     s[n-1] == L'\r' || s[n-1] == L'\n')) --n;
    wchar_t *r = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    if (!r) return NULL;
    memcpy(r, s, n * sizeof(wchar_t));
    r[n] = 0;
    return r;
}

static void vec_push_dup(Vec *v, const wchar_t *s, size_t n) {
    wchar_t *d = dup_trim_n(s, n);
    if (d && d[0]) vec_push(v, d);
    else free(d);
}

/* 去掉扩展名前后的点和空白，转小写 */
static wchar_t *norm_ext(const wchar_t *s, size_t n) {
    wchar_t *d = dup_trim_n(s, n);
    if (!d) return NULL;
    wchar_t *p = d;
    while (*p == L'.') ++p;
    size_t L = wcslen(p);
    while (L > 0 && (p[L-1] == L'.' || p[L-1] == L' ')) p[--L] = 0;
    if (p != d) memmove(d, p, (L + 1) * sizeof(wchar_t));
    for (wchar_t *q = d; *q; ++q) *q = towlower(*q);
    if (!d[0]) { free(d); return NULL; }
    return d;
}

/* ==================================================== 配置路径 ======= */

/*
 * 首次运行时生成的模板。
 *
 * 示例规则**一律注释掉**，这一点很关键：
 * 如果让 [发票] ext = pdf 这样的示例默认生效，那么所有人装上之后的
 * 第一件事就是发现自己的 PDF 全被归进了「发票」文件夹 ——
 * 内置的「文档」分类被悄悄顶掉了，而且用户根本不知道是自己没写的那段配置干的。
 *
 * 默认行为必须和没装这个功能时完全一致。想要什么，自己把注释去掉。
 */
static const wchar_t *TEMPLATE =
L"; LocalCraft 自定义分类规则\n"
L"; ==================================================\n"
L";\n"
L"; 这个文件是给你手改的。改完保存，回到程序点一下「重新载入」即可生效。\n"
L";\n"
L"; 怎么写\n"
L"; --------------------------------------------------\n"
L"; 每个 [分类名] 段落定义一类。段落里可以写下面几种条件，\n"
L"; **满足任意一条**就算命中这个分类：\n"
L";\n"
L";     ext        = pdf, ofd            按扩展名（逗号分隔，不用写点）\n"
L";     name       = 发票, 增值税, 报销  文件名里包含这些关键词\n"
L";     name_regex = ^\\d{4}-\\d{2}      文件名匹配这条正则\n"
L";     order      = 10                 优先级，数字小的先试（默认 100）\n"
L";\n"
L"; 几条规矩\n"
L"; --------------------------------------------------\n"
L"; · 以分号 ; 开头的整行是注释，程序完全忽略。\n"
L"; · 一个文件只会归到一个分类：按 order 从小到大试，第一个命中的生效。\n"
L"; · 都没命中时，用程序内置的规则（图片 / 文档 / 音视频 / 压缩包…）。\n"
L"; · 分类名会直接用来建文件夹，不要写 \\ / : * ? \" < > | 这些字符。\n"
L"; · 自定义分类优先于内置规则：写了 [发票] ext = pdf，\n"
L";   那所有 pdf 就都会去「发票」，不会再进「文档」。\n"
L";\n"
L"; 下面是一些例子，**默认全都是注释状态**，想要哪个自己把开头的分号去掉。\n"
L"; ==================================================\n"
L"\n"
L"; [发票]\n"
L"; ext   = pdf, ofd\n"
L"; name  = 发票, 增值税, 报销\n"
L"; order = 10\n"
L"\n"
L"; [合同]\n"
L"; name  = 合同, 协议, 契约\n"
L"; order = 20\n"
L"\n"
L"; [设计稿]\n"
L"; ext   = psd, ai, sketch, fig\n"
L"; order = 30\n"
L"\n"
L"; [截图]\n"
L"; name_regex = (截图|screenshot|ScreenShot)\n"
L"; order = 40\n"
L"\n"
L"; [按年份归档的报表]\n"
L"; ext   = xlsx, xls\n"
L"; name  = 报表, 月报, 季报\n"
L"; order = 50\n";

const wchar_t *categories_config_path(void) {
    static wchar_t path[MAX_PATH * 2];
    static int inited = 0;
    if (!inited) {
        inited = 1;
        wchar_t *root = lc_data_root();
        swprintf(path, MAX_PATH * 2, L"%ls\\分类规则.ini", root ? root : L".");
        free(root);
    }
    return path;
}

/* 把宽字符串按 UTF-8 写文件 */
static int write_text_utf8(const wchar_t *path, const wchar_t *text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return -1;
    /* 写 UTF-8 BOM：记事本靠它认出是 UTF-8，否则会把中文按 ANSI 打开 */
    size_t body = (size_t)n - 1;
    char *buf = (char *)malloc(body + 4);
    if (!buf) return -1;
    buf[0] = (char)0xEF; buf[1] = (char)0xBB; buf[2] = (char)0xBF;
    WideCharToMultiByte(CP_UTF8, 0, text, -1, buf + 3, n, NULL, NULL);

    wchar_t *lp = lc_long_path(path);
    HANDLE h = CreateFileW(lp ? lp : path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) { free(buf); return -1; }
    DWORD w = 0;
    BOOL ok = WriteFile(h, buf, (DWORD)(body + 3), &w, NULL);
    CloseHandle(h);
    free(buf);
    return ok ? 0 : -1;
}

static unsigned char *read_all(const wchar_t *path, size_t *len) {
    *len = 0;
    wchar_t *lp = lc_long_path(path);
    HANDLE h = CreateFileW(lp ? lp : path, GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (8 << 20)) {
        CloseHandle(h);
        return NULL;
    }
    size_t n = (size_t)sz.QuadPart;
    unsigned char *b = (unsigned char *)malloc(n + 1);
    if (!b) { CloseHandle(h); return NULL; }
    DWORD r = 0;
    if (!ReadFile(h, b, (DWORD)n, &r, NULL)) r = 0;
    CloseHandle(h);
    b[r] = 0;
    *len = r;
    return b;
}

/* ==================================================== 解析 INI ======== */

static void cat_free(UserCategory *c) {
    if (!c) return;
    free(c->name);
    for (size_t i = 0; i < c->exts.len; ++i) free(c->exts.items[i]);
    for (size_t i = 0; i < c->keywords.len; ++i) free(c->keywords.items[i]);
    vec_free(&c->exts);
    vec_free(&c->keywords);
    free(c->regex_src);
    if (c->regex) regex_free((LcRegex *)c->regex);
    c->regex = NULL;
}

static UserCategory *set_add(CategorySet *s, const wchar_t *name) {
    if (s->n + 1 > s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 16;
        UserCategory *ni = (UserCategory *)realloc(s->items, cap * sizeof(UserCategory));
        if (!ni) return NULL;
        s->items = ni;
        s->cap = cap;
    }
    UserCategory *c = &s->items[s->n];
    memset(c, 0, sizeof(*c));
    c->name  = _wcsdup(name);
    c->order = 100;                       /* 默认优先级 */
    c->decl_seq = (int)s->n;
    vec_init(&c->exts);
    vec_init(&c->keywords);
    s->n++;
    return c;
}

/* 把 "a, b, c" 拆开逐项塞进去 */
static void split_push(Vec *v, const wchar_t *val, int is_ext) {
    const wchar_t *p = val;
    while (*p) {
        const wchar_t *s = p;
        while (*p && *p != L',' && *p != L'，' && *p != L';' && *p != L'；') ++p;
        if (p > s) {
            if (is_ext) {
                wchar_t *e = norm_ext(s, (size_t)(p - s));
                if (e) vec_push(v, e);
            } else {
                vec_push_dup(v, s, (size_t)(p - s));
            }
        }
        if (*p) ++p;
    }
}

/*
 * 解析配置文本。
 * 解析出错时不整份作废 —— 跳过那一行继续读后面的。
 * 原因：用户手改配置很容易某一处手滑，如果因此整份配置失效，
 * 他看到的只是"分类功能不起作用了"，完全不知道错在哪一行。
 * 跳过 + 界面上报出具体行号，比整份作废有用得多。
 */
static void parse_ini(CategorySet *s, const wchar_t *text, int *err_line,
                      wchar_t *err_msg, size_t errcch) {
    *err_line = 0;
    if (errcch) err_msg[0] = 0;

    UserCategory *cur = NULL;
    const wchar_t *p = text;
    int lineno = 0;

    while (*p) {
        const wchar_t *nl = wcschr(p, L'\n');
        size_t len = nl ? (size_t)(nl - p) : wcslen(p);
        lineno++;

        /* 去掉行首空白 */
        const wchar_t *l = p;
        size_t L = len;
        while (L > 0 && (l[0] == L' ' || l[0] == L'\t')) { ++l; --L; }

        if (L == 0 || l[0] == L';' || l[0] == L'#' || l[0] == L'\r') {
            /* 空行或注释 */
        } else if (l[0] == L'[') {
            const wchar_t *close = wcschr(l, L']');
            if (!close || (size_t)(close - l) > L) {
                if (!*err_line) {
                    *err_line = lineno;
                    swprintf(err_msg, errcch, L"第 %d 行：分类名缺少右方括号 ]", lineno);
                }
            } else {
                wchar_t *nm = dup_trim_n(l + 1, (size_t)(close - l - 1));
                if (!nm || !nm[0]) {
                    if (!*err_line) {
                        *err_line = lineno;
                        swprintf(err_msg, errcch, L"第 %d 行：分类名是空的", lineno);
                    }
                    free(nm);
                } else {
                    /* 分类名会用来建目录，非法字符直接拒绝 */
                    if (wcspbrk(nm, L"\\/:*?\"<>|")) {
                        if (!*err_line) {
                            *err_line = lineno;
                            swprintf(err_msg, errcch,
                                     L"第 %d 行：分类名「%ls」含有不能用于文件夹的字符 \\ / : * ? \" < > |",
                                     lineno, nm);
                        }
                        free(nm);
                        cur = NULL;
                    } else {
                        cur = set_add(s, nm);
                        free(nm);
                    }
                }
            }
        } else if (cur) {
            const wchar_t *eq = wcschr(l, L'=');
            if (!eq || (size_t)(eq - l) > L) {
                if (!*err_line) {
                    *err_line = lineno;
                    swprintf(err_msg, errcch, L"第 %d 行：不是「键 = 值」的格式", lineno);
                }
            } else {
                wchar_t *key = dup_trim_n(l, (size_t)(eq - l));
                const wchar_t *vstart = eq + 1;
                size_t vlen = L - (size_t)(vstart - l);
                if (key) {
                    if (_wcsicmp(key, L"ext") == 0) {
                        wchar_t *v = dup_trim_n(vstart, vlen);
                        if (v) { split_push(&cur->exts, v, 1); free(v); }
                    } else if (_wcsicmp(key, L"name") == 0) {
                        wchar_t *v = dup_trim_n(vstart, vlen);
                        if (v) { split_push(&cur->keywords, v, 0); free(v); }
                    } else if (_wcsicmp(key, L"name_regex") == 0) {
                        free(cur->regex_src);
                        cur->regex_src = dup_trim_n(vstart, vlen);
                    } else if (_wcsicmp(key, L"order") == 0) {
                        wchar_t *v = dup_trim_n(vstart, vlen);
                        if (v) { cur->order = (int)wcstol(v, NULL, 10); free(v); }
                    } else if (!*err_line) {
                        *err_line = lineno;
                        swprintf(err_msg, errcch,
                                 L"第 %d 行：不认识的键「%ls」（可用：ext / name / name_regex / order）",
                                 lineno, key);
                    }
                    free(key);
                }
            }
        } else if (!*err_line) {
            *err_line = lineno;
            swprintf(err_msg, errcch, L"第 %d 行：这一行不在任何 [分类] 段落里", lineno);
        }

        if (!nl) break;
        p = nl + 1;
    }
}

/* ==================================================== 载入 / 释放 ===== */

static int cmp_cat(const void *a, const void *b) {
    const UserCategory *x = (const UserCategory *)a;
    const UserCategory *y = (const UserCategory *)b;
    if (x->order != y->order) return x->order - y->order;
    return x->decl_seq - y->decl_seq;      /* 同优先级保持声明顺序 */
}

static void set_free(CategorySet *s) {
    if (!s) return;
    for (size_t i = 0; i < s->n; ++i) cat_free(&s->items[i]);
    free(s->items);
    free(s->config_path);
    free(s);
}

/*
 * 读取配置。文件不存在时**自动生成一份带注释的模板** ——
 * 用户不用去看文档就知道这文件该怎么写。
 */
static CategorySet *categories_load_internal(wchar_t *err, size_t errcch,
                                             int *created, wchar_t *first_err,
                                             size_t first_errcch) {
    if (created) *created = 0;
    if (first_err && first_errcch) first_err[0] = 0;

    CategorySet *s = (CategorySet *)calloc(1, sizeof(CategorySet));
    if (!s) return NULL;

    const wchar_t *path = categories_config_path();
    s->config_path = _wcsdup(path);

    if (!lc_is_file(path)) {
        if (write_text_utf8(path, TEMPLATE) == 0) {
            if (created) *created = 1;
        } else {
            /* 写不出来不算致命：仍然用空配置跑，只是没有自定义分类 */
            swprintf(err, errcch, L"无法创建配置文件：%ls", path);
        }
    }

    size_t len = 0;
    unsigned char *raw = read_all(path, &len);
    if (raw) {
        wchar_t *text = decode_text_bytes(raw, len);
        free(raw);
        if (text) {
            int eline = 0;
            wchar_t emsg[512];
            emsg[0] = 0;
            parse_ini(s, text, &eline, emsg, 512);
            if (eline && first_err && first_errcch) {
                wcsncpy(first_err, emsg, first_errcch - 1);
                first_err[first_errcch - 1] = 0;
            }
            free(text);
        }
    }

    /* 按 order 排序，匹配时顺序才稳定 */
    if (s->n > 1) qsort(s->items, s->n, sizeof(UserCategory), cmp_cat);

    /* 正则在这里就编译，把错误提前暴露出来，而不是等匹配到某个文件才发现 */
    if (first_err && first_errcch) {
        for (size_t i = 0; i < s->n; ++i) {
            if (!s->items[i].regex_src || !s->items[i].regex_src[0]) continue;
            wchar_t rerr[256];
            rerr[0] = 0;
            LcRegex *re = regex_compile(s->items[i].regex_src, 0, rerr, 256);
            if (!re) {
                if (!first_err[0]) {
                    swprintf(first_err, first_errcch,
                             L"分类「%ls」的正则有问题：%ls",
                             s->items[i].name, rerr[0] ? rerr : L"无法编译");
                }
            } else {
                s->items[i].regex = re;      /* 留着复用，匹配时不必反复编译 */
            }
        }
    }
    return s;
}

CategorySet *categories_get(void) {
    ensure_lock();
    EnterCriticalSection(&g_lock);
    if (!g_set) {
        wchar_t err[512];
        err[0] = 0;
        g_set = categories_load_internal(err, 512, NULL, NULL, 0);
    }
    CategorySet *s = g_set;
    LeaveCriticalSection(&g_lock);
    return s;
}

int categories_reload(wchar_t *err, size_t errcch, wchar_t *warn, size_t warncch,
                      int *created) {
    ensure_lock();
    wchar_t e[512];
    e[0] = 0;
    wchar_t w[512];
    w[0] = 0;
    int c = 0;
    CategorySet *ns = categories_load_internal(e, 512, &c, w, 512);
    if (!ns) {
        if (err && errcch) swprintf(err, errcch, L"载入配置失败");
        return -1;
    }
    EnterCriticalSection(&g_lock);
    CategorySet *old = g_set;
    g_set = ns;
    LeaveCriticalSection(&g_lock);
    set_free(old);

    if (err && errcch) { wcsncpy(err, e, errcch - 1); err[errcch - 1] = 0; }
    if (warn && warncch) { wcsncpy(warn, w, warncch - 1); warn[warncch - 1] = 0; }
    if (created) *created = c;
    return 0;
}

/* ==================================================== 匹配 ============ */

static int name_has_keyword(const wchar_t *base, const wchar_t *kw) {
    if (!kw || !kw[0]) return 0;
    /* 忽略大小写地找子串 */
    size_t kl = wcslen(kw);
    size_t bl = wcslen(base);
    if (kl > bl) return 0;
    for (size_t i = 0; i + kl <= bl; ++i)
        if (_wcsnicmp(base + i, kw, kl) == 0) return 1;
    return 0;
}

static int cat_matches(const UserCategory *c, const wchar_t *path,
                       const wchar_t *base, const wchar_t *ext_no_dot) {
    /* 扩展名 */
    for (size_t i = 0; i < c->exts.len; ++i)
        if (lc_wcs_ieq((const wchar_t *)c->exts.items[i], ext_no_dot)) return 1;

    /* 关键词 */
    for (size_t i = 0; i < c->keywords.len; ++i)
        if (name_has_keyword(base, (const wchar_t *)c->keywords.items[i])) return 1;

    /* 正则（编译失败的在载入时就记下来了，这里 regex 为 NULL 就跳过） */
    if (c->regex) {
        if (regex_matches((LcRegex *)c->regex, base)) return 1;
    }
    (void)path;
    return 0;
}

const wchar_t *categories_match(const wchar_t *path) {
    CategorySet *s = categories_get();
    if (!s || s->n == 0 || !path) return NULL;

    const wchar_t *base = lc_basename(path);
    const wchar_t *ext  = lc_extname(path);
    if (*ext == L'.') ++ext;
    wchar_t ext_lower[64];
    wcsncpy(ext_lower, ext, 63);
    ext_lower[63] = 0;
    for (wchar_t *q = ext_lower; *q; ++q) *q = towlower(*q);

    for (size_t i = 0; i < s->n; ++i)
        if (cat_matches(&s->items[i], path, base, ext_lower))
            return s->items[i].name;
    return NULL;
}

/* ==================================================== 给界面看的快照 == */

size_t categories_count(void) {
    CategorySet *s = categories_get();
    return s ? s->n : 0;
}

/* 把 Vec 里的条目用 "、" 拼成一句话，方便界面直接显示 */
static void join_vec(Vec *v, wchar_t *out, size_t cch) {
    if (!out || cch == 0) return;
    out[0] = 0;
    size_t used = 0;
    for (size_t i = 0; i < v->len; ++i) {
        const wchar_t *it = (const wchar_t *)v->items[i];
        size_t need = wcslen(it) + (i ? 3 : 0);
        if (used + need + 1 >= cch) { /* 装不下就截断，别溢出 */
            if (used + 4 < cch) wcscat(out, L"…");
            break;
        }
        if (i) wcscat(out, L"、");
        wcscat(out, it);
        used += need;
    }
}

int categories_detail(size_t idx, wchar_t *name, size_t ncch,
                      wchar_t *exts, size_t ecch,
                      wchar_t *kws, size_t kcch,
                      wchar_t *regex_src, size_t rcch,
                      int *order) {
    CategorySet *s = categories_get();
    if (!s || idx >= s->n) return -1;
    UserCategory *c = &s->items[idx];
    if (name && ncch) { wcsncpy(name, c->name, ncch - 1); name[ncch - 1] = 0; }
    if (exts) join_vec(&c->exts, exts, ecch);
    if (kws)  join_vec(&c->keywords, kws, kcch);
    if (regex_src && rcch) {
        wcsncpy(regex_src, c->regex_src ? c->regex_src : L"", rcch - 1);
        regex_src[rcch - 1] = 0;
    }
    if (order) *order = c->order;
    return 0;
}
