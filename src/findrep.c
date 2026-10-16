/* ==========================================================================
 * findrep.c —— 文本内容批量查找替换
 *
 * 定位
 * ----
 * 前几个模块改的是**文件名和位置**（重命名、移动、归类），
 * 这个模块改的是**文件内容**。这是第一次动文件里的字节。
 *
 * 最关键的问题：内容改动怎么回退？
 * ------------------------------
 * 回退引擎只认 4 种原子步骤，它们的逆操作是固定的：
 *     MOVE(src→dst)  ⟹  MOVE(dst→src)
 *     CREATE(dst)    ⟹  MOVE(dst → 回收站)
 *
 * 所以"原地改内容"不能只写成一个 CREATE —— 那样回退只会把改过的文件删掉，
 * 原内容就永远找不回来了。
 *
 * 用的办法是拆成两条步骤，让逆序回退天然正确：
 *
 *     正向：  ① MOVE(原文件 → 回收站)      先留个底
 *             ② CREATE(原文件)              内容由生成器写进去
 *
 *     回退：  ②' MOVE(原文件 → 回收站)      删掉改过的版本
 *             ①' MOVE(回收站 → 原文件)      把原件搬回来
 *
 * 结果：回退引擎一行都不用改，文件内容却能被完整还原。
 *
 * 编码
 * ----
 * 改内容必须**保持原编码**。把一个 GBK 文件按 UTF-8 写回去，
 * 在记事本里打开就是满屏乱码 —— 而且这属于"善意地毁掉用户文件"。
 * 所以扫描时记住编码，写回时用同一种。
 * ========================================================================== */
#include "lc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ==================================================== 编码 ============== */

enum { ENC_UTF8 = 0, ENC_UTF8_BOM, ENC_UTF16LE, ENC_UTF16BE, ENC_GBK };

/* 严格校验 UTF-8，通过就认为文本是 UTF-8（没有 BOM 时） */
static int is_valid_utf8(const unsigned char *d, size_t n) {
    for (size_t i = 0; i < n; ) {
        unsigned char b = d[i];
        int extra;
        if      (b < 0x80)           extra = 0;
        else if ((b & 0xE0) == 0xC0) extra = 1;
        else if ((b & 0xF0) == 0xE0) extra = 2;
        else if ((b & 0xF8) == 0xF0) extra = 3;
        else return 0;
        if (i + (size_t)extra >= n) return 0;
        for (int k = 1; k <= extra; ++k)
            if ((d[i + k] & 0xC0) != 0x80) return 0;
        i += (size_t)extra + 1;
    }
    return 1;
}

static int detect_encoding(const unsigned char *d, size_t n) {
    if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) return ENC_UTF8_BOM;
    if (n >= 2 && d[0] == 0xFF && d[1] == 0xFE) return ENC_UTF16LE;
    if (n >= 2 && d[0] == 0xFE && d[1] == 0xFF) return ENC_UTF16BE;
    if (is_valid_utf8(d, n)) return ENC_UTF8;
    return ENC_GBK;
}

/*
 * 二进制判定。
 * 这一步是**安全阀**：如果不对二进制文件设防，用户拿"全部文件"跑一次替换，
 * 整个文件夹的 exe / jpg / zip 都会被当成文本处理，然后在里面找到几个
 * 字节"匹配"就写回去 —— 文件当场损坏，而且回退日志再全也救不回
 * 用户不知道要回退的那些文件。
 */
static int looks_binary(const unsigned char *d, size_t n) {
    if (n >= 2 && ((d[0] == 0xFF && d[1] == 0xFE) ||
                   (d[0] == 0xFE && d[1] == 0xFF)))
        return 0;                       /* UTF-16 文本，不是二进制 */

    size_t check = n < 8192 ? n : 8192;
    size_t ctrl = 0;
    for (size_t i = 0; i < check; ++i) {
        unsigned char b = d[i];
        if (b == 0) return 1;           /* 出现 NUL，基本可以断定不是文本 */
        if (b < 0x09 || (b > 0x0D && b < 0x20)) ctrl++;
    }
    return check > 0 && ctrl * 100 > check * 5;   /* 控制字符超 5% */
}

/* 宽字符串按指定编码写回字节 */
static unsigned char *encode_text(const wchar_t *w, int enc, size_t *out_len) {
    *out_len = 0;
    if (enc == ENC_UTF16LE || enc == ENC_UTF16BE) {
        size_t n = wcslen(w);
        unsigned char *b = (unsigned char *)malloc(n * 2 + 2);
        if (!b) return NULL;
        size_t p = 0;
        b[p++] = (enc == ENC_UTF16LE) ? 0xFF : 0xFE;
        b[p++] = (enc == ENC_UTF16LE) ? 0xFE : 0xFF;
        for (size_t i = 0; i < n; ++i) {
            wchar_t c = w[i];
            if (enc == ENC_UTF16LE) { b[p++] = (unsigned char)(c & 0xFF);
                                      b[p++] = (unsigned char)((c >> 8) & 0xFF); }
            else                    { b[p++] = (unsigned char)((c >> 8) & 0xFF);
                                      b[p++] = (unsigned char)(c & 0xFF); }
        }
        *out_len = p;
        return b;
    }

    UINT cp = (enc == ENC_GBK) ? 936u : CP_UTF8;
    int n = WideCharToMultiByte(cp, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    /* WideCharToMultiByte 会把结尾的 NUL 算进去，这里去掉 */
    size_t body = (size_t)n - 1;
    size_t bom  = (enc == ENC_UTF8_BOM) ? 3 : 0;
    unsigned char *b = (unsigned char *)malloc(body + bom + 1);
    if (!b) return NULL;
    if (bom) { b[0] = 0xEF; b[1] = 0xBB; b[2] = 0xBF; }
    WideCharToMultiByte(cp, 0, w, -1, (char *)(b + bom), n, NULL, NULL);
    b[body + bom] = 0;
    *out_len = body + bom;
    return b;
}

/* ==================================================== 文本扩展名 ======= */

/*
 * 默认认为是文本的扩展名。
 * 不是"白名单之外一律拒绝" —— 用户可以在界面上关掉这个限制。
 * 但默认开着，因为**默认值决定了大多数人会怎么用**。
 */
static const wchar_t *TEXT_EXT[] = {
    L".txt", L".md", L".markdown", L".csv", L".tsv", L".json", L".xml",
    L".html", L".htm", L".css", L".js", L".mjs", L".ts", L".jsx", L".tsx",
    L".py", L".c", L".h", L".cpp", L".hpp", L".cs", L".java", L".go", L".rs",
    L".php", L".rb", L".sh", L".bat", L".cmd", L".ps1", L".sql", L".ini",
    L".cfg", L".conf", L".log", L".yml", L".yaml", L".toml", L".properties",
    L".srt", L".vtt", L".tex", L".rst", L".text", L".nfo", L".reg",
    NULL
};

static int is_text_ext(const wchar_t *path) {
    const wchar_t *e = lc_extname(path);
    for (int i = 0; TEXT_EXT[i]; ++i)
        if (lc_wcs_ieq(e, TEXT_EXT[i])) return 1;
    return 0;
}

/* 扩展名是否在逗号分隔的名单里。空名单 = 全部通过 */
static int in_ext_list(const wchar_t *path, const wchar_t *list) {
    if (!list || !list[0]) return 1;
    const wchar_t *e = lc_extname(path);
    if (*e == L'.') ++e;
    const wchar_t *p = list;
    while (*p) {
        while (*p == L' ' || *p == L',' || *p == L';' || *p == L'\t') ++p;
        if (!*p) break;
        const wchar_t *s = p;
        while (*p && *p != L',' && *p != L';') ++p;
        size_t n = (size_t)(p - s);
        while (n > 0 && (s[n-1] == L' ' || s[n-1] == L'\t')) --n;
        if (n > 0 && *s == L'.') { ++s; --n; }
        if (n > 0 && wcslen(e) == n && _wcsnicmp(e, s, n) == 0) return 1;
    }
    return 0;
}

/* ==================================================== 结果结构 ========= */


/* 单条样例最多显示多少字符 */
#define SAMPLE_MAX 160

static wchar_t *dup_n(const wchar_t *s, size_t n) {
    wchar_t *r = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    if (!r) return NULL;
    memcpy(r, s, n * sizeof(wchar_t));
    r[n] = 0;
    return r;
}

/* 把一段文本压成一行样例：去掉换行、过长截断 */
static wchar_t *make_sample(const wchar_t *line, size_t len) {
    if (len > SAMPLE_MAX) len = SAMPLE_MAX;
    wchar_t *r = dup_n(line, len);
    if (!r) return NULL;
    for (wchar_t *p = r; *p; ++p)
        if (*p == L'\r' || *p == L'\n' || *p == L'\t') *p = L' ';
    return r;
}

/* ==================================================== 匹配与替换 ======= */

/*
 * 对一段文本做替换。
 * repl 为 NULL 时只计数、不产出结果串（预览阶段用）。
 * 返回匹配数；出错返回 -1。
 */
static int do_replace(const wchar_t *text, const wchar_t *find,
                      const wchar_t *repl, int use_regex, int ignore_case,
                      WStr *out, wchar_t *err, size_t errcch) {
    if (use_regex) {
        LcRegex *re = regex_compile(find, ignore_case, err, errcch);
        if (!re) return -1;
        int n = regex_count_matches(re, text);
        if (n < 0) { regex_free(re); return -1; }
        if (out) {
            if (regex_replace_all(re, text, repl ? repl : L"", out) != 0) {
                regex_free(re);
                return -1;
            }
        }
        regex_free(re);
        return n;
    }

    /* 字面量：逐段扫描 */
    size_t flen = wcslen(find);
    if (flen == 0) return 0;
    size_t tlen = wcslen(text);
    if (flen > tlen) return 0;

    int count = 0;
    size_t i = 0;
    while (i + flen <= tlen) {
        int hit = ignore_case ? (_wcsnicmp(text + i, find, flen) == 0)
                              : (wcsncmp(text + i, find, flen) == 0);
        if (hit) {
            if (out && repl) wstr_puts(out, repl);
            count++;
            i += flen;
        } else {
            if (out) wstr_putc(out, text[i]);
            i++;
        }
    }
    if (out) wstr_puts(out, text + i);
    return count;
}

/* ==================================================== 扫描 ============== */


static int read_whole(const wchar_t *path, unsigned char **out, size_t *len) {
    *out = NULL; *len = 0;
    wchar_t *lp = lc_long_path(path);
    HANDLE h = CreateFileW(lp ? lp : path, GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) return -1;

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0) { CloseHandle(h); return -1; }
    size_t n = (size_t)sz.QuadPart;
    unsigned char *b = (unsigned char *)malloc(n + 1);
    if (!b) { CloseHandle(h); return -1; }

    size_t got = 0;
    while (got < n) {
        DWORD r = 0;
        if (!ReadFile(h, b + got, (DWORD)(n - got), &r, NULL) || r == 0) break;
        got += r;
    }
    CloseHandle(h);
    b[got] = 0;
    *out = b;
    *len = got;
    return 0;
}

static int write_whole(const wchar_t *path, const unsigned char *data, size_t len) {
    wchar_t *lp = lc_long_path(path);
    HANDLE h = CreateFileW(lp ? lp : path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) return -1;
    size_t put = 0;
    while (put < len) {
        DWORD w = 0;
        if (!WriteFile(h, data + put, (DWORD)(len - put), &w, NULL) || w == 0) {
            CloseHandle(h);
            return -1;
        }
        put += w;
    }
    CloseHandle(h);
    return 0;
}

/*
 * 扫描一个文件夹，找出所有"有匹配"的文本文件。
 * 全程**只读**，不改任何东西。
 */
int findrep_scan(const wchar_t *dir, const FindRepOpts *o,
                        FindRepResult *res, wchar_t *err, size_t errcch) {
    memset(res, 0, sizeof(*res));

    if (!dir || !dir[0]) { swprintf(err, errcch, L"请先选择文件夹"); return -1; }
    if (!o->find || !o->find[0]) { swprintf(err, errcch, L"请填写要查找的内容"); return -1; }

    /* 正则先编译一次，模式错误要在这里就报出来，而不是每个文件报一次 */
    if (o->use_regex) {
        LcRegex *re = regex_compile(o->find, o->ignore_case, err, errcch);
        if (!re) return -1;
        regex_free(re);
    }

    long long maxb = o->max_bytes > 0 ? o->max_bytes : (64ll << 20);
    int limit = o->limit > 0 ? o->limit : 5000;
    int per_file_samples = o->sample_per_file > 0 ? o->sample_per_file : 2;

    Arena *a = arena_new();
    FileList fl;
    filelist_init(&fl, a);
    if (lc_scan_dir(dir, o->recursive, 1, 1, &fl) != 0) {
        arena_free(a);
        swprintf(err, errcch, L"无法读取目录");
        return -1;
    }

    for (size_t i = 0; i < fl.n && res->n_files < (size_t)limit; ++i) {
        const wchar_t *path = fl.paths[i];

        /* 扩展名过滤 */
        if (!o->any_ext && !is_text_ext(path)) { res->skipped_ext++; continue; }
        if (o->include && o->include[0] && !in_ext_list(path, o->include)) {
            res->skipped_ext++; continue;
        }
        if (o->exclude && o->exclude[0] && in_ext_list(path, o->exclude)) {
            res->skipped_ext++; continue;
        }

        /* 体积上限 */
        {
            wchar_t *lp = lc_long_path(path);
            WIN32_FILE_ATTRIBUTE_DATA fad;
            long long sz = -1;
            if (GetFileAttributesExW(lp ? lp : path, GetFileExInfoStandard, &fad)) {
                sz = ((long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
            }
            free(lp);
            if (sz < 0) { res->unreadable++; continue; }
            if (sz > maxb) { res->skipped_big++; continue; }
        }

        unsigned char *data = NULL;
        size_t len = 0;
        if (read_whole(path, &data, &len) != 0) { res->unreadable++; continue; }
        res->scanned++;

        if (looks_binary(data, len)) {
            res->skipped_binary++;
            free(data);
            continue;
        }

        int enc = detect_encoding(data, len);
        wchar_t *text = decode_text_bytes(data, len);
        free(data);
        if (!text) { res->unreadable++; continue; }

        int n = do_replace(text, o->find, NULL, o->use_regex, o->ignore_case,
                           NULL, err, errcch);
        if (n < 0) { free(text); arena_free(a); return -1; }

        if (n > 0) {
            if (res->n_files + 1 > res->cap_files) {
                size_t cap = res->cap_files ? res->cap_files * 2 : 64;
                FindRepFile *nf = (FindRepFile *)realloc(res->files,
                                                        cap * sizeof(FindRepFile));
                if (!nf) { free(text); break; }
                res->files = nf;
                res->cap_files = cap;
            }
            FindRepFile *f = &res->files[res->n_files++];
            f->path     = _wcsdup(path);
            f->encoding = enc;
            f->bytes    = (long long)len;
            f->matches  = n;
            f->selected = 1;
            res->total_matches += n;

            /* 收集前几行样例：逐行判断，避免再要一套"匹配位置"接口 */
            if (per_file_samples > 0) {
                int taken = 0;
                int lineno = 0;
                const wchar_t *p = text;
                while (*p && taken < per_file_samples) {
                    const wchar_t *nl = wcschr(p, L'\n');
                    size_t llen = nl ? (size_t)(nl - p) : wcslen(p);
                    lineno++;

                    /*
                     * 这一行里有匹配吗？
                     *
                     * 必须**先把这一行单独截出来**再替换。p 指向行首，但它
                     * 后面的字符串一直延伸到文件结尾；直接对 p 跑替换会把
                     * 整篇剩下的内容都算进来，"改后"那一列就会显示成
                     * 该行加上后面所有内容，而不是这一行。
                     */
                    size_t line_len = llen;
                    while (line_len > 0 && (p[line_len - 1] == L'\r' ||
                                            p[line_len - 1] == L'\n'))
                        line_len--;

                    wchar_t *line_only = dup_n(p, line_len);
                    if (!line_only) { if (!nl) break; p = nl + 1; continue; }

                    WStr one;
                    wstr_init(&one);
                    int ln_hits = do_replace(line_only, o->find, o->replace,
                                             o->use_regex, o->ignore_case,
                                             &one, err, errcch);
                    free(line_only);

                    if (ln_hits > 0) {
                        if (res->n_hits + 1 > res->cap_hits) {
                            size_t cap = res->cap_hits ? res->cap_hits * 2 : 64;
                            FindRepHit *nh = (FindRepHit *)realloc(res->hits,
                                                        cap * sizeof(FindRepHit));
                            if (nh) { res->hits = nh; res->cap_hits = cap; }
                        }
                        if (res->n_hits < res->cap_hits) {
                            FindRepHit *h = &res->hits[res->n_hits++];
                            h->file   = _wcsdup(path);
                            h->line   = lineno;
                            h->before = make_sample(p, line_len);
                            h->after  = make_sample(one.data ? one.data : L"", SAMPLE_MAX);
                            taken++;
                        }
                    }
                    wstr_free(&one);

                    if (!nl) break;
                    p = nl + 1;
                }
            }
        }
        free(text);
    }

    arena_free(a);
    return 0;
}

void findrep_free(FindRepResult *r) {
    if (!r) return;
    for (size_t i = 0; i < r->n_files; ++i) free(r->files[i].path);
    for (size_t i = 0; i < r->n_hits; ++i) {
        free(r->hits[i].file);
        free(r->hits[i].before);
        free(r->hits[i].after);
    }
    free(r->files);
    free(r->hits);
    memset(r, 0, sizeof(*r));
}

/* ==================================================== 生成器 =========== */

/*
 * 生成器规格：edit:<flags>:<hexfind>:<hexreplace>
 *
 * 为什么用十六进制而不是直接放文本：规格是一整条字符串，要用冒号分隔字段。
 * 用户的查找内容里完全可能带冒号（比如替换 "http://"），
 * 直接拼进去解析就错位了。每个 wchar_t 编成 4 位十六进制，
 * 既没有分隔符冲突，也不涉及编码转换，编解码各十来行就够。
 */
static void hex_encode(const wchar_t *s, WStr *out) {
    for (const wchar_t *p = s; p && *p; ++p)
        wstr_printf(out, L"%04x", (unsigned)(*p & 0xFFFF));
}

static wchar_t *hex_decode(const wchar_t *s) {
    size_t n = s ? wcslen(s) : 0;
    if (n % 4) return NULL;
    size_t chars = n / 4;
    wchar_t *out = (wchar_t *)malloc((chars + 1) * sizeof(wchar_t));
    if (!out) return NULL;
    for (size_t i = 0; i < chars; ++i) {
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            wchar_t c = s[i * 4 + k];
            unsigned d;
            if      (c >= L'0' && c <= L'9') d = (unsigned)(c - L'0');
            else if (c >= L'a' && c <= L'f') d = (unsigned)(c - L'a' + 10);
            else if (c >= L'A' && c <= L'F') d = (unsigned)(c - L'A' + 10);
            else { free(out); return NULL; }
            v = (v << 4) | d;
        }
        out[i] = (wchar_t)v;
    }
    out[chars] = 0;
    return out;
}

int findrep_make_spec(const FindRepOpts *o, wchar_t *out, size_t cch) {
    WStr hf, hr;
    wstr_init(&hf);
    wstr_init(&hr);
    hex_encode(o->find, &hf);
    hex_encode(o->replace ? o->replace : L"", &hr);

    int flags = (o->use_regex ? 1 : 0) | (o->ignore_case ? 2 : 0);
    int n = _snwprintf(out, cch - 1, L"edit:%d:%ls:%ls",
                       flags, hf.data ? hf.data : L"", hr.data ? hr.data : L"");
    out[cch - 1] = 0;

    wstr_free(&hf);
    wstr_free(&hr);
    return (n < 0 || (size_t)n >= cch) ? -1 : 0;
}

/*
 * 执行一次替换。src 是**回收站里的原始副本**，dst 是原路径。
 * executor 在 STEP_CREATE 里调这个。
 */
int findrep_apply(const wchar_t *spec, const wchar_t *src,
                         const wchar_t *dst, wchar_t *err, size_t errcch) {
    if (!spec || wcsncmp(spec, L"edit:", 5) != 0) {
        swprintf(err, errcch, L"生成规格格式不对");
        return -1;
    }

    /* 解析 flags:find:replace */
    const wchar_t *p = spec + 5;
    wchar_t *endp = NULL;
    long flags = wcstol(p, &endp, 10);
    if (!endp || *endp != L':') { swprintf(err, errcch, L"规格缺少标志位"); return -1; }
    p = endp + 1;

    const wchar_t *sep = wcschr(p, L':');
    if (!sep) { swprintf(err, errcch, L"规格缺少分隔符"); return -1; }

    wchar_t *hexf = dup_n(p, (size_t)(sep - p));
    wchar_t *hexr = _wcsdup(sep + 1);
    if (!hexf || !hexr) { free(hexf); free(hexr);
                          swprintf(err, errcch, L"内存不足"); return -1; }

    wchar_t *find = hex_decode(hexf);
    wchar_t *repl = hex_decode(hexr);
    free(hexf); free(hexr);
    if (!find || !repl) {
        free(find); free(repl);
        swprintf(err, errcch, L"规格里的查找内容解码失败");
        return -1;
    }
    if (!find[0]) {
        free(find); free(repl);
        swprintf(err, errcch, L"查找内容为空");
        return -1;
    }

    unsigned char *data = NULL;
    size_t len = 0;
    if (read_whole(src, &data, &len) != 0) {
        free(find); free(repl);
        swprintf(err, errcch, L"读不到原始副本：%ls", src);
        return -1;
    }

    int enc = detect_encoding(data, len);
    wchar_t *text = decode_text_bytes(data, len);
    free(data);
    if (!text) {
        free(find); free(repl);
        swprintf(err, errcch, L"原文件解码失败");
        return -1;
    }

    WStr out;
    wstr_init(&out);
    int n = do_replace(text, find, repl, (flags & 1) != 0, (flags & 2) != 0,
                       &out, err, errcch);
    free(text);
    free(find);
    free(repl);
    if (n < 0) { wstr_free(&out); return -1; }

    /* 按**原编码**写回。这一步错了就是满屏乱码。 */
    size_t blen = 0;
    unsigned char *bytes = encode_text(out.data ? out.data : L"", enc, &blen);
    wstr_free(&out);
    if (!bytes) { swprintf(err, errcch, L"编码回写失败"); return -1; }

    int rc = write_whole(dst, bytes, blen);
    free(bytes);
    if (rc != 0) { swprintf(err, errcch, L"写入失败：%ls", dst); return -1; }
    return 0;
}
