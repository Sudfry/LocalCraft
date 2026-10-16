/* ==========================================================================
 * export.c —— 模块 5：导出 Markdown / Word(.docx) / PDF
 *
 * 中文场景下真正要保证的三件事：
 *   1. **标题层级正确** —— docx 里必须用 Heading 1..6 样式并写 outlineLvl，
 *      否则 Word 的导航窗格与目录全部失效；PDF 里按字号/加粗体现层级。
 *   2. **中文排版不乱** —— docx 每个 run 都显式声明东亚字体（w:eastAsia），
 *      否则 Word 会拿西文字体去渲染中文，出现忽大忽小的方块字。
 *   3. **上下标与基础样式保留** —— <sub>/<sup> 映射到 w:vertAlign，
 *      粗体/斜体/行内代码/删除线都映射到对应属性。
 *
 * docx 就是一堆 XML 打成的 zip 包；我们只用 STORED 方式打包（无需压缩），
 * Word 完全可以正常打开。
 * ========================================================================== */
#include "lc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

/* ==================================================== 小工具 =========== */

static int write_bytes(const wchar_t *path, const void *data, size_t len) {
    wchar_t *lp = lc_long_path(path);
    if (!lp) return -1;
    HANDLE h = CreateFileW(lp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) return -1;
    const unsigned char *p = (const unsigned char *)data;
    size_t done = 0;
    int rc = 0;
    while (done < len) {
        DWORD chunk = 0;
        DWORD want = (DWORD)((len - done) > (1u << 20) ? (1u << 20) : (len - done));
        if (!WriteFile(h, p + done, want, &chunk, NULL) || chunk == 0) { rc = -1; break; }
        done += chunk;
    }
    CloseHandle(h);
    return rc;
}

/* XML 转义：宽字符 -> 转义后的 UTF-8，直接追加到 Str */
static void xml_escape(Str *out, const wchar_t *s) {
    char buf[8];
    for (; s && *s; ++s) {
        wchar_t c = *s;
        switch (c) {
            case L'&':  str_puts(out, "&amp;");  continue;
            case L'<':  str_puts(out, "&lt;");   continue;
            case L'>':  str_puts(out, "&gt;");   continue;
            case L'"':  str_puts(out, "&quot;"); continue;
            case L'\'': str_puts(out, "&apos;"); continue;
            default: break;
        }
        unsigned int cp = (unsigned int)c;
        if (cp < 0x80) {
            buf[0] = (char)cp; buf[1] = 0;
        } else if (cp < 0x800) {
            buf[0] = (char)(0xC0 | (cp >> 6));
            buf[1] = (char)(0x80 | (cp & 0x3F));
            buf[2] = 0;
        } else {
            buf[0] = (char)(0xE0 | (cp >> 12));
            buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[2] = (char)(0x80 | (cp & 0x3F));
            buf[3] = 0;
        }
        str_puts(out, buf);
    }
}

/* ==================================================== Markdown 解析 ==== */

typedef enum {
    MD_PARAGRAPH = 0,
    MD_HEADING,
    MD_BULLET,
    MD_NUMBER,
    MD_QUOTE,
    MD_CODE,
    MD_RULE
} MdKind;

typedef struct {
    MdKind   kind;
    int      level;       /* 标题级别 1..6；列表缩进层级 */
    wchar_t *text;        /* 原始行内容（不含前缀） */
} MdBlock;

typedef struct {
    MdBlock *blocks;
    size_t   n, cap;
    Arena   *arena;
} MdDoc;

static int mddoc_push(MdDoc *d, MdKind kind, int level, const wchar_t *text,
                      size_t len) {
    if (d->n + 1 > d->cap) {
        size_t cap = d->cap ? d->cap * 2 : 64;
        MdBlock *nb = (MdBlock *)realloc(d->blocks, cap * sizeof(MdBlock));
        if (!nb) return -1;
        d->blocks = nb;
        d->cap = cap;
    }
    d->blocks[d->n].kind = kind;
    d->blocks[d->n].level = level;
    d->blocks[d->n].text = arena_wcsndup(d->arena, text, len);
    d->n++;
    return 0;
}

/* 极简 Markdown 块级解析：够用就好，不追求完整 CommonMark */
static int md_parse(const wchar_t *md, MdDoc *doc) {
    if (!md) return -1;
    const wchar_t *p = md;
    int in_code = 0;

    while (*p) {
        const wchar_t *eol = wcschr(p, L'\n');
        size_t len = eol ? (size_t)(eol - p) : wcslen(p);
        /* 去掉行尾 \r */
        while (len > 0 && (p[len - 1] == L'\r')) len--;

        if (len >= 3 && wcsncmp(p, L"```", 3) == 0) {
            if (!in_code) { in_code = 1; }
            else { in_code = 0; }
            p = eol ? eol + 1 : p + len;
            continue;
        }
        if (in_code) {
            mddoc_push(doc, MD_CODE, 0, p, len);
            p = eol ? eol + 1 : p + len;
            continue;
        }

        /* 空行 */
        int blank = 1;
        for (size_t i = 0; i < len; ++i)
            if (p[i] != L' ' && p[i] != L'\t') { blank = 0; break; }
        if (blank) { p = eol ? eol + 1 : p + len; continue; }

        /* 分隔线 */
        if ((len >= 3 && wcsncmp(p, L"---", 3) == 0) ||
            (len >= 3 && wcsncmp(p, L"***", 3) == 0) ||
            (len >= 3 && wcsncmp(p, L"___", 3) == 0)) {
            mddoc_push(doc, MD_RULE, 0, L"", 0);
            p = eol ? eol + 1 : p + len;
            continue;
        }

        /* ATX 标题 */
        if (p[0] == L'#') {
            int lvl = 0;
            while ((size_t)lvl < len && p[lvl] == L'#') lvl++;
            if (lvl >= 1 && lvl <= 6 && lvl < (int)len && p[lvl] == L' ') {
                size_t s = (size_t)lvl + 1;
                size_t e = len;
                /* 去掉结尾的 # */
                while (e > s && (p[e-1] == L'#' || p[e-1] == L' ')) e--;
                mddoc_push(doc, MD_HEADING, lvl, p + s, e - s);
                p = eol ? eol + 1 : p + len;
                continue;
            }
        }

        /* 引用 */
        if (p[0] == L'>') {
            size_t s = 1;
            while (s < len && p[s] == L' ') s++;
            mddoc_push(doc, MD_QUOTE, 0, p + s, len - s);
            p = eol ? eol + 1 : p + len;
            continue;
        }

        /* 无序列表 */
        if (len >= 2 && (p[0] == L'-' || p[0] == L'*' || p[0] == L'+') && p[1] == L' ') {
            int indent = 0;
            size_t s = 2;
            /* 计算前导空格（两个空格算一级） */
            for (size_t i = 0; i < len && (p[i] == L' ' || p[i] == L'\t'); ++i)
                indent += (p[i] == L'\t') ? 2 : 1;
            indent /= 2;
            mddoc_push(doc, MD_BULLET, indent, p + s, len - s);
            p = eol ? eol + 1 : p + len;
            continue;
        }

        /* 有序列表 */
        if (len >= 3 && p[0] >= L'0' && p[0] <= L'9') {
            size_t i = 0;
            while (i < len && p[i] >= L'0' && p[i] <= L'9') ++i;
            if (i > 0 && i + 1 < len && (p[i] == L'.' || p[i] == L')') && p[i+1] == L' ') {
                mddoc_push(doc, MD_NUMBER, 0, p + i + 2, len - i - 2);
                p = eol ? eol + 1 : p + len;
                continue;
            }
        }

        /* 普通段落 */
        mddoc_push(doc, MD_PARAGRAPH, 0, p, len);
        p = eol ? eol + 1 : p + len;
    }
    return 0;
}

/* ==================================================== 行内解析 ========= */

typedef struct {
    wchar_t *text;
    int      bold, italic, code, strike, sub, sup;
} MdRun;

typedef struct {
    MdRun  *runs;
    size_t  n, cap;
    Arena  *arena;
} MdRuns;

static int runs_push(MdRuns *r, const wchar_t *text, size_t len,
                     int bold, int italic, int code, int strike, int sub, int sup) {
    if (len == 0) return 0;
    if (r->n + 1 > r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 16;
        MdRun *nr = (MdRun *)realloc(r->runs, cap * sizeof(MdRun));
        if (!nr) return -1;
        r->runs = nr;
        r->cap = cap;
    }
    MdRun *run = &r->runs[r->n++];
    run->text = arena_wcsndup(r->arena, text, len);
    run->bold = bold; run->italic = italic; run->code = code;
    run->strike = strike; run->sub = sub; run->sup = sup;
    return 0;
}

/* 解析 **粗体** *斜体* `代码` ~~删除~~ ~下标~ ^上标~ <sub> <sup> 以及 [文字](链接) */
static void inline_parse(MdRuns *out, const wchar_t *s) {
    size_t i = 0, n = wcslen(s);
    size_t plain_start = 0;

    #define FLUSH_PLAIN() do {                                              \
        if (i > plain_start)                                                \
            runs_push(out, s + plain_start, i - plain_start, 0,0,0,0,0,0);  \
    } while (0)

    while (i < n) {
        /* ** 或 __ ：粗体 */
        if ((s[i] == L'*' && i + 1 < n && s[i+1] == L'*') ||
            (s[i] == L'_' && i + 1 < n && s[i+1] == L'_')) {
            wchar_t marker = s[i];
            const wchar_t *close = wcsstr(s + i + 2, marker == L'*' ? L"**" : L"__");
            if (close) {
                FLUSH_PLAIN();
                runs_push(out, s + i + 2, (size_t)(close - s - i - 2), 1,0,0,0,0,0);
                i = (size_t)(close - s) + 2;
                plain_start = i;
                continue;
            }
        }
        /* ~~ 删除线 */
        if (s[i] == L'~' && i + 1 < n && s[i+1] == L'~') {
            const wchar_t *close = wcsstr(s + i + 2, L"~~");
            if (close) {
                FLUSH_PLAIN();
                runs_push(out, s + i + 2, (size_t)(close - s - i - 2), 0,0,0,1,0,0);
                i = (size_t)(close - s) + 2;
                plain_start = i;
                continue;
            }
        }
        /* ` 行内代码 */
        if (s[i] == L'`') {
            const wchar_t *close = wcschr(s + i + 1, L'`');
            if (close) {
                FLUSH_PLAIN();
                runs_push(out, s + i + 1, (size_t)(close - s - i - 1), 0,0,1,0,0,0);
                i = (size_t)(close - s) + 1;
                plain_start = i;
                continue;
            }
        }
        /* * 或 _ 斜体（避开已处理的 ** ） */
        if ((s[i] == L'*' || s[i] == L'_') &&
            !(i + 1 < n && s[i+1] == s[i])) {
            wchar_t marker = s[i];
            const wchar_t *close = wcschr(s + i + 1, marker);
            if (close && close > s + i + 1) {
                FLUSH_PLAIN();
                runs_push(out, s + i + 1, (size_t)(close - s - i - 1), 0,1,0,0,0,0);
                i = (size_t)(close - s) + 1;
                plain_start = i;
                continue;
            }
        }
        /* ^上标^ */
        if (s[i] == L'^') {
            const wchar_t *close = wcschr(s + i + 1, L'^');
            if (close && close > s + i + 1) {
                FLUSH_PLAIN();
                runs_push(out, s + i + 1, (size_t)(close - s - i - 1), 0,0,0,0,0,1);
                i = (size_t)(close - s) + 1;
                plain_start = i;
                continue;
            }
        }
        /* ~下标~ （单个波浪线，且不是 ~~） */
        if (s[i] == L'~' && !(i + 1 < n && s[i+1] == L'~') &&
            !(i > 0 && s[i-1] == L'~')) {
            const wchar_t *close = wcschr(s + i + 1, L'~');
            if (close && close > s + i + 1) {
                FLUSH_PLAIN();
                runs_push(out, s + i + 1, (size_t)(close - s - i - 1), 0,0,0,0,1,0);
                i = (size_t)(close - s) + 1;
                plain_start = i;
                continue;
            }
        }
        /* <sub> / <sup> */
        if (s[i] == L'<') {
            int is_sub = 0, is_sup = 0;
            if (lc_wcs_starts_ci(s + i, L"<sub>")) is_sub = 1;
            else if (lc_wcs_starts_ci(s + i, L"<sup>")) is_sup = 1;
            if (is_sub || is_sup) {
                size_t skip = is_sub ? 5 : 5;
                const wchar_t *close = is_sub ? wcsstr(s + i + skip, L"</sub>")
                                             : wcsstr(s + i + skip, L"</sup>");
                if (close) {
                    FLUSH_PLAIN();
                    runs_push(out, s + i + skip, (size_t)(close - s - i - skip),
                              0,0,0,0, is_sub, is_sup);
                    i = (size_t)(close - s) + 6;
                    plain_start = i;
                    continue;
                }
            }
            /* <br> 当换行 */
            if (lc_wcs_starts_ci(s + i, L"<br/>") || lc_wcs_starts_ci(s + i, L"<br>")) {
                FLUSH_PLAIN();
                runs_push(out, L"\n", 1, 0,0,0,0,0,0);
                i += lc_wcs_starts_ci(s + i, L"<br/>") ? 5 : 4;
                plain_start = i;
                continue;
            }
        }
        /* [文字](链接) -> 只保留文字 */
        if (s[i] == L'[') {
            const wchar_t *close = wcschr(s + i + 1, L']');
            if (close && close[1] == L'(') {
                const wchar_t *pend = wcschr(close + 2, L')');
                if (pend) {
                    FLUSH_PLAIN();
                    runs_push(out, s + i + 1, (size_t)(close - s - i - 1),
                              0,0,0,0,0,0);
                    i = (size_t)(pend - s) + 1;
                    plain_start = i;
                    continue;
                }
            }
        }
        ++i;
    }
    FLUSH_PLAIN();
    #undef FLUSH_PLAIN
}

/* ==================================================== Markdown 导出 ==== */

int export_markdown_file(const wchar_t *md_text, const wchar_t *out_path,
                         wchar_t *err, size_t errcch) {
    char *u8 = lc_wcs_to_utf8(md_text ? md_text : L"");
    if (!u8) { swprintf(err, errcch, L"内存不足"); return -1; }
    int rc = write_bytes(out_path, u8, strlen(u8));
    free(u8);
    if (rc != 0) { swprintf(err, errcch, L"无法写入文件：%ls", out_path); return -1; }
    return 0;
}

/* ==================================================== docx 导出 ======== */

static const char *DOCX_CONTENT_TYPES =
"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
"<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
"<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
"<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
"<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
"<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
"<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
"<Override PartName=\"/docProps/app.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/>"
"</Types>";

static const char *DOCX_RELS =
"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
"<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
"<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
"<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/>"
"<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties\" Target=\"docProps/app.xml\"/>"
"</Relationships>";

static const char *DOCX_DOC_RELS =
"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
"<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
"<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
"</Relationships>";

/*
 * 关键：每个 run 都写 w:rFonts 的 ascii/hAnsi/eastAsia 三个字体。
 * 少了 eastAsia，Word 用西文字体去渲染中文，就会出现忽大忽小的方块字。
 */
static const char *FONTS_RPR =
"<w:rFonts w:ascii=\"Segoe UI\" w:hAnsi=\"Segoe UI\" w:eastAsia=\"\xE5\xBE\xAE\xE8\xBD\xAF\xE9\x9B\x85\xE9\xBB\x91\"/>";

static void docx_run(Str *out, const MdRun *r) {
    str_puts(out, "<w:r><w:rPr>");
    str_puts(out, FONTS_RPR);
    if (r->bold)   str_puts(out, "<w:b/><w:bCs/>");
    if (r->italic) str_puts(out, "<w:i/><w:iCs/>");
    if (r->strike) str_puts(out, "<w:strike/>");
    if (r->code)   str_puts(out, "<w:rStyle w:val=\"CodeChar\"/><w:shd w:val=\"clear\" w:fill=\"F2F3F5\"/>");
    if (r->sub)    str_puts(out, "<w:vertAlign w:val=\"subscript\"/>");
    if (r->sup)    str_puts(out, "<w:vertAlign w:val=\"superscript\"/>");
    str_puts(out, "</w:rPr><w:t xml:space=\"preserve\">");
    xml_escape(out, r->text);
    str_puts(out, "</w:t></w:r>");
}

static void docx_paragraph(Str *out, MdRuns *runs, const char *style,
                           int numbered, int bullet, int level) {
    str_puts(out, "<w:p><w:pPr>");
    if (style) {
        str_printf(out, "<w:pStyle w:val=\"%s\"/>", style);
    }
    if (numbered || bullet) {
        str_puts(out, "<w:numPr>");
        str_printf(out, "<w:ilvl w:val=\"%d\"/>", level < 0 ? 0 : (level > 8 ? 8 : level));
        str_printf(out, "<w:numId w:val=\"%d\"/>", bullet ? 1 : 2);
        str_puts(out, "</w:numPr>");
    }
    str_puts(out, "</w:pPr>");
    for (size_t i = 0; i < runs->n; ++i) docx_run(out, &runs->runs[i]);
    if (runs->n == 0) str_puts(out, "<w:r><w:t></w:t></w:r>");
    str_puts(out, "</w:p>");
}

/* 标题样式必须带 outlineLvl，否则 Word 导航窗格与目录认不出层级 */
static void docx_styles(Str *out) {
    str_puts(out,
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
    "<w:docDefaults><w:rPrDefault><w:rPr>");
    str_puts(out, FONTS_RPR);
    str_puts(out, "<w:sz w:val=\"21\"/><w:szCs w:val=\"21\"/></w:rPr></w:rPrDefault>"
                  "<w:pPrDefault><w:pPr><w:spacing w:after=\"120\" w:line=\"300\" w:lineRule=\"auto\"/>"
                  "</w:pPr></w:pPrDefault></w:docDefaults>");

    str_puts(out, "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
                  "<w:name w:val=\"Normal\"/></w:style>");

    /* Heading 1..6：字号递减，全部带 outlineLvl */
    static const int sizes[6] = { 36, 30, 26, 23, 22, 21 };
    for (int i = 0; i < 6; ++i) {
        str_printf(out,
            "<w:style w:type=\"paragraph\" w:styleId=\"Heading%d\">"
            "<w:name w:val=\"heading %d\"/><w:basedOn w:val=\"Normal\"/>"
            "<w:qFormat/>"
            "<w:pPr><w:keepNext/><w:spacing w:before=\"240\" w:after=\"120\"/>"
            "<w:outlineLvl w:val=\"%d\"/></w:pPr>"
            "<w:rPr>", i + 1, i + 1, i);
        str_puts(out, FONTS_RPR);
        str_printf(out, "<w:b/><w:bCs/><w:sz w:val=\"%d\"/><w:szCs w:val=\"%d\"/>",
                   sizes[i], sizes[i]);
        if (i == 0) str_puts(out, "<w:color w:val=\"1F2328\"/>");
        str_puts(out, "</w:rPr></w:style>");
    }
    str_puts(out, "<w:style w:type=\"paragraph\" w:styleId=\"Title\">"
                  "<w:name w:val=\"Title\"/><w:basedOn w:val=\"Normal\"/>"
                  "<w:pPr><w:spacing w:before=\"0\" w:after=\"240\"/>"
                  "<w:outlineLvl w:val=\"0\"/></w:pPr>"
                  "<w:rPr>");
    str_puts(out, FONTS_RPR);
    str_puts(out, "<w:b/><w:sz w:val=\"44\"/><w:szCs w:val=\"44\"/></w:rPr></w:style>");

    str_puts(out, "<w:style w:type=\"paragraph\" w:styleId=\"Quote\">"
                  "<w:name w:val=\"Quote\"/><w:basedOn w:val=\"Normal\"/>"
                  "<w:pPr><w:ind w:left=\"480\"/><w:pBdr>"
                  "<w:left w:val=\"single\" w:sz=\"18\" w:space=\"8\" w:color=\"C9CDD4\"/>"
                  "</w:pBdr></w:pPr>"
                  "<w:rPr><w:i/></w:rPr></w:style>");

    str_puts(out, "<w:style w:type=\"character\" w:styleId=\"CodeChar\">"
                  "<w:name w:val=\"Code Char\"/>"
                  "<w:rPr>");
    str_puts(out, "<w:rFonts w:ascii=\"Consolas\" w:hAnsi=\"Consolas\" "
                  "w:eastAsia=\"\xE5\xBE\xAE\xE8\xBD\xAF\xE9\x9B\x85\xE9\xBB\x91\"/>");
    str_puts(out, "<w:sz w:val=\"19\"/></w:rPr></w:style>");

    str_puts(out, "</w:styles>");
}

int export_docx_file(const wchar_t *md_text, const wchar_t *out_path,
                     wchar_t *err, size_t errcch) {
    Arena *a = arena_new();
    if (!a) { swprintf(err, errcch, L"内存不足"); return -1; }

    MdDoc doc;
    memset(&doc, 0, sizeof(doc));
    doc.arena = a;
    if (md_parse(md_text, &doc) != 0) {
        arena_free(a);
        swprintf(err, errcch, L"Markdown 解析失败");
        return -1;
    }

    Str xml;
    str_init(&xml);
    str_puts(&xml,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:body>");

    MdRuns runs;
    memset(&runs, 0, sizeof(runs));
    runs.arena = a;

    for (size_t i = 0; i < doc.n; ++i) {
        MdBlock *b = &doc.blocks[i];
        runs.n = 0;

        if (b->kind != MD_RULE) inline_parse(&runs, b->text);

        switch (b->kind) {
        case MD_HEADING: {
            /* 始终做 1:1 映射：# -> Heading1 … ###### -> Heading6。
               不做"首个 H1 变 Title"的特例，层级关系才可预测，
               Word 的导航窗格与目录也才与源文档一致。 */
            char style[32];
            snprintf(style, sizeof(style), "Heading%d",
                     b->level < 1 ? 1 : (b->level > 6 ? 6 : b->level));
            /* 标题本身要加粗 */
            for (size_t k = 0; k < runs.n; ++k) runs.runs[k].bold = 1;
            docx_paragraph(&xml, &runs, style, 0, 0, 0);
            break;
        }
        case MD_BULLET:
            docx_paragraph(&xml, &runs, NULL, 0, 1, b->level);
            break;
        case MD_NUMBER:
            docx_paragraph(&xml, &runs, NULL, 1, 0, 0);
            break;
        case MD_QUOTE:
            docx_paragraph(&xml, &runs, "Quote", 0, 0, 0);
            break;
        case MD_CODE: {
            MdRuns cr;
            memset(&cr, 0, sizeof(cr));
            cr.arena = a;
            runs_push(&cr, b->text, wcslen(b->text), 0, 0, 1, 0, 0, 0);
            docx_paragraph(&xml, &cr, "Quote", 0, 0, 0);
            free(cr.runs);
            break;
        }
        case MD_RULE: {
            str_puts(&xml, "<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\" "
                           "w:sz=\"6\" w:space=\"1\" w:color=\"D0D5DD\"/></w:pBdr>"
                           "</w:pPr></w:p>");
            break;
        }
        case MD_PARAGRAPH:
        default:
            docx_paragraph(&xml, &runs, NULL, 0, 0, 0);
            break;
        }
    }
    free(runs.runs);
    str_puts(&xml, "<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/>"
                   "<w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" "
                   "w:left=\"1440\"/></w:sectPr></w:body></w:document>");

    /* 编号定义（无序/有序列表） */
    Str numbering;
    str_init(&numbering);
    str_puts(&numbering,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:abstractNum w:abstractNumId=\"0\">");
    for (int lv = 0; lv < 9; ++lv)
        str_printf(&numbering,
            "<w:lvl w:ilvl=\"%d\"><w:start w:val=\"1\"/><w:numFmt w:val=\"bullet\"/>"
            "<w:lvlText w:val=\"\xE2\x80\xA2\"/><w:lvlJc w:val=\"left\"/>"
            "<w:pPr><w:ind w:left=\"%d\" w:hanging=\"360\"/></w:pPr></w:lvl>",
            lv, 720 + lv * 360);
    str_puts(&numbering, "</w:abstractNum>"
        "<w:abstractNum w:abstractNumId=\"1\">");
    for (int lv = 0; lv < 9; ++lv)
        str_printf(&numbering,
            "<w:lvl w:ilvl=\"%d\"><w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/>"
            "<w:lvlText w:val=\"%%%d.\"/><w:lvlJc w:val=\"left\"/>"
            "<w:pPr><w:ind w:left=\"%d\" w:hanging=\"360\"/></w:pPr></w:lvl>",
            lv, lv + 1, 720 + lv * 360);
    str_puts(&numbering, "</w:abstractNum>"
        "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>"
        "<w:num w:numId=\"2\"><w:abstractNumId w:val=\"1\"/></w:num>"
        "</w:numbering>");

    /* docProps */
    Str core, app, styles;
    str_init(&core);
    str_init(&app);
    str_init(&styles);
    docx_styles(&styles);
    str_puts(&core,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<cp:coreProperties "
        "xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        "<dc:creator>LocalCraft</dc:creator>"
        "<cp:lastModifiedBy>LocalCraft</cp:lastModifiedBy>"
        "</cp:coreProperties>");
    str_puts(&app,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\">"
        "<Application>LocalCraft</Application></Properties>");

    /* ---- 打成 zip ---- */
    LcZipWriter *w = zip_writer_create();
    int rc = -1;
    if (w) {
        zip_writer_add(w, "[Content_Types].xml", DOCX_CONTENT_TYPES,
                       strlen(DOCX_CONTENT_TYPES));
        zip_writer_add(w, "_rels/.rels", DOCX_RELS, strlen(DOCX_RELS));
        zip_writer_add(w, "word/_rels/document.xml.rels", DOCX_DOC_RELS,
                       strlen(DOCX_DOC_RELS));
        zip_writer_add(w, "word/document.xml", xml.data ? xml.data : "",
                       xml.len);
        zip_writer_add(w, "word/styles.xml", styles.data ? styles.data : "",
                       styles.len);
        zip_writer_add(w, "word/numbering.xml", numbering.data ? numbering.data : "",
                       numbering.len);
        zip_writer_add(w, "docProps/core.xml", core.data ? core.data : "", core.len);
        zip_writer_add(w, "docProps/app.xml", app.data ? app.data : "", app.len);
        rc = zip_writer_save(w, out_path);
        zip_writer_free(w);
    }
    if (rc != 0) swprintf(err, errcch, L"无法写入 docx 文件：%ls", out_path);

    str_free(&xml);
    str_free(&numbering);
    str_free(&styles);
    str_free(&core);
    str_free(&app);
    free(doc.blocks);
    arena_free(a);
    return rc;
}

/* ==================================================== PDF 导出 ======== */
/* PDF 里没有"样式"，只有画字。中文要正确显示，必须用 CID 字体 +
   UniGB-UCS2-H 编码。这里用 PDF 标准的中文宋体（不内嵌字体文件，
   体积小、文字可选中；主流阅读器都自带对应的替代字体）。 */

static void pdf_escape_hex(Str *out, const wchar_t *s) {
    /* UniGB-UCS2-H：每个字符写成 UCS-2 大端十六进制 */
    for (; s && *s; ++s) {
        unsigned int c = (unsigned int)*s;
        if (c >= 0xD800 && c <= 0xDBFF) {
            /* 代理对 -> 补成 BMP 之外的字符，PDF 的 UCS2 CMap 不支持，退化成 '?' */
            c = L'?';
        }
        str_printf(out, "%04X", c);
    }
}

static double char_width(wchar_t c) {
    if (c < 0x80) {
        if (c == L' ') return 0.28;
        if (c >= L'A' && c <= L'Z') return 0.62;
        if (c >= L'a' && c <= L'z') return 0.52;
        if (c >= L'0' && c <= L'9') return 0.55;
        return 0.5;
    }
    /* 中日韩全角 */
    if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3000 && c <= 0x303F) ||
        (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x3040 && c <= 0x30FF))
        return 1.0;
    return 0.6;
}

typedef struct {
    Str     content;
    double  page_w, page_h, margin;
    double  x, y;
    double  font_size, leading;
    int     page_count;
    int    *page_start;   /* 每页内容的起始偏移，用于分页 */
    size_t *starts;
    size_t  n_pages, cap_pages;
} PdfCtx;

#define PDF_A4_W 595.28
#define PDF_A4_H 841.89
#define PDF_MARGIN 56.7

static void pdf_begin_page(PdfCtx *c) {
    if (c->n_pages + 1 > c->cap_pages) {
        size_t cap = c->cap_pages ? c->cap_pages * 2 : 8;
        size_t *ns = (size_t *)realloc(c->starts, cap * sizeof(size_t));
        if (!ns) return;
        c->starts = ns;
        c->cap_pages = cap;
    }
    c->starts[c->n_pages++] = c->content.len;
    c->x = c->margin;
    c->y = c->page_h - c->margin;
}

static void pdf_newline(PdfCtx *c) {
    c->x = c->margin;
    c->y -= c->leading;
    if (c->y < c->margin) pdf_begin_page(c);
}

static void pdf_text_run(PdfCtx *c, const wchar_t *text, int bold) {
    if (!text || !*text) return;
    size_t n = wcslen(text);
    if (n == 0) return;

    /* 先算总宽，判断是否需要换行 */
    double w = 0;
    for (size_t i = 0; i < n; ++i) w += char_width(text[i]) * c->font_size;
    if (c->x + w > c->page_w - c->margin && c->x > c->margin) pdf_newline(c);

    str_printf(&c->content, "BT /F1 %.2f Tf %s %.2f %.2f Td <",
               c->font_size, bold ? "2 Tr 0.35 w" : "0 Tr",
               c->x, c->y);
    pdf_escape_hex(&c->content, text);
    str_puts(&c->content, "> Tj ET\n");
    c->x += w;
}

/* 按可用宽度折行（中文可任意断行，西文尽量在空格断） */
static void pdf_wrapped(PdfCtx *c, const wchar_t *text, int bold) {
    double avail = c->page_w - c->margin * 2;
    size_t n = wcslen(text);
    size_t start = 0;
    double w = 0;
    for (size_t i = 0; i < n; ++i) {
        double cw = char_width(text[i]) * c->font_size;
        if (text[i] == L'\n') {
            wchar_t buf[4096];
            size_t l = i - start;
            if (l > 4095) l = 4095;
            memcpy(buf, text + start, l * sizeof(wchar_t));
            buf[l] = 0;
            pdf_text_run(c, buf, bold);
            pdf_newline(c);
            start = i + 1;
            w = 0;
            continue;
        }
        if (w + cw > avail) {
            /* 西文单词尽量不回退到行中；中文直接断 */
            size_t cut = i;
            if (text[i] < 0x80) {
                size_t back = i;
                while (back > start && text[back] != L' ' && i - back < 24) --back;
                if (back > start && text[back] == L' ') cut = back;
            }
            if (cut <= start) cut = i;
            wchar_t buf[4096];
            size_t l = cut - start;
            if (l > 4095) l = 4095;
            memcpy(buf, text + start, l * sizeof(wchar_t));
            buf[l] = 0;
            pdf_text_run(c, buf, bold);
            pdf_newline(c);
            start = (cut < n && text[cut] == L' ') ? cut + 1 : cut;
            i = start - 1;
            w = 0;
            continue;
        }
        w += cw;
    }
    if (start < n) {
        wchar_t buf[4096];
        size_t l = n - start;
        if (l > 4095) l = 4095;
        memcpy(buf, text + start, l * sizeof(wchar_t));
        buf[l] = 0;
        pdf_text_run(c, buf, bold);
    }
}

int export_pdf_file(const wchar_t *md_text, const wchar_t *out_path,
                    wchar_t *err, size_t errcch) {
    Arena *a = arena_new();
    if (!a) { swprintf(err, errcch, L"内存不足"); return -1; }

    MdDoc doc;
    memset(&doc, 0, sizeof(doc));
    doc.arena = a;
    if (md_parse(md_text, &doc) != 0) {
        arena_free(a);
        swprintf(err, errcch, L"Markdown 解析失败");
        return -1;
    }

    PdfCtx c;
    memset(&c, 0, sizeof(c));
    str_init(&c.content);
    c.page_w = PDF_A4_W;
    c.page_h = PDF_A4_H;
    c.margin = PDF_MARGIN;
    c.leading = 20.0;
    pdf_begin_page(&c);

    static const double HSIZE[6] = { 22.0, 18.0, 15.5, 13.5, 12.5, 12.0 };
    MdRuns runs;
    memset(&runs, 0, sizeof(runs));
    runs.arena = a;

    for (size_t i = 0; i < doc.n; ++i) {
        MdBlock *b = &doc.blocks[i];
        runs.n = 0;
        if (b->kind != MD_RULE) inline_parse(&runs, b->text);

        switch (b->kind) {
        case MD_HEADING: {
            int lv = b->level < 1 ? 1 : (b->level > 6 ? 6 : b->level);
            c.font_size = HSIZE[lv - 1];
            c.leading = c.font_size * 1.55;
            c.y -= c.font_size * 0.55;                 /* 标题前留白 */
            if (c.y < c.margin + c.leading) pdf_begin_page(&c);
            for (size_t k = 0; k < runs.n; ++k) {
                pdf_wrapped(&c, runs.runs[k].text, 1);
            }
            pdf_newline(&c);
            c.font_size = 10.5;
            c.leading = 17.5;
            break;
        }
        case MD_RULE:
            str_printf(&c.content, "0.75 w 0.78 0.80 0.84 RG %.2f %.2f m %.2f %.2f l S\n",
                       c.margin, c.y - 4, c.page_w - c.margin, c.y - 4);
            c.y -= c.leading;
            break;
        case MD_CODE:
            c.font_size = 9.5;
            c.leading = 15.0;
            c.x = c.margin;
            {
                WStr ind;
                wstr_init(&ind);
                wstr_puts(&ind, L"    ");
                wstr_puts(&ind, b->text);
                pdf_wrapped(&c, ind.data, 0);
                wstr_free(&ind);
            }
            pdf_newline(&c);
            c.font_size = 10.5;
            c.leading = 17.5;
            break;
        case MD_BULLET:
        case MD_NUMBER: {
            c.x = c.margin + 14 + (b->kind == MD_BULLET ? b->level * 14 : 0);
            if (b->kind == MD_BULLET) {
                wchar_t bullet[4] = { 0x2022, 0 };      /* • */
                str_printf(&c.content, "BT /F1 %.2f Tf 0 Tr %.2f %.2f Td <",
                           c.font_size, c.margin + 4 + b->level * 14, c.y);
                pdf_escape_hex(&c.content, bullet);
                str_puts(&c.content, "> Tj ET\n");
            } else {
                str_printf(&c.content, "BT /F1 %.2f Tf 0 Tr %.2f %.2f Td <%04X> Tj ET\n",
                           c.font_size, c.margin + 4, c.y, (unsigned)L'\x25CF');
            }
            for (size_t k = 0; k < runs.n; ++k)
                pdf_wrapped(&c, runs.runs[k].text, runs.runs[k].bold);
            pdf_newline(&c);
            break;
        }
        case MD_QUOTE: {
            c.x = c.margin + 18;
            str_printf(&c.content, "0.75 w 0.78 0.80 0.84 RG %.2f %.2f m %.2f %.2f l S\n",
                       c.margin + 6, c.y - 2, c.margin + 6, c.y - 14);
            for (size_t k = 0; k < runs.n; ++k)
                pdf_wrapped(&c, runs.runs[k].text, 0);
            pdf_newline(&c);
            break;
        }
        case MD_PARAGRAPH:
        default:
            for (size_t k = 0; k < runs.n; ++k)
                pdf_wrapped(&c, runs.runs[k].text, runs.runs[k].bold);
            pdf_newline(&c);
            c.y -= 6;   /* 段间距 */
            break;
        }
        if (c.y < c.margin + c.leading) pdf_begin_page(&c);
    }
    free(runs.runs);
    free(doc.blocks);

    /* ---- 拼 PDF ---- */
    Str pdf;
    str_init(&pdf);
    str_puts(&pdf, "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n");

    size_t n_pages = c.n_pages;
    if (n_pages == 0) n_pages = 1;

    /* 对象编号：
       1 = Catalog, 2 = Pages, 3 = Font, 4 = DescendantFont,
       5..(4+n_pages) = Page, 后面 = Contents */
    size_t obj_page0   = 5;
    size_t obj_content0 = obj_page0 + n_pages;
    size_t total_objs  = obj_content0 + n_pages - 1;

    size_t *offsets = (size_t *)calloc(total_objs + 1, sizeof(size_t));
    if (!offsets) {
        str_free(&pdf); str_free(&c.content); free(c.starts);
        arena_free(a);
        swprintf(err, errcch, L"内存不足");
        return -1;
    }

    #define BEGIN_OBJ(n) do { offsets[n] = pdf.len; str_printf(&pdf, "%llu 0 obj\n", (unsigned long long)(n)); } while (0)
    #define END_OBJ()    str_puts(&pdf, "endobj\n")

    /* 1 Catalog */
    BEGIN_OBJ(1);
    str_printf(&pdf, "<< /Type /Catalog /Pages 2 0 R >>\n");
    END_OBJ();

    /* 2 Pages */
    BEGIN_OBJ(2);
    str_puts(&pdf, "<< /Type /Pages /Kids [");
    for (size_t i = 0; i < n_pages; ++i)
        str_printf(&pdf, "%llu 0 R ", (unsigned long long)(obj_page0 + i));
    str_printf(&pdf, "] /Count %llu >>\n", (unsigned long long)n_pages);
    END_OBJ();

    /* 3 Type0 字体（中文，UniGB-UCS2-H 编码） */
    BEGIN_OBJ(3);
    str_puts(&pdf,
        "<< /Type /Font /Subtype /Type0 /BaseFont /STSong-Light "
        "/Encoding /UniGB-UCS2-H /DescendantFonts [4 0 R] >>\n");
    END_OBJ();

    /* 4 CIDFont */
    BEGIN_OBJ(4);
    str_puts(&pdf,
        "<< /Type /Font /Subtype /CIDFontType0 /BaseFont /STSong-Light "
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (GB1) /Supplement 4 >> "
        "/FontDescriptor << /Type /FontDescriptor /FontName /STSong-Light "
        "/Flags 4 /FontBBox [-25 -254 1000 880] /ItalicAngle 0 /Ascent 880 "
        "/Descent -120 /CapHeight 880 /StemV 93 >> "
        "/DW 1000 >>\n");
    END_OBJ();

    /* 页面对象 */
    for (size_t i = 0; i < n_pages; ++i) {
        BEGIN_OBJ(obj_page0 + i);
        str_printf(&pdf,
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] "
            "/Resources << /Font << /F1 3 0 R >> >> /Contents %llu 0 R >>\n",
            PDF_A4_W, PDF_A4_H, (unsigned long long)(obj_content0 + i));
        END_OBJ();
    }

    /* 内容流 */
    for (size_t i = 0; i < n_pages; ++i) {
        size_t s = c.starts[i];
        size_t e = (i + 1 < n_pages) ? c.starts[i + 1] : c.content.len;
        BEGIN_OBJ(obj_content0 + i);
        str_printf(&pdf, "<< /Length %llu >>\nstream\n",
                   (unsigned long long)(e - s));
        if (e > s) str_putn(&pdf, c.content.data + s, e - s);
        str_puts(&pdf, "endstream\n");
        END_OBJ();
    }

    /* 交叉引用表 */
    size_t xref_off = pdf.len;
    str_printf(&pdf, "xref\n0 %llu\n", (unsigned long long)(total_objs + 1));
    str_puts(&pdf, "0000000000 65535 f \n");
    for (size_t i = 1; i <= total_objs; ++i)
        str_printf(&pdf, "%010llu 00000 n \n", (unsigned long long)offsets[i]);

    str_printf(&pdf, "trailer\n<< /Size %llu /Root 1 0 R >>\nstartxref\n%llu\n%%%%EOF\n",
               (unsigned long long)(total_objs + 1), (unsigned long long)xref_off);

    #undef BEGIN_OBJ
    #undef END_OBJ

    int rc = write_bytes(out_path, pdf.data, pdf.len);
    if (rc != 0) swprintf(err, errcch, L"无法写入 PDF 文件：%ls", out_path);

    free(offsets);
    str_free(&pdf);
    str_free(&c.content);
    free(c.starts);
    arena_free(a);
    return rc;
}

/* ==================================================== 按扩展名分派 ==== */

int export_auto(const wchar_t *md_text, const wchar_t *out_path,
                wchar_t *err, size_t errcch) {
    const wchar_t *ext = lc_extname(out_path);
    if (lc_wcs_ieq(ext, L".docx")) return export_docx_file(md_text, out_path, err, errcch);
    if (lc_wcs_ieq(ext, L".pdf"))  return export_pdf_file(md_text, out_path, err, errcch);
    if (lc_wcs_ieq(ext, L".md") || lc_wcs_ieq(ext, L".markdown") ||
        lc_wcs_ieq(ext, L".txt"))
        return export_markdown_file(md_text, out_path, err, errcch);
    swprintf(err, errcch, L"不支持的导出格式：%ls（可用 .md / .docx / .pdf）", ext);
    return -1;
}
