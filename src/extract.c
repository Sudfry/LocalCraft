/* ==========================================================================
 * extract.c —— 文档纯文本抽取（模块 3/4 的地基）
 *
 * 支持：
 *   .txt .md .csv .tsv .json .xml .html .htm .log .ini .yaml .c .py …
 *       -> 直接解码（自动识别 BOM / UTF-8 / GBK）
 *   .docx  -> word/document.xml 里的 <w:t>，<w:p> 当段落
 *   .pptx  -> ppt/slides/slideN.xml 里的 <a:t>，<a:p> 当段落
 *   .xlsx  -> xl/sharedStrings.xml + xl/worksheets/sheetN.xml
 *   .pdf   -> 基础抽取：FlateDecode 流里的 Tj/TJ 文本
 *
 * 只做"够用于去重与摘要"的抽取，不追求版式还原。
 * 抽取失败一律返回 NULL 并给出中文原因，绝不静默返回空串。
 * ========================================================================== */
#include "lc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ==================================================== 扩展名工具 ======= */

static int ext_is(const wchar_t *path, const wchar_t *ext) {
    return lc_wcs_ieq(lc_extname(path), ext);
}

static int ext_in(const wchar_t *path, const wchar_t *const *list) {
    for (int i = 0; list[i]; ++i)
        if (ext_is(path, list[i])) return 1;
    return 0;
}

static const wchar_t *TEXT_EXTS[] = {
    L".txt", L".md", L".markdown", L".csv", L".tsv", L".json", L".xml",
    L".log", L".ini", L".yaml", L".yml", L".c", L".h", L".cpp", L".hpp",
    L".py", L".js", L".ts", L".java", L".cs", L".go", L".rs", L".rb",
    L".php", L".sql", L".sh", L".bat", L".ps1", L".css", L".tex", L".rst",
    NULL
};
static const wchar_t *HTML_EXTS[] = { L".html", L".htm", L".xhtml", NULL };

int extract_supported(const wchar_t *path) {
    if (!path) return 0;
    if (ext_in(path, TEXT_EXTS)) return 1;
    if (ext_in(path, HTML_EXTS)) return 1;
    if (ext_is(path, L".docx") || ext_is(path, L".docm")) return 1;
    if (ext_is(path, L".xlsx") || ext_is(path, L".xlsm")) return 1;
    if (ext_is(path, L".pptx") || ext_is(path, L".pptm")) return 1;
    if (ext_is(path, L".pdf")) return 1;
    return 0;
}

const wchar_t *extract_kind(const wchar_t *path) {
    if (!path) return L"";
    if (ext_is(path, L".docx") || ext_is(path, L".docm")) return L"文档";
    if (ext_is(path, L".xlsx") || ext_is(path, L".xlsm")) return L"表格";
    if (ext_is(path, L".pptx") || ext_is(path, L".pptm")) return L"演示";
    if (ext_is(path, L".pdf")) return L"PDF";
    if (ext_in(path, HTML_EXTS)) return L"网页";
    if (ext_in(path, TEXT_EXTS)) return L"文本";
    return L"";
}

/* ==================================================== 读整个文件 ======= */

static unsigned char *read_all(const wchar_t *path, size_t *len_out) {
    wchar_t *lp = lc_long_path(path);
    if (!lp) return NULL;
    HANDLE h = CreateFileW(lp, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 ||
        sz.QuadPart > (LONGLONG)(256ll << 20)) {
        CloseHandle(h);
        return NULL;
    }
    size_t n = (size_t)sz.QuadPart;
    unsigned char *buf = (unsigned char *)malloc(n + 1);
    if (!buf) { CloseHandle(h); return NULL; }

    size_t got = 0;
    while (got < n) {
        DWORD chunk = 0;
        DWORD want = (DWORD)((n - got) > (1u << 20) ? (1u << 20) : (n - got));
        if (!ReadFile(h, buf + got, want, &chunk, NULL) || chunk == 0) break;
        got += chunk;
    }
    CloseHandle(h);
    buf[got] = 0;
    *len_out = got;
    return buf;
}

/* ==================================================== XML 小工具 ======= */

static void ws_put_entity(WStr *out, const char *name, size_t namelen) {
    if (namelen == 3 && strncmp(name, "amp", 3) == 0)      wstr_putc(out, L'&');
    else if (namelen == 2 && strncmp(name, "lt", 2) == 0)  wstr_putc(out, L'<');
    else if (namelen == 2 && strncmp(name, "gt", 2) == 0)  wstr_putc(out, L'>');
    else if (namelen == 4 && strncmp(name, "quot", 4) == 0) wstr_putc(out, L'"');
    else if (namelen == 4 && strncmp(name, "apos", 4) == 0) wstr_putc(out, L'\'');
    else if (namelen == 4 && strncmp(name, "nbsp", 4) == 0) wstr_putc(out, L' ');
    else if (namelen > 1 && name[0] == '#') {
        long cp = 0;
        if (name[1] == 'x' || name[1] == 'X')
            cp = strtol(name + 2, NULL, 16);
        else
            cp = strtol(name + 1, NULL, 10);
        if (cp > 0 && cp <= 0x10FFFF) wstr_putc(out, (wchar_t)cp);
    }
}

/* 按 UTF-8 解码一段 XML 文本（含实体），追加到 out */
static void ws_put_xml_text(WStr *out, const char *p, const char *end) {
    while (p < end) {
        unsigned char c = (unsigned char)*p;
        if (c == '&') {
            const char *semi = (const char *)memchr(p, ';', (size_t)(end - p));
            if (semi && semi - p <= 12) {
                ws_put_entity(out, p + 1, (size_t)(semi - p - 1));
                p = semi + 1;
                continue;
            }
            wstr_putc(out, L'&');
            ++p;
            continue;
        }
        if (c < 0x80) { wstr_putc(out, (wchar_t)c); ++p; continue; }
        int extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2
                  : (c & 0xF8) == 0xF0 ? 3 : 0;
        if (extra == 0 || p + extra >= end) { wstr_putc(out, L'?'); ++p; continue; }
        if (extra == 1) {
            wstr_putc(out, (wchar_t)(((c & 0x1F) << 6) | (p[1] & 0x3F)));
        } else if (extra == 2) {
            wstr_putc(out, (wchar_t)(((c & 0x0F) << 12) |
                       ((p[1] & 0x3F) << 6) | (p[2] & 0x3F)));
        } else {
            unsigned cp = (unsigned)(((c & 0x07) << 18) |
                          ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) |
                          (p[3] & 0x3F));
            if (cp >= 0x10000) {
                cp -= 0x10000;
                wstr_putc(out, (wchar_t)(0xD800 + (cp >> 10)));
                wstr_putc(out, (wchar_t)(0xDC00 + (cp & 0x3FF)));
            } else {
                wstr_putc(out, (wchar_t)cp);
            }
        }
        p += extra + 1;
    }
}

/* 从 p 处（指向 '<'）解析标签名，返回名字长度；*is_close 表示 </ */
static size_t tag_name(const char *p, const char *end, int *is_close, int *self_close) {
    *is_close = 0;
    *self_close = 0;
    if (p >= end || *p != '<') return 0;
    ++p;
    if (p < end && *p == '/') { *is_close = 1; ++p; }
    const char *s = p;
    while (p < end && *p != '>' && *p != ' ' && *p != '\t' && *p != '\r'
           && *p != '\n' && *p != '/') ++p;
    size_t n = (size_t)(p - s);
    /* 找标签结尾，判断是否自闭合 */
    const char *q = p;
    while (q < end && *q != '>') ++q;
    if (q > s && *(q - 1) == '/') *self_close = 1;
    return n;
}

static int name_is(const char *p, size_t n, const char *want) {
    return strlen(want) == n && strncmp(p, want, n) == 0;
}

/* 判断 name[0..nlen) 是否出现在空格分隔的列表里 */
static int tag_list_has(const char *list, const char *name, size_t nlen) {
    if (!list) return 0;
    const char *w = list;
    while (*w) {
        const char *sp = strchr(w, ' ');
        size_t wl = sp ? (size_t)(sp - w) : strlen(w);
        if (wl == nlen && strncmp(w, name, nlen) == 0) return 1;
        if (!sp) break;
        w = sp + 1;
    }
    return 0;
}

/* 通用 XML 文本抽取：
   text_tags 里的元素内容会被取出（一个空格分隔的名字列表），
   para_tags 里的元素会在前面加一个换行。 */
static void xml_extract(const char *xml, size_t len, WStr *out,
                        const char *text_tags, const char *para_tags) {
    const char *p = xml, *end = xml + len;
    int last_was_newline = 1;

    while (p < end) {
        if (*p != '<') { ++p; continue; }

        int is_close = 0, self_close = 0;
        const char *name = p + 1 + (p + 1 < end && p[1] == '/' ? 1 : 0);
        size_t nlen = tag_name(p, end, &is_close, &self_close);
        if (nlen == 0) { ++p; continue; }
        const char *tagend = p;
        while (tagend < end && *tagend != '>') ++tagend;
        if (tagend >= end) break;

        /* 段落标签 -> 换行 */
        if (!is_close && tag_list_has(para_tags, name, nlen)) {
            if (!last_was_newline) { wstr_putc(out, L'\n'); last_was_newline = 1; }
        }

        /* 文本标签 -> 取内容 */
        if (!is_close && tag_list_has(text_tags, name, nlen)) {
            if (self_close) { p = tagend + 1; continue; }
            const char *content = tagend + 1;
            /* 找到对应的闭合标签（文本元素不嵌套同名标签） */
            char closebuf[64];
            snprintf(closebuf, sizeof(closebuf), "</%.*s", (int)nlen, name);
            size_t cl = strlen(closebuf);
            const char *close = end;
            for (const char *scan = content; scan + cl <= end; ++scan) {
                if (scan[0] == '<' && strncmp(scan, closebuf, cl) == 0) {
                    close = scan;
                    break;
                }
            }
            if (close > content) {
                ws_put_xml_text(out, content, close);
                last_was_newline = 0;
            }
            p = close;
            continue;
        }

        /* 软换行 / 制表符 */
        if (self_close) {
            if (name_is(name, nlen, "w:br") || name_is(name, nlen, "a:br")) {
                wstr_putc(out, L'\n');
                last_was_newline = 1;
            } else if (name_is(name, nlen, "w:tab")) {
                wstr_putc(out, L'\t');
            }
        }

        p = tagend + 1;
    }
}

/* ==================================================== docx / pptx ===== */

static wchar_t *extract_docx(const wchar_t *path, size_t max_chars,
                             wchar_t *err, size_t errcch) {
    LcZip *z = zip_open(path);
    if (!z) {
        swprintf(err, errcch, L"无法作为 zip 打开（可能不是有效的 docx）");
        return NULL;
    }
    unsigned char *xml = NULL;
    size_t xlen = 0;
    if (zip_read_by_name(z, "word/document.xml", &xml, &xlen) != 0) {
        zip_close(z);
        swprintf(err, errcch, L"docx 里找不到 word/document.xml");
        return NULL;
    }
    zip_close(z);

    WStr out;
    wstr_init(&out);
    xml_extract((const char *)xml, xlen, &out, "w:t", "w:p");
    free(xml);

    if (out.len == 0) { wstr_free(&out); return NULL; }
    if (max_chars && out.len > max_chars) out.data[out.len = max_chars] = 0;
    return wstr_steal(&out);
}

static int cmp_slide_name(const void *a, const void *b) {
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    /* 抽 slide 后面的数字比大小，避免 slide10 排在 slide2 前面 */
    const char *dx = x, *dy = y;
    while (*dx && (*dx < '0' || *dx > '9')) ++dx;
    while (*dy && (*dy < '0' || *dy > '9')) ++dy;
    long nx = strtol(dx, NULL, 10), ny = strtol(dy, NULL, 10);
    if (nx != ny) return nx < ny ? -1 : 1;
    return strcmp(x, y);
}

static wchar_t *extract_pptx(const wchar_t *path, size_t max_chars,
                             wchar_t *err, size_t errcch) {
    LcZip *z = zip_open(path);
    if (!z) {
        swprintf(err, errcch, L"无法作为 zip 打开（可能不是有效的 pptx）");
        return NULL;
    }

    size_t n = zip_count(z);

    /* 收集所有 ppt/slides/slideN.xml，并按 N 数字排序（否则 slide10 会排在 slide2 前） */
    const char **slides = (const char **)malloc((n ? n : 1) * sizeof(char *));
    size_t ns = 0;
    for (size_t i = 0; i < n; ++i) {
        const char *nm = zip_name_at(z, i);
        if (!nm) continue;
        if (strncmp(nm, "ppt/slides/slide", 16) != 0) continue;
        size_t l = strlen(nm);
        if (l < 5 || _stricmp(nm + l - 4, ".xml") != 0) continue;
        slides[ns++] = nm;
    }
    if (ns > 1) qsort(slides, ns, sizeof(char *), cmp_slide_name);

    WStr out;
    wstr_init(&out);
    for (size_t i = 0; i < ns; ++i) {
        unsigned char *xml = NULL;
        size_t xlen = 0;
        if (zip_read_by_name(z, slides[i], &xml, &xlen) != 0) continue;
        if (i) wstr_puts(&out, L"\n");
        xml_extract((const char *)xml, xlen, &out, "a:t", "a:p");
        free(xml);
        if (max_chars && out.len > max_chars) break;
    }
    free(slides);
    zip_close(z);

    if (out.len == 0) { wstr_free(&out); return NULL; }
    if (max_chars && out.len > max_chars) out.data[out.len = max_chars] = 0;
    return wstr_steal(&out);
}

/* ==================================================== xlsx ============ */

/* 取 xl/sharedStrings.xml 里的所有 <t> 内容 */
static void collect_shared_strings(const char *xml, size_t len, Vec *out, Arena *a) {
    const char *p = xml, *end = xml + len;
    while (p < end) {
        const char *hit = (const char *)memchr(p, '<', (size_t)(end - p));
        if (!hit) break;
        int is_close = 0, self_close = 0;
        size_t nlen = tag_name(hit, end, &is_close, &self_close);
        if (!is_close && nlen == 1 && hit[1] == 't') {
            const char *tagend = hit;
            while (tagend < end && *tagend != '>') ++tagend;
            if (tagend >= end) break;
            const char *close = tagend + 1;
            while (close + 3 < end && !(close[0] == '<' && close[1] == '/' &&
                                        close[2] == 't' && close[3] == '>'))
                ++close;
            WStr s;
            wstr_init(&s);
            if (close > tagend + 1) ws_put_xml_text(&s, tagend + 1, close);
            vec_push(out, arena_wcsdup(a, s.data ? s.data : L""));
            wstr_free(&s);
            p = (close + 4 < end) ? close + 4 : end;
            continue;
        }
        p = hit + 1;
    }
}

static wchar_t *extract_xlsx(const wchar_t *path, size_t max_chars,
                             wchar_t *err, size_t errcch) {
    LcZip *z = zip_open(path);
    if (!z) {
        swprintf(err, errcch, L"无法作为 zip 打开（可能不是有效的 xlsx）");
        return NULL;
    }

    Arena *ta = arena_new();
    Vec shared;
    vec_init(&shared);

    unsigned char *xml = NULL;
    size_t xlen = 0;
    if (zip_read_by_name(z, "xl/sharedStrings.xml", &xml, &xlen) == 0) {
        collect_shared_strings((const char *)xml, xlen, &shared, ta);
        free(xml);
    } else {
        wstr_free(NULL);   /* 无共享字符串表：数字型表格，正常 */
    }

    /* 收集 sheetN.xml */
    size_t n = zip_count(z);
    const char **sheets = (const char **)malloc((n ? n : 1) * sizeof(char *));
    size_t ns = 0;
    for (size_t i = 0; i < n; ++i) {
        const char *nm = zip_name_at(z, i);
        if (!nm) continue;
        if (strncmp(nm, "xl/worksheets/sheet", 19) != 0) continue;
        size_t l = strlen(nm);
        if (l < 5 || _stricmp(nm + l - 4, ".xml") != 0) continue;
        sheets[ns++] = nm;
    }
    if (ns > 1) qsort(sheets, ns, sizeof(char *), cmp_slide_name);

    WStr out;
    wstr_init(&out);
    for (size_t si = 0; si < ns; ++si) {
        xml = NULL; xlen = 0;
        if (zip_read_by_name(z, sheets[si], &xml, &xlen) != 0) continue;
        if (si) wstr_puts(&out, L"\n");

        const char *p = (const char *)xml, *end = (const char *)xml + xlen;
        int in_row = 0, first_cell = 1;
        while (p < end) {
            const char *hit = (const char *)memchr(p, '<', (size_t)(end - p));
            if (!hit) break;
            int is_close = 0, self_close = 0;
            size_t nlen = tag_name(hit, end, &is_close, &self_close);
            const char *tagend = hit;
            while (tagend < end && *tagend != '>') ++tagend;
            if (tagend >= end) break;

            if (!is_close && nlen == 3 && strncmp(hit + 1, "row", 3) == 0) {
                if (in_row) { wstr_putc(&out, L'\n'); first_cell = 1; }
                in_row = 1;
            } else if (!is_close && nlen == 1 && hit[1] == 'c') {
                /* 看这个单元格是不是共享字符串引用：t="s" */
                const char *attr = hit + 2;
                int is_shared = 0;
                while (attr < tagend) {
                    if (attr + 3 < tagend && attr[0] == 't' && attr[1] == '=' &&
                        attr[2] == '"' && attr[3] == 's') { is_shared = 1; break; }
                    ++attr;
                }
                /* 找本单元格里的 <v> 或 <is><t> */
                const char *celend = tagend + 1;
                if (!self_close) {
                    while (celend + 3 < end &&
                           !(celend[0] == '<' && celend[1] == '/' && celend[2] == 'c'))
                        ++celend;
                }
                const char *v = NULL, *ve = NULL;
                for (const char *q = tagend + 1; q + 2 < celend; ++q) {
                    if (q[0] == '<' && q[1] == 'v' && q[2] == '>') {
                        v = q + 3;
                        ve = v;
                        while (ve < celend && *ve != '<') ++ve;
                        break;
                    }
                    if (q[0] == '<' && q[1] == 't' && q[2] == '>') {
                        v = q + 3;
                        ve = v;
                        while (ve < celend && *ve != '<') ++ve;
                        break;
                    }
                }
                if (!first_cell) wstr_putc(&out, L'\t');
                first_cell = 0;
                if (v && ve && ve > v) {
                    if (is_shared) {
                        char numbuf[32];
                        size_t nl = (size_t)(ve - v);
                        if (nl < 31) {
                            memcpy(numbuf, v, nl);
                            numbuf[nl] = 0;
                            long idx = strtol(numbuf, NULL, 10);
                            if (idx >= 0 && (size_t)idx < shared.len)
                                wstr_puts(&out, (const wchar_t *)shared.items[idx]);
                        }
                    } else {
                        ws_put_xml_text(&out, v, ve);
                    }
                }
            } else if (!is_close && nlen == 3 && strncmp(hit + 1, "row", 3) != 0) {
                /* 其它标签忽略 */
            }
            p = tagend + 1;
            if (max_chars && out.len > max_chars) break;
        }
        free(xml);
        if (max_chars && out.len > max_chars) break;
    }
    free(sheets);
    vec_free(&shared);
    arena_free(ta);
    zip_close(z);

    if (out.len == 0) { wstr_free(&out); return NULL; }
    if (max_chars && out.len > max_chars) out.data[out.len = max_chars] = 0;
    return wstr_steal(&out);
}

/* ==================================================== PDF ============= */

/* 在字典里找 "FlateDecode" */
static int dict_has(const char *p, const char *end, const char *word) {
    size_t wl = strlen(word);
    for (const char *q = p; q + wl <= end; ++q)
        if (memcmp(q, word, wl) == 0) return 1;
    return 0;
}

static void pdf_text_ops(const char *s, const char *end, WStr *out) {
    /* 只处理 (...) Tj / (...) ' / [ ... ] TJ 这三种最常见的形式 */
    const char *p = s;
    int last_newline = 1;
    while (p < end) {
        if (*p == '(') {
            /* 找配对的 ')'，处理转义 */
            const char *q = p + 1;
            WStr tmp;
            wstr_init(&tmp);
            int depth = 1;
            while (q < end && depth > 0) {
                if (*q == '\\' && q + 1 < end) {
                    char e = q[1];
                    switch (e) {
                        case 'n': wstr_putc(&tmp, L'\n'); break;
                        case 'r': wstr_putc(&tmp, L'\r'); break;
                        case 't': wstr_putc(&tmp, L'\t'); break;
                        case 'b': wstr_putc(&tmp, L'\b'); break;
                        case 'f': wstr_putc(&tmp, L'\f'); break;
                        case '(': wstr_putc(&tmp, L'('); break;
                        case ')': wstr_putc(&tmp, L')'); break;
                        case '\\': wstr_putc(&tmp, L'\\'); break;
                        default:
                            if (e >= '0' && e <= '7') {
                                int v = 0, k = 0;
                                while (k < 3 && q + 1 + k < end &&
                                       q[1 + k] >= '0' && q[1 + k] <= '7') {
                                    v = v * 8 + (q[1 + k] - '0');
                                    ++k;
                                }
                                wstr_putc(&tmp, (wchar_t)v);
                                q += k;
                            } else {
                                wstr_putc(&tmp, (wchar_t)(unsigned char)e);
                            }
                    }
                    q += 2;
                    continue;
                }
                if (*q == '(') depth++;
                else if (*q == ')') { depth--; if (depth == 0) break; }
                if (depth > 0) {
                    /* PDF 文本字符串是 PDFDocEncoding 或 Latin-1 的单字节 */
                    unsigned char c = (unsigned char)*q;
                    wstr_putc(&tmp, c < 0x80 ? (wchar_t)c : (wchar_t)c);
                    ++q;
                }
            }
            /* 只有后面跟 Tj / ' / " 才算文本绘制（否则只是字典里的字符串） */
            const char *after = q + 1;
            while (after < end && (*after == ' ' || *after == '\r' ||
                                    *after == '\n' || *after == '\t')) ++after;
            int is_text = 0;
            if (after + 1 < end && after[0] == 'T' && after[1] == 'j') is_text = 1;
            else if (after < end && (*after == '\'')) is_text = 1;
            else if (after < end && (*after == '"')) is_text = 1;

            if (is_text && tmp.len) {
                wstr_puts(out, tmp.data);
                last_newline = 0;
            } else if (is_text) {
                /* 空字符串往往是排版换行 */
                if (!last_newline) { wstr_putc(out, L'\n'); last_newline = 1; }
            }
            wstr_free(&tmp);
            p = q + 1;
            continue;
        }
        /* TJ 数组里的字符串已经在上面处理过；这里只处理换行提示 */
        if (p + 1 < end && p[0] == 'T' && p[1] == 'D') {
            if (!last_newline) { wstr_putc(out, L'\n'); last_newline = 1; }
        }
        if (p + 1 < end && p[0] == 'T' && p[1] == 'd') {
            if (!last_newline) { wstr_putc(out, L'\n'); last_newline = 1; }
        }
        ++p;
    }
}

static wchar_t *extract_pdf(const wchar_t *path, size_t max_chars,
                            wchar_t *err, size_t errcch) {
    size_t len = 0;
    unsigned char *data = read_all(path, &len);
    if (!data) { swprintf(err, errcch, L"无法读取文件"); return NULL; }

    WStr out;
    wstr_init(&out);
    int cid_font_seen = 0;

    const char *p = (const char *)data;
    const char *end = p + len;
    while (p < end) {
        const char *s = (const char *)memchr(p, 's', (size_t)(end - p));
        if (!s) break;
        if (end - s < 7 || strncmp(s, "stream", 6) != 0) { p = s + 1; continue; }

        /* 检查 stream 之前的字典 */
        const char *dict_start = s - 1;
        int back = 0;
        while (dict_start > (const char *)data && back < 400 &&
               !(dict_start[0] == 'd' && dict_start[1] == 'i' &&
                 dict_start[2] == 'c' && dict_start[3] == 't')) {
            --dict_start; ++back;
        }
        int flate = dict_has(dict_start, s, "FlateDecode");
        if (dict_has(dict_start, s, "Type0") ||
            dict_has(dict_start, s, "Identity-H") ||
            dict_has(dict_start, s, "CIDFontType"))
            cid_font_seen = 1;

        /* 跳过 "stream" 后面的换行 */
        const char *body = s + 6;
        if (body < end && *body == '\r') ++body;
        if (body < end && *body == '\n') ++body;

        const char *ee = body;
        int found = 0;
        while (ee + 9 <= end) {
            if (strncmp(ee, "endstream", 9) == 0) { found = 1; break; }
            ++ee;
        }
        if (!found) break;
        size_t blen = (size_t)(ee - body);
        while (blen > 0 && (body[blen-1] == '\r' || body[blen-1] == '\n')) --blen;

        if (flate && blen > 0) {
            size_t cap = blen * 24 + 65536;
            if (cap > (64u << 20)) cap = (64u << 20);
            unsigned char *dec = (unsigned char *)malloc(cap);
            if (dec) {
                size_t dlen = 0;
                if (lc_inflate((const unsigned char *)body, blen, dec, cap, &dlen) == 0)
                    pdf_text_ops((const char *)dec, (const char *)dec + dlen, &out);
                free(dec);
            }
        } else {
            pdf_text_ops(body, body + blen, &out);
        }

        p = ee + 9;
        if (max_chars && out.len > max_chars) break;
    }

    free(data);

    if (out.len == 0) {
        wstr_free(&out);
        if (cid_font_seen)
            swprintf(err, errcch,
                     L"该 PDF 使用 CID 字体（Identity-H 编码），需要 ToUnicode 映射表才能还原文字，"
                     L"当前版本暂不支持完整抽取");
        else
            swprintf(err, errcch, L"未能从该 PDF 中抽取出文本（可能是扫描件或纯图片）");
        return NULL;
    }
    if (max_chars && out.len > max_chars) out.data[out.len = max_chars] = 0;
    return wstr_steal(&out);
}

/* ==================================================== HTML ============ */

static wchar_t *extract_html(const wchar_t *path, size_t max_chars,
                             wchar_t *err, size_t errcch) {
    size_t len = 0;
    unsigned char *data = read_all(path, &len);
    if (!data) { swprintf(err, errcch, L"无法读取文件"); return NULL; }

    /* 先按 UTF-8/GBK 解码成宽字符，再剥标签（比在字节上做更稳） */
    wchar_t *raw = decode_text_bytes(data, len);
    free(data);
    if (!raw) { swprintf(err, errcch, L"无法识别文件编码"); return NULL; }

    WStr out;
    wstr_init(&out);
    int in_tag = 0, in_script = 0, in_style = 0;
    for (size_t i = 0; raw[i]; ++i) {
        wchar_t c = raw[i];
        if (c == L'<') {
            in_tag = 1;
            if (lc_wcs_starts_ci(raw + i, L"<script")) in_script = 1;
            else if (lc_wcs_starts_ci(raw + i, L"<style")) in_style = 1;
            else if (lc_wcs_starts_ci(raw + i, L"</script")) in_script = 0;
            else if (lc_wcs_starts_ci(raw + i, L"</style")) in_style = 0;
            else if (lc_wcs_starts_ci(raw + i, L"</p") ||
                     lc_wcs_starts_ci(raw + i, L"<br") ||
                     lc_wcs_starts_ci(raw + i, L"</div") ||
                     lc_wcs_starts_ci(raw + i, L"</h") ||
                     lc_wcs_starts_ci(raw + i, L"</tr"))
                wstr_putc(&out, L'\n');
            continue;
        }
        if (c == L'>') { in_tag = 0; continue; }
        if (in_tag || in_script || in_style) continue;
        wstr_putc(&out, c);
        if (max_chars && out.len > max_chars) break;
    }
    free(raw);
    if (max_chars && out.len > max_chars) out.data[out.len = max_chars] = 0;

    if (out.len == 0) { wstr_free(&out); return NULL; }
    return wstr_steal(&out);
}

/* ==================================================== Markdown 去标记 ==== */

/*
 * 抽取的目的是"内容"。对 .md 源，把标记剥掉再去分词：
 *   # 标题      -> 标题
 *   **粗体**    -> 粗体
 *   H~2~O       -> H2O     （否则关键词里会冒出 "2"、"-" 这种噪声）
 *   `代码`      -> 代码
 *   [文字](链接) -> 文字
 *   ``` 围栏     -> 整段保留但去掉围栏行
 */
static void strip_markdown_w(wchar_t *s) {
    if (!s) return;
    wchar_t *r = s, *w = s;
    int at_line_start = 1;
    int in_fence = 0;

    while (*r) {
        /* 围栏行：整行丢掉 */
        if (at_line_start && r[0] == L'`' && r[1] == L'`' && r[2] == L'`') {
            in_fence = !in_fence;
            while (*r && *r != L'\n') ++r;
            continue;
        }
        if (at_line_start && !in_fence) {
            /* 行首的 # 标题标记 */
            if (*r == L'#') {
                wchar_t *q = r;
                while (*q == L'#') ++q;
                if (*q == L' ' || *q == L'\t') {
                    r = q;
                    while (*r == L' ' || *r == L'\t') ++r;
                    continue;
                }
            }
            /* 引用符号 */
            if (*r == L'>') {
                ++r;
                while (*r == L' ' || *r == L'\t') ++r;
                continue;
            }
            /* 列表符号：换成两个空格，保留缩进感 */
            if ((*r == L'-' || *r == L'*' || *r == L'+') &&
                (r[1] == L' ' || r[1] == L'\t')) {
                *w++ = L' ';
                *w++ = L' ';
                r += 2;
                continue;
            }
            /* 有序列表 "1. " */
            if (*r >= L'0' && *r <= L'9') {
                wchar_t *q = r;
                while (*q >= L'0' && *q <= L'9') ++q;
                if ((*q == L'.' || *q == L')') && (q[1] == L' ' || q[1] == L'\t')) {
                    r = q + 2;
                    continue;
                }
            }
            /* 分隔线 */
            if ((r[0] == L'-' && r[1] == L'-' && r[2] == L'-') ||
                (r[0] == L'*' && r[1] == L'*' && r[2] == L'*')) {
                while (*r && *r != L'\n') ++r;
                continue;
            }
        }

        /* 行内标记：直接丢掉 */
        if (*r == L'*' || *r == L'`' || *r == L'~' || *r == L'^') { ++r; continue; }

        /* [文字](链接) -> 文字 */
        if (*r == L'[') {
            wchar_t *close = wcschr(r + 1, L']');
            if (close && close[1] == L'(') {
                wchar_t *pend = wcschr(close + 2, L')');
                if (pend) {
                    for (wchar_t *t = r + 1; t < close; ++t) *w++ = *t;
                    r = pend + 1;
                    continue;
                }
            }
        }

        if (*r == L'\n') { at_line_start = 1; *w++ = *r++; continue; }
        at_line_start = 0;
        *w++ = *r++;
    }
    *w = 0;
}

/* ==================================================== 总入口 ========== */

wchar_t *extract_text(const wchar_t *path, size_t max_chars,
                      wchar_t *err, size_t errcch) {
    if (err && errcch) err[0] = 0;
    if (!path || !lc_is_file(path)) {
        if (err) swprintf(err, errcch, L"文件不存在");
        return NULL;
    }
    if (!max_chars) max_chars = 4u << 20;

    if (ext_is(path, L".docx") || ext_is(path, L".docm"))
        return extract_docx(path, max_chars, err, errcch);
    if (ext_is(path, L".pptx") || ext_is(path, L".pptm"))
        return extract_pptx(path, max_chars, err, errcch);
    if (ext_is(path, L".xlsx") || ext_is(path, L".xlsm"))
        return extract_xlsx(path, max_chars, err, errcch);
    if (ext_is(path, L".pdf"))
        return extract_pdf(path, max_chars, err, errcch);
    if (ext_in(path, HTML_EXTS))
        return extract_html(path, max_chars, err, errcch);

    if (ext_in(path, TEXT_EXTS)) {
        size_t len = 0;
        unsigned char *data = read_all(path, &len);
        if (!data) { if (err) swprintf(err, errcch, L"无法读取文件"); return NULL; }
        wchar_t *w = decode_text_bytes(data, len);
        free(data);
        if (!w) { if (err) swprintf(err, errcch, L"无法识别文件编码"); return NULL; }
        /* Markdown 源先剥标记：否则 "**粗体**"、"H~2~O" 会污染分词结果 */
        if (ext_is(path, L".md") || ext_is(path, L".markdown"))
            strip_markdown_w(w);
        if (max_chars && wcslen(w) > max_chars) w[max_chars] = 0;
        return w;
    }

    if (err) swprintf(err, errcch, L"暂不支持抽取该格式：%ls", lc_extname(path));
    return NULL;
}
