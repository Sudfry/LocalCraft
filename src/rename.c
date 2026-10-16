/* ==========================================================================
 * rename.c —— 模块 1a：批量重命名规则引擎
 *
 * 规则只负责"算出新名字"，不碰文件。产出的 Action 交给 planner，
 * 于是重命名天然获得：预览对照、冲突检测、可回退。
 *
 * 处理顺序（未使用模板时）：
 *     查找替换 → 前后缀 → 编号 → 大小写 → 扩展名大小写
 * ========================================================================== */
#include "lc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>

/* ===================================================== 规则生命周期 === */

void rename_rule_init(RenameRule *r, Arena *a) {
    memset(r, 0, sizeof(*r));
    r->find = NULL;
    r->replace = arena_wcsdup(a, L"");
    r->target = arena_wcsdup(a, L"stem");
    r->template_ = NULL;
    r->prefix = arena_wcsdup(a, L"");
    r->suffix = arena_wcsdup(a, L"");
    r->number = 0;
    r->number_pos = arena_wcsdup(a, L"suffix");
    r->number_start = 1;
    r->number_step = 1;
    r->number_width = 3;
    r->number_sep = arena_wcsdup(a, L"_");
    r->case_mode = arena_wcsdup(a, L"keep");
    r->ext_case = arena_wcsdup(a, L"keep");
    vec_init(&r->include_ext);
    vec_init(&r->exclude_ext);
    r->recursive = 0;
    r->skip_hidden = 1;
    r->sort = arena_wcsdup(a, L"name");
    r->reverse = 0;
    r->limit = 0;
    r->files_only = 1;
}

void rename_rule_free(RenameRule *r) {
    if (!r) return;
    /* 字符串都在 Arena 里，这里只回收 Vec */
    vec_free(&r->include_ext);
    vec_free(&r->exclude_ext);
}

/* ===================================================== 描述 =========== */

int rename_rule_describe(const RenameRule *r, WStr *out, Arena *a) {
    (void)a;
    int any = 0;
    if (r->template_ && r->template_[0]) {
        wstr_printf(out, L"模板 %ls", r->template_);
        any = 1;
    } else {
        if (r->find && r->find[0]) {
            wstr_printf(out, L"%ls替换 「%ls」→「%ls」",
                        r->use_regex ? L"正则" : L"字面",
                        r->find, r->replace ? r->replace : L"");
            any = 1;
        }
        if (r->prefix && r->prefix[0]) {
            if (any) wstr_puts(out, L"；");
            wstr_printf(out, L"前缀 %ls", r->prefix);
            any = 1;
        }
        if (r->suffix && r->suffix[0]) {
            if (any) wstr_puts(out, L"；");
            wstr_printf(out, L"后缀 %ls", r->suffix);
            any = 1;
        }
        if (r->number) {
            if (any) wstr_puts(out, L"；");
            wstr_printf(out, L"编号 %d 起，%d 位", r->number_start, r->number_width);
            any = 1;
        }
    }
    if (r->case_mode && wcscmp(r->case_mode, L"keep") != 0) {
        if (any) wstr_puts(out, L"；");
        wstr_printf(out, L"大小写 %ls", r->case_mode);
        any = 1;
    }
    if (r->ext_case && wcscmp(r->ext_case, L"keep") != 0) {
        if (any) wstr_puts(out, L"；");
        wstr_printf(out, L"扩展名 %ls", r->ext_case);
        any = 1;
    }
    if (!any) wstr_puts(out, L"（空规则）");
    return 0;
}

/* ===================================================== 扫描 =========== */

typedef struct {
    wchar_t *path;
    wchar_t *name;        /* 文件名（排序用） */
    long long size;
    int64_t  mtime;
} ScanEntry;

static int vec_contains_str(Vec *v, const wchar_t *s) {
    for (size_t i = 0; i < v->len; ++i)
        if (lc_wcs_ieq((const wchar_t *)v->items[i], s)) return 1;
    return 0;
}

static int ext_allowed(const RenameRule *r, const wchar_t *path) {
    const wchar_t *ext = lc_extname(path);
    if (r->include_ext.len > 0) {
        if (!*ext) return 0;
        if (!vec_contains_str((Vec *)&r->include_ext, ext)) return 0;
    }
    if (r->exclude_ext.len > 0 && *ext) {
        if (vec_contains_str((Vec *)&r->exclude_ext, ext)) return 0;
    }
    return 1;
}

static int cmp_name(const void *pa, const void *pb) {
    const ScanEntry *a = (const ScanEntry *)pa, *b = (const ScanEntry *)pb;
    return _wcsicmp(a->name, b->name);
}
static int cmp_mtime(const void *pa, const void *pb) {
    const ScanEntry *a = (const ScanEntry *)pa, *b = (const ScanEntry *)pb;
    if (a->mtime < b->mtime) return -1;
    if (a->mtime > b->mtime) return 1;
    return 0;
}
static int cmp_size(const void *pa, const void *pb) {
    const ScanEntry *a = (const ScanEntry *)pa, *b = (const ScanEntry *)pb;
    if (a->size < b->size) return -1;
    if (a->size > b->size) return 1;
    return 0;
}
static int cmp_path(const void *pa, const void *pb) {
    const ScanEntry *a = (const ScanEntry *)pa, *b = (const ScanEntry *)pb;
    return _wcsicmp(a->path, b->path);
}

static int64_t filetime_to_unix(FILETIME ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    if (u.QuadPart == 0) return 0;
    return (int64_t)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

int rename_scan(const wchar_t *root, const RenameRule *r, FileList *out) {
    if (!lc_is_dir(root)) return -1;

    /* 先收集所有文件（复用通用扫描器），再按需过滤 + 排序 */
    FileList raw;
    filelist_init(&raw, out->arena);
    if (lc_scan_dir(root, r->recursive, r->skip_hidden, r->files_only, &raw) != 0)
        return -1;

    ScanEntry *ents = NULL;
    size_t n = 0, cap = 0;
    for (size_t i = 0; i < raw.n; ++i) {
        if (!ext_allowed(r, raw.paths[i])) continue;
        WIN32_FILE_ATTRIBUTE_DATA fad;
        wchar_t *lp = lc_long_path(raw.paths[i]);
        long long size = 0;
        int64_t mtime = 0;
        if (lp && GetFileAttributesExW(lp, GetFileExInfoStandard, &fad)) {
            size = ((long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
            mtime = filetime_to_unix(fad.ftLastWriteTime);
        }
        free(lp);

        if (n + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 64;
            ScanEntry *ne = (ScanEntry *)realloc(ents, ncap * sizeof(ScanEntry));
            if (!ne) break;
            ents = ne;
            cap = ncap;
        }
        ents[n].path = raw.paths[i];
        ents[n].name = (wchar_t *)lc_basename(raw.paths[i]);
        ents[n].size = size;
        ents[n].mtime = mtime;
        n++;
    }

    if (n > 1) {
        if (wcscmp(r->sort, L"mtime") == 0)      qsort(ents, n, sizeof(ScanEntry), cmp_mtime);
        else if (wcscmp(r->sort, L"size") == 0)  qsort(ents, n, sizeof(ScanEntry), cmp_size);
        else if (wcscmp(r->sort, L"path") == 0)  qsort(ents, n, sizeof(ScanEntry), cmp_path);
        else if (wcscmp(r->sort, L"none") == 0)  { /* 保持扫描顺序 */ }
        else                                     qsort(ents, n, sizeof(ScanEntry), cmp_name);
        if (r->reverse) {
            for (size_t i = 0; i < n / 2; ++i) {
                ScanEntry t = ents[i];
                ents[i] = ents[n - 1 - i];
                ents[n - 1 - i] = t;
            }
        }
    }

    size_t take = n;
    if (r->limit > 0 && (size_t)r->limit < take) take = (size_t)r->limit;
    for (size_t i = 0; i < take; ++i) filelist_push(out, ents[i].path);

    free(ents);
    /* raw.paths 里的字符串属于 arena，不用释放 */
    free(raw.paths);
    return 0;
}

/* ===================================================== 模板渲染 ======= */

typedef struct {
    const wchar_t *stem;
    const wchar_t *ext;       /* 含点 */
    const wchar_t *parent;
    int            n;         /* 1 基序号（已按 start/step 调整） */
    int            i;         /* 0 基 */
    long long      size;
    int64_t        mtime;
} TplCtx;

static int apply_format_int(const wchar_t *fmt, int value, WStr *out) {
    if (!fmt || !*fmt) { wstr_printf(out, L"%d", value); return 0; }
    /* 支持 03d / 5d 这类整型格式 */
    wchar_t spec[32];
    swprintf(spec, 32, L"%%%ls", fmt);
    wstr_printf(out, spec, value);
    return 0;
}

static int render_template(const wchar_t *tpl, const TplCtx *ctx,
                           WStr *out, wchar_t *err, size_t errcch) {
    const wchar_t *p = tpl;
    while (*p) {
        if (*p != L'{') { wstr_putc(out, *p++); continue; }
        const wchar_t *close = wcschr(p, L'}');
        if (!close) {
            swprintf(err, errcch, L"模板里有一个没有闭合的「{」");
            return -1;
        }
        wchar_t token[64];
        size_t tlen = (size_t)(close - p - 1);
        if (tlen >= 64) tlen = 63;
        memcpy(token, p + 1, tlen * sizeof(wchar_t));
        token[tlen] = 0;

        wchar_t *colon = wcschr(token, L':');
        wchar_t *fmt = NULL;
        if (colon) { *colon = 0; fmt = colon + 1; }

        if (wcscmp(token, L"name") == 0 || wcscmp(token, L"stem") == 0) {
            wstr_puts(out, ctx->stem);
        } else if (wcscmp(token, L"ext") == 0) {
            wstr_puts(out, ctx->ext);
        } else if (wcscmp(token, L"extname") == 0) {
            wstr_puts(out, ctx->ext[0] == L'.' ? ctx->ext + 1 : ctx->ext);
        } else if (wcscmp(token, L"parent") == 0) {
            wstr_puts(out, ctx->parent ? ctx->parent : L"");
        } else if (wcscmp(token, L"n") == 0) {
            apply_format_int(fmt, ctx->n, out);
        } else if (wcscmp(token, L"i") == 0) {
            apply_format_int(fmt, ctx->i, out);
        } else if (wcscmp(token, L"size") == 0) {
            if (fmt && *fmt) {
                wchar_t spec[32];
                swprintf(spec, 32, L"%%%ls", fmt);
                wstr_printf(out, spec, (long long)ctx->size);
            } else {
                wstr_printf(out, L"%lld", (long long)ctx->size);
            }
        } else if (wcscmp(token, L"mtime") == 0 || wcscmp(token, L"ctime") == 0) {
            wchar_t buf[128] = L"";
            if (fmt && *fmt) {
                time_t t = (time_t)ctx->mtime;
                struct tm tmv;
                if (localtime_s(&tmv, &t) == 0) wcsftime(buf, 128, fmt, &tmv);
            } else {
                lc_format_time(ctx->mtime, buf, 128);
            }
            wstr_puts(out, buf);
        } else if (wcscmp(token, L"date") == 0 || wcscmp(token, L"year") == 0 ||
                   wcscmp(token, L"month") == 0 || wcscmp(token, L"day") == 0 ||
                   wcscmp(token, L"time") == 0) {
            const wchar_t *dfmt = wcscmp(token, L"date") == 0 ? L"%Y%m%d"
                                : wcscmp(token, L"year") == 0 ? L"%Y"
                                : wcscmp(token, L"month") == 0 ? L"%m"
                                : wcscmp(token, L"day") == 0 ? L"%d"
                                : L"%H%M%S";
            wchar_t buf[64] = L"";
            time_t t = (time_t)ctx->mtime;
            struct tm tmv;
            if (localtime_s(&tmv, &t) == 0) wcsftime(buf, 64, dfmt, &tmv);
            wstr_puts(out, buf);
        } else {
            swprintf(err, errcch,
                     L"模板里的变量「{%ls}」不存在。可用变量："
                     L"name ext extname n i parent size mtime date year month day",
                     token);
            return -1;
        }
        p = close + 1;
    }
    return 0;
}

/* ===================================================== 大小写 ========= */

static void apply_case(wchar_t *s, const wchar_t *mode) {
    if (!s || !mode) return;
    if (wcscmp(mode, L"lower") == 0) lc_wcs_lower(s);
    else if (wcscmp(mode, L"upper") == 0) lc_wcs_upper(s);
    else if (wcscmp(mode, L"title") == 0) lc_wcs_title(s);
}

/* ===================================================== 新名字 ========= */

int rename_new_name(const wchar_t *path, int index, const RenameRule *r,
                    WStr *out, wchar_t *errbuf, size_t errcch) {
    const wchar_t *base = lc_basename(path);
    const wchar_t *extp = lc_extname(path);
    size_t stem_len = (size_t)(extp - base);

    wchar_t *stem = (wchar_t *)malloc((stem_len + 1) * sizeof(wchar_t));
    if (!stem) return -1;
    memcpy(stem, base, stem_len * sizeof(wchar_t));
    stem[stem_len] = 0;

    wchar_t ext[512];
    wcsncpy(ext, extp, 511);
    ext[511] = 0;

    int number_value = r->number_start + (index - 1) * r->number_step;
    WStr newname;
    wstr_init(&newname);
    /*: 渲染结果里是否已经带了扩展名（模板写了 {ext}/{extname} 就是带了） */
    int ext_in_name = 0;

    if (r->template_ && r->template_[0]) {
        TplCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.stem = stem;
        ctx.ext = ext;
        wchar_t *dir = lc_dirname(path);
        ctx.parent = lc_basename(dir);
        ctx.n = number_value;
        ctx.i = index - 1;
        ctx.size = lc_size_of(path);
        ctx.mtime = 0;
        WIN32_FILE_ATTRIBUTE_DATA fad;
        wchar_t *lp = lc_long_path(path);
        if (lp && GetFileAttributesExW(lp, GetFileExInfoStandard, &fad))
            ctx.mtime = filetime_to_unix(fad.ftLastWriteTime);
        free(lp);

        if (render_template(r->template_, &ctx, &newname, errbuf, errcch) != 0) {
            free(stem); free(dir); wstr_free(&newname);
            return -1;
        }
        free(dir);

        /* 模板里出现 {ext}/{extname} 时视为「完整文件名」，否则视为「主名」，
           由下面的统一收尾负责补扩展名。 */
        ext_in_name = (wcsstr(r->template_, L"{ext}") != NULL ||
                       wcsstr(r->template_, L"{extname}") != NULL);
    } else {
        /* ---- 查找替换 ---- */
        wchar_t *working = NULL;
        if (r->find && r->find[0]) {
            if (wcscmp(r->target, L"name") == 0) {
                WStr full;
                wstr_init(&full);
                wstr_puts(&full, stem);
                wstr_puts(&full, ext);
                wchar_t *repl;
                if (r->use_regex) {
                    LcRegex *re = regex_compile(r->find, r->ignore_case, errbuf, errcch);
                    if (!re) { free(stem); wstr_free(&full); return -1; }
                    WStr res;
                    wstr_init(&res);
                    int rc = regex_replace_all(re, full.data, r->replace, &res);
                    regex_free(re);
                    if (rc != 0) { free(stem); wstr_free(&full); wstr_free(&res); return -1; }
                    repl = wstr_steal(&res);
                } else {
                    repl = lc_wcs_replace_all(full.data, r->find, r->replace,
                                              r->ignore_case);
                }
                wstr_free(&full);
                if (!repl) { free(stem); return -1; }
                const wchar_t *pe = lc_extname(repl);
                size_t sl = (size_t)(pe - repl);
                wchar_t *ns = (wchar_t *)malloc((sl + 1) * sizeof(wchar_t));
                if (ns) {
                    memcpy(ns, repl, sl * sizeof(wchar_t));
                    ns[sl] = 0;
                    free(stem);
                    stem = ns;
                }
                wcsncpy(ext, pe, 511);
                ext[511] = 0;
                free(repl);
            } else if (wcscmp(r->target, L"ext") == 0) {
                wchar_t *ne;
                if (r->use_regex) {
                    LcRegex *re = regex_compile(r->find, r->ignore_case, errbuf, errcch);
                    if (!re) { free(stem); return -1; }
                    WStr res;
                    wstr_init(&res);
                    int rc = regex_replace_all(re, ext, r->replace, &res);
                    regex_free(re);
                    if (rc != 0) { free(stem); wstr_free(&res); return -1; }
                    ne = wstr_steal(&res);
                } else {
                    ne = lc_wcs_replace_all(ext, r->find, r->replace, r->ignore_case);
                }
                if (ne) { wcsncpy(ext, ne, 511); ext[511] = 0; free(ne); }
            } else {
                LcRegex *re = NULL;
                if (r->use_regex) {
                    re = regex_compile(r->find, r->ignore_case, errbuf, errcch);
                    if (!re) { free(stem); return -1; }
                    WStr res;
                    wstr_init(&res);
                    int rc = regex_replace_all(re, stem, r->replace, &res);
                    regex_free(re);
                    if (rc != 0) { free(stem); wstr_free(&res); return -1; }
                    working = wstr_steal(&res);
                } else {
                    working = lc_wcs_replace_all(stem, r->find, r->replace,
                                                 r->ignore_case);
                }
                if (working) { free(stem); stem = working; }
            }
        }

        /* ---- 前后缀 ---- */
        WStr mid;
        wstr_init(&mid);
        wstr_puts(&mid, r->prefix ? r->prefix : L"");
        wstr_puts(&mid, stem);
        wstr_puts(&mid, r->suffix ? r->suffix : L"");

        /* ---- 编号 ---- */
        if (r->number) {
            wchar_t num[32];
            swprintf(num, 32, L"%0*d", r->number_width, number_value);
            const wchar_t *sep = r->number_sep ? r->number_sep : L"";
            WStr numbered;
            wstr_init(&numbered);
            if (wcscmp(r->number_pos, L"prefix") == 0) {
                wstr_puts(&numbered, num);
                wstr_puts(&numbered, sep);
                wstr_puts(&numbered, mid.data);
            } else if (wcscmp(r->number_pos, L"only") == 0) {
                if (*sep) wstr_puts(&numbered, sep);
                wstr_puts(&numbered, num);
            } else {
                wstr_puts(&numbered, mid.data);
                if (mid.len && *sep) wstr_puts(&numbered, sep);
                wstr_puts(&numbered, num);
            }
            wstr_free(&mid);
            mid = numbered;
        }

        /* ---- 大小写 ---- */
        apply_case(mid.data, r->case_mode);

        wstr_puts(&newname, mid.data);
        wstr_free(&mid);
    }

    /* ---- 统一收尾：扩展名 ----
       模板里已经写了 {ext} 的话，渲染结果本身就带扩展名，
       这里绝不能再拼一次（否则会出现 a.jpg.jpg）。 */
    if (!ext_in_name) {
        apply_case(ext, r->ext_case);
        if (ext[0] && ext[0] != L'.') {
            WStr fixed;
            wstr_init(&fixed);
            wstr_putc(&fixed, L'.');
            wstr_puts(&fixed, ext);
            wcsncpy(ext, fixed.data, 511);
            ext[511] = 0;
            wstr_free(&fixed);
        }
        wstr_puts(&newname, ext);
    }

    wchar_t *safe = lc_sanitize_component(newname.data);
    wstr_clear(out);
    wstr_puts(out, safe);
    free(safe);
    free(stem);
    wstr_free(&newname);
    return 0;
}

/* ===================================================== 生成计划 ======= */

Plan *rename_build_plan(Arena *a, const wchar_t *root, const RenameRule *r,
                        Policy policy, const wchar_t *trash_root) {
    (void)trash_root;
    Plan *plan = (Plan *)arena_zalloc(a, sizeof(Plan));

    WStr desc;
    wstr_init(&desc);
    rename_rule_describe(r, &desc, a);

    plan_init(plan, a, root, policy, L"rename", desc.data);
    wstr_free(&desc);

    FileList files;
    filelist_init(&files, a);
    if (rename_scan(root, r, &files) != 0) {
        plan_compute_stats(plan);
        return plan;
    }

    WStr newname;
    wstr_init(&newname);
    for (size_t i = 0; i < files.n; ++i) {
        wchar_t err[512];
        err[0] = 0;
        if (rename_new_name(files.paths[i], (int)i + 1, r, &newname, err, 512) != 0) {
            Action *bad = plan_add(plan, L"rename", files.paths[i], files.paths[i],
                                   L"规则错误");
            if (bad) action_add_conflict(bad, C_INVALID_NAME, LEVEL_ERROR, L"%ls", err);
            if (bad) bad->selected = 0;
            continue;
        }
        wchar_t *dir = lc_dirname(files.paths[i]);
        WStr dst;
        wstr_init(&dst);
        wstr_puts(&dst, dir);
        if (dst.len && !lc_is_sep(dst.data[dst.len - 1])) wstr_putc(&dst, L'\\');
        wstr_puts(&dst, newname.data);
        free(dir);

        Action *act = plan_add(plan, L"rename", files.paths[i], dst.data, L"重命名");
        if (act) {
            act->rule_desc = arena_wcsdup(a, desc.data);
        }
        wstr_free(&dst);
    }
    wstr_free(&newname);
    free(files.paths);

    plan_finalize(plan, 1, 1);
    return plan;
}
