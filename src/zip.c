/* ==========================================================================
 * LocalCraft — ZIP 读写 + 原始 DEFLATE (inflate) 解压
 * --------------------------------------------------------------------------
 * 纯 C11 实现，不依赖任何第三方库。参考 RFC 1951 (DEFLATE) / PKWARE APPNOTE。
 *
 * 【API】以 src/lc.h 的 ZIP 段为准（LcZip / LcLcZipWriter）。本文件额外提供
 * 若干便利扩展（不在 lc.h 里，名字都不冲突）：
 *
 *   typedef struct LcZip LcZip;                 // lc.h
 *   LcZip *zip_open_memory(const void *data, size_t len);   // 内存归档
 *   int    zip_find(const LcZip *z, const char *name_utf8); // 下标 / -1
 *   int    zip_is_dir_at(const LcZip *z, size_t i);
 *   int    zip_method_at(const LcZip *z, size_t i);         // 0 / 8
 *   long long zip_size_at(const LcZip *z, size_t i);        // 解压后大小
 *   long long zip_crc_at(const LcZip *z, size_t i);
 *   uint32_t lc_crc32_update(uint32_t crc, const void *data, size_t len);
 *   int    zip_writer_add_w(LcLcZipWriter *w, const wchar_t *name,
 *                           const void *data, size_t len);
 *
 * 【语义约定】
 *   zip_read_at / zip_read_by_name：返回 0 表示成功，*out 为 malloc 缓冲区
 *   （调用方 free/lc_free），*outlen 为字节数；失败返回非 0 且 *out=NULL。
 *   目录条目读出长度为 0 的非 NULL 缓冲区。
 *   lc_inflate：返回 0 成功；LC_INFLATE_BAD(1) 流损坏/截断；
 *   LC_INFLATE_FULL(2) 输出会超过 dstcap。
 *
 * 【未实现】加密、多卷、bzip2/LZMA 等压缩方法、ZIP64 写入；这些一律**明确
 * 失败**而不是读出垃圾数据。ZIP64 读取支持 CD/EOCD 的 8 字节尺寸与偏移。
 * ========================================================================== */

#include "lc.h"

#include <stdlib.h>
#include <string.h>

/* =========================================================== 常量 / 工具 */

#ifndef LC_INFLATE_OK
#define LC_INFLATE_OK    0
#define LC_INFLATE_BAD   1   /* 流损坏 / 截断 / 非法码 */
#define LC_INFLATE_FULL  2   /* 输出会超过 dstcap */
#endif

#define MAXBITS 15

/* 单个条目 / 整个归档的大小上限（防止损坏文件让我们申请 4GB） */
#define ZIP_MAX_ENTRY   ((uint64_t)256 * 1024 * 1024)
#define ZIP_MAX_ARCHIVE ((uint64_t)512 * 1024 * 1024)

/* 小端读取（不做越界检查，调用方先确保边界） */
static uint16_t rd16(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const unsigned char *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static char *lc_strdup_(const char *s) {
    size_t n;
    char *p;
    if (!s) return NULL;
    n = strlen(s);
    p = (char *)lc_alloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

/* ============================================================ CRC-32 */

static uint32_t s_crc_tab[256];
static int      s_crc_ready = 0;

static void crc_init(void) {
    uint32_t n, k, c;
    if (s_crc_ready) return;
    for (n = 0; n < 256; n++) {
        c = n;
        for (k = 0; k < 8; k++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        s_crc_tab[n] = c;
    }
    s_crc_ready = 1;
}

uint32_t lc_crc32_update(uint32_t crc, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    uint32_t c;
    crc_init();
    c = crc ^ 0xFFFFFFFFu;
    while (len--) c = s_crc_tab[(c ^ *p++) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t lc_crc32(const void *data, size_t len) {
    return lc_crc32_update(0u, data, len);
}

/* ============================================================ 位读取器 */

typedef struct {
    const unsigned char *src;
    size_t   len;
    size_t   pos;
    uint32_t bitbuf;
    int      bitcnt;
    int      err;
} Br;

static void br_refill(Br *b) {
    while (b->bitcnt <= 24 && b->pos < b->len) {
        b->bitbuf |= (uint32_t)b->src[b->pos++] << b->bitcnt;
        b->bitcnt += 8;
    }
}

/* n <= 16；出错时置 err 并返回 0 */
static uint32_t br_get(Br *b, int n) {
    uint32_t v;
    if (b->bitcnt < n) {
        br_refill(b);
        if (b->bitcnt < n) { b->err = 1; return 0u; }
    }
    v = b->bitbuf & ((1u << n) - 1u);
    b->bitbuf >>= n;
    b->bitcnt -= n;
    return v;
}

/* 丢掉不足一字节的位，并把整字节还给输入流 */
static void br_align_sync(Br *b) {
    int drop = b->bitcnt & 7;
    b->bitbuf >>= drop;
    b->bitcnt -= drop;
    b->pos -= (size_t)(b->bitcnt >> 3);
    b->bitbuf = 0;
    b->bitcnt = 0;
}

/* =========================================================== Huffman */

typedef struct {
    uint16_t count[MAXBITS + 1];
    uint16_t symbol[288];
} Huff;

/* kind: 0 = 码长树（必须完整）；1 = 字面量/距离树（允许单码不完整，同 zlib） */
static int huff_build(Huff *h, const unsigned char *lens, int n, int kind) {
    int i, len, left, max = 0;
    uint16_t offs[MAXBITS + 1];

    memset(h->count, 0, sizeof h->count);
    for (i = 0; i < n; i++) h->count[lens[i]]++;
    if (h->count[0] == n) {          /* 一个码都没有：构树合法，任何解码都会失败 */
        memset(h->symbol, 0, sizeof h->symbol);
        return 0;
    }
    left = 1;
    for (len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= (int)h->count[len];
        if (left < 0) return -1;     /* 码长过订阅 */
        if (h->count[len]) max = len;
    }
    if (left > 0 && !(kind != 0 && max == 1)) return -1;  /* 不完整 */

    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++)
        offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (i = 0; i < n; i++)
        if (lens[i]) h->symbol[offs[lens[i]]++] = (uint16_t)i;
    return 0;
}

/* 逐位规范 Huffman 解码（puff 算法），失败返回 -1 */
static int huff_decode(Br *br, const Huff *h) {
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len <= MAXBITS; len++) {
        int count;
        if (br->bitcnt == 0) {
            br_refill(br);
            if (br->bitcnt == 0) { br->err = 1; return -1; }
        }
        code |= (int)(br->bitbuf & 1u);
        br->bitbuf >>= 1;
        br->bitcnt--;
        count = (int)h->count[len];
        if (code - count < first)
            return (int)h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    br->err = 1;
    return -1;
}

static const unsigned char clc_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static void fixed_trees(Huff *lit, Huff *dist) {
    static Huff flit, fdist;
    static int ready = 0;
    unsigned char l[288];
    int i;
    if (!ready) {
        for (i = 0; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        (void)huff_build(&flit, l, 288, 1);
        for (i = 0; i < 32; i++) l[i] = 5;
        (void)huff_build(&fdist, l, 32, 1);
        ready = 1;
    }
    *lit = flit;
    *dist = fdist;
}

static const uint16_t len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const unsigned short dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const unsigned char dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* ============================================================ inflate */

int lc_inflate(const unsigned char *src, size_t srclen,
               unsigned char *dst, size_t dstcap, size_t *dstlen) {
    Br b;
    size_t out = 0;
    int final;

    if (!src && srclen) return LC_INFLATE_BAD;
    if (!dst && dstcap) return LC_INFLATE_BAD;
    if (dstlen) *dstlen = 0;

    b.src = src; b.len = srclen; b.pos = 0;
    b.bitbuf = 0; b.bitcnt = 0; b.err = 0;

    do {
        uint32_t btype;
        if (b.err) return LC_INFLATE_BAD;

        final = (int)br_get(&b, 1);
        btype = br_get(&b, 2);
        if (b.err) return LC_INFLATE_BAD;

        if (btype == 0) {                       /* stored */
            unsigned int len, nlen;
            br_align_sync(&b);
            if (b.len - b.pos < 4) return LC_INFLATE_BAD;
            len  = (unsigned int)rd16(src + b.pos);
            nlen = (unsigned int)rd16(src + b.pos + 2);
            b.pos += 4;
            if ((len ^ 0xFFFFu) != nlen) return LC_INFLATE_BAD;
            if (b.len - b.pos < len) return LC_INFLATE_BAD;
            if (dstcap - out < len) return LC_INFLATE_FULL;
            memcpy(dst + out, src + b.pos, len);
            b.pos += len;
            out += len;
        } else if (btype == 1 || btype == 2) {  /* fixed / dynamic */
            Huff lit, dist;
            unsigned char lens[288 + 32];

            if (btype == 1) {
                fixed_trees(&lit, &dist);
            } else {
                unsigned int hlit, hdist, hclen, i, total;
                Huff clh;

                hlit  = br_get(&b, 5) + 257u;
                hdist = br_get(&b, 5) + 1u;
                hclen = br_get(&b, 4) + 4u;
                if (b.err) return LC_INFLATE_BAD;
                if (hlit > 288u || hdist > 32u) return LC_INFLATE_BAD;

                memset(lens, 0, 19);
                for (i = 0; i < hclen; i++)
                    lens[clc_order[i]] = (unsigned char)br_get(&b, 3);
                if (b.err) return LC_INFLATE_BAD;
                if (huff_build(&clh, lens, 19, 0) != 0) return LC_INFLATE_BAD;

                memset(lens, 0, sizeof lens);
                total = hlit + hdist;
                i = 0;
                while (i < total) {
                    int sym = huff_decode(&b, &clh);
                    unsigned int rep;
                    if (sym < 0) return LC_INFLATE_BAD;
                    if (sym < 16) {
                        lens[i++] = (unsigned char)sym;
                        continue;
                    }
                    if (sym == 16) {
                        unsigned char prev;
                        if (i == 0) return LC_INFLATE_BAD;
                        prev = lens[i - 1];
                        rep = br_get(&b, 2) + 3u;
                        if (b.err || i + rep > total) return LC_INFLATE_BAD;
                        while (rep--) lens[i++] = prev;
                    } else if (sym == 17) {
                        rep = br_get(&b, 3) + 3u;
                        if (b.err || i + rep > total) return LC_INFLATE_BAD;
                        while (rep--) lens[i++] = 0;
                    } else {
                        rep = br_get(&b, 7) + 11u;
                        if (b.err || i + rep > total) return LC_INFLATE_BAD;
                        while (rep--) lens[i++] = 0;
                    }
                }
                if (huff_build(&lit, lens, (int)hlit, 1) != 0)
                    return LC_INFLATE_BAD;
                if (huff_build(&dist, lens + hlit, (int)hdist, 1) != 0)
                    return LC_INFLATE_BAD;
            }

            for (;;) {
                int sym = huff_decode(&b, &lit);
                if (sym < 0) return LC_INFLATE_BAD;
                if (sym < 256) {
                    if (out >= dstcap) return LC_INFLATE_FULL;
                    dst[out++] = (unsigned char)sym;
                } else if (sym == 256) {
                    break;                       /* 块结束 */
                } else {
                    unsigned int li = (unsigned int)(sym - 257);
                    unsigned int len, dsym, d, i;
                    if (li >= 29u) return LC_INFLATE_BAD;   /* 286/287 非法 */
                    len = (unsigned int)len_base[li];
                    if (len_extra[li]) {
                        len += br_get(&b, len_extra[li]);
                        if (b.err) return LC_INFLATE_BAD;
                    }
                    dsym = (unsigned int)huff_decode(&b, &dist);
                    if ((int)dsym < 0 || dsym >= 30u) return LC_INFLATE_BAD;
                    d = (unsigned int)dist_base[dsym];
                    if (dist_extra[dsym]) {
                        d += br_get(&b, dist_extra[dsym]);
                        if (b.err) return LC_INFLATE_BAD;
                    }
                    if ((uint64_t)d > (uint64_t)out) return LC_INFLATE_BAD;
                    if ((uint64_t)len > (uint64_t)(dstcap - out))
                        return LC_INFLATE_FULL;
                    if (d >= len) {
                        memcpy(dst + out, dst + out - d, len);
                        out += len;
                    } else {
                        /* LZ77 重叠复制：必须逐字节前进 */
                        for (i = 0; i < len; i++)
                            dst[out + i] = dst[out - d + i];
                        out += len;
                    }
                }
            }
        } else {
            return LC_INFLATE_BAD;               /* BTYPE = 3 非法 */
        }
    } while (!final);

    if (dstlen) *dstlen = out;
    return LC_INFLATE_OK;
}

/* ======================================================== 名称解码 */

static int utf8_valid(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        unsigned int cp;
        int need, k;
        if (c < 0x80) { i++; continue; }
        else if ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1Fu; }
        else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0Fu; }
        else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07u; }
        else return 0;
        if (i + (size_t)need >= n) return 0;
        for (k = 1; k <= need; k++) {
            if ((s[i + (size_t)k] & 0xC0) != 0x80) return 0;
            cp = (cp << 6) | (unsigned int)(s[i + (size_t)k] & 0x3Fu);
        }
        if (need == 1 && cp < 0x80u) return 0;
        if (need == 2 && cp < 0x800u) return 0;
        if (need == 3 && cp < 0x10000u) return 0;
        if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return 0;
        i += (size_t)need + 1;
    }
    return 1;
}

/* 用指定 ANSI 代码页把原始名字转成 UTF-8 */
static char *ansi_to_utf8(const unsigned char *raw, size_t n, UINT cp) {
    int wn;
    wchar_t *w;
    char *u;
    if (n == 0) return lc_strdup_("");
    wn = MultiByteToWideChar(cp, 0, (const char *)raw, (int)n, NULL, 0);
    if (wn <= 0) return NULL;
    w = (wchar_t *)lc_alloc((size_t)wn * sizeof(wchar_t));
    if (!w) return NULL;
    if (MultiByteToWideChar(cp, 0, (const char *)raw, (int)n, w, wn) <= 0) {
        lc_free(w);
        return NULL;
    }
    u = lc_wcs_to_utf8_n(w, (size_t)wn);
    lc_free(w);
    return u;
}

/* 把 raw 当作某代码页解码再编码回去，是否与原字节完全一致。
   一致 => 这段字节很可能真的就是该代码页的文本。 */
static int cp_roundtrip_ok(const unsigned char *raw, size_t n, UINT cp) {
    wchar_t *w;
    char *back;
    int wn, bn, ok = 0;
    if (n == 0) return 1;
    wn = MultiByteToWideChar(cp, 0, (const char *)raw, (int)n, NULL, 0);
    if (wn <= 0) return 0;
    w = (wchar_t *)lc_alloc((size_t)wn * sizeof(wchar_t));
    if (!w) return 0;
    MultiByteToWideChar(cp, 0, (const char *)raw, (int)n, w, wn);
    bn = WideCharToMultiByte(cp, 0, w, wn, NULL, 0, NULL, NULL);
    if (bn == (int)n) {
        back = (char *)lc_alloc((size_t)bn);
        if (back) {
            if (WideCharToMultiByte(cp, 0, w, wn, back, bn, NULL, NULL) == bn &&
                memcmp(back, raw, n) == 0)
                ok = 1;
            lc_free(back);
        }
    }
    lc_free(w);
    return ok;
}

/* 归档内名字统一暴露为 UTF-8。
 *
 * 规则：
 *   1. 纯 ASCII —— 原样。
 *   2. 带 bit11（UTF-8 标记）—— 直接当 UTF-8 用（字节非法时才退回 ANSI）。
 *   3. 不带 bit11 的非 ASCII —— 老工具（WinRAR / 旧版资源管理器）写的是本地
 *      ANSI 代码页（简体中文机上就是 GBK）。注意 GBK 的字节序列**经常恰好也
 *      是合法 UTF-8**（如 "图片" = CD BC C6 AC），所以不能"合法 UTF-8 就用
 *      UTF-8"；这里改用"能否用该代码页无损往返"来判断，最后才退到 CP437。 */
static char *zip_name_decode(const unsigned char *raw, size_t n, uint16_t flags) {
    size_t i;
    int ascii = 1;
    char *u;

    if (n == 0) return lc_strdup_("");
    for (i = 0; i < n; i++)
        if (raw[i] >= 0x80u) { ascii = 0; break; }

    if (ascii || (flags & 0x0800u) != 0) {
        if (ascii || utf8_valid(raw, n)) {
            u = (char *)lc_alloc(n + 1);
            if (!u) return NULL;
            memcpy(u, raw, n);
            u[n] = 0;
            return u;
        }
    }
    if (cp_roundtrip_ok(raw, n, CP_ACP)) {
        u = ansi_to_utf8(raw, n, CP_ACP);
        if (u) return u;
    }
    if (utf8_valid(raw, n)) {
        u = (char *)lc_alloc(n + 1);
        if (!u) return NULL;
        memcpy(u, raw, n);
        u[n] = 0;
        return u;
    }
    if (cp_roundtrip_ok(raw, n, 437)) {
        u = ansi_to_utf8(raw, n, 437);
        if (u) return u;
    }
    return ansi_to_utf8(raw, n, CP_ACP);
}

/* ======================================================== ZIP 读取 */

typedef struct {
    char     *name;        /* UTF-8 */
    uint32_t  crc;
    uint64_t  comp;
    uint64_t  uncomp;
    uint64_t  lho;
    uint16_t  method;
    uint16_t  flags;
    int       is_dir;
} ZipEntry;

struct LcZip {
    unsigned char *data;
    size_t         size;
    ZipEntry      *ents;
    size_t         count;
    int            owned;    /* data 是否由本结构负责释放 */
};

static int find_eocd(const unsigned char *p, size_t n, size_t *pos) {
    size_t start, i;
    if (n < 22) return 0;
    start = (n > 65557u) ? n - 65557u : 0;   /* 64K 注释上限 */
    i = n - 22;
    for (;;) {
        if (rd32(p + i) == 0x06054b50u) { *pos = i; return 1; }
        if (i == start) break;
        i--;
    }
    return 0;
}

LcZip *zip_open_memory(const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    LcZip *z;
    size_t eocd = 0, pos, i;
    uint64_t nent, cdsize, cdoff;

    if (!data || len < 22) return NULL;
    if (!find_eocd(p, len, &eocd)) return NULL;

    if (rd16(p + eocd + 4) != 0 || rd16(p + eocd + 6) != 0) return NULL; /* 多卷 */
    if (rd16(p + eocd + 8) != rd16(p + eocd + 10)) return NULL;           /* 多卷 */
    nent   = rd16(p + eocd + 10);
    cdsize = rd32(p + eocd + 12);
    cdoff  = rd32(p + eocd + 16);

    if (rd16(p + eocd + 10) == 0xFFFFu || cdsize == 0xFFFFFFFFu ||
        cdoff == 0xFFFFFFFFu) {
        /* ZIP64：定位 ZIP64 EOCD locator -> ZIP64 EOCD */
        uint64_t z64;
        if (eocd < 20) return NULL;
        if (rd32(p + eocd - 20) != 0x07064b50u) return NULL;
        z64 = rd64(p + eocd - 12);
        if (z64 > (uint64_t)len || (uint64_t)len - z64 < 56) return NULL;
        if (rd32(p + (size_t)z64) != 0x06064b50u) return NULL;
        nent   = rd64(p + (size_t)z64 + 32);
        cdsize = rd64(p + (size_t)z64 + 40);
        cdoff  = rd64(p + (size_t)z64 + 48);
    }

    if (nent > (uint64_t)1 << 22) return NULL;
    if (cdoff > (uint64_t)len) return NULL;
    if (cdsize > (uint64_t)len - cdoff) return NULL;

    z = (LcZip *)lc_alloc(sizeof *z);
    if (!z) return NULL;
    memset(z, 0, sizeof *z);
    z->data = (unsigned char *)p;
    z->size = len;
    z->owned = 0;               /* 调用方拥有这块内存 */
    z->count = (size_t)nent;
    if (z->count) {
        z->ents = (ZipEntry *)lc_alloc(z->count * sizeof(ZipEntry));
        if (!z->ents) { lc_free(z); return NULL; }
        memset(z->ents, 0, z->count * sizeof(ZipEntry));
    }

    pos = (size_t)cdoff;
    for (i = 0; i < z->count; i++) {
        ZipEntry *e = &z->ents[i];
        uint16_t nlen, elen, clen;
        uint32_t csz32, usz32, lho32;
        uint64_t u, c, off;
        size_t extra_off, ep, eend;

        if (pos + 46 > len || rd32(p + pos) != 0x02014b50u) goto fail;
        e->flags  = rd16(p + pos + 8);
        e->method = rd16(p + pos + 10);
        e->crc    = rd32(p + pos + 16);
        csz32     = rd32(p + pos + 20);
        usz32     = rd32(p + pos + 24);
        nlen      = rd16(p + pos + 28);
        elen      = rd16(p + pos + 30);
        clen      = rd16(p + pos + 32);
        lho32     = rd32(p + pos + 42);

        extra_off = pos + 46 + (size_t)nlen;
        if (extra_off > len || (size_t)elen > len - extra_off) goto fail;

        u = usz32; c = csz32; off = lho32;
        if (usz32 == 0xFFFFFFFFu || csz32 == 0xFFFFFFFFu ||
            lho32 == 0xFFFFFFFFu) {
            int have_u = 0, have_c = 0, have_o = 0;
            ep = extra_off;
            eend = extra_off + elen;
            while (ep + 4 <= eend) {
                uint16_t id = rd16(p + ep);
                uint16_t esz = rd16(p + ep + 2);
                size_t d0 = ep + 4, dp, dend;
                if ((size_t)esz > eend - d0) break;
                dp = d0;
                dend = d0 + esz;
                if (id == 0x0001u) {
                    if (usz32 == 0xFFFFFFFFu) {
                        if (dp + 8 > dend) break;
                        u = rd64(p + dp); dp += 8; have_u = 1;
                    }
                    if (csz32 == 0xFFFFFFFFu) {
                        if (dp + 8 > dend) break;
                        c = rd64(p + dp); dp += 8; have_c = 1;
                    }
                    if (lho32 == 0xFFFFFFFFu) {
                        if (dp + 8 > dend) break;
                        off = rd64(p + dp); dp += 8; have_o = 1;
                    }
                }
                ep = d0 + esz;
            }
            /* 需要的 64 位字段没找到 -> 明确失败，不读垃圾 */
            if ((usz32 == 0xFFFFFFFFu && !have_u) ||
                (csz32 == 0xFFFFFFFFu && !have_c) ||
                (lho32 == 0xFFFFFFFFu && !have_o)) goto fail;
        }
        e->uncomp = u;
        e->comp   = c;
        e->lho    = off;

        e->name = zip_name_decode(p + pos + 46, nlen, e->flags);
        if (!e->name) goto fail;
        e->is_dir = (nlen > 0 && p[pos + 46 + nlen - 1] == '/') ? 1 : 0;

        pos = extra_off + elen + clen;
    }
    return z;

fail:
    zip_close(z);
    return NULL;
}

static unsigned char *read_whole_file(const wchar_t *path, size_t *out_len) {
    HANDLE h;
    LARGE_INTEGER sz;
    DWORD got = 0;
    unsigned char *buf;
    h = CreateFileW(path, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 ||
        (uint64_t)sz.QuadPart > ZIP_MAX_ARCHIVE) {
        CloseHandle(h);
        return NULL;
    }
    buf = (unsigned char *)lc_alloc((size_t)sz.QuadPart);
    if (!buf) { CloseHandle(h); return NULL; }
    if (!ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL) ||
        got != (DWORD)sz.QuadPart) {
        lc_free(buf);
        CloseHandle(h);
        return NULL;
    }
    CloseHandle(h);
    *out_len = (size_t)sz.QuadPart;
    return buf;
}

LcZip *zip_open(const wchar_t *path) {
    size_t len = 0;
    unsigned char *buf;
    LcZip *z;
    if (!path) return NULL;
    buf = read_whole_file(path, &len);
    if (!buf) return NULL;
    z = zip_open_memory(buf, len);
    if (!z) { lc_free(buf); return NULL; }
    z->data = buf;              /* 接管所有权 */
    z->owned = 1;
    return z;
}

void zip_close(LcZip *z) {
    size_t i;
    if (!z) return;
    if (z->ents) {
        for (i = 0; i < z->count; i++) lc_free(z->ents[i].name);
        lc_free(z->ents);
    }
    if (z->owned) lc_free(z->data);
    lc_free(z);
}

size_t zip_count(const LcZip *z) {
    return z ? z->count : 0;
}

const char *zip_name_at(const LcZip *z, size_t i) {
    if (!z || i >= z->count) return NULL;
    return z->ents[i].name;
}

int zip_find(const LcZip *z, const char *name_utf8) {
    size_t i;
    if (!z || !name_utf8) return -1;
    for (i = 0; i < z->count; i++)
        if (z->ents[i].name && strcmp(z->ents[i].name, name_utf8) == 0)
            return (int)i;
    return -1;
}

int zip_is_dir_at(const LcZip *z, size_t i) {
    if (!z || i >= z->count) return 0;
    return z->ents[i].is_dir;
}

int zip_method_at(const LcZip *z, size_t i) {
    if (!z || i >= z->count) return -1;
    return (int)z->ents[i].method;
}

long long zip_size_at(const LcZip *z, size_t i) {
    if (!z || i >= z->count) return -1;
    return (long long)z->ents[i].uncomp;
}

long long zip_crc_at(const LcZip *z, size_t i) {
    if (!z || i >= z->count) return -1;
    return (long long)z->ents[i].crc;
}

static unsigned char *empty_buffer(size_t *len_out) {
    unsigned char *b = (unsigned char *)lc_alloc(1);
    if (b && len_out) *len_out = 0;
    return b;
}

static unsigned char *read_entry(const LcZip *z, size_t index, size_t *len_out) {
    const ZipEntry *e;
    const unsigned char *p;
    uint64_t dstart;
    uint16_t nlen, elen;
    unsigned char *out = NULL;
    size_t outlen = 0;

    if (len_out) *len_out = 0;
    if (!z || index >= z->count) return NULL;
    e = &z->ents[index];
    if (e->is_dir) return empty_buffer(len_out);
    if (e->method != 0 && e->method != 8) return NULL;   /* 不支持的方法 */
    if (e->uncomp > ZIP_MAX_ENTRY || e->comp > ZIP_MAX_ENTRY) return NULL;

    p = z->data;
    if (e->lho + 30u > (uint64_t)z->size) return NULL;
    if (rd32(p + (size_t)e->lho) != 0x04034b50u) return NULL;
    nlen = rd16(p + (size_t)e->lho + 26);
    elen = rd16(p + (size_t)e->lho + 28);
    dstart = e->lho + 30u + (uint64_t)nlen + (uint64_t)elen;
    if (dstart > (uint64_t)z->size) return NULL;
    if (e->comp > (uint64_t)z->size - dstart) return NULL;

    if (e->method == 0) {
        if (e->comp > ZIP_MAX_ENTRY) return NULL;
        out = (unsigned char *)lc_alloc((size_t)e->comp + 1);
        if (!out) return NULL;
        memcpy(out, p + (size_t)dstart, (size_t)e->comp);
        outlen = (size_t)e->comp;
    } else {
        size_t cap;
        int rc = LC_INFLATE_BAD;
        int attempt;
        if (e->uncomp > 0) cap = (size_t)e->uncomp;
        else cap = (size_t)(e->comp * 4u + 1024u);
        if (cap == 0) cap = 64;
        for (attempt = 0; attempt < 40; attempt++) {
            out = (unsigned char *)lc_alloc(cap);
            if (!out) return NULL;
            rc = lc_inflate(p + (size_t)dstart, (size_t)e->comp, out, cap,
                            &outlen);
            if (rc == LC_INFLATE_OK) break;
            lc_free(out);
            out = NULL;
            if (rc != LC_INFLATE_FULL) return NULL;   /* 真损坏 */
            if (cap >= (size_t)ZIP_MAX_ENTRY) return NULL;
            cap *= 2;
        }
        if (rc != LC_INFLATE_OK) return NULL;
    }

    if (lc_crc32(out, outlen) != e->crc) {     /* 完整性校验 */
        lc_free(out);
        return NULL;
    }
    if (len_out) *len_out = outlen;
    return out;
}

int zip_read_at(const LcZip *z, size_t i, unsigned char **out, size_t *outlen) {
    unsigned char *b;
    if (out) *out = NULL;
    if (outlen) *outlen = 0;
    if (!out || !outlen) return -1;
    b = read_entry(z, i, outlen);
    if (!b) { *outlen = 0; return -1; }
    *out = b;
    return 0;
}

int zip_read_by_name(const LcZip *z, const char *name_utf8,
                     unsigned char **out, size_t *outlen) {
    int i;
    if (out) *out = NULL;
    if (outlen) *outlen = 0;
    if (!z || !name_utf8 || !out || !outlen) return -1;
    i = zip_find(z, name_utf8);
    if (i < 0) return -1;
    return zip_read_at(z, (size_t)i, out, outlen);
}

/* ======================================================== ZIP 写入 */

typedef struct {
    char          *name;
    unsigned char *data;
    size_t         len;
    uint32_t       crc;
    uint64_t       off;
    uint16_t       flags;
    int            is_dir;
} Zent;

struct LcZipWriter {
    Zent  *e;
    size_t n, cap;
};

typedef struct {
    unsigned char *p;
    size_t         len, cap;
    int            oom;
} Buf;

static int buf_reserve(Buf *b, size_t extra) {
    size_t cap;
    unsigned char *np;
    if (b->oom) return -1;
    if (b->len + extra <= b->cap) return 0;
    cap = b->cap ? b->cap : 4096;
    while (cap < b->len + extra) cap *= 2;
    np = (unsigned char *)realloc(b->p, cap);
    if (!np) { b->oom = 1; return -1; }
    b->p = np;
    b->cap = cap;
    return 0;
}

static void buf_put(Buf *b, const void *d, size_t n) {
    if (n == 0) return;
    if (buf_reserve(b, n) != 0) return;
    memcpy(b->p + b->len, d, n);
    b->len += n;
}

static void buf_u16(Buf *b, uint16_t v) {
    unsigned char t[2];
    t[0] = (unsigned char)(v & 0xFFu);
    t[1] = (unsigned char)((v >> 8) & 0xFFu);
    buf_put(b, t, 2);
}

static void buf_u32(Buf *b, uint32_t v) {
    unsigned char t[4];
    t[0] = (unsigned char)(v & 0xFFu);
    t[1] = (unsigned char)((v >> 8) & 0xFFu);
    t[2] = (unsigned char)((v >> 16) & 0xFFu);
    t[3] = (unsigned char)((v >> 24) & 0xFFu);
    buf_put(b, t, 4);
}

static void dos_now(uint16_t *t, uint16_t *d) {
    SYSTEMTIME st;
    int year;
    GetLocalTime(&st);
    year = st.wYear < 1980 ? 1980 : st.wYear;
    *t = (uint16_t)((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
    *d = (uint16_t)(((year - 1980) << 9) | (st.wMonth << 5) | st.wDay);
}

LcZipWriter *zip_writer_create(void) {
    LcZipWriter *w = (LcZipWriter *)lc_alloc(sizeof *w);
    if (w) memset(w, 0, sizeof *w);
    return w;
}

int zip_writer_add(LcZipWriter *w, const char *utf8_name,
                   const void *data, size_t len) {
    Zent *e;
    size_t nl, i;
    char *nm;
    unsigned char *cp = NULL;
    int non_ascii = 0;

    if (!w || !utf8_name) return -1;
    nl = strlen(utf8_name);
    if (nl == 0 || nl > 0xFFFFu) return -1;
    if (len && !data) return -1;
#if SIZE_MAX > 0xFFFFFFFFu
    if (len > 0xFFFFFFFFu) return -1;         /* 不做 ZIP64 写入 */
#endif
    if (w->n == w->cap) {
        size_t ncap = w->cap ? w->cap * 2 : 16;
        Zent *ne = (Zent *)realloc(w->e, ncap * sizeof(Zent));
        if (!ne) return -1;
        memset(ne + w->cap, 0, (ncap - w->cap) * sizeof(Zent));
        w->e = ne;
        w->cap = ncap;
    }
    nm = (char *)lc_alloc(nl + 1);
    if (!nm) return -1;
    for (i = 0; i < nl; i++) {
        unsigned char c = (unsigned char)utf8_name[i];
        nm[i] = (c == '\\') ? '/' : (char)c;
        if (c >= 0x80u) non_ascii = 1;
    }
    nm[nl] = 0;
    if (len) {
        cp = (unsigned char *)lc_alloc(len);
        if (!cp) { lc_free(nm); return -1; }
        memcpy(cp, data, len);
    }
    e = &w->e[w->n];
    e->name   = nm;
    e->data   = cp;
    e->len    = len;
    e->crc    = lc_crc32(cp, len);
    e->flags  = (uint16_t)(non_ascii ? 0x0800u : 0u);
    e->is_dir = (nl > 0 && nm[nl - 1] == '/') ? 1 : 0;
    w->n++;
    return 0;
}

int zip_writer_add_w(LcZipWriter *w, const wchar_t *name,
                     const void *data, size_t len) {
    char *u;
    int rc;
    if (!name) return -1;
    u = lc_wcs_to_utf8(name);
    if (!u) return -1;
    rc = zip_writer_add(w, u, data, len);
    lc_free(u);
    return rc;
}

void zip_writer_free(LcZipWriter *w) {
    size_t i;
    if (!w) return;
    for (i = 0; i < w->n; i++) {
        lc_free(w->e[i].name);
        lc_free(w->e[i].data);
    }
    lc_free(w->e);
    lc_free(w);
}

static int zw_build(LcZipWriter *w, unsigned char **outp, size_t *outlen) {
    Buf b;
    uint16_t t, d;
    size_t i, cd_off, cd_size;

    if (!w || !outp || !outlen) return -1;
    *outp = NULL;
    *outlen = 0;
    if (w->n > 0xFFFFu) return -1;            /* 不做 ZIP64 写入 */

    memset(&b, 0, sizeof b);
    dos_now(&t, &d);

    for (i = 0; i < w->n; i++) {
        Zent *e = &w->e[i];
        size_t nl = strlen(e->name);
        e->off = (uint64_t)b.len;
#if SIZE_MAX > 0xFFFFFFFFu
        if (e->off > 0xFFFFFFFFu) { free(b.p); return -1; }
#endif
        buf_u32(&b, 0x04034b50u);             /* 本地文件头 */
        buf_u16(&b, 20);                      /* version needed */
        buf_u16(&b, e->flags);
        buf_u16(&b, 0);                       /* method = stored */
        buf_u16(&b, t);
        buf_u16(&b, d);
        buf_u32(&b, e->crc);
        buf_u32(&b, (uint32_t)e->len);
        buf_u32(&b, (uint32_t)e->len);
        buf_u16(&b, (uint16_t)nl);
        buf_u16(&b, 0);                       /* extra len */
        buf_put(&b, e->name, nl);
        buf_put(&b, e->data, e->len);
        if (b.oom) { free(b.p); return -1; }
    }

    cd_off = b.len;
    for (i = 0; i < w->n; i++) {
        Zent *e = &w->e[i];
        size_t nl = strlen(e->name);
        buf_u32(&b, 0x02014b50u);             /* 中央目录条目 */
        buf_u16(&b, 20);                      /* version made by (MS-DOS) */
        buf_u16(&b, 20);                      /* version needed */
        buf_u16(&b, e->flags);
        buf_u16(&b, 0);                       /* method = stored */
        buf_u16(&b, t);
        buf_u16(&b, d);
        buf_u32(&b, e->crc);
        buf_u32(&b, (uint32_t)e->len);
        buf_u32(&b, (uint32_t)e->len);
        buf_u16(&b, (uint16_t)nl);
        buf_u16(&b, 0);                       /* extra */
        buf_u16(&b, 0);                       /* comment */
        buf_u16(&b, 0);                       /* disk start */
        buf_u16(&b, 0);                       /* internal attrs */
        buf_u32(&b, e->is_dir ? 0x10u : 0u);  /* DOS attrs in low byte */
        buf_u32(&b, (uint32_t)e->off);
        buf_put(&b, e->name, nl);
        if (b.oom) { free(b.p); return -1; }
    }

    cd_size = b.len - cd_off;

    buf_u32(&b, 0x06054b50u);                 /* EOCD */
    buf_u16(&b, 0);
    buf_u16(&b, 0);
    buf_u16(&b, (uint16_t)w->n);
    buf_u16(&b, (uint16_t)w->n);
    buf_u32(&b, (uint32_t)cd_size);           /* 中央目录字节数 */
    buf_u32(&b, (uint32_t)cd_off);            /* 中央目录偏移 */
    buf_u16(&b, 0);                           /* comment len */

    if (b.oom) { free(b.p); return -1; }
    *outp = b.p;
    *outlen = b.len;
    return 0;
}

int zip_writer_data(LcZipWriter *w, unsigned char **out, size_t *outlen) {
    return zw_build(w, out, outlen);
}

int zip_writer_save(LcZipWriter *w, const wchar_t *path) {
    unsigned char *buf = NULL;
    size_t len = 0;
    HANDLE h;
    DWORD written = 0;
    BOOL ok;

    if (!path) return -1;
    if (zw_build(w, &buf, &len) != 0) return -1;

    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { free(buf); return -1; }
    ok = WriteFile(h, buf, (DWORD)len, &written, NULL);
    if (ok) ok = (written == (DWORD)len);
    if (ok) ok = FlushFileBuffers(h);
    CloseHandle(h);
    free(buf);
    return ok ? 0 : -1;
}
