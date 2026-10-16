/* ==========================================================================
 * meta.c —— 图片 EXIF 与音频 ID3 元数据读取
 *
 * 为什么不用现成库
 * ----------------
 * 项目从第一天就定下"不引入任何第三方库" —— 目标机不需要装任何东西，
 * 单文件 exe 直接跑。EXIF 和 ID3 这两个格式虽然啰嗦，但只用得到一小部分字段，
 * 自己解反而可控：读到哪算哪，读不出来就当没有，绝不会因为一个畸形文件崩掉。
 *
 * 两个格式的坑，按踩到的顺序记下来
 * --------------------------------
 * EXIF：
 *   · 字节序由 TIFF 头的前两字节决定（II = 小端，MM = 大端），不是固定小端
 *   · 值有三种存法：内联在条目里（≤4 字节）、指向文件另一处的偏移、
 *     以及 RATIONAL（分子分母各 4 字节）。不区分就会读出天书
 *   · 偏移都是相对 TIFF 头算的，而 TIFF 头在 JPEG 里是从 APP1 的第 7 字节开始，
 *     直接拿偏移去索引整个文件必然错位
 *   · 所有偏移都要做边界检查 —— 这个文件是用户给的，可能是坏的、截断的、
 *     甚至故意构造的。不做边界检查就是崩溃或者读到别的内存
 *
 * ID3：
 *   · v2 的长度是"同步安全整数"（每字节只用低 7 位），不是普通整数
 *   · 文本帧第一个字节是编码标识（0=Latin1 1=UTF16+BOM 2=UTF16BE 3=UTF8）
 *   · v2.3 和 v2.4 的帧头长度也不一样（4 字节 vs 同步安全 4 字节）
 *   · 还有更老的 ID3v1：文件末尾 128 字节，没有长度字段，靠固定偏移
 * ========================================================================== */
#include "lc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ==================================================== 读出结构 ======== */

/* 一次最多读文件头尾这么多字节。EXIF/ID3 的信息都在头里，
   尾部只为了 ID3v1 那 128 字节，不需要把整个大文件读进内存。 */
#define META_HEAD_MAX (1024 * 1024)
#define META_TAIL_MAX 256

typedef struct {
    unsigned char *head;   /* 文件开头 */
    size_t         head_len;
    unsigned char *tail;   /* 文件末尾 */
    size_t         tail_len;
    long long      file_size;
} RawFile;

/* ==================================================== 小工具 ========== */

/* 把 Latin-1 字节串转成宽字符（EXIF 的 ASCII 字段其实常常塞着 Latin-1） */
static void copy_latin1(wchar_t *dst, size_t cch, const unsigned char *src, size_t n) {
    if (!dst || cch == 0) return;
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < cch; ++i) {
        unsigned char b = src[i];
        if (b == 0) break;                 /* 字段里以 NUL 结尾 */
        dst[k++] = (wchar_t)b;             /* Latin-1 → Unicode 是直接映射 */
    }
    dst[k] = 0;
    /* 去掉首尾空白，很多相机型号后面跟着一串空格 */
    size_t L = k;
    while (L > 0 && (dst[L-1] == L' ' || dst[L-1] == L'\t')) dst[--L] = 0;
    wchar_t *s = dst;
    while (*s == L' ') ++s;
    if (s != dst) memmove(dst, s, (wcslen(s) + 1) * sizeof(wchar_t));
}

static void copy_utf16(wchar_t *dst, size_t cch, const unsigned char *src, size_t n,
                       int big_endian_bom) {
    if (!dst || cch == 0) return;
    size_t k = 0;
    int be = big_endian_bom;
    size_t i = 0;
    /* 有 BOM 就按 BOM 走，没有就用调用方给的默认字节序 */
    if (n >= 2 && src[0] == 0xFF && src[1] == 0xFE) { be = 0; i = 2; }
    else if (n >= 2 && src[0] == 0xFE && src[1] == 0xFF) { be = 1; i = 2; }

    for (; i + 1 < n && k + 1 < cch; i += 2) {
        wchar_t c = be ? (wchar_t)((src[i] << 8) | src[i+1])
                       : (wchar_t)(src[i] | (src[i+1] << 8));
        if (c == 0) break;
        dst[k++] = c;
    }
    dst[k] = 0;
    size_t L = k;
    while (L > 0 && (dst[L-1] == L' ' || dst[L-1] == L'\t')) dst[--L] = 0;
    wchar_t *s = dst;
    while (*s == L' ') ++s;
    if (s != dst) memmove(dst, s, (wcslen(s) + 1) * sizeof(wchar_t));
}

/* 字节串按 UTF-8 试一次，成功就按 UTF-8 解，否则按 Latin-1 */
static void copy_maybe_utf8(wchar_t *dst, size_t cch, const unsigned char *src, size_t n) {
    if (!dst || cch == 0) return;
    if (n == 0) { dst[0] = 0; return; }

    /* 严格校验 UTF-8 */
    int ok = 1;
    for (size_t i = 0; i < n && ok; ) {
        unsigned char b = src[i];
        int extra;
        if      (b < 0x80)           extra = 0;
        else if ((b & 0xE0) == 0xC0) extra = 1;
        else if ((b & 0xF0) == 0xE0) extra = 2;
        else if ((b & 0xF8) == 0xF0) extra = 3;
        else { ok = 0; break; }
        if (i + (size_t)extra >= n) { ok = 0; break; }
        for (int k = 1; k <= extra; ++k)
            if ((src[i+k] & 0xC0) != 0x80) { ok = 0; break; }
        i += (size_t)extra + 1;
    }
    if (ok) {
        /* 借用整份解码：临时补个结尾 NUL */
        unsigned char *tmp = (unsigned char *)malloc(n + 1);
        if (tmp) {
            memcpy(tmp, src, n);
            tmp[n] = 0;
            wchar_t *w = decode_text_bytes(tmp, n);
            free(tmp);
            if (w) {
                wcsncpy(dst, w, cch - 1);
                dst[cch-1] = 0;
                free(w);
                return;
            }
        }
    }
    copy_latin1(dst, cch, src, n);
}

/* ==================================================== 读文件头尾 ===== */

static int read_head_tail(const wchar_t *path, RawFile *rf) {
    memset(rf, 0, sizeof(*rf));
    wchar_t *lp = lc_long_path(path);
    HANDLE h = CreateFileW(lp ? lp : path, GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) return -1;

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) { CloseHandle(h); return -1; }
    rf->file_size = sz.QuadPart;

    size_t want = (size_t)((rf->file_size < META_HEAD_MAX) ? rf->file_size : META_HEAD_MAX);
    rf->head = (unsigned char *)malloc(want + 1);
    if (!rf->head) { CloseHandle(h); return -1; }
    DWORD got = 0;
    if (!ReadFile(h, rf->head, (DWORD)want, &got, NULL)) got = 0;
    rf->head_len = got;
    rf->head[got] = 0;

    /* 文件小的话头里已经包含尾部了，不用再读 */
    if (rf->file_size > (long long)want) {
        size_t tn = (size_t)((rf->file_size < META_TAIL_MAX) ? rf->file_size : META_TAIL_MAX);
        rf->tail = (unsigned char *)malloc(tn + 1);
        if (rf->tail) {
            LARGE_INTEGER off;
            off.QuadPart = rf->file_size - (long long)tn;
            if (SetFilePointerEx(h, off, NULL, FILE_BEGIN)) {
                DWORD tg = 0;
                ReadFile(h, rf->tail, (DWORD)tn, &tg, NULL);
                rf->tail_len = tg;
                rf->tail[tg] = 0;
            }
        }
    } else {
        /*
         * 整个文件都已经在 head 里了 —— 那"文件的最后 128 字节"就是 head 的末尾。
         *
         * 这里一开始漏了，后果是：小文件里的 ID3v1 标签（它固定就在文件末尾 128 字节）
         * 永远读不到。而且这个 bug **只在小文件上出现**，大文件反而正常 ——
         * 属于最容易漏过去的那一类，测试里专门留了一条。
         */
        rf->tail = rf->head;
        rf->tail_len = rf->head_len;
    }
    CloseHandle(h);
    return 0;
}

static void raw_free(RawFile *rf) {
    /* tail 可能只是 head 的一个视图，不能重复 free */
    if (rf->tail && rf->tail != rf->head) free(rf->tail);
    free(rf->head);
    rf->head = rf->tail = NULL;
}

/* ==================================================== EXIF ============ */

typedef struct {
    int      be;                 /* 1 = 大端 */
    size_t   tiff_off;           /* TIFF 头在 head 里的偏移 */
    size_t   limit;              /* 可索引范围（不许越过当前 APP1 段） */
    const unsigned char *head_ptr;   /* 文件头缓冲区的起点 */
} TiffCtx;

static unsigned rd16(const unsigned char *p, int be) {
    return be ? ((unsigned)p[0] << 8) | p[1]
              : ((unsigned)p[1] << 8) | p[0];
}
static unsigned rd32(const unsigned char *p, int be) {
    return be ? ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
                ((unsigned)p[2] << 8) | p[3]
              : ((unsigned)p[3] << 24) | ((unsigned)p[2] << 16) |
                ((unsigned)p[1] << 8) | p[0];
}

/* 每种类型的字节数。0 表示不认识。 */
static size_t type_size(unsigned t) {
    switch (t) {
        case 1: case 2: case 6: case 7: return 1;   /* BYTE ASCII SBYTE UNDEFINED */
        case 3: case 8:                             return 2;   /* SHORT SSHORT */
        case 4: case 9: case 11:                    return 4;   /* LONG SLONG FLOAT */
        case 5: case 10: case 12:                   return 8;   /* RATIONAL SRATIONAL DOUBLE */
        default: return 0;
    }
}

/*
 * 取一个 IFD 条目的原始字节位置和长度。
 * 长度 ≤4 时值内联在条目里，否则条目里存的是偏移。
 * 这个判断错了后面全乱，所以单独拎出来。
 */
static const unsigned char *entry_data(const TiffCtx *tc, const unsigned char *e,
                                       size_t *out_len) {
    unsigned type  = rd16(e + 2, tc->be);
    unsigned count = rd32(e + 4, tc->be);
    size_t   ts    = type_size(type);
    if (!ts) { *out_len = 0; return NULL; }

    /* 防溢出：先判断会不会超过 size_t */
    if (count > (64u * 1024u * 1024u)) { *out_len = 0; return NULL; }
    size_t total = ts * (size_t)count;
    *out_len = total;

    if (total <= 4) return e + 8;                 /* 内联 */

    unsigned off = rd32(e + 8, tc->be);
    size_t abs = tc->tiff_off + (size_t)off;
    if (abs + total > tc->limit) { *out_len = 0; return NULL; }   /* 越界，拒绝 */
    return tc->head_ptr + abs;
}

#define TIFF_TAG_MAKE        0x010F
#define TIFF_TAG_MODEL       0x0110
#define TIFF_TAG_ORIENTATION 0x0112
#define TIFF_TAG_EXIF_IFD    0x8769
#define TIFF_TAG_DATETIME    0x0132
#define TIFF_TAG_EXIF_W      0xA002
#define TIFF_TAG_EXIF_H      0xA003
#define TIFF_TAG_DT_ORIG     0x9003
#define TIFF_TAG_DT_DIGIT    0x9004
#define TIFF_TAG_EXPOSURE    0x829A
#define TIFF_TAG_FNUMBER     0x829D
#define TIFF_TAG_ISO         0x8827
#define TIFF_TAG_FOCAL       0x920A
#define TIFF_TAG_LENS        0xA434
#define TIFF_TAG_ISO_ALT     0x8833

static void store_ascii(wchar_t *dst, size_t cch, const unsigned char *e,
                        const TiffCtx *tc) {
    size_t n = 0;
    const unsigned char *d = entry_data(tc, e, &n);
    if (!d || n == 0) { dst[0] = 0; return; }
    copy_maybe_utf8(dst, cch, d, n);
}

static unsigned store_short_val(const unsigned char *e, const TiffCtx *tc, int *ok) {
    *ok = 0;
    size_t n = 0;
    const unsigned char *d = entry_data(tc, e, &n);
    if (!d || n < 2) return 0;
    *ok = 1;
    return rd16(d, tc->be);
}

/* 有符号/无符号 32 位整数 */
static long store_long_val(const unsigned char *e, const TiffCtx *tc, int *ok) {
    *ok = 0;
    size_t n = 0;
    const unsigned char *d = entry_data(tc, e, &n);
    if (!d || n < 4) return 0;
    *ok = 1;
    return (long)rd32(d, tc->be);
}

/* RATIONAL → "分子/分母" 约分后的字符串；fmt 决定怎么呈现 */
static void store_rational(wchar_t *dst, size_t cch, const unsigned char *e,
                           const TiffCtx *tc, int mode) {
    dst[0] = 0;
    size_t n = 0;
    const unsigned char *d = entry_data(tc, e, &n);
    if (!d || n < 8) return;
    unsigned num = rd32(d, tc->be);
    unsigned den = rd32(d + 4, tc->be);
    if (den == 0) return;

    if (mode == 0) {
        /* 曝光时间：小于 1 秒写成 1/xxx 更符合摄影习惯 */
        if (num < den) {
            unsigned r = (num == 0) ? 0 : (unsigned)((den + num/2) / num);
            if (r > 0) swprintf(dst, cch, L"1/%u", r);
        } else {
            double v = (double)num / (double)den;
            swprintf(dst, cch, L"%.1f", v);
        }
    } else if (mode == 1) {
        /* 光圈 F 值 */
        double v = (double)num / (double)den;
        swprintf(dst, cch, L"%.1f", v);
    } else {
        /* 焦距：取整更常见 */
        double v = (double)num / (double)den;
        if (v == (double)(long)(v + 0.5)) swprintf(dst, cch, L"%ld", (long)(v + 0.5));
        else                              swprintf(dst, cch, L"%.1f", v);
    }
}

/*
 * 扫一个 IFD，对每个条目调回调。
 * 回调返回 1 表示"已经够用了，可以停"（这里不用，但接口留着清晰）。
 */
typedef void (*IfdVisitor)(void *ud, unsigned tag, const unsigned char *entry);

static void walk_ifd(const TiffCtx *tc, size_t ifd_off, IfdVisitor visit, void *ud) {
    size_t abs = tc->tiff_off + ifd_off;
    if (abs + 2 > tc->limit) return;
    unsigned n = rd16(tc->head_ptr + abs, tc->be);
    /* 一个 IFD 条目 12 字节。数量离谱的直接放弃 —— 畸形文件常见手法。 */
    if (n > 4096) return;
    for (unsigned i = 0; i < n; ++i) {
        size_t eoff = abs + 2 + (size_t)i * 12;
        if (eoff + 12 > tc->limit) return;
        const unsigned char *e = tc->head_ptr + eoff;
        visit(ud, rd16(e, tc->be), e);
    }
}

typedef struct {
    MediaMeta     *m;
    const TiffCtx *tc;
    int            pass;        /* 0 = IFD0，1 = ExifIFD */
    unsigned       exif_ifd_off;/* 记下 ExifIFD 的位置，第一遍扫完再去扫它 */
    int            have_exif_ifd;
} ExifWalk;

static void exif_visit(void *ud, unsigned tag, const unsigned char *e) {
    ExifWalk *w = (ExifWalk *)ud;
    MediaMeta *m = w->m;
    const TiffCtx *tc = w->tc;

    if (w->pass == 0) {
        switch (tag) {
        case TIFF_TAG_MAKE:  store_ascii(m->exif_make,  64, e, tc); break;
        case TIFF_TAG_MODEL: store_ascii(m->exif_model, 64, e, tc); break;
        case TIFF_TAG_DATETIME:
            /* IFD0 里的 DateTime 是"文件最后修改时间"，质量不如 DateTimeOriginal，
               所以只在 ExifIFD 没给的时候才用它。 */
            if (!m->exif_date[0]) {
                wchar_t t[64] = L"";
                store_ascii(t, 64, e, tc);
                if (t[0]) wcscpy(m->exif_date, t);
            }
            break;
        case TIFF_TAG_EXIF_IFD: {
            int ok = 0;
            long off = store_long_val(e, tc, &ok);
            if (ok && off > 0) { w->exif_ifd_off = (unsigned)off; w->have_exif_ifd = 1; }
            break;
        }
        case TIFF_TAG_EXIF_W: {
            int ok = 0;
            unsigned v = store_short_val(e, tc, &ok);
            if (!ok) { long l = store_long_val(e, tc, &ok); v = (unsigned)l; }
            if (ok) swprintf(m->exif_w, 16, L"%u", v);
            break;
        }
        case TIFF_TAG_EXIF_H: {
            int ok = 0;
            unsigned v = store_short_val(e, tc, &ok);
            if (!ok) { long l = store_long_val(e, tc, &ok); v = (unsigned)l; }
            if (ok) swprintf(m->exif_h, 16, L"%u", v);
            break;
        }
        case TIFF_TAG_ORIENTATION: {
            int ok = 0;
            unsigned v = store_short_val(e, tc, &ok);
            if (ok) swprintf(m->exif_orient, 16, L"%u", v);
            break;
        }
        default: break;
        }
    } else {
        switch (tag) {
        case TIFF_TAG_DT_ORIG:  store_ascii(m->exif_date, 64, e, tc); break;
        case TIFF_TAG_DT_DIGIT:
            if (!m->exif_date[0]) store_ascii(m->exif_date, 64, e, tc);
            break;
        case TIFF_TAG_EXPOSURE: store_rational(m->exif_exposure, 24, e, tc, 0); break;
        case TIFF_TAG_FNUMBER:  store_rational(m->exif_fnum, 16, e, tc, 1); break;
        case TIFF_TAG_FOCAL:    store_rational(m->exif_focal, 16, e, tc, 2); break;
        case TIFF_TAG_LENS:     store_ascii(m->exif_lens, 64, e, tc); break;
        case TIFF_TAG_ISO: case TIFF_TAG_ISO_ALT: {
            int ok = 0;
            unsigned v = store_short_val(e, tc, &ok);
            if (!ok) { long l = store_long_val(e, tc, &ok); v = (unsigned)l; }
            if (ok) swprintf(m->exif_iso, 16, L"%u", v);
            break;
        }
        default: break;
        }
    }
}

/* 在 head 里找 JPEG 的 APP1/Exif 段 */
static int parse_exif(const RawFile *rf, MediaMeta *m) {
    const unsigned char *p = rf->head;
    size_t n = rf->head_len;
    if (n < 4) return 0;
    if (p[0] != 0xFF || p[1] != 0xD8) return 0;      /* 不是 JPEG */

    size_t i = 2;
    while (i + 4 <= n) {
        if (p[i] != 0xFF) { ++i; continue; }          /* 段之间可能有填充字节 */
        unsigned char mk = p[i+1];
        if (mk == 0xFF) { ++i; continue; }
        if (mk == 0xD8 || mk == 0x01 || (mk >= 0xD0 && mk <= 0xD7)) { i += 2; continue; }
        if (mk == 0xDA || mk == 0xD9) break;          /* 到图像数据了，后面没有元数据 */

        unsigned len = ((unsigned)p[i+2] << 8) | p[i+3];
        if (len < 2) break;
        size_t seg_end = i + 2 + len;
        if (seg_end > n) break;

        if (mk == 0xE1 && len >= 8 &&
            memcmp(p + i + 4, "Exif\0\0", 6) == 0) {
            size_t tiff = i + 10;                     /* APP1 数据从 i+4 开始，跳过 "Exif\0\0" */
            if (tiff + 8 > n) break;
            const unsigned char *t = p + tiff;
            int be;
            if      (t[0] == 'I' && t[1] == 'I') be = 0;
            else if (t[0] == 'M' && t[1] == 'M') be = 1;
            else break;

            TiffCtx tc;
            tc.be = be;
            tc.tiff_off = tiff;
            tc.head_ptr = p;
            /* 可索引范围到这一段结束为止，不许越到后面的段里去 */
            tc.limit = (seg_end < n) ? seg_end : n;

            if ((unsigned)rd16(t + 2, be) != 42) break;   /* TIFF 魔数 */
            unsigned ifd0 = rd32(t + 4, be);
            if (ifd0 == 0) break;

            ExifWalk w;
            memset(&w, 0, sizeof(w));
            w.m = m; w.tc = &tc; w.pass = 0;
            walk_ifd(&tc, ifd0, exif_visit, &w);

            if (w.have_exif_ifd) {
                w.pass = 1;
                walk_ifd(&tc, w.exif_ifd_off, exif_visit, &w);
            }

            m->has_exif = 1;
            return 1;
        }
        i = seg_end;
    }
    return 0;
}

/* ==================================================== ID3 ============= */

static unsigned synchsafe32(const unsigned char *p) {
    return ((unsigned)(p[0] & 0x7F) << 21) | ((unsigned)(p[1] & 0x7F) << 14) |
           ((unsigned)(p[2] & 0x7F) << 7)  |  (unsigned)(p[3] & 0x7F);
}

/* 文本帧：第一个字节是编码标识 */
static void id3_text(wchar_t *dst, size_t cch, const unsigned char *data, size_t n) {
    dst[0] = 0;
    if (n < 1) return;
    unsigned enc = data[0];
    const unsigned char *s = data + 1;
    size_t sn = n - 1;
    if (enc == 0)      copy_latin1(dst, cch, s, sn);
    else if (enc == 1) copy_utf16(dst, cch, s, sn, 0);   /* 带 BOM 的 UTF-16 */
    else if (enc == 2) copy_utf16(dst, cch, s, sn, 1);   /* UTF-16BE，无 BOM */
    else               copy_maybe_utf8(dst, cch, s, sn); /* 3 = UTF-8 */

    /* 有些标签里塞了多个值，用 NUL 分隔。只取第一个。 */
    for (wchar_t *q = dst; *q; ++q) if (*q == 0) { *q = 0; break; }
}

static void parse_id3v2(const RawFile *rf, MediaMeta *m) {
    const unsigned char *p = rf->head;
    size_t n = rf->head_len;
    if (n < 10) return;
    if (memcmp(p, "ID3", 3) != 0) return;

    unsigned ver = p[3];
    unsigned flags = p[5];
    unsigned size = synchsafe32(p + 6);          /* 不含头部的 10 字节 */
    if (size == 0) return;

    size_t total = 10 + (size_t)size;
    if (total > n) total = n;                    /* 文件被截断就只读到有的部分 */

    size_t off = 10;
    /* 扩展头：v2.3/2.4 里 flags 的 bit6 表示存在扩展头 */
    if (flags & 0x40) {
        if (off + 4 > total) return;
        unsigned ext = (ver >= 4) ? synchsafe32(p + off)
                                  : (((unsigned)p[off] << 24) | ((unsigned)p[off+1] << 16) |
                                     ((unsigned)p[off+2] << 8) | p[off+3]);
        off += 4 + ext;
    }

    while (off + 10 <= total) {
        const unsigned char *fh = p + off;
        /* 遇到全 0 就是填充区，结束 */
        if (fh[0] == 0 && fh[1] == 0 && fh[2] == 0 && fh[3] == 0) break;

        char id[5] = {0};
        memcpy(id, fh, 4);

        unsigned fsize = (ver >= 4)
            ? synchsafe32(fh + 4)                                     /* v2.4 也是同步安全 */
            : (((unsigned)fh[4] << 24) | ((unsigned)fh[5] << 16) |    /* v2.2/2.3 是普通整数 */
               ((unsigned)fh[6] << 8) | fh[7]);
        size_t hdr = (ver == 2) ? 6 : 10;        /* v2.2 的帧头只有 6 字节，ID 是 3 字符 */
        if (ver == 2) { id[3] = 0; }

        if (fsize == 0 || off + hdr + fsize > total) break;
        const unsigned char *data = fh + hdr;
        size_t dlen = fsize;

        /* v2.2 用 3 字符 ID，v2.3/2.4 用 4 字符，两套都要认 */
        if (strcmp(id, "TIT2") == 0 || strcmp(id, "TT2") == 0)
            id3_text(m->id3_title, 128, data, dlen);
        else if (strcmp(id, "TPE1") == 0 || strcmp(id, "TP1") == 0)
            id3_text(m->id3_artist, 128, data, dlen);
        else if (strcmp(id, "TALB") == 0 || strcmp(id, "TAL") == 0)
            id3_text(m->id3_album, 128, data, dlen);
        else if (strcmp(id, "TRCK") == 0 || strcmp(id, "TRK") == 0)
            id3_text(m->id3_track, 32, data, dlen);
        else if (strcmp(id, "TCON") == 0 || strcmp(id, "TCO") == 0)
            id3_text(m->id3_genre, 64, data, dlen);
        else if (strcmp(id, "TYER") == 0 || strcmp(id, "TYE") == 0)
            id3_text(m->id3_year, 16, data, dlen);
        else if (strcmp(id, "TDRC") == 0) {
            /* v2.4 用 TDRC 存完整时间戳，年份取前 4 位 */
            wchar_t t[64] = L"";
            id3_text(t, 64, data, dlen);
            if (t[0]) { wcsncpy(m->id3_year, t, 15); m->id3_year[15] = 0; }
        }

        off += hdr + fsize;
    }
    m->has_id3 = 1;
}

/* 更老的 ID3v1：文件最后 128 字节，固定偏移，没有长度字段 */
static void parse_id3v1(const RawFile *rf, MediaMeta *m) {
    const unsigned char *t = rf->tail;
    size_t n = rf->tail_len;
    if (!t || n < 128) return;
    if (memcmp(t + n - 128, "TAG", 3) != 0) return;

    const unsigned char *b = t + n - 128;
    if (!m->id3_title[0])  copy_latin1(m->id3_title,  128, b +  3, 30);
    if (!m->id3_artist[0]) copy_latin1(m->id3_artist, 128, b + 33, 30);
    if (!m->id3_album[0])  copy_latin1(m->id3_album,  128, b + 63, 30);
    if (!m->id3_year[0])   copy_latin1(m->id3_year,    16, b + 93,  4);
    if (!m->id3_track[0] && b[125] == 0 && b[126] != 0) {
        swprintf(m->id3_track, 32, L"%u", (unsigned)b[126]);
    }
    m->has_id3 = 1;
}

/* ==================================================== 对外接口 ======= */

/*
 * 按扩展名粗略判断有没有可能带元数据。
 * 只用来在界面上给个提示，不做任何控制流判断 ——
 * 真正有没有元数据一律以 meta_read 的结果为准。
 */
int meta_ext_maybe(const wchar_t *path) {
    static const wchar_t *IMG[] = {
        L".jpg", L".jpeg", L".jpe", L".tif", L".tiff", L".heic", L".heif",
        L".dng", L".cr2", L".cr3", L".nef", L".arw", L".raf", L".rw2",
        L".orf", L".pef", L".srw", NULL
    };
    static const wchar_t *AUD[] = {
        L".mp3", L".m4a", L".aac", L".flac", L".wav", L".ogg", L".wma", NULL
    };
    if (!path) return 0;
    const wchar_t *e = lc_extname(path);
    for (int i = 0; IMG[i]; ++i) if (lc_wcs_ieq(e, IMG[i])) return 1;
    for (int i = 0; AUD[i]; ++i) if (lc_wcs_ieq(e, AUD[i])) return 1;
    return 0;
}
int meta_read(const wchar_t *path, MediaMeta *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!path) return -1;

    RawFile rf;
    if (read_head_tail(path, &rf) != 0) return -1;

    parse_exif(&rf, out);
    parse_id3v2(&rf, out);
    parse_id3v1(&rf, out);

    raw_free(&rf);
    return (out->has_exif || out->has_id3) ? 0 : 1;   /* 1 = 读到了文件，但里面没有元数据 */
}

/* EXIF 日期原始形式是 "2026:01:15 14:30:22"，转成 "2026-01-15" 这类更常用。
   返回 0 表示解析成功，-1 表示原始值不是能认的日期。 */
static int exif_date_fmt(const MediaMeta *m, const wchar_t *fmt,
                         wchar_t *out, size_t cch) {
    out[0] = 0;
    if (!m->exif_date[0]) return -1;

    /* 解析 "YYYY:MM:DD HH:MM:SS"；有些相机用 "-" 分隔或用空格 */
    int Y = 0, Mo = 0, D = 0, H = 0, Mi = 0, S = 0;
    swscanf(m->exif_date, L"%d%*c%d%*c%d%*c%d%*c%d%*c%d",
            &Y, &Mo, &D, &H, &Mi, &S);
    if (Y < 1000 || Y > 9999) return -1;      /* 解析失败 */
    if (Mo < 1 || Mo > 12 || D < 1 || D > 31) return -1;

    const wchar_t *f = (fmt && *fmt) ? fmt : L"%Y-%m-%d";
    /* 不用 wcsftime：它要 struct tm，而这里只是把固定的六个数塞进格式串。
       手动展开支持的几个占位符，够用且不会有本地化意外。 */
    WStr s; wstr_init(&s);
    for (const wchar_t *p = f; *p; ++p) {
        if (*p != L'%') { wstr_putc(&s, *p); continue; }
        ++p;
        switch (*p) {
        case L'Y': wstr_printf(&s, L"%04d", Y); break;
        case L'm': wstr_printf(&s, L"%02d", Mo); break;
        case L'd': wstr_printf(&s, L"%02d", D); break;
        case L'H': wstr_printf(&s, L"%02d", H); break;
        case L'M': wstr_printf(&s, L"%02d", Mi); break;
        case L'S': wstr_printf(&s, L"%02d", S); break;
        case L'%': wstr_putc(&s, L'%'); break;
        case 0:    wstr_putc(&s, L'%'); --p; break;
        default:   wstr_putc(&s, L'%'); wstr_putc(&s, *p); break;
        }
    }
    wcsncpy(out, s.data ? s.data : L"", cch - 1);
    out[cch-1] = 0;
    wstr_free(&s);
    return out[0] ? 0 : -1;
}

const wchar_t *meta_lookup(const MediaMeta *m, const wchar_t *ns,
                           const wchar_t *key, const wchar_t *fmt,
                           wchar_t *buf, size_t cch) {
    if (!m || !ns || !key) return NULL;

    if (_wcsicmp(ns, L"exif") == 0) {
        if (!m->has_exif) return NULL;
        if (_wcsicmp(key, L"date") == 0 || _wcsicmp(key, L"datetime") == 0) {
            if (!m->exif_date[0]) return NULL;
            /*
             * 解析失败必须返回 NULL，不能返回空串。
             *
             * 调用方把"返回非 NULL"当成"这个字段有值"。如果日期读成了
             * 认不出来的垃圾（文件损坏、偏移错位的畸形 EXIF），返回一个空串
             * 会让文件名里出现一个空洞（`_相机型号_001.jpg`），
             * 看起来像 bug 而不是"这个文件没有拍摄时间"。
             * 返回 NULL 则会显示「未知」，含义是清楚的。
             */
            if (exif_date_fmt(m, fmt, buf, cch) != 0) return NULL;
            return buf;
        }
        if (_wcsicmp(key, L"year") == 0) {
            if (!m->exif_date[0]) return NULL;
            /* 年份同样要能认出来才给 */
            int Y = 0;
            if (swscanf(m->exif_date, L"%d", &Y) != 1) return NULL;
            if (Y < 1000 || Y > 9999) return NULL;
            swprintf(buf, cch, L"%04d", Y);
            return buf;
        }
        if (_wcsicmp(key, L"model") == 0) {
            if (!m->exif_model[0]) return NULL;
            swprintf(buf, cch, L"%ls", m->exif_model); return buf;
        }
        if (_wcsicmp(key, L"make") == 0) {
            if (!m->exif_make[0]) return NULL;
            swprintf(buf, cch, L"%ls", m->exif_make); return buf;
        }
        if (_wcsicmp(key, L"lens") == 0) {
            if (!m->exif_lens[0]) return NULL;
            swprintf(buf, cch, L"%ls", m->exif_lens); return buf;
        }
        if (_wcsicmp(key, L"iso") == 0) {
            if (!m->exif_iso[0]) return NULL;
            swprintf(buf, cch, L"%ls", m->exif_iso); return buf;
        }
        if (_wcsicmp(key, L"fnum") == 0 || _wcsicmp(key, L"f") == 0) {
            if (!m->exif_fnum[0]) return NULL;
            swprintf(buf, cch, L"%ls", m->exif_fnum); return buf;
        }
        if (_wcsicmp(key, L"exposure") == 0) {
            if (!m->exif_exposure[0]) return NULL;
            swprintf(buf, cch, L"%ls", m->exif_exposure); return buf;
        }
        if (_wcsicmp(key, L"focal") == 0) {
            if (!m->exif_focal[0]) return NULL;
            swprintf(buf, cch, L"%ls", m->exif_focal); return buf;
        }
        if (_wcsicmp(key, L"w") == 0 || _wcsicmp(key, L"width") == 0) {
            if (!m->exif_w[0]) return NULL;
            swprintf(buf, cch, L"%ls", m->exif_w); return buf;
        }
        if (_wcsicmp(key, L"h") == 0 || _wcsicmp(key, L"height") == 0) {
            if (!m->exif_h[0]) return NULL;
            swprintf(buf, cch, L"%ls", m->exif_h); return buf;
        }
        return NULL;
    }

    if (_wcsicmp(ns, L"id3") == 0) {
        if (!m->has_id3) return NULL;
        if (_wcsicmp(key, L"title") == 0) {
            if (!m->id3_title[0]) return NULL;
            swprintf(buf, cch, L"%ls", m->id3_title); return buf;
        }
        if (_wcsicmp(key, L"artist") == 0) {
            if (!m->id3_artist[0]) return NULL;
            swprintf(buf, cch, L"%ls", m->id3_artist); return buf;
        }
        if (_wcsicmp(key, L"album") == 0) {
            if (!m->id3_album[0]) return NULL;
            swprintf(buf, cch, L"%ls", m->id3_album); return buf;
        }
        if (_wcsicmp(key, L"year") == 0) {
            if (!m->id3_year[0]) return NULL;
            swprintf(buf, cch, L"%ls", m->id3_year); return buf;
        }
        if (_wcsicmp(key, L"track") == 0) {
            if (!m->id3_track[0]) return NULL;
            swprintf(buf, cch, L"%ls", m->id3_track); return buf;
        }
        if (_wcsicmp(key, L"genre") == 0) {
            if (!m->id3_genre[0]) return NULL;
            swprintf(buf, cch, L"%ls", m->id3_genre); return buf;
        }
        return NULL;
    }
    return NULL;
}
