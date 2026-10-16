/* ==========================================================================
 * test_meta.c —— EXIF / ID3 元数据解析自检
 *
 * EXIF 和 ID3 都是**手写解析器**，这类代码"能编译"证明不了任何事 ——
 * 偏移算错一个字节，读出来的就是另一个字段的内容，还不会报错。
 * 所以测试里直接**手工构造真实的字节结构**，再读回来比对：
 *
 *   · 小端和大端两种字节序（不是所有相机都是小端，这是最容易写错的地方）
 *   · 值内联在条目里（≤4 字节）和值放在别处（偏移）两种存法
 *   · RATIONAL 的三种呈现（曝光时间 / 光圈 / 焦距）
 *   · 字段齐全、字段缺失、完全没有 EXIF 三种情况
 *   · 截断文件、越界偏移 —— 这类文件解析器必须当没有，而不是崩
 *   · ID3v2.3 的 UTF-16 中文标签、ID3v1 的固定偏移老格式
 * ========================================================================== */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "lc.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (cond) { g_pass++; }                                                 \
        else {                                                                  \
            g_fail++;                                                           \
            wprintf(L"  FAIL  ");                                               \
            wprintf(__VA_ARGS__);                                               \
            wprintf(L"   [line %d]\n", __LINE__);                               \
        }                                                                       \
    } while (0)

static wchar_t g_root[MAX_PATH * 2];

/* ==================================================== 文件写入 ======== */

static void write_bytes(const wchar_t *path, const void *data, size_t len) {
    wchar_t *lp = lc_long_path(path);
    HANDLE h = CreateFileW(lp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, data, (DWORD)len, &w, NULL);
    CloseHandle(h);
}

/* ==================================================== EXIF 构造器 ==== */

typedef struct {
    unsigned char *buf;
    size_t         len, cap;
    int            be;
} Blob;

static void blob_init(Blob *b, int be) {
    b->cap = 65536;
    b->buf = (unsigned char *)calloc(1, b->cap);
    b->len = 0;
    b->be = be;
}

static void blob_bytes(Blob *b, const void *d, size_t n) {
    if (b->len + n > b->cap) return;
    memcpy(b->buf + b->len, d, n);
    b->len += n;
}

static void blob_u8(Blob *b, unsigned v) {
    unsigned char c = (unsigned char)v;
    blob_bytes(b, &c, 1);
}

static void blob_u16(Blob *b, unsigned v) {
    unsigned char c[2];
    if (b->be) { c[0] = (unsigned char)(v >> 8); c[1] = (unsigned char)v; }
    else       { c[0] = (unsigned char)v; c[1] = (unsigned char)(v >> 8); }
    blob_bytes(b, c, 2);
}

static void blob_u32(Blob *b, unsigned v) {
    unsigned char c[4];
    if (b->be) { c[0]=(unsigned char)(v>>24); c[1]=(unsigned char)(v>>16);
                 c[2]=(unsigned char)(v>>8);  c[3]=(unsigned char)v; }
    else       { c[0]=(unsigned char)v;       c[1]=(unsigned char)(v>>8);
                 c[2]=(unsigned char)(v>>16); c[3]=(unsigned char)(v>>24); }
    blob_bytes(b, c, 4);
}

static void blob_at_u32(Blob *b, size_t off, unsigned v) {
    unsigned char c[4];
    if (b->be) { c[0]=(unsigned char)(v>>24); c[1]=(unsigned char)(v>>16);
                 c[2]=(unsigned char)(v>>8);  c[3]=(unsigned char)v; }
    else       { c[0]=(unsigned char)v;       c[1]=(unsigned char)(v>>8);
                 c[2]=(unsigned char)(v>>16); c[3]=(unsigned char)(v>>24); }
    if (off + 4 <= b->cap) memcpy(b->buf + off, c, 4);
}

/*
 * 当前构造用的字节序。
 *
 * mk_short / mk_long / mk_rational 都必须跟着它走 —— 一开始这里写死成小端了，
 * 结果大端测试读出 16390（= 0x4006）而不是 1600（= 0x0640）：字节反了。
 * 那是**测试自己**的 bug，不是解析器的 —— 但这个 bug 很有意思：
 * 它让"解析器正确处理大端"这件事看起来是坏的，而实际上写测试的代码才是坏的。
 */
static int g_wr_be = 0;

static void wr_u16(unsigned char *d, unsigned v) {
    if (g_wr_be) { d[0]=(unsigned char)(v>>8); d[1]=(unsigned char)v; }
    else         { d[0]=(unsigned char)v;      d[1]=(unsigned char)(v>>8); }
}
static void wr_u32(unsigned char *d, unsigned v) {
    if (g_wr_be) { d[0]=(unsigned char)(v>>24); d[1]=(unsigned char)(v>>16);
                   d[2]=(unsigned char)(v>>8);  d[3]=(unsigned char)v; }
    else         { d[0]=(unsigned char)v;       d[1]=(unsigned char)(v>>8);
                   d[2]=(unsigned char)(v>>16); d[3]=(unsigned char)(v>>24); }
}

/* 一个 IFD 条目 */
typedef struct {
    unsigned tag, type, count;
    unsigned char payload[64];
    size_t        plen;
    int           is_offset;    /* 1 = 值放在数据区（这里都用内联，除日期字符串） */
} Entry;

static Entry mk_ascii(unsigned tag, const char *s) {
    Entry e; memset(&e, 0, sizeof(e));
    e.tag = tag; e.type = 2;
    e.plen = strlen(s) + 1;
    e.count = (unsigned)e.plen;
    memcpy(e.payload, s, e.plen);
    return e;
}
static Entry mk_short(unsigned tag, unsigned v) {
    Entry e; memset(&e, 0, sizeof(e));
    e.tag = tag; e.type = 3; e.count = 1;
    wr_u16(e.payload, v);
    e.plen = 2;
    return e;
}
static Entry mk_rational(unsigned tag, unsigned num, unsigned den) {
    Entry e; memset(&e, 0, sizeof(e));
    e.tag = tag; e.type = 5; e.count = 1;
    wr_u32(e.payload, num);
    wr_u32(e.payload + 4, den);
    e.plen = 8;
    return e;
}

static int cmp_entry(const void *a, const void *b) {
    return (int)((const Entry *)a)->tag - (int)((const Entry *)b)->tag;
}

/*
 * 把一堆条目拼成完整的 EXIF 块（含 "Exif\0\0" 前缀）。
 *
 * 布局：[Exif\0\0] [TIFF头 8B] [IFD0] [ExifIFD] [数据区]
 * 所有偏移都相对 TIFF 头，这是 EXIF 最容易搞错的一处。
 */
static size_t build_exif_blob(unsigned char *out, int be,
                              Entry *ifd0, int n0, Entry *exif, int ne) {
    Blob b; blob_init(&b, be);

    blob_bytes(&b, "Exif\0\0", 6);
    size_t tiff_base = b.len;                 /* TIFF 头从这里开始 */

    blob_bytes(&b, be ? "MM" : "II", 2);
    blob_u16(&b, 42);
    size_t ifd0_ptr_pos = b.len;              /* IFD0 偏移要回填 */
    blob_u32(&b, 0);

    /* IFD0 */
    size_t ifd0_off = b.len - tiff_base;
    qsort(ifd0, n0, sizeof(Entry), cmp_entry);
    /* 给 ExifIFD 指针留一个位置 */
    blob_u16(&b, (unsigned)(n0 + 1));

    size_t exif_ptr_pos = 0;
    for (int i = 0; i < n0; ++i) {
        blob_u16(&b, ifd0[i].tag);
        blob_u16(&b, ifd0[i].type);
        blob_u32(&b, ifd0[i].count);
        if (ifd0[i].plen <= 4) {
            unsigned char pad[4] = {0,0,0,0};
            memcpy(pad, ifd0[i].payload, ifd0[i].plen);
            blob_bytes(&b, pad, 4);
        } else {
            blob_u32(&b, 0);                  /* 稍后回填 */
        }
    }
    /* ExifIFD 指针条目（tag 0x8769, type LONG） */
    exif_ptr_pos = b.len;
    blob_u16(&b, 0x8769);
    blob_u16(&b, 4);
    blob_u32(&b, 1);
    blob_u32(&b, 0);                          /* 回填 */
    blob_u32(&b, 0);                          /* 下一个 IFD = 0 */

    size_t exif_off = b.len - tiff_base;
    qsort(exif, ne, sizeof(Entry), cmp_entry);
    blob_u16(&b, (unsigned)ne);
    for (int i = 0; i < ne; ++i) {
        blob_u16(&b, exif[i].tag);
        blob_u16(&b, exif[i].type);
        blob_u32(&b, exif[i].count);
        if (exif[i].plen <= 4) {
            unsigned char pad[4] = {0,0,0,0};
            memcpy(pad, exif[i].payload, exif[i].plen);
            blob_bytes(&b, pad, 4);
        } else {
            blob_u32(&b, 0);                  /* 稍后回填 */
        }
    }
    blob_u32(&b, 0);

    /* 数据区：把 >4 字节的值放这里，回填偏移 */
    size_t patch0 = 0, patche = 0;
    size_t scan;

    /* 重新走一遍 IFD0，给长值分配位置 */
    {
        size_t pos = 2 + (size_t)(n0 + 1) * 12;
        /* 条目起始位置 = ifd0 起点 + 2 */
        size_t entry_start = 0;
        /* 上面 blob 里 ifd0 的起点：ifd0_off + tiff_base */
        entry_start = tiff_base + ifd0_off + 2;
        for (int i = 0; i < n0; ++i) {
            if (ifd0[i].plen > 4) {
                size_t here = tiff_base + (size_t)(b.len - tiff_base);
                blob_at_u32(&b, entry_start + (size_t)i * 12 + 8,
                            (unsigned)(b.len - tiff_base));
                blob_bytes(&b, ifd0[i].payload, ifd0[i].plen);
                if (b.len % 2) blob_u8(&b, 0);
                (void)here;
            }
        }
        (void)pos;
    }
    {
        size_t entry_start = tiff_base + exif_off + 2;
        for (int i = 0; i < ne; ++i) {
            if (exif[i].plen > 4) {
                blob_at_u32(&b, entry_start + (size_t)i * 12 + 8,
                            (unsigned)(b.len - tiff_base));
                blob_bytes(&b, exif[i].payload, exif[i].plen);
                if (b.len % 2) blob_u8(&b, 0);
            }
        }
    }
    (void)scan; (void)patch0; (void)patche;

    /* 回填两个指针 */
    blob_at_u32(&b, ifd0_ptr_pos, (unsigned)ifd0_off);
    blob_at_u32(&b, exif_ptr_pos + 8, (unsigned)exif_off);

    memcpy(out, b.buf, b.len);
    size_t n = b.len;
    free(b.buf);
    return n;
}

/* 把 EXIF 块包成 JPEG：SOI + APP1 段 + 一点图像数据 + EOI */
static size_t wrap_jpeg(unsigned char *out, const unsigned char *exif, size_t exif_len) {
    size_t p = 0;
    out[p++] = 0xFF; out[p++] = 0xD8;                     /* SOI */
    out[p++] = 0xFF; out[p++] = 0xE1;                     /* APP1 */
    unsigned seg = (unsigned)(exif_len + 2);
    out[p++] = (unsigned char)(seg >> 8);
    out[p++] = (unsigned char)(seg & 0xFF);
    memcpy(out + p, exif, exif_len); p += exif_len;
    /* 一段假的扫描数据，够让解析器认为"到图像数据了" */
    out[p++] = 0xFF; out[p++] = 0xDA;
    for (int i = 0; i < 64; ++i) out[p++] = (unsigned char)(i * 3);
    out[p++] = 0xFF; out[p++] = 0xD9;                     /* EOI */
    return p;
}

/* ==================================================== 测试 1 ========== */

static void test_exif_little_endian(void) {
    wprintf(L"[1] EXIF 小端：齐全的字段\n");

    Entry ifd0[] = {
        mk_ascii(0x010F, "Canon"),
        mk_ascii(0x0110, "Canon EOS R6"),
        mk_ascii(0x0132, "2026:03:15 09:00:00"),
    };
    Entry exif[] = {
        mk_ascii(0x9003, "2026:03:15 14:30:22"),
        mk_short(0x8827, 400),
        mk_rational(0x829D, 28, 10),      /* F2.8 */
        mk_rational(0x829A, 1, 250),      /* 1/250 */
        mk_rational(0x920A, 50, 1),       /* 50mm */
        mk_ascii(0xA434, "RF50mm F1.8 STM"),
    };

    unsigned char exifbuf[8192];
    size_t el = build_exif_blob(exifbuf, 0, ifd0, 3, exif, 6);

    unsigned char jpg[16384];
    size_t jl = wrap_jpeg(jpg, exifbuf, el);

    wchar_t pa[MAX_PATH * 2];
    swprintf(pa, MAX_PATH * 2, L"%ls\\le.jpg", g_root);
    write_bytes(pa, jpg, jl);

    MediaMeta m;
    CHECK(meta_read(pa, &m) == 0, L"应读到元数据");
    CHECK(m.has_exif, L"应识别为有 EXIF");
    CHECK(wcscmp(m.exif_make, L"Canon") == 0, L"厂商应为 Canon，实际「%ls」", m.exif_make);
    CHECK(wcscmp(m.exif_model, L"Canon EOS R6") == 0,
          L"机型应为 Canon EOS R6，实际「%ls」", m.exif_model);
    CHECK(wcscmp(m.exif_date, L"2026:03:15 14:30:22") == 0,
          L"拍摄时间应为原始形式，实际「%ls」", m.exif_date);
    CHECK(wcscmp(m.exif_iso, L"400") == 0, L"ISO 应为 400，实际「%ls」", m.exif_iso);
    CHECK(wcscmp(m.exif_fnum, L"2.8") == 0, L"光圈应为 2.8，实际「%ls」", m.exif_fnum);
    CHECK(wcscmp(m.exif_exposure, L"1/250") == 0,
          L"曝光应为 1/250，实际「%ls」", m.exif_exposure);
    CHECK(wcscmp(m.exif_focal, L"50") == 0, L"焦距应为 50，实际「%ls」", m.exif_focal);
    CHECK(wcscmp(m.exif_lens, L"RF50mm F1.8 STM") == 0,
          L"镜头应正确，实际「%ls」", m.exif_lens);

    /*
     * 关键：IFD0 里的 DateTime 是"文件修改时间"，不能被当成拍摄时间用。
     *
     * 这条断言一开始写反了 —— 写成了"应该等于 IFD0 的值"，
     * 正好和想验证的事情相反，于是测试红着却指向了错误的方向。
     */
    CHECK(wcscmp(m.exif_date, L"2026:03:15 09:00:00") != 0,
          L"应优先用 DateTimeOriginal，而不是 IFD0 的 DateTime，实际「%ls」",
          m.exif_date);
}

/* ==================================================== 测试 2 ========== */

static void test_exif_big_endian(void) {
    wprintf(L"[2] EXIF 大端（字节序反过来，最容易写错）\n");
    g_wr_be = 1;      /* 必须在构造条目之前设好 */

    Entry ifd0[] = {
        mk_ascii(0x010F, "NIKON CORPORATION"),
        mk_ascii(0x0110, "NIKON Z6_2"),
    };
    Entry exif[] = {
        mk_ascii(0x9003, "2025:07:04 18:05:09"),
        mk_short(0x8827, 1600),
        mk_rational(0x829D, 40, 10),      /* F4.0 */
        mk_rational(0x829A, 1, 60),       /* 1/60 */
        mk_rational(0x920A, 85, 1),
    };

    unsigned char exifbuf[8192];
    size_t el = build_exif_blob(exifbuf, 1, ifd0, 2, exif, 5);
    unsigned char jpg[16384];
    size_t jl = wrap_jpeg(jpg, exifbuf, el);

    wchar_t pa[MAX_PATH * 2];
    swprintf(pa, MAX_PATH * 2, L"%ls\\be.jpg", g_root);
    write_bytes(pa, jpg, jl);

    MediaMeta m;
    CHECK(meta_read(pa, &m) == 0, L"应读到元数据");
    CHECK(wcscmp(m.exif_model, L"NIKON Z6_2") == 0,
          L"大端机型应正确，实际「%ls」", m.exif_model);
    CHECK(wcscmp(m.exif_date, L"2025:07:04 18:05:09") == 0,
          L"大端日期应正确，实际「%ls」", m.exif_date);
    CHECK(wcscmp(m.exif_iso, L"1600") == 0,
          L"大端 ISO 应正确，实际「%ls」", m.exif_iso);
    CHECK(wcscmp(m.exif_fnum, L"4.0") == 0,
          L"大端光圈应正确，实际「%ls」", m.exif_fnum);
    CHECK(wcscmp(m.exif_focal, L"85") == 0,
          L"大端焦距应正确，实际「%ls」", m.exif_focal);
}

/* ==================================================== 测试 3 ========== */

static void test_missing_and_absent(void) {
    wprintf(L"[3] 字段缺失 / 完全没有 EXIF\n");

    /* 只有日期，别的都没有 */
    Entry ifd0[] = { mk_ascii(0x010F, "Apple") };
    Entry exif[] = { mk_ascii(0x9003, "2024:01:01 10:00:00") };
    unsigned char exifbuf[4096];
    size_t el = build_exif_blob(exifbuf, 0, ifd0, 1, exif, 1);
    unsigned char jpg[8192];
    size_t jl = wrap_jpeg(jpg, exifbuf, el);

    wchar_t pa[MAX_PATH * 2];
    swprintf(pa, MAX_PATH * 2, L"%ls\\min.jpg", g_root);
    write_bytes(pa, jpg, jl);

    MediaMeta m;
    CHECK(meta_read(pa, &m) == 0, L"应读到元数据");
    CHECK(m.exif_date[0] != 0, L"日期应该读到了");
    CHECK(m.exif_iso[0] == 0, L"没有的字段应为空串");
    CHECK(m.exif_model[0] == 0, L"没有的机型应为空串");

    /* meta_lookup 对不存在的字段必须返回 NULL，而不是空串 */
    wchar_t buf[256];
    CHECK(meta_lookup(&m, L"exif", L"iso", NULL, buf, 256) == NULL,
          L"不存在的字段应返回 NULL");
    CHECK(meta_lookup(&m, L"exif", L"date", NULL, buf, 256) != NULL,
          L"存在的日期字段应返回非 NULL");
    CHECK(wcscmp(buf, L"2024-01-01") == 0,
          L"日期默认格式化应为 YYYY-MM-DD，实际「%ls」", buf);

    /* 自定义日期格式 */
    CHECK(meta_lookup(&m, L"exif", L"date", L"%Y%m%d", buf, 256) != NULL, L"应返回");
    CHECK(wcscmp(buf, L"20240101") == 0, L"%Y%m%d 应为 20240101，实际「%ls」", buf);
    CHECK(meta_lookup(&m, L"exif", L"date", L"%Y/%m", buf, 256) != NULL, L"应返回");
    CHECK(wcscmp(buf, L"2024/01") == 0, L"%Y/%m 应为 2024/01，实际「%ls」", buf);

    /* 不认识的命名空间 */
    CHECK(meta_lookup(&m, L"vcs", L"date", NULL, buf, 256) == NULL,
          L"不认识的命名空间应返回 NULL");

    /* 完全没有 EXIF 的 JPEG：返回 1（读到文件但没元数据） */
    unsigned char plain[] = { 0xFF,0xD8, 0xFF,0xE0, 0x00,0x10, 'J','F','I','F',0,
                              1,1,0, 0,1,0,1, 0,0, 0xFF,0xD9 };
    wchar_t pb[MAX_PATH * 2];
    swprintf(pb, MAX_PATH * 2, L"%ls\\plain.jpg", g_root);
    write_bytes(pb, plain, sizeof(plain));
    MediaMeta m2;
    CHECK(meta_read(pb, &m2) == 1, L"没有 EXIF 的 JPEG 应返回 1");
    CHECK(!m2.has_exif && !m2.has_id3, L"应标记为没有元数据");
}

/* ==================================================== 测试 4 ========== */

static void test_malformed(void) {
    wprintf(L"[4] 畸形 / 截断文件：必须当没有，不能崩\n");

    wchar_t p[MAX_PATH * 2];
    MediaMeta m;

    /* 空文件 */
    swprintf(p, MAX_PATH * 2, L"%ls\\empty.jpg", g_root);
    write_bytes(p, "", 0);
    CHECK(meta_read(p, &m) == -1, L"空文件应返回 -1");

    /* 只有 SOI 就断了 */
    { unsigned char d[] = { 0xFF, 0xD8 };
      swprintf(p, MAX_PATH * 2, L"%ls\\trunc1.jpg", g_root);
      write_bytes(p, d, sizeof(d));
      CHECK(meta_read(p, &m) == 1, L"截断的 SOI 应返回 1（无元数据）"); }

    /* APP1 段长度声称得很大，超出实际文件 */
    { unsigned char d[] = { 0xFF,0xD8, 0xFF,0xE1, 0xFF,0xFF, 'E','x','i','f',0,0,
                            0x49,0x49,0x2A,0 };
      swprintf(p, MAX_PATH * 2, L"%ls\\trunc2.jpg", g_root);
      write_bytes(p, d, sizeof(d));
      CHECK(meta_read(p, &m) == 1, L"APP1 长度越界应安全返回"); }

    /* TIFF 头正确但 IFD 偏移指向文件外 */
    { unsigned char d[] = { 0xFF,0xD8, 0xFF,0xE1, 0x00,0x14, 'E','x','i','f',0,0,
                            0x49,0x49,0x2A,0x00, 0xFF,0xFF,0xFF,0xFF,
                            0xFF,0xD9 };
      swprintf(p, MAX_PATH * 2, L"%ls\\badptr.jpg", g_root);
      write_bytes(p, d, sizeof(d));
      CHECK(meta_read(p, &m) == 1, L"IFD 偏移越界应安全返回"); }

    /* 字节序标记是垃圾 */
    { unsigned char d[] = { 0xFF,0xD8, 0xFF,0xE1, 0x00,0x14, 'E','x','i','f',0,0,
                            0x58,0x58,0x2A,0x00, 0x08,0,0,0, 0xFF,0xD9 };
      swprintf(p, MAX_PATH * 2, L"%ls\\badendian.jpg", g_root);
      write_bytes(p, d, sizeof(d));
      CHECK(meta_read(p, &m) == 1, L"非法字节序应安全返回"); }

    /* 不是 JPEG 也不是 MP3 —— 随便一段二进制 */
    { unsigned char d[512];
      for (int i = 0; i < 512; ++i) d[i] = (unsigned char)(i * 7);
      swprintf(p, MAX_PATH * 2, L"%ls\\junk.bin", g_root);
      write_bytes(p, d, sizeof(d));
      CHECK(meta_read(p, &m) == 1, L"随机二进制应安全返回"); }

    /* 不存在的文件 */
    swprintf(p, MAX_PATH * 2, L"%ls\\nope.jpg", g_root);
    CHECK(meta_read(p, &m) == -1, L"不存在的文件应返回 -1");
}

/* ==================================================== 测试 5 ========== */

/*
 * 同步安全整数：每字节只用低 7 位，4 个字节装 28 位。
 *
 * 一开始这里写成"先算出一个 32 位值再按普通整数拆字节"，
 * 对于 256 会拆成 [0,0,1,0]，而正确的编码是 [0,0,2,0] —— 差了一倍。
 * 直接产出 4 个字节就没有这个中间步骤可以搞错。
 */
static void put_synchsafe(unsigned char *d, unsigned n) {
    d[0] = (unsigned char)((n >> 21) & 0x7F);
    d[1] = (unsigned char)((n >> 14) & 0x7F);
    d[2] = (unsigned char)((n >> 7) & 0x7F);
    d[3] = (unsigned char)(n & 0x7F);
}

static void put_be32(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

/* 造一个 UTF-16LE + BOM 的文本帧载荷 */
static size_t mk_text_payload(unsigned char *out, const wchar_t *text) {
    size_t n = 0;
    out[n++] = 0x01;                                  /* 编码 = UTF-16 带 BOM */
    out[n++] = 0xFF; out[n++] = 0xFE;                 /* BOM */
    for (const wchar_t *p = text; *p; ++p) {
        out[n++] = (unsigned char)(*p & 0xFF);
        out[n++] = (unsigned char)((*p >> 8) & 0xFF);
    }
    out[n++] = 0; out[n++] = 0;
    return n;
}

static void test_id3v2(void) {
    wprintf(L"[5] ID3v2.3：UTF-16 中文标签\n");

    unsigned char frames[4096];
    size_t fn = 0;

    struct { const char *id; const wchar_t *val; } items[] = {
        { "TIT2", L"夜曲" },
        { "TPE1", L"周杰伦" },
        { "TALB", L"十一月的萧邦" },
        { "TYER", L"2005" },
        { "TRCK", L"1" },
        { "TCON", L"Pop" },
    };

    for (int i = 0; i < 6; ++i) {
        unsigned char payload[512];
        /* frame id 是 ASCII，用 wchar 传进来再转不太方便，这里直接拼 */
        memcpy(frames + fn, items[i].id, 4); fn += 4;
        size_t pl = mk_text_payload(payload, items[i].val);
        put_be32(frames + fn, (unsigned)pl); fn += 4;
        frames[fn++] = 0; frames[fn++] = 0;               /* flags */
        memcpy(frames + fn, payload, pl); fn += pl;
    }

    unsigned char mp3[8192];
    size_t p = 0;
    memcpy(mp3 + p, "ID3", 3); p += 3;
    mp3[p++] = 3; mp3[p++] = 0; mp3[p++] = 0;             /* v2.3, 无扩展头 */
    put_synchsafe(mp3 + p, (unsigned)fn);
    p += 4;
    memcpy(mp3 + p, frames, fn); p += fn;
    /* 一段假音频数据 */
    for (int i = 0; i < 128; ++i) mp3[p++] = (unsigned char)i;

    wchar_t pa[MAX_PATH * 2];
    swprintf(pa, MAX_PATH * 2, L"%ls\\cn.mp3", g_root);
    write_bytes(pa, mp3, p);

    MediaMeta m;
    CHECK(meta_read(pa, &m) == 0, L"应读到 ID3");
    CHECK(m.has_id3, L"应识别为有 ID3");
    CHECK(wcscmp(m.id3_title, L"夜曲") == 0, L"标题应为「夜曲」，实际「%ls」", m.id3_title);
    CHECK(wcscmp(m.id3_artist, L"周杰伦") == 0,
          L"艺术家应为「周杰伦」，实际「%ls」", m.id3_artist);
    CHECK(wcscmp(m.id3_album, L"十一月的萧邦") == 0,
          L"专辑应为「十一月的萧邦」，实际「%ls」", m.id3_album);
    CHECK(wcscmp(m.id3_year, L"2005") == 0, L"年份应为 2005，实际「%ls」", m.id3_year);
    CHECK(wcscmp(m.id3_track, L"1") == 0, L"音轨应为 1，实际「%ls」", m.id3_track);

    /* 通过 meta_lookup 取的路径也要对 */
    wchar_t buf[256];
    CHECK(meta_lookup(&m, L"id3", L"title", NULL, buf, 256) != NULL, L"应取到标题");
    CHECK(wcscmp(buf, L"夜曲") == 0, L"meta_lookup 取的标题应一致");
    CHECK(meta_lookup(&m, L"ID3", L"ARTIST", NULL, buf, 256) != NULL,
          L"命名空间与字段名应大小写不敏感");
    CHECK(meta_lookup(&m, L"exif", L"model", NULL, buf, 256) == NULL,
          L"这个文件没有 EXIF，查 exif:model 应返回 NULL");
}

/* ==================================================== 测试 6 ========== */

static size_t mk_v1_field(unsigned char *dst, const char *s, size_t n) {
    size_t L = strlen(s);
    if (L > n) L = n;
    memcpy(dst, s, L);
    memset(dst + L, 0, n - L);
    return n;
}

static void test_id3v1(void) {
    wprintf(L"[6] ID3v1：老格式，固定偏移\n");

    unsigned char mp3[2048];
    size_t p = 0;
    for (int i = 0; i < 800; ++i) mp3[p++] = (unsigned char)(i % 251);

    unsigned char *t = mp3 + p;
    memset(t, 0, 128);
    memcpy(t, "TAG", 3);
    mk_v1_field(t + 3,  "Lemon", 30);
    mk_v1_field(t + 33, "Yonezu", 30);
    mk_v1_field(t + 63, "BOOTLEG", 30);
    mk_v1_field(t + 93, "2018", 4);
    t[125] = 0;
    t[126] = 3;                        /* 音轨 3 */
    t[127] = 17;                       /* 流派 */
    p += 128;

    wchar_t pa[MAX_PATH * 2];
    swprintf(pa, MAX_PATH * 2, L"%ls\\v1.mp3", g_root);
    write_bytes(pa, mp3, p);

    MediaMeta m;
    CHECK(meta_read(pa, &m) == 0, L"应读到 ID3v1");
    CHECK(m.has_id3, L"应识别为有 ID3");
    CHECK(wcscmp(m.id3_title, L"Lemon") == 0, L"标题应为 Lemon，实际「%ls」", m.id3_title);
    CHECK(wcscmp(m.id3_artist, L"Yonezu") == 0, L"艺术家应为 Yonezu，实际「%ls」", m.id3_artist);
    CHECK(wcscmp(m.id3_year, L"2018") == 0, L"年份应为 2018，实际「%ls」", m.id3_year);
    CHECK(wcscmp(m.id3_track, L"3") == 0, L"音轨应为 3，实际「%ls」", m.id3_track);

    /* 没有 ID3 的 mp3 */
    unsigned char bare[512];
    bare[0] = 0xFF; bare[1] = 0xFB; bare[2] = 0x90; bare[3] = 0x64;
    memset(bare + 4, 0, sizeof(bare) - 4);
    swprintf(pa, MAX_PATH * 2, L"%ls\\bare.mp3", g_root);
    write_bytes(pa, bare, sizeof(bare));
    MediaMeta m2;
    CHECK(meta_read(pa, &m2) == 1, L"无标签的 mp3 应返回 1");
    CHECK(!m2.has_id3, L"不应标记为有 ID3");
}

/* ==================================================== main ============ */

int main(void) {
    SetConsoleOutputCP(CP_UTF8);

    wchar_t exe[MAX_PATH * 2], *slash;
    GetModuleFileNameW(NULL, exe, MAX_PATH * 2);
    slash = wcsrchr(exe, L'\\'); if (slash) *slash = 0;
    slash = wcsrchr(exe, L'\\'); if (slash) *slash = 0;

    swprintf(g_root, MAX_PATH * 2, L"%ls\\testtmp\\meta-%u", exe,
             (unsigned)GetTickCount64());
    lc_mkdir_p(g_root);

    wprintf(L"=== LocalCraft 元数据解析自检 ===\n");
    wprintf(L"工作目录：%ls\n\n", g_root);

    test_exif_little_endian();
    test_exif_big_endian();
    g_wr_be = 0;   /* 后面的测试都按小端构造 */
    test_missing_and_absent();
    test_malformed();
    test_id3v2();
    test_id3v1();

    wprintf(L"\n=== 通过 %d / 失败 %d ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
