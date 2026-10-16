/* ==========================================================================
 * table.c —— 表格批量处理（xlsx / csv 的读取、筛选、排序、合并、导出）
 *
 * 为什么要有这一层
 * ----------------
 * 现有的 xlsx 读取（extract.c）是**给摘要和去重用的**：它把整张表摊平成一段
 * 文本，能算 SimHash 就够了。但表格处理要的是**单元格结构**：
 * 第几行第几列、这一列是什么类型。所以这里另写一套读取。
 *
 * 日期是最容易踩的坑
 * ----------------
 * xlsx 里日期根本不存在 —— 它是"一个数字 + 一个格式"。单元格存的是 45000，
 * 靠单元格样式告诉 Excel"请把它显示成 2023-03-15"。
 *
 * 所以如果只读数字，用户拿到的表格里所有日期都变成 45000 这种天书。
 * 这一层必须去读 xl/styles.xml，找出哪些样式是日期格式，再做换算。
 * 不做这一步的话，功能"能用"，但用在真实的中文办公表格上基本没法看。
 *
 * 输出只管 CSV
 * ------------
 * 这个模块**只输出 CSV**，不写 xlsx。理由：
 *   · CSV 用 Excel / WPS / 记事本都能打开，通用性最好
 *   · 写 xlsx 要自己拼一套 OOXML（还牵扯共享字符串表、样式、列宽），
 *     出错的方式非常多，而收益只是"双击能直接打开"
 *   · 这个程序的价值在于**批量处理**，产物再进 Excel 精修是正常流程
 * 与其做一个半吊子的 xlsx 写出器，不如把 CSV 做扎实 —— 尤其是
 * 编码（中文 Excel 默认按 GBK 打开 CSV，写成 UTF-8 会满屏乱码，
 * 这个坑比"能不能直接打开"严重得多）。
 * ========================================================================== */
#include "lc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ==================================================== 表格模型 ========= */

void table_init(Table *t) { memset(t, 0, sizeof(*t)); }

void table_free(Table *t) {
    if (!t) return;
    for (size_t i = 0; i < t->nrows * t->ncols; ++i) free(t->cells[i]);
    free(t->cells);
    memset(t, 0, sizeof(*t));
}

static void table_ensure(Table *t, size_t rows, size_t cols) {
    size_t want_rows = rows > t->nrows ? rows : t->nrows;
    size_t want_cols = cols > t->ncols ? cols : t->ncols;
    if (want_rows == t->nrows && want_cols == t->ncols) return;

    /*
     * 按新尺寸重新分配再搬数据。数组布局始终是 nrows * ncols 紧密排列，
     * 索引只用 t->ncols，不存在第二个"容量列数"要同步的地方。
     *
     * 这里一开始写错了：先更新了 t->ncols，再用**新**列数去算"旧数据有多少格"，
     * 于是新格的起始位置算错，一部分格子没被初始化成 NULL ——
     * 后面解引用就是野指针，程序当场崩。
     *
     * 这个 bug 只在"表格宽度和高度同时变大"时显形：
     * 先长行后长列是好的，先长列后长行就会踩到。
     */
    size_t cap = want_rows * want_cols;
    wchar_t **nc = (wchar_t **)calloc(cap ? cap : 1, sizeof(wchar_t *));
    if (!nc) return;
    for (size_t r = 0; r < t->nrows; ++r)
        for (size_t c = 0; c < t->ncols; ++c)
            nc[r * want_cols + c] = t->cells[r * t->ncols + c];

    free(t->cells);
    t->cells = nc;
    t->nrows = want_rows;
    t->ncols = want_cols;
}void table_set(Table *t, size_t r, size_t c, const wchar_t *val) {
    table_ensure(t, r + 1, c + 1);
    if (r >= t->nrows || c >= t->ncols) return;
    size_t k = r * t->ncols + c;
    free(t->cells[k]);
    t->cells[k] = _wcsdup(val ? val : L"");
}

const wchar_t *table_get(const Table *t, size_t r, size_t c) {
    if (!t || r >= t->nrows || c >= t->ncols) return L"";
    const wchar_t *v = t->cells[r * t->ncols + c];
    return v ? v : L"";
}

/* ==================================================== 小工具 =========== */

/* 把 UTF-8 的一段当 XML 文本解出来（处理 &amp; 之类的实体） */
static wchar_t *xml_text(const char *s, size_t n) {
    WStr w;
    wstr_init(&w);
    for (size_t i = 0; i < n; ) {
        if (s[i] == '&') {
            if (i + 5 <= n && strncmp(s + i, "&amp;", 5) == 0)  { wstr_putc(&w, L'&');  i += 5; continue; }
            if (i + 4 <= n && strncmp(s + i, "&lt;", 4) == 0)   { wstr_putc(&w, L'<');  i += 4; continue; }
            if (i + 4 <= n && strncmp(s + i, "&gt;", 4) == 0)   { wstr_putc(&w, L'>');  i += 4; continue; }
            if (i + 6 <= n && strncmp(s + i, "&quot;", 6) == 0) { wstr_putc(&w, L'"');  i += 6; continue; }
            if (i + 6 <= n && strncmp(s + i, "&apos;", 6) == 0) { wstr_putc(&w, L'\''); i += 6; continue; }
            if (i + 4 <= n && s[i+1] == '#') {
                /* 数字实体 &#123; 或 &#x1F; */
                size_t j = i + 2;
                long cp = 0;
                int hex = 0;
                if (j < n && (s[j] == 'x' || s[j] == 'X')) { hex = 1; ++j; }
                while (j < n && s[j] != ';') {
                    char ch = s[j];
                    long d = hex ? (ch >= '0' && ch <= '9' ? ch - '0'
                                   : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                                   : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1)
                                 : (ch >= '0' && ch <= '9' ? ch - '0' : -1);
                    if (d < 0) break;
                    cp = cp * (hex ? 16 : 10) + d;
                    ++j;
                }
                if (j < n && s[j] == ';' && cp > 0 && cp <= 0x10FFFF) {
                    wstr_putc(&w, (wchar_t)cp);      /* BMP 以内直接放 */
                    i = j + 1;
                    continue;
                }
            }
        }
        /* 普通字节：按 UTF-8 走进解码器 */
        size_t j = i;
        while (j < n && s[j] != '&') ++j;
        char *tmp = (char *)malloc(j - i + 1);
        if (tmp) {
            memcpy(tmp, s + i, j - i);
            tmp[j - i] = 0;
            wchar_t *u = lc_utf8_to_wcs(tmp);
            if (u) { wstr_puts(&w, u); free(u); }
            free(tmp);
        }
        i = j;
    }
    wchar_t *r = wstr_steal(&w);
    return r ? r : _wcsdup(L"");
}

/* 列名 "AB12" → 0 基列号 */
static int col_index(const char *ref) {
    int n = 0;
    for (const char *p = ref; *p && *p != '\0'; ++p) {
        if (*p >= 'A' && *p <= 'Z')      n = n * 26 + (*p - 'A' + 1);
        else if (*p >= 'a' && *p <= 'z') n = n * 26 + (*p - 'a' + 1);
        else break;
    }
    return n > 0 ? n - 1 : 0;
}

/* 取出标签里的某个属性值（UTF-8） */
static int attr_value(const char *tag, size_t taglen, const char *name,
                      const char **out, size_t *outlen) {
    size_t nl = strlen(name);
    for (size_t i = 0; i + nl + 1 < taglen; ++i) {
        if ((i == 0 || tag[i-1] == ' ') && strncmp(tag + i, name, nl) == 0 &&
            tag[i + nl] == '=') {
            size_t j = i + nl + 1;
            char q = tag[j];
            if (q != '"' && q != '\'') continue;
            ++j;
            size_t start = j;
            while (j < taglen && tag[j] != q) ++j;
            *out = tag + start;
            *outlen = j - start;
            return 0;
        }
    }
    return -1;
}

static int is_tag(const char *p, const char *end, const char *name,
                  int *is_close, const char **tagend) {
    *is_close = 0;
    if (*p != '<') return 0;
    const char *q = p + 1;
    if (q < end && *q == '/') { *is_close = 1; ++q; }
    size_t nl = strlen(name);
    if ((size_t)(end - q) < nl) return 0;
    if (strncmp(q, name, nl) != 0) return 0;
    char after = (q + nl < end) ? q[nl] : '>';
    if (after != '>' && after != ' ' && after != '/' && after != '\t' &&
        after != '\r' && after != '\n') return 0;
    const char *te = q;
    while (te < end && *te != '>') ++te;
    if (te >= end) return 0;
    *tagend = te;
    return 1;
}

/* ==================================================== 日期样式 ========= */

/*
 * xlsx 的内置日期格式编号（ECMA-376 定的）。
 * 这些编号是"日期/时间"，不需要看格式串本身。
 */
static int is_builtin_date_fmt(unsigned id) {
    return (id >= 14 && id <= 22) || (id >= 45 && id <= 47) || id == 27 ||
           (id >= 30 && id <= 36) || (id >= 50 && id <= 58);
}

/*
 * 从 xl/styles.xml 算出"每个单元格样式是不是日期"。
 * 返回一个数组，索引是 cellXfs 的序号，1 表示这个样式是日期。
 */
static unsigned char *parse_date_styles(const char *xml, size_t len, size_t *n_out) {
    *n_out = 0;
    if (!xml || len == 0) return NULL;

    /* 第一步：自定义格式 id -> 格式串 */
    typedef struct { unsigned id; char fmt[128]; } Fmt;
    Fmt fmts[256];
    size_t nf = 0;

    const char *p = xml, *end = xml + len;
    while (p < end) {
        int cl = 0;
        const char *te = NULL;
        if (is_tag(p, end, "numFmt", &cl, &te) && !cl) {
            const char *a = NULL; size_t al = 0;
            unsigned id = 0;
            if (attr_value(p, (size_t)(te - p) + 1, "numFmtId", &a, &al) == 0) {
                char tmp[32]; size_t k = al < 31 ? al : 31;
                memcpy(tmp, a, k); tmp[k] = 0;
                id = (unsigned)strtoul(tmp, NULL, 10);
            }
            a = NULL; al = 0;
            if (attr_value(p, (size_t)(te - p) + 1, "formatCode", &a, &al) == 0 &&
                nf < 256) {
                fmts[nf].id = id;
                size_t k = al < 127 ? al : 127;
                memcpy(fmts[nf].fmt, a, k);
                fmts[nf].fmt[k] = 0;
                nf++;
            }
        }
        p = te ? te + 1 : p + 1;
    }

    /* 第二步：cellXfs 里的每个 xf 引用了哪个 numFmtId */
    unsigned char *out = NULL;
    size_t cap = 0, n = 0;

    p = xml;
    while (p < end) {
        int cl = 0;
        const char *te = NULL;
        if (is_tag(p, end, "cellXfs", &cl, &te) && !cl) {
            const char *q = te + 1;
            while (q < end) {
                int c2 = 0;
                const char *te2 = NULL;
                if (is_tag(q, end, "cellXfs", &c2, &te2) && c2) break;
                if (is_tag(q, end, "xf", &c2, &te2) && !c2) {
                    const char *a = NULL; size_t al = 0;
                    unsigned id = 0;
                    if (attr_value(q, (size_t)(te2 - q) + 1, "numFmtId", &a, &al) == 0) {
                        char tmp[32]; size_t k = al < 31 ? al : 31;
                        memcpy(tmp, a, k); tmp[k] = 0;
                        id = (unsigned)strtoul(tmp, NULL, 10);
                    }
                    int isdate = is_builtin_date_fmt(id);
                    if (!isdate) {
                        for (size_t f = 0; f < nf; ++f) {
                            if (fmts[f].id != id) continue;
                            /* 自定义格式：含 y / d / h（不含纯 m，因为 m 也可能是月份，
                               但表格里带 y 或 d 的几乎一定是日期）*/
                            for (const char *c = fmts[f].fmt; *c; ++c) {
                                char ch = *c;
                                if (ch == '[') { while (*c && *c != ']') ++c; continue; }
                                if (ch == '"') { while (*c && *c != '"') ++c; continue; }
                                if (ch == 'y' || ch == 'Y' || ch == 'd' || ch == 'D' ||
                                    ch == 'h' || ch == 'H') { isdate = 1; break; }
                            }
                            break;
                        }
                    }
                    if (n + 1 > cap) {
                        size_t capnew = cap ? cap * 2 : 32;
                        unsigned char *no = (unsigned char *)realloc(out, capnew);
                        if (!no) break;
                        out = no;
                        cap = capnew;
                    }
                    out[n++] = (unsigned char)(isdate ? 1 : 0);
                }
                q = te2 ? te2 + 1 : q + 1;
            }
            break;
        }
        p = te ? te + 1 : p + 1;
    }
    *n_out = n;
    return out;
}

static const char *BASE_DATE_STR = "1899-12-30";

/* xlsx 的天数 → 日期字符串。注意 1900 年那个著名的闰年 bug。 */
static void serial_to_date(double serial, wchar_t *out, size_t cch) {
    out[0] = 0;
    if (serial <= 0) return;

    /* Excel 把 1900 当闰年（错的），所以 1900-03-01 之前的序列号要 +1 修正。
       用 1899-12-30 作基准能绕开这个坑。 */
    long long days = (long long)serial;
    double frac = serial - (double)days;

    /*
     * 基准是 1899-12-30。但 1900 年那个"闰年"是 Excel 的著名错误：
     * 它认为 1900 是闰年，于是序列号 60 对应一个根本不存在的 1900-02-29。
     *
     * 结果就是分两段：
     *   序列号 >= 61  → 直接以 1899-12-30 为基准加天数
     *   序列号 <= 59  → 要补回那一天
     *
     * 这里一开始统一减了 1 天，导致所有日期都早一天。这种错很隐蔽：
     * 年份月份都对，只有日子差一天，不看具体数据根本发现不了。
     */
    if (days <= 59) days += 1;

    /* 从 1899-12-30 起算 */
    int y = 1899, m = 12, d = 30;
    /* 直接按天数推进，闰年规则手写 */
    long long rem = days;
    while (rem > 0) {
        static const int mdays[13] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
        int dim = mdays[m];
        if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) dim = 29;
        int left = dim - d;
        if (rem <= left) { d += (int)rem; rem = 0; }
        else { rem -= (left + 1); d = 1; m++; if (m > 12) { m = 1; y++; } }
    }

    int secs = (int)(frac * 86400.0 + 0.5);
    int hh = secs / 3600, mi = (secs % 3600) / 60;

    if (secs > 0)
        swprintf(out, cch, L"%04d-%02d-%02d %02d:%02d", y, m, d, hh, mi);
    else
        swprintf(out, cch, L"%04d-%02d-%02d", y, m, d);
    (void)BASE_DATE_STR;
}

/* ==================================================== 读 xlsx ========== */

/* 收集 sharedStrings.xml 里的字符串 */
static void read_shared(const char *xml, size_t len, Table *dummy, Vec *out, Arena *a) {
    (void)dummy;
    const char *p = xml, *end = xml + len;
    while (p < end) {
        int cl = 0;
        const char *te = NULL;
        if (is_tag(p, end, "si", &cl, &te) && !cl) {
            /* <si> 里可能有多个 <t>（富文本分段），拼起来 */
            const char *q = te + 1;
            WStr s; wstr_init(&s);
            while (q < end) {
                int c2 = 0;
                const char *te2 = NULL;
                if (is_tag(q, end, "si", &c2, &te2) && c2) break;
                if (is_tag(q, end, "t", &c2, &te2) && !c2) {
                    const char *close = te2 + 1;
                    /* 找到 </t> */
                    while (close + 3 < end &&
                           !(close[0]=='<' && close[1]=='/' && close[2]=='t' && close[3]=='>'))
                        ++close;
                    if (close + 3 < end && close > te2 + 1) {
                        wchar_t *piece = xml_text(te2 + 1, (size_t)(close - te2 - 1));
                        if (piece) { wstr_puts(&s, piece); free(piece); }
                    }
                }
                q = te2 ? te2 + 1 : q + 1;
            }
            vec_push(out, arena_wcsdup(a, s.data ? s.data : L""));
            wstr_free(&s);
            p = te + 1;
            continue;
        }
        p = te ? te + 1 : p + 1;
    }
}

int table_read_xlsx(const wchar_t *path, Table *t, wchar_t *err, size_t errcch) {
    table_init(t);
    if (err && errcch) err[0] = 0;

    LcZip *z = zip_open(path);
    if (!z) { swprintf(err, errcch, L"打不开（可能不是有效的 xlsx）"); return -1; }

    Arena *a = arena_new();

    /* 样式表：哪些样式是日期 */
    unsigned char *date_style = NULL;
    size_t n_style = 0;
    unsigned char *styles = NULL;
    size_t slen = 0;
    if (zip_read_by_name(z, "xl/styles.xml", &styles, &slen) == 0) {
        date_style = parse_date_styles((const char *)styles, slen, &n_style);
        free(styles);
    }

    /* 共享字符串表 */
    Vec shared;
    vec_init(&shared);
    unsigned char *xml = NULL;
    size_t xlen = 0;
    if (zip_read_by_name(z, "xl/sharedStrings.xml", &xml, &xlen) == 0) {
        read_shared((const char *)xml, xlen, t, &shared, a);
        free(xml);
    }

    /* 第一张 sheet */
    const char *sheet = NULL;
    size_t zn = zip_count(z);
    for (size_t i = 0; i < zn; ++i) {
        const char *nm = zip_name_at(z, i);
        if (!nm) continue;
        if (strncmp(nm, "xl/worksheets/sheet", 19) != 0) continue;
        size_t l = strlen(nm);
        if (l < 5 || _stricmp(nm + l - 4, ".xml") != 0) continue;
        sheet = nm;
        break;
    }
    if (!sheet) {
        free(date_style);
        vec_free(&shared);
        arena_free(a);
        zip_close(z);
        swprintf(err, errcch, L"这个 xlsx 里没有找到工作表");
        return -1;
    }

    xml = NULL; xlen = 0;
    if (zip_read_by_name(z, sheet, &xml, &xlen) != 0) {
        free(date_style);
        vec_free(&shared);
        arena_free(a);
        zip_close(z);
        swprintf(err, errcch, L"工作表读取失败");
        return -1;
    }

    /* 解析 sheet */
    const char *p = (const char *)xml, *end = (const char *)xml + xlen;
    size_t row = 0;
    int in_sheet_data = 0;

    while (p < end) {
        int cl = 0;
        const char *te = NULL;

        if (is_tag(p, end, "sheetData", &cl, &te)) { in_sheet_data = !cl; p = te + 1; continue; }
        if (!in_sheet_data) { p = te ? te + 1 : p + 1; continue; }

        if (is_tag(p, end, "row", &cl, &te) && !cl) {
            /* 行号可能不连续（空行被省略），按 r 属性对齐 */
            const char *a2 = NULL; size_t al = 0;
            if (attr_value(p, (size_t)(te - p) + 1, "r", &a2, &al) == 0) {
                char tmp[24]; size_t k = al < 23 ? al : 23;
                memcpy(tmp, a2, k); tmp[k] = 0;
                size_t r = (size_t)strtoul(tmp, NULL, 10);
                if (r > 0) row = r - 1;
            }
            p = te + 1;
            continue;
        }
        if (is_tag(p, end, "row", &cl, &te) && cl) { row++; p = te + 1; continue; }

        if (is_tag(p, end, "c", &cl, &te) && !cl) {
            const char *a2 = NULL; size_t al = 0;
            int col = 0;
            if (attr_value(p, (size_t)(te - p) + 1, "r", &a2, &al) == 0) {
                char tmp[24]; size_t k = al < 23 ? al : 23;
                memcpy(tmp, a2, k); tmp[k] = 0;
                col = col_index(tmp);
            }
            const char *tattr = NULL; size_t tal = 0;
            char ttype = 0;
            if (attr_value(p, (size_t)(te - p) + 1, "t", &tattr, &tal) == 0 && tal > 0)
                ttype = tattr[0];
            const char *sattr = NULL; size_t sal = 0;
            int style_idx = -1;
            if (attr_value(p, (size_t)(te - p) + 1, "s", &sattr, &sal) == 0) {
                char tmp[16]; size_t k = sal < 15 ? sal : 15;
                memcpy(tmp, sattr, k); tmp[k] = 0;
                style_idx = (int)strtol(tmp, NULL, 10);
            }

            /* 单元格内容：<v> 或 <is><t> */
            const char *q = te + 1;
            wchar_t *value = NULL;
            while (q < end) {
                int c2 = 0;
                const char *te2 = NULL;
                if (is_tag(q, end, "c", &c2, &te2) && c2) break;
                if (is_tag(q, end, "v", &c2, &te2) && !c2) {
                    const char *close = te2 + 1;
                    while (close + 3 < end &&
                           !(close[0]=='<' && close[1]=='/' && close[2]=='v' && close[3]=='>'))
                        ++close;
                    if (close > te2 + 1) value = xml_text(te2 + 1, (size_t)(close - te2 - 1));
                    q = (close + 3 < end) ? close + 4 : end;
                    break;
                }
                if (is_tag(q, end, "is", &c2, &te2) && !c2) {
                    /* 内联字符串 */
                    const char *r2 = te2 + 1;
                    while (r2 < end) {
                        int c3 = 0; const char *te3 = NULL;
                        if (is_tag(r2, end, "is", &c3, &te3) && c3) break;
                        if (is_tag(r2, end, "t", &c3, &te3) && !c3) {
                            const char *close = te3 + 1;
                            while (close + 3 < end &&
                                   !(close[0]=='<'&&close[1]=='/'&&close[2]=='t'&&close[3]=='>'))
                                ++close;
                            if (close > te3 + 1) value = xml_text(te3 + 1, (size_t)(close - te3 - 1));
                            break;
                        }
                        r2 = te3 ? te3 + 1 : r2 + 1;
                    }
                    q = te2 + 1;
                    break;
                }
                q = te2 ? te2 + 1 : q + 1;
            }

            /* 按类型换算 */
            if (ttype == 's' && value) {
                /* 共享字符串：value 里是索引 */
                long idx = wcstol(value, NULL, 10);
                if (idx >= 0 && (size_t)idx < shared.len) {
                    free(value);
                    value = _wcsdup((const wchar_t *)shared.items[idx]);
                }
            } else if (value && style_idx >= 0 &&
                       (size_t)style_idx < n_style && date_style[style_idx] &&
                       (ttype == 0 || ttype == 'n')) {
                /*
                 * 数字 + 日期样式 → 换算成日期。
                 *
                 * 必须同时接受 t 属性缺失**和** t="n" —— 两种都是合法的
                 * "这是数字"。Excel 通常省略 t，而 openpyxl 会显式写 t="n"。
                 * 只认其中一种的话，另一批文件里的日期就会原样显示成
                 * 46027 这种天书。
                 *
                 * （这个差异是拿 openpyxl 生成的真实文件测出来的，
                 *   自己拼的假数据永远发现不了。）
                 */
                wchar_t buf[64];
                serial_to_date(wcstod(value, NULL), buf, 64);
                if (buf[0]) { free(value); value = _wcsdup(buf); }
            } else if (ttype == 'b' && value) {
                free(value);
                value = _wcsdup(wcscmp(value, L"1") == 0 ? L"TRUE" : L"FALSE");
            }

            table_set(t, row, (size_t)col, value ? value : L"");
            free(value);
            p = te + 1;
            continue;
        }

        p = te ? te + 1 : p + 1;
    }

    free(date_style);
    free(xml);
    vec_free(&shared);
    arena_free(a);
    zip_close(z);

    /*
     * 裁掉尾部整行全空的。
     *
     * 行号是按 r 属性对齐的（中间的空行要保留 —— Excel 里它确实是空行），
     * 但 Excel 常在末尾留几百个空行定义，不裁掉的话产物后面会拖一大片空行，
     * 而且"合并 20 个表"时每个表都带一截，结果没法看。
     */
    while (t->nrows > 0) {
        int empty = 1;
        for (size_t c = 0; c < t->ncols && empty; ++c)
            if (table_get(t, t->nrows - 1, c)[0]) empty = 0;
        if (!empty) break;
        for (size_t c = 0; c < t->ncols; ++c) {
            free(t->cells[(t->nrows - 1) * t->ncols + c]);
            t->cells[(t->nrows - 1) * t->ncols + c] = NULL;
        }
        t->nrows--;
    }

    /* 去掉右侧整列全空的部分（Excel 常留一大堆空列定义） */
    while (t->ncols > 0) {
        int empty = 1;
        for (size_t r = 0; r < t->nrows && empty; ++r)
            if (table_get(t, r, t->ncols - 1)[0]) empty = 0;
        if (!empty) break;
        t->ncols--;
    }
    return 0;
}

/* ==================================================== 读 CSV =========== */

int table_read_csv(const wchar_t *path, Table *t, wchar_t *err, size_t errcch) {
    table_init(t);
    if (err && errcch) err[0] = 0;

    wchar_t *lp = lc_long_path(path);
    HANDLE h = CreateFileW(lp ? lp : path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) { swprintf(err, errcch, L"打不开文件"); return -1; }

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (64ll << 20)) {
        CloseHandle(h);
        swprintf(err, errcch, L"文件为空或过大");
        return -1;
    }
    size_t len = (size_t)sz.QuadPart;
    unsigned char *buf = (unsigned char *)malloc(len + 1);
    if (!buf) { CloseHandle(h); swprintf(err, errcch, L"内存不足"); return -1; }
    DWORD got = 0;
    if (!ReadFile(h, buf, (DWORD)len, &got, NULL)) got = 0;
    CloseHandle(h);
    buf[got] = 0;

    /* 编码：CSV 可能是 UTF-8(带 BOM) 或 GBK —— 交给统一的解码器 */
    wchar_t *text = decode_text_bytes(buf, got);
    free(buf);
    if (!text) { swprintf(err, errcch, L"解码失败"); return -1; }

    /* 解析：支持引号包裹和引号内的逗号/换行 */
    size_t row = 0, col = 0;
    WStr field; wstr_init(&field);
    int in_quotes = 0;

    for (const wchar_t *p = text; ; ++p) {
        wchar_t c = *p;
        int eof = (c == 0);

        if (in_quotes) {
            if (c == L'"') {
                if (*(p+1) == L'"') { wstr_putc(&field, L'"'); ++p; }
                else in_quotes = 0;
            } else if (!eof) {
                wstr_putc(&field, c);
            }
        } else {
            if (c == L'"') in_quotes = 1;
            else if (c == L',' || c == L'\t' || c == L'\n' || eof) {
                table_set(t, row, col, field.data ? field.data : L"");
                wstr_free(&field); wstr_init(&field);
                col++;
                if (c == L'\n' || eof) {
                    if (c == L'\n' && *(p+1) == 0) { row++; break; }
                    row++; col = 0;
                    if (eof) break;
                }
            } else if (c != L'\r') {
                wstr_putc(&field, c);
            }
        }
        if (eof) break;
    }
    wstr_free(&field);
    free(text);

    /* 补一行（最后一行没有换行时上面的逻辑会少算） */
    if (t->nrows == 0 && t->ncols > 0) table_ensure(t, 1, t->ncols);
    return 0;
}

/* ==================================================== 写 CSV =========== */

/*
 * 输出编码由调用方决定。
 *
 * 这是整个模块最容易出事的地方：中文版的 Excel / WPS 双击打开 CSV 时，
 * **默认按 ANSI（也就是 GBK）解码**。如果我们写成 UTF-8，用户看到的就是
 * 满屏乱码，而且很可能以为是文件坏了。
 *
 * 所以默认写成带 BOM 的 UTF-8 —— BOM 能让 Excel 认出是 UTF-8。
 * 但也提供一个明确的 GBK 选项，因为有些老工具不认 BOM。
 */
int table_write_csv(const Table *t, const wchar_t *path, int utf8_bom,
                    wchar_t *err, size_t errcch) {
    if (err && errcch) err[0] = 0;

    WStr out;
    wstr_init(&out);
    for (size_t r = 0; r < t->nrows; ++r) {
        for (size_t c = 0; c < t->ncols; ++c) {
            if (c) wstr_putc(&out, L',');
            const wchar_t *v = table_get(t, r, c);
            /* 含逗号、引号、换行的字段要用引号包起来，内部引号翻倍 */
            int need_q = (wcschr(v, L',') || wcschr(v, L'"') || wcschr(v, L'\n') ||
                          wcschr(v, L'\r'));
            if (need_q) {
                wstr_putc(&out, L'"');
                for (const wchar_t *q = v; *q; ++q) {
                    if (*q == L'"') wstr_putc(&out, L'"');
                    wstr_putc(&out, *q);
                }
                wstr_putc(&out, L'"');
            } else {
                wstr_puts(&out, v);
            }
        }
        wstr_puts(&out, L"\r\n");      /* CRLF：Excel 和记事本都认 */
    }

    /* 编码 */
    const wchar_t *body = out.data ? out.data : L"";
    unsigned char *bytes = NULL;
    size_t blen = 0;

    if (utf8_bom) {
        int n = WideCharToMultiByte(CP_UTF8, 0, body, -1, NULL, 0, NULL, NULL);
        if (n <= 0) { wstr_free(&out); swprintf(err, errcch, L"编码转换失败"); return -1; }
        size_t body_len = (size_t)n - 1;
        bytes = (unsigned char *)malloc(body_len + 4);
        if (!bytes) { wstr_free(&out); swprintf(err, errcch, L"内存不足"); return -1; }
        bytes[0] = 0xEF; bytes[1] = 0xBB; bytes[2] = 0xBF;
        WideCharToMultiByte(CP_UTF8, 0, body, -1, (char *)(bytes + 3), n, NULL, NULL);
        blen = body_len + 3;
    } else {
        int n = WideCharToMultiByte(936, 0, body, -1, NULL, 0, NULL, NULL);
        if (n <= 0) { wstr_free(&out); swprintf(err, errcch, L"编码转换失败"); return -1; }
        size_t body_len = (size_t)n - 1;
        bytes = (unsigned char *)malloc(body_len + 1);
        if (!bytes) { wstr_free(&out); swprintf(err, errcch, L"内存不足"); return -1; }
        WideCharToMultiByte(936, 0, body, -1, (char *)bytes, n, NULL, NULL);
        blen = body_len;
    }
    wstr_free(&out);

    wchar_t *lp = lc_long_path(path);
    HANDLE h = CreateFileW(lp ? lp : path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) {
        free(bytes);
        swprintf(err, errcch, L"写入失败（目标可能被占用）");
        return -1;
    }
    DWORD w = 0;
    BOOL ok = WriteFile(h, bytes, (DWORD)blen, &w, NULL);
    CloseHandle(h);
    free(bytes);
    if (!ok) { swprintf(err, errcch, L"写入失败"); return -1; }
    return 0;
}

/* ==================================================== 生成器 =========== */

/*
 * 生成器规格：tbl:<hex>
 *
 * 里面 hex 编码的是一段 key=value 文本。为什么整体编码而不是逐段拼：
 * 参数里有路径、有用户填的筛选词，这些里面什么都可能出现（冒号、逗号、
 * 竖线、换行）。逐段拼就得给每个字段定一套转义规则，太容易出错。
 * 整段编码只需要一条规则，而且路径里不可能出现的东西都不用担心。
 *
 * 参数文本的格式：
 *     op=merge|single
 *     dir=<源目录>                    merge 时用
 *     file=<相对路径>                 可以出现多次
 *     sort=<列号> / sort_numeric=0|1 / sort_desc=0|1
 *     keep=0,2,3                      只保留这些列
 *     filter=<列>,<方式>,<值>
 *     header=0|1                      第一行是不是表头
 *     enc=utf8bom|gbk
 */

static void hex_encode_str(const wchar_t *s, WStr *out) {
    for (const wchar_t *p = s; p && *p; ++p)
        wstr_printf(out, L"%04x", (unsigned)(*p & 0xFFFF));
}

static wchar_t *hex_decode_str(const wchar_t *s) {
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

int table_make_spec(const wchar_t *params, wchar_t *out, size_t cch) {
    WStr h; wstr_init(&h);
    hex_encode_str(params, &h);
    int n = _snwprintf(out, cch - 1, L"tbl:%ls", h.data ? h.data : L"");
    out[cch - 1] = 0;
    wstr_free(&h);
    return (n < 0 || (size_t)n >= cch) ? -1 : 0;
}

/* 从参数文本里取一个键的值（返回新分配的字符串） */
static wchar_t *pk(const wchar_t *params, const wchar_t *key) {
    size_t kl = wcslen(key);
    const wchar_t *p = params;
    while (p && *p) {
        const wchar_t *nl = wcschr(p, L'\n');
        size_t len = nl ? (size_t)(nl - p) : wcslen(p);
        if (len > kl && wcsncmp(p, key, kl) == 0 && p[kl] == L'=') {
            wchar_t *v = (wchar_t *)malloc((len - kl) * sizeof(wchar_t));
            if (!v) return NULL;
            memcpy(v, p + kl + 1, (len - kl - 1) * sizeof(wchar_t));
            v[len - kl - 1] = 0;
            return v;
        }
        p = nl ? nl + 1 : NULL;
    }
    return NULL;
}

/* 收集所有 file= 的值（可能有多行） */
static void pk_all(const wchar_t *params, const wchar_t *key, Vec *out) {
    size_t kl = wcslen(key);
    const wchar_t *p = params;
    while (p && *p) {
        const wchar_t *nl = wcschr(p, L'\n');
        size_t len = nl ? (size_t)(nl - p) : wcslen(p);
        if (len > kl && wcsncmp(p, key, kl) == 0 && p[kl] == L'=') {
            wchar_t *v = (wchar_t *)malloc((len - kl) * sizeof(wchar_t));
            if (v) {
                memcpy(v, p + kl + 1, (len - kl - 1) * sizeof(wchar_t));
                v[len - kl - 1] = 0;
                vec_push(out, v);
            }
        }
        p = nl ? nl + 1 : NULL;
    }
}

/* 读一个文件成 Table，按扩展名选读取器 */
int load_table_pub(const wchar_t *path, Table *t, wchar_t *err, size_t errcch) {
    const wchar_t *e = lc_extname(path);
    if (lc_wcs_ieq(e, L".csv") || lc_wcs_ieq(e, L".tsv") || lc_wcs_ieq(e, L".txt"))
        return table_read_csv(path, t, err, errcch);
    return table_read_xlsx(path, t, err, errcch);
}

int table_generate(const wchar_t *spec, const wchar_t *src, const wchar_t *dst,
                   wchar_t *err, size_t errcch) {
    if (errcch) err[0] = 0;
    if (!spec || wcsncmp(spec, L"tbl:", 4) != 0) {
        swprintf(err, errcch, L"生成规格格式不对");
        return -1;
    }

    wchar_t *params = hex_decode_str(spec + 4);
    if (!params) { swprintf(err, errcch, L"规格解码失败"); return -1; }

    wchar_t *op      = pk(params, L"op");
    wchar_t *dir     = pk(params, L"dir");
    wchar_t *enc     = pk(params, L"enc");
    wchar_t *header  = pk(params, L"header");
    wchar_t *sortc   = pk(params, L"sort");
    wchar_t *sortnum = pk(params, L"sort_numeric");
    wchar_t *sortdes = pk(params, L"sort_desc");
    wchar_t *keep    = pk(params, L"keep");
    wchar_t *filt    = pk(params, L"filter");

    Vec files;
    vec_init(&files);
    pk_all(params, L"file", &files);

    int rc = -1;
    Table merged;
    table_init(&merged);
    int have = 0;

    const wchar_t *base_dir = (dir && dir[0]) ? dir : NULL;

    if (files.len > 0 && base_dir) {
        for (size_t i = 0; i < files.len; ++i) {
            wchar_t full[MAX_PATH * 2];
            swprintf(full, MAX_PATH * 2, L"%ls\\%ls", base_dir, (wchar_t *)files.items[i]);

            Table one;
            wchar_t e2[256]; e2[0] = 0;
            if (load_table_pub(full, &one, e2, 256) != 0) {
                /* 单个文件读不了就跳过 —— 合并场景里不该因为一个坏文件全盘失败 */
                continue;
            }
            if (!have) {
                merged = one;
                have = 1;
            } else {
                table_append(&merged, &one, 1, 1);
                table_free(&one);
            }
        }
    } else if (src && src[0]) {
        /* 单文件模式：src 就是输入 */
        wchar_t e2[256]; e2[0] = 0;
        if (load_table_pub(src, &merged, e2, 256) == 0) have = 1;
        else swprintf(err, errcch, L"%ls", e2[0] ? e2 : L"读取失败");
    }

    if (!have) {
        if (!err[0]) swprintf(err, errcch, L"没有读到任何可处理的数据");
        goto done;
    }

    int has_header = (header && wcscmp(header, L"0") == 0) ? 0 : 1;

    /* 筛选 */
    if (filt && filt[0]) {
        /* 形如 "列,方式,值" —— 值里可能也有逗号，所以只切前两个 */
        wchar_t *c1 = wcschr(filt, L',');
        if (c1) {
            *c1 = 0;
            wchar_t *c2 = wcschr(c1 + 1, L',');
            wchar_t *mode = c1 + 1;
            wchar_t *val2 = NULL;
            if (c2) { *c2 = 0; val2 = c2 + 1; }
            int col = (int)wcstol(filt, NULL, 10);
            table_filter(&merged, col, mode, val2, has_header);
        }
    }

    /* 排序 */
    if (sortc && sortc[0]) {
        int col = (int)wcstol(sortc, NULL, 10);
        int num = (sortnum && wcscmp(sortnum, L"1") == 0);
        int des = (sortdes && wcscmp(sortdes, L"1") == 0);
        table_sort(&merged, col, num, des, has_header);
    }

    /* 选列 */
    if (keep && keep[0]) {
        int cols[256]; size_t nc = 0;
        const wchar_t *p = keep;
        while (*p && nc < 256) {
            while (*p == L' ') ++p;
            if (!*p) break;
            cols[nc++] = (int)wcstol(p, (wchar_t **)&p, 10);
            while (*p && *p != L',') ++p;
            if (*p == L',') ++p;
        }
        if (nc > 0) table_select_cols(&merged, cols, nc);
    }

    /* 输出 CSV */
    int bom = !(enc && wcscmp(enc, L"gbk") == 0);
    wchar_t e3[256]; e3[0] = 0;
    if (table_write_csv(&merged, dst, bom, e3, 256) != 0) {
        swprintf(err, errcch, L"%ls", e3[0] ? e3 : L"写出失败");
        goto done;
    }
    rc = 0;

done:
    table_free(&merged);
    for (size_t i = 0; i < files.len; ++i) free(files.items[i]);
    vec_free(&files);
    free(op); free(dir); free(enc); free(header); free(sortc);
    free(sortnum); free(sortdes); free(keep); free(filt);
    free(params);
    return rc;
}


/* 取第 c 列的值当数字；不是数字返回 0 并把 ok 置 0 */
static double cell_num(const Table *t, size_t r, size_t c, int *ok) {
    const wchar_t *v = table_get(t, r, c);
    if (!v[0]) { *ok = 0; return 0; }
    wchar_t *endp = NULL;
    double d = wcstod(v, &endp);
    *ok = (endp && *endp == 0);
    return d;
}

static const Table *g_sort_table;
static int g_sort_col, g_sort_numeric, g_sort_desc;

static int cmp_rows(const void *a, const void *b) {
    size_t ra = *(const size_t *)a, rb = *(const size_t *)b;
    int ok1 = 0, ok2 = 0;
    int c = 0;

    if (g_sort_numeric) {
        double da = cell_num(g_sort_table, ra, (size_t)g_sort_col, &ok1);
        double db = cell_num(g_sort_table, rb, (size_t)g_sort_col, &ok2);
        if (ok1 && ok2) c = (da < db) ? -1 : (da > db) ? 1 : 0;
        else if (ok1) c = -1;        /* 数字排在文本前面 */
        else if (ok2) c = 1;
        else c = 0;
    }
    if (c == 0) {
        /* 非数字、或数值相等时按文本比 */
        const wchar_t *sa = table_get(g_sort_table, ra, (size_t)g_sort_col);
        const wchar_t *sb = table_get(g_sort_table, rb, (size_t)g_sort_col);
        c = wcscmp(sa, sb);
    }
    return g_sort_desc ? -c : c;
}

/* 排序（跳过表头行） */
int table_sort(Table *t, int col, int numeric, int desc, int has_header) {
    if (!t || col < 0 || (size_t)col >= t->ncols) return -1;
    size_t start = has_header ? 1 : 0;
    if (t->nrows <= start + 1) return 0;

    size_t n = t->nrows - start;
    size_t *idx = (size_t *)malloc(n * sizeof(size_t));
    if (!idx) return -1;
    for (size_t i = 0; i < n; ++i) idx[i] = start + i;

    g_sort_table = t; g_sort_col = col;
    g_sort_numeric = numeric; g_sort_desc = desc;
    qsort(idx, n, sizeof(size_t), cmp_rows);

    /* 重排：先把行指针整体搬走，再按新顺序写回 */
    wchar_t **rows = (wchar_t **)malloc(n * t->ncols * sizeof(wchar_t *));
    if (!rows) { free(idx); return -1; }
    for (size_t i = 0; i < n; ++i)
        for (size_t c = 0; c < t->ncols; ++c)
            rows[i * t->ncols + c] = t->cells[idx[i] * t->ncols + c];
    for (size_t i = 0; i < n; ++i)
        for (size_t c = 0; c < t->ncols; ++c)
            t->cells[(start + i) * t->ncols + c] = rows[i * t->ncols + c];

    free(rows);
    free(idx);
    g_sort_table = NULL;
    return 0;
}

/* 只保留指定列（cols 里是按顺序的列号） */
int table_select_cols(Table *t, const int *cols, size_t ncols) {
    if (!t || !cols || ncols == 0) return -1;
    for (size_t i = 0; i < ncols; ++i)
        if (cols[i] < 0 || (size_t)cols[i] >= t->ncols) return -1;

    size_t rows = t->nrows;
    size_t total = (rows > 0 && ncols > 0) ? rows * ncols : 1;
    wchar_t **nc = (wchar_t **)calloc(total, sizeof(wchar_t *));
    if (!nc) return -1;

    for (size_t r = 0; r < rows; ++r)
        for (size_t c = 0; c < ncols; ++c) {
            const wchar_t *v = table_get(t, r, (size_t)cols[c]);
            nc[r * ncols + c] = _wcsdup(v ? v : L"");
        }

    for (size_t i = 0; i < t->nrows * t->ncols; ++i) free(t->cells[i]);
    free(t->cells);
    t->cells = nc;
    t->ncols = ncols;
    t->nrows = rows;
    return 0;
}

/* 按列条件筛选：保留 col 列满足 op 的行 */
int table_filter(Table *t, int col, const wchar_t *op, const wchar_t *val2,
                 int has_header) {
    if (!t || col < 0 || (size_t)col >= t->ncols) return -1;
    if (!op) op = L"contains";

    size_t keep = 0;
    size_t start = has_header ? 1 : 0;

    for (size_t r = 0; r < t->nrows; ++r) {
        int ok = 1;
        if (r >= start) {
            const wchar_t *v = table_get(t, r, (size_t)col);
            const wchar_t *needle = val2 ? val2 : L"";

            if (wcscmp(op, L"empty") == 0) {
                ok = (v[0] == 0);
            } else if (wcscmp(op, L"notempty") == 0) {
                ok = (v[0] != 0);
            } else if (wcscmp(op, L"eq") == 0) {
                ok = (wcscmp(v, needle) == 0);
            } else if (wcscmp(op, L"ne") == 0) {
                ok = (wcscmp(v, needle) != 0);
            } else if (wcscmp(op, L"contains") == 0) {
                ok = needle[0] && wcsstr(v, needle) != NULL;
            } else if (wcscmp(op, L"notcontains") == 0) {
                ok = !needle[0] || wcsstr(v, needle) == NULL;
            } else if (wcscmp(op, L"gt") == 0 || wcscmp(op, L"lt") == 0 ||
                       wcscmp(op, L"ge") == 0 || wcscmp(op, L"le") == 0) {
                int o1 = 0, o2 = 0;
                double a = cell_num(t, r, (size_t)col, &o1);
                double b = 0;
                wchar_t *endp = NULL;
                b = needle ? wcstod(needle, &endp) : 0;
                o2 = (endp && *endp == 0);
                if (!o1 || !o2) ok = 0;
                else if (wcscmp(op, L"gt") == 0) ok = (a > b);
                else if (wcscmp(op, L"lt") == 0) ok = (a < b);
                else if (wcscmp(op, L"ge") == 0) ok = (a >= b);
                else                              ok = (a <= b);
            }
        }
        if (ok) {
            if (keep != r) {
                for (size_t c = 0; c < t->ncols; ++c) {
                    free(t->cells[keep * t->ncols + c]);          /* 目标位置原来那份丢弃 */
                    t->cells[keep * t->ncols + c] = t->cells[r * t->ncols + c];
                    /*
                     * ★ 必须把源位置置空。
                     *
                     * 不置空的话，同一个指针会在数组里存在两份（位置 keep 和 r）。
                     * 循环末尾清理尾部时会再 free 一次 r 位置的内容 ——
                     * 于是位置 keep 上留下悬空指针，写 CSV 时读到的就是已经被
                     * 回收的内存。表现出来是"筛选后的行莫名重复"，
                     * 而且内存正好被复用成同样的内容时看着还"挺正常"。
                     */
                    t->cells[r * t->ncols + c] = NULL;
                }
            }
            keep++;
        } else {
            for (size_t c = 0; c < t->ncols; ++c) {
                free(t->cells[r * t->ncols + c]);
                t->cells[r * t->ncols + c] = NULL;
            }
        }
    }

    /* 尾部腾空 */
    for (size_t r = keep; r < t->nrows; ++r)
        for (size_t c = 0; c < t->ncols; ++c) {
            free(t->cells[r * t->ncols + c]);
            t->cells[r * t->ncols + c] = NULL;
        }
    t->nrows = keep;
    return 0;
}

/*
 * 把 src 追加到 dst 后面（用于合并多个工作簿）。
 * skip_header 表示 src 的第一行是表头，不追加。
 */
int table_append(Table *dst, const Table *src, int skip_header, int align_header) {
    if (!dst || !src) return -1;
    size_t cols = dst->ncols > src->ncols ? dst->ncols : src->ncols;
    if (cols == 0) return 0;

    size_t start_r = 0;
    if (skip_header && src->nrows > 0) start_r = 1;

    /* 如果目标还是空的，把 src 的表头也带过来 */
    if (dst->nrows == 0 && src->nrows > 0 && skip_header) {
        for (size_t c = 0; c < src->ncols; ++c)
            table_set(dst, 0, c, table_get(src, 0, c));
    }
    (void)align_header;

    size_t base = dst->nrows;
    for (size_t r = start_r; r < src->nrows; ++r) {
        for (size_t c = 0; c < src->ncols; ++c) {
            table_set(dst, base + (r - start_r), c, table_get(src, r, c));
        }
    }
    if (cols > dst->ncols) table_ensure(dst, dst->nrows, cols);
    return 0;
}
