/* ==========================================================================
 * organize.c —— 模块 6：本地文件夹扫描与分类
 *
 * 扫描阶段**只读**：只列目录、算目标路径，绝不落地。
 * 分类动作同样是 Action，因此复用模块 1 的预览、冲突检测与回退。
 * ========================================================================== */
#include "lc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>

/* ============================================== 扩展名 -> 大类 ======== */

typedef struct { const wchar_t *ext; const wchar_t *cat; } ExtCat;

static const ExtCat EXT_MAP[] = {
    /* 图片 */
    {L".jpg",L"图片"},{L".jpeg",L"图片"},{L".png",L"图片"},{L".gif",L"图片"},
    {L".bmp",L"图片"},{L".webp",L"图片"},{L".tif",L"图片"},{L".tiff",L"图片"},
    {L".heic",L"图片"},{L".heif",L"图片"},{L".svg",L"图片"},{L".ico",L"图片"},
    {L".raw",L"图片"},{L".cr2",L"图片"},{L".cr3",L"图片"},{L".nef",L"图片"},
    {L".arw",L"图片"},{L".raf",L"图片"},{L".rw2",L"图片"},{L".dng",L"图片"},
    {L".orf",L"图片"},{L".pef",L"图片"},{L".srw",L"图片"},
    /* 视频 */
    {L".mp4",L"视频"},{L".mkv",L"视频"},{L".avi",L"视频"},{L".mov",L"视频"},
    {L".wmv",L"视频"},{L".flv",L"视频"},{L".webm",L"视频"},{L".m4v",L"视频"},
    {L".mpg",L"视频"},{L".mpeg",L"视频"},{L".ts",L"视频"},{L".rmvb",L"视频"},
    {L".3gp",L"视频"},
    /* 音频 */
    {L".mp3",L"音频"},{L".wav",L"音频"},{L".flac",L"音频"},{L".aac",L"音频"},
    {L".ogg",L"音频"},{L".m4a",L"音频"},{L".wma",L"音频"},{L".ape",L"音频"},
    {L".opus",L"音频"},{L".mid",L"音频"},{L".aiff",L"音频"},
    /* 文档 */
    {L".doc",L"文档"},{L".docx",L"文档"},{L".pdf",L"文档"},{L".txt",L"文档"},
    {L".md",L"文档"},{L".rtf",L"文档"},{L".odt",L"文档"},{L".wps",L"文档"},
    {L".tex",L"文档"},{L".epub",L"文档"},{L".mobi",L"文档"},{L".azw3",L"文档"},
    /* 表格 */
    {L".xls",L"表格"},{L".xlsx",L"表格"},{L".csv",L"表格"},{L".tsv",L"表格"},
    {L".ods",L"表格"},{L".numbers",L"表格"},
    /* 演示 */
    {L".ppt",L"演示"},{L".pptx",L"演示"},{L".odp",L"演示"},{L".key",L"演示"},
    /* 压缩包 */
    {L".zip",L"压缩包"},{L".rar",L"压缩包"},{L".7z",L"压缩包"},{L".tar",L"压缩包"},
    {L".gz",L"压缩包"},{L".bz2",L"压缩包"},{L".xz",L"压缩包"},{L".iso",L"压缩包"},
    {L".cab",L"压缩包"},{L".tgz",L"压缩包"},
    /* 程序 */
    {L".exe",L"程序"},{L".msi",L"程序"},{L".bat",L"程序"},{L".cmd",L"程序"},
    {L".ps1",L"程序"},{L".sh",L"程序"},{L".dll",L"程序"},{L".sys",L"程序"},
    {L".apk",L"程序"},{L".jar",L"程序"},
    /* 代码 */
    {L".py",L"代码"},{L".js",L"代码"},{L".ts",L"代码"},{L".java",L"代码"},
    {L".c",L"代码"},{L".cpp",L"代码"},{L".h",L"代码"},{L".hpp",L"代码"},
    {L".cs",L"代码"},{L".go",L"代码"},{L".rs",L"代码"},{L".rb",L"代码"},
    {L".php",L"代码"},{L".html",L"代码"},{L".css",L"代码"},{L".json",L"代码"},
    {L".xml",L"代码"},{L".yml",L"代码"},{L".yaml",L"代码"},{L".sql",L"代码"},
    {L".vue",L"代码"},{L".swift",L"代码"},{L".kt",L"代码"},
    /* 字体 */
    {L".ttf",L"字体"},{L".otf",L"字体"},{L".woff",L"字体"},{L".woff2",L"字体"},
    {L".fon",L"字体"},
    {NULL, NULL}
};

const wchar_t *organize_category_of(const wchar_t *path) {
    /*
     * 先问用户自定义分类。
     *
     * 自定义优先于内置是刻意的：用户既然专门写了 [发票] 这个段落，
     * 就说明他要的分类粒度比内置的"文档 / 图片 / 音视频"细得多。
     * 如果让内置先命中，他会发现自己写的规则永远不生效，而且不知道为什么。
     *
     * 匹配规则是"段落里任意一条条件满足即命中"，多个分类按 order 从小到大试，
     * 第一个命中的生效 —— 一个文件只归一类。允许同时命中多类的话，
     * 按 {category} 建目录时同一个文件会对应多个目标路径，预览表就没法看了。
     */
    const wchar_t *custom = categories_match(path);
    if (custom && custom[0]) return custom;

    const wchar_t *ext = lc_extname(path);
    if (!ext || !*ext) return L"其他";
    for (int i = 0; EXT_MAP[i].ext; ++i)
        if (lc_wcs_ieq(ext, EXT_MAP[i].ext)) return EXT_MAP[i].cat;
    return L"其他";
}

const wchar_t *organize_size_bucket(long long bytes) {
    const long long MB = 1024LL * 1024;
    if (bytes < MB) return L"小于1MB";
    if (bytes < 10 * MB) return L"1-10MB";
    if (bytes < 100 * MB) return L"10-100MB";
    if (bytes < 1024LL * MB) return L"100MB-1GB";
    return L"大于1GB";
}

/* ================================================== 规则 ============== */

void organize_rule_init(OrganizeRule *r, Arena *a) {
    memset(r, 0, sizeof(*r));
    r->template_ = arena_wcsdup(a, L"{category}");
    r->mode = arena_wcsdup(a, L"move");
    r->recursive = 1;
    r->skip_hidden = 1;
    vec_init(&r->include_ext);
    vec_init(&r->exclude_ext);
    r->skip_already_sorted = 1;
    r->files_only = 1;
}

void organize_rule_free(OrganizeRule *r) {
    if (!r) return;
    vec_free(&r->include_ext);
    vec_free(&r->exclude_ext);
}

/* ================================================== 扫描 ============== */

static int ovec_has(Vec *v, const wchar_t *s) {
    for (size_t i = 0; i < v->len; ++i)
        if (lc_wcs_ieq((const wchar_t *)v->items[i], s)) return 1;
    return 0;
}

int organize_scan(const wchar_t *root, const OrganizeRule *r, FileList *out) {
    if (!lc_is_dir(root)) return -1;

    FileList raw;
    filelist_init(&raw, out->arena);
    if (lc_scan_dir(root, r->recursive, r->skip_hidden, 1, &raw) != 0)
        return -1;

    int64_t now = lc_now_unix();
    for (size_t i = 0; i < raw.n; ++i) {
        const wchar_t *path = raw.paths[i];
        const wchar_t *ext = lc_extname(path);
        if (r->include_ext.len > 0) {
            if (!*ext || !ovec_has((Vec *)&r->include_ext, ext)) continue;
        }
        if (r->exclude_ext.len > 0 && *ext &&
            ovec_has((Vec *)&r->exclude_ext, ext)) continue;

        WIN32_FILE_ATTRIBUTE_DATA fad;
        wchar_t *lp = lc_long_path(path);
        int ok = lp && GetFileAttributesExW(lp, GetFileExInfoStandard, &fad);
        free(lp);
        if (!ok) continue;

        long long size = ((long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        if (r->min_size > 0 && size < r->min_size) continue;
        if (r->max_size > 0 && size > r->max_size) continue;

        if (r->older_than_days > 0 || r->newer_than_days > 0) {
            ULARGE_INTEGER u;
            u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
            u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
            int64_t mtime = (int64_t)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
            double age_days = (double)(now - mtime) / 86400.0;
            if (r->older_than_days > 0 && age_days < r->older_than_days) continue;
            if (r->newer_than_days > 0 && age_days > r->newer_than_days) continue;
        }
        filelist_push(out, path);
    }

    free(raw.paths);
    if (r->limit > 0 && out->n > (size_t)r->limit) out->n = (size_t)r->limit;
    return 0;
}

/* ================================================== 目标目录 ========== */

int organize_target_rel(const wchar_t *path, const OrganizeRule *r,
                        WStr *out, wchar_t *errbuf, size_t errcch) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    wchar_t *lp = lc_long_path(path);
    int ok = lp && GetFileAttributesExW(lp, GetFileExInfoStandard, &fad);
    free(lp);

    int64_t mtime = 0;
    long long size = 0;
    if (ok) {
        ULARGE_INTEGER u;
        u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
        u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
        mtime = (int64_t)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
        size = ((long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    }

    wchar_t year[8] = L"", month[4] = L"", day[4] = L"";
    time_t t = (time_t)mtime;
    struct tm tmv;
    if (localtime_s(&tmv, &t) == 0) {
        wcsftime(year, 8, L"%Y", &tmv);
        wcsftime(month, 4, L"%m", &tmv);
        wcsftime(day, 4, L"%d", &tmv);
    }

    wchar_t *stem = lc_stem(path);
    wchar_t letter[4] = L"#";
    if (stem && stem[0]) {
        letter[0] = (wchar_t)towupper(stem[0]);
        letter[1] = 0;
    }
    free(stem);

    const wchar_t *extp = lc_extname(path);
    wchar_t extname[64];
    if (extp && *extp) {
        wcsncpy(extname, extp + 1, 63);
        extname[63] = 0;
        lc_wcs_lower(extname);
    } else {
        wcscpy(extname, L"无扩展名");
    }

    /* 逐字符渲染模板 */
    const wchar_t *p = r->template_ ? r->template_ : L"{category}";
    WStr raw;
    wstr_init(&raw);
    while (*p) {
        if (*p != L'{') { wstr_putc(&raw, *p++); continue; }
        const wchar_t *close = wcschr(p, L'}');
        if (!close) {
            swprintf(errbuf, errcch, L"分类模板里有一个没有闭合的「{」");
            wstr_free(&raw);
            return -1;
        }
        wchar_t token[64];
        size_t tl = (size_t)(close - p - 1);
        if (tl >= 64) tl = 63;
        memcpy(token, p + 1, tl * sizeof(wchar_t));
        token[tl] = 0;

        if (wcscmp(token, L"category") == 0)       wstr_puts(&raw, organize_category_of(path));
        else if (wcscmp(token, L"year") == 0)      wstr_puts(&raw, year);
        else if (wcscmp(token, L"month") == 0)     wstr_puts(&raw, month);
        else if (wcscmp(token, L"day") == 0)       wstr_puts(&raw, day);
        else if (wcscmp(token, L"ext") == 0)       wstr_puts(&raw, extname);
        else if (wcscmp(token, L"letter") == 0)    wstr_puts(&raw, letter);
        else if (wcscmp(token, L"size") == 0)      wstr_puts(&raw, organize_size_bucket(size));
        else if (wcscmp(token, L"none") == 0)      { /* 空 */ }
        else {
            swprintf(errbuf, errcch,
                     L"分类模板里的变量「{%ls}」不存在。可用："
                     L"category year month day ext letter size", token);
            wstr_free(&raw);
            return -1;
        }
        p = close + 1;
    }

    /* 按 / 或 \ 拆成层级，每一层做名字规整 */
    wstr_clear(out);
    int first = 1;
    const wchar_t *q = raw.data;
    while (q && *q) {
        while (*q && (lc_is_sep(*q))) ++q;
        if (!*q) break;
        const wchar_t *seg_start = q;
        while (*q && !lc_is_sep(*q)) ++q;
        size_t seg_len = (size_t)(q - seg_start);
        wchar_t *seg = (wchar_t *)malloc((seg_len + 1) * sizeof(wchar_t));
        if (!seg) break;
        memcpy(seg, seg_start, seg_len * sizeof(wchar_t));
        seg[seg_len] = 0;
        wchar_t *safe = lc_sanitize_component(seg);
        free(seg);
        if (safe && safe[0]) {
            if (!first) wstr_putc(out, L'\\');
            wstr_puts(out, safe);
            first = 0;
        }
        free(safe);
    }
    wstr_free(&raw);
    return 0;
}

/* ================================================== 生成计划 ========== */

/*
 * preset 非空时只处理这些文件，不去扫描目录。
 *
 * 这是给"文件夹监控"用的：监控队列里已经攒了"刚刚变化的这几个文件"，
 * 再拿整个目录扫一遍就没意义了 —— 用户想要的是把手头这几个处理掉。
 */
Plan *organize_build_plan_ex(Arena *a, const wchar_t *root, const OrganizeRule *r,
                             Policy policy, const wchar_t *trash_root,
                             FileList *preset) {
    (void)trash_root;
    Plan *plan = (Plan *)arena_zalloc(a, sizeof(Plan));

    WStr desc;
    wstr_init(&desc);
    wstr_printf(&desc, L"按模板 %ls %ls分类",
                r->template_ ? r->template_ : L"{category}",
                lc_wcs_ieq(r->mode, L"move") ? L"移动" : L"复制");
    plan_init(plan, a, root, policy, L"organize", desc.data);
    wstr_free(&desc);

    FileList files;
    filelist_init(&files, a);
    if (preset) {
        for (size_t i = 0; i < preset->n; ++i)
            filelist_push(&files, preset->paths[i]);
    } else if (organize_scan(root, r, &files) != 0) {
        plan_compute_stats(plan);
        return plan;
    }

    int is_copy = lc_wcs_ieq(r->mode, L"copy");

    for (size_t i = 0; i < files.n; ++i) {
        WStr rel;
        wstr_init(&rel);
        wchar_t err[512];
        err[0] = 0;
        if (organize_target_rel(files.paths[i], r, &rel, err, 512) != 0) {
            Action *bad = plan_add(plan, L"organize", files.paths[i], files.paths[i],
                                   L"规则错误");
            if (bad) {
                action_add_conflict(bad, C_INVALID_NAME, LEVEL_ERROR, L"%ls", err);
                bad->selected = 0;
            }
            wstr_free(&rel);
            continue;
        }

        WStr dst;
        wstr_init(&dst);
        wstr_puts(&dst, root);
        if (rel.len) {
            if (dst.len && !lc_is_sep(dst.data[dst.len - 1])) wstr_putc(&dst, L'\\');
            wstr_puts(&dst, rel.data);
        }
        if (dst.len && !lc_is_sep(dst.data[dst.len - 1])) wstr_putc(&dst, L'\\');
        wstr_puts(&dst, lc_basename(files.paths[i]));

        WStr note;
        wstr_init(&note);
        wstr_printf(&note, L"归入 %ls", rel.len ? rel.data : L"根目录");

        Action *act = plan_add(plan, L"organize", files.paths[i], dst.data, note.data);
        if (act) {
            act->is_copy = is_copy;
            act->rule_desc = arena_wcsdup(a, rel.data);
            /* 已经在目标位置 → 默认不勾选 */
            wchar_t *ps = lc_dirname(files.paths[i]);
            wchar_t *pd = lc_dirname(dst.data);
            if (r->skip_already_sorted && ps && pd && lc_same_path(ps, pd)) {
                act->selected = 0;
                if (act->note) free(act->note);
                act->note = arena_wcsdup(a, L"已在目标位置");
            }
            free(ps); free(pd);
        }
        wstr_free(&note);
        wstr_free(&dst);
        wstr_free(&rel);
    }
    free(files.paths);

    plan_finalize(plan, 1, 1);
    return plan;
}

/* ================================================== 统计 ============== */

/* 原版入口：内部扫描整个目录 */
Plan *organize_build_plan(Arena *a, const wchar_t *root, const OrganizeRule *r,
                          Policy policy, const wchar_t *trash_root) {
    return organize_build_plan_ex(a, root, r, policy, trash_root, NULL);
}
int organize_category_stats(const wchar_t *root, int recursive, Arena *a,
                            CategoryStat **out, size_t *n_out,
                            int *total_count, long long *total_size) {
    (void)a;
    FileList files;
    filelist_init(&files, NULL);   /* 不用 arena，下面自己管内存 */

    /* lc_scan_dir 需要 arena；这里换成临时 arena */
    Arena *tmp = arena_new();
    FileList fl;
    filelist_init(&fl, tmp);
    if (lc_scan_dir(root, recursive, 1, 1, &fl) != 0) {
        arena_free(tmp);
        free(files.paths);
        return -1;
    }

    CategoryStat *stats = NULL;
    size_t n = 0, cap = 0;
    int tcount = 0;
    long long tsize = 0;

    for (size_t i = 0; i < fl.n; ++i) {
        const wchar_t *cat = organize_category_of(fl.paths[i]);
        long long sz = lc_size_of(fl.paths[i]);
        tcount++;
        tsize += sz;

        size_t j = 0;
        for (; j < n; ++j) if (wcscmp(stats[j].category, cat) == 0) break;
        if (j == n) {
            if (n + 1 > cap) {
                size_t ncap = cap ? cap * 2 : 16;
                CategoryStat *ns = (CategoryStat *)realloc(stats, ncap * sizeof(CategoryStat));
                if (!ns) break;
                stats = ns;
                cap = ncap;
            }
            stats[n].category = _wcsdup(cat);
            stats[n].count = 0;
            stats[n].size = 0;
            j = n++;
        }
        stats[j].count++;
        stats[j].size += sz;
    }

    /* 按数量降序（简单的插入排序，类别数很少） */
    for (size_t i = 1; i < n; ++i) {
        CategoryStat key = stats[i];
        size_t j = i;
        while (j > 0 && stats[j-1].count < key.count) {
            stats[j] = stats[j-1];
            --j;
        }
        stats[j] = key;
    }

    arena_free(tmp);
    free(files.paths);
    *out = stats;
    *n_out = n;
    if (total_count) *total_count = tcount;
    if (total_size) *total_size = tsize;
    return 0;
}
