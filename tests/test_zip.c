/* ==========================================================================
 * LocalCraft — tests/test_zip.c
 * ZIP 读写 + DEFLATE inflate 的验证程序
 *
 * ZIP 的公开 API 原型来自 src/lc.h（LcZip / LcZipWriter）。下面只补声明
 * src/zip.c 提供的扩展接口。
 * ========================================================================== */

#include "lc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------- zip.c 的扩展接口 */
uint32_t lc_crc32_update(uint32_t crc, const void *data, size_t len);
LcZip *zip_open_memory(const void *data, size_t len);
int    zip_find(const LcZip *z, const char *name_utf8);
int    zip_is_dir_at(const LcZip *z, size_t i);
int    zip_method_at(const LcZip *z, size_t i);
long long zip_size_at(const LcZip *z, size_t i);
int    zip_writer_add_w(LcZipWriter *w, const wchar_t *name,
                        const void *data, size_t len);

#define LC_INFLATE_OK    0
#define LC_INFLATE_BAD   1
#define LC_INFLATE_FULL  2

/* ============================================================ 断言 */

static int g_pass = 0;
static int g_fail = 0;
static int g_section_fail = 0;

static void ck(int cond, const char *what) {
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
        g_section_fail++;
        printf("    [FAIL] %s\n", what);
    }
}

static void section(const char *name) {
    g_section_fail = 0;
    printf("\n== %s ==\n", name);
}

static void section_end(const char *name) {
    printf("   %s: %s\n", name, g_section_fail ? "FAILED" : "ok");
}

/* ============================================================ 小工具 */

static unsigned char *slurp(const wchar_t *path, size_t *len_out) {
    HANDLE h;
    LARGE_INTEGER sz;
    DWORD got = 0;
    unsigned char *buf;
    *len_out = 0;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) {
        CloseHandle(h);
        return NULL;
    }
    buf = (unsigned char *)malloc((size_t)sz.QuadPart);
    if (!buf) { CloseHandle(h); return NULL; }
    if (!ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL) ||
        got != (DWORD)sz.QuadPart) {
        free(buf);
        CloseHandle(h);
        return NULL;
    }
    CloseHandle(h);
    *len_out = (size_t)sz.QuadPart;
    return buf;
}

/* 依次尝试 build\zipwork\<name> / zipwork\<name> / <name> */
static unsigned char *slurp_rel(const char *name, size_t *len_out) {
    static const char *dirs[] = { "build\\zipwork\\", "zipwork\\", "" };
    wchar_t wpath[512];
    char rel[512];
    size_t i;
    for (i = 0; i < 3; i++) {
        unsigned char *b;
        sprintf(rel, "%s%s", dirs[i], name);
        MultiByteToWideChar(CP_ACP, 0, rel, -1, wpath, 512);
        b = slurp(wpath, len_out);
        if (b) return b;
    }
    return NULL;
}

/* 读取包装：失败返回 NULL */
static unsigned char *rd_name(const LcZip *z, const char *name, size_t *n) {
    unsigned char *b = NULL;
    *n = 0;
    if (zip_read_by_name(z, name, &b, n) != 0) return NULL;
    return b;
}

static unsigned char *rd_at(const LcZip *z, size_t i, size_t *n) {
    unsigned char *b = NULL;
    *n = 0;
    if (zip_read_at(z, i, &b, n) != 0) return NULL;
    return b;
}

static void hexdump(const unsigned char *p, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) printf("%02X ", p[i]);
    printf("\n");
}

/* ============================================== 测试用固定 Huffman 编码器 */
/* 只用来生成"我们自己知道内容"的 deflate 流，覆盖全部长度/距离码。 */

typedef struct {
    unsigned char *p;
    size_t         len, cap;
    uint32_t       bb;
    int            bc;
} BW;

static void bw_init(BW *b) { memset(b, 0, sizeof *b); }
static void bw_free(BW *b) { free(b->p); b->p = NULL; b->len = b->cap = 0; }

static void bw_raw(BW *b, unsigned v) {
    if (b->len == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 1024;
        b->p = (unsigned char *)realloc(b->p, b->cap);
        if (!b->p) { printf("OOM\n"); exit(2); }
    }
    b->p[b->len++] = (unsigned char)(v & 0xFFu);
}

static void bw_bits(BW *b, uint32_t v, int n) {
    b->bb |= v << b->bc;
    b->bc += n;
    while (b->bc >= 8) {
        bw_raw(b, b->bb & 0xFFu);
        b->bb >>= 8;
        b->bc -= 8;
    }
}

static void bw_flush(BW *b) {
    if (b->bc > 0) {
        bw_raw(b, b->bb & 0xFFu);
        b->bb = 0;
        b->bc = 0;
    }
}

static uint32_t revbits(uint32_t c, int n) {
    uint32_t r = 0;
    while (n-- > 0) { r = (r << 1) | (c & 1u); c >>= 1; }
    return r;
}

/* 固定 Huffman 字面量/长度码（RFC1951 3.2.6），位反转后按 LSB-first 写出 */
static void bw_fixed(BW *b, int sym) {
    uint32_t c;
    int n;
    if (sym <= 143)       { n = 8; c = 0x30u + (uint32_t)sym; }
    else if (sym <= 255)  { n = 9; c = 0x190u + (uint32_t)(sym - 144); }
    else if (sym <= 279)  { n = 7; c = (uint32_t)(sym - 256); }
    else                  { n = 8; c = 0xC0u + (uint32_t)(sym - 280); }
    bw_bits(b, revbits(c, n), n);
}

/* 固定 Huffman 的距离码：定长 5 位，码值 == 距离符号 */
static void bw_fixed_dist(BW *b, int dsym) {
    bw_bits(b, revbits((uint32_t)dsym, 5), 5);
}

static const unsigned short T_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char T_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const unsigned short T_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const unsigned char T_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* 长度码 257+k，extra 位全 1；距离码 j，extra 位全 1。
 * 位序严格按 RFC1951：长度码 -> 长度 extra -> 距离码 -> 距离 extra */
static void emit_pair(BW *b, int k, int j, unsigned char *expect, size_t *en) {
    unsigned int len = T_len_base[k];
    unsigned int dist = T_dist_base[j];
    unsigned int i, e;
    bw_fixed(b, 257 + k);
    if (T_len_extra[k]) {
        e = (1u << T_len_extra[k]) - 1u;
        len += e;
        bw_bits(b, e, T_len_extra[k]);
    }
    bw_fixed_dist(b, j);
    if (T_dist_extra[j]) {
        e = (1u << T_dist_extra[j]) - 1u;
        dist += e;
        bw_bits(b, e, T_dist_extra[j]);
    }
    for (i = 0; i < len; i++) {
        expect[*en] = expect[*en - dist];
        (*en)++;
    }
}

/* ============================================================ 1. CRC-32 */

static void test_crc(void) {
    section("CRC-32");
    ck(lc_crc32("", 0) == 0x00000000u, "CRC32(\"\") == 0");
    ck(lc_crc32("123456789", 9) == 0xCBF43926u, "CRC32(\"123456789\") == 0xCBF43926");
    ck(lc_crc32("a", 1) == 0xE8B7BE43u, "CRC32(\"a\") == 0xE8B7BE43");
    ck(lc_crc32("The quick brown fox jumps over the lazy dog", 43) == 0x414FA339u,
       "CRC32(pangram) == 0x414FA339");
    {
        const char *s = "The quick brown fox jumps over the lazy dog";
        uint32_t a = lc_crc32_update(0, s, 19);
        uint32_t b = lc_crc32_update(a, s + 19, 43 - 19);
        ck(b == lc_crc32(s, 43), "incremental CRC == one-shot CRC");
    }
    section_end("CRC-32");
}

/* ========================================================= 2. inflate 边界 */

static void test_inflate_edge(void) {
    unsigned char out[4096];
    size_t n = 0;
    int rc;

    section("inflate edge cases");

    /* (a) 空的 stored 块，且流正好在块边界结束 */
    {
        static const unsigned char v[] = { 0x01, 0x00, 0x00, 0xFF, 0xFF };
        n = 123;
        rc = lc_inflate(v, sizeof v, out, sizeof out, &n);
        ck(rc == LC_INFLATE_OK, "empty stored block: rc == OK");
        ck(n == 0, "empty stored block: produced 0 bytes");
    }

    /* (b) stored "hello" */
    {
        static const unsigned char v[] = {
            0x01, 0x05, 0x00, 0xFA, 0xFF, 'h', 'e', 'l', 'l', 'o'
        };
        rc = lc_inflate(v, sizeof v, out, sizeof out, &n);
        ck(rc == LC_INFLATE_OK && n == 5 && memcmp(out, "hello", 5) == 0,
           "stored 'hello' round-trip");
    }

    /* (c) 两个 stored 块（BFINAL 链接），最后正好在边界结束 */
    {
        static const unsigned char v[] = {
            0x00, 0x03, 0x00, 0xFC, 0xFF, 'a', 'b', 'c',
            0x01, 0x04, 0x00, 0xFB, 0xFF, 'd', 'e', 'f', 'g'
        };
        rc = lc_inflate(v, sizeof v, out, sizeof out, &n);
        ck(rc == LC_INFLATE_OK && n == 7 && memcmp(out, "abcdefg", 7) == 0,
           "two stored blocks, BFINAL chaining");
    }

    /* (d) 空输出缓冲区（dstcap == 0）也要正确报告 FULL */
    {
        static const unsigned char v[] = {
            0x01, 0x05, 0x00, 0xFA, 0xFF, 'h', 'e', 'l', 'l', 'o'
        };
        rc = lc_inflate(v, sizeof v, out, 0, &n);
        ck(rc == LC_INFLATE_FULL, "dstcap==0 with 5 bytes of output -> FULL");
    }

    /* (e) 输出不够（差 1 字节） */
    {
        static const unsigned char v[] = {
            0x01, 0x05, 0x00, 0xFA, 0xFF, 'h', 'e', 'l', 'l', 'o'
        };
        rc = lc_inflate(v, sizeof v, out, 4, &n);
        ck(rc == LC_INFLATE_FULL, "dstcap 4 for 5 bytes -> FULL");
        ck(n == 0, "FULL leaves *dstlen 0");
    }

    /* (f) 截断的流必须报错而不是崩溃 */
    {
        static const unsigned char v[] = {
            0x01, 0x05, 0x00, 0xFA, 0xFF, 'h', 'e', 'l', 'l', 'o'
        };
        size_t k;
        int all_bad = 1;
        for (k = 0; k < 4; k++) {
            rc = lc_inflate(v, k, out, sizeof out, &n);
            if (rc == LC_INFLATE_OK) all_bad = 0;
        }
        ck(all_bad, "truncated stored header -> error, no crash");
    }

    /* (g) 非法 BTYPE == 3 */
    {
        static const unsigned char v[] = { 0x07, 0x00, 0x00, 0x00, 0x00 };
        rc = lc_inflate(v, sizeof v, out, sizeof out, &n);
        ck(rc != LC_INFLATE_OK, "BTYPE==3 rejected");
    }

    /* (h) LEN/NLEN 校验失败 */
    {
        static const unsigned char v[] = { 0x01, 0x05, 0x00, 0x00, 0x00,
                                           'h', 'e', 'l', 'l', 'o' };
        rc = lc_inflate(v, sizeof v, out, sizeof out, &n);
        ck(rc != LC_INFLATE_OK, "bad NLEN rejected");
    }

    /* (i) 固定 Huffman：覆盖全部 29 个长度码 + 30 个距离码 + 全部 256 个字面量，
     *     并做 BFINAL 双块链接。期望输出由测试按 LZ77 语义独立重算。 */
    {
        BW b;
        unsigned char *expect = (unsigned char *)malloc(1u << 20);
        size_t en = 0, i;
        int k;
        bw_init(&b);
        /* 块 1：BFINAL=0, BTYPE=01 */
        bw_bits(&b, 0, 1);
        bw_bits(&b, 1, 2);
        for (i = 0; i < 33000; i++) {
            unsigned v = (unsigned)((i * 7u + 3u) & 0xFFu);
            bw_fixed(&b, (int)v);
            expect[en++] = (unsigned char)v;
        }
        bw_fixed(&b, 256);              /* 块 1 结束 */
        /* 块 2：BFINAL=1, BTYPE=01 */
        bw_bits(&b, 1, 1);
        bw_bits(&b, 1, 2);
        for (k = 0; k < 29; k++) emit_pair(&b, k, k, expect, &en);
        for (k = 0; k < 30; k++) emit_pair(&b, 0, k, expect, &en);
        bw_fixed(&b, 256);
        bw_flush(&b);

        {
            size_t outlen = 0;
            unsigned char *big = (unsigned char *)malloc(en + 16);
            rc = lc_inflate(b.p, b.len, big, en, &outlen);
            ck(rc == LC_INFLATE_OK, "fixed-Huffman all-codes stream: rc == OK");
            ck(outlen == en, "fixed-Huffman all-codes stream: length matches");
            ck(outlen == en && memcmp(big, expect, en) == 0,
               "fixed-Huffman all-codes stream: bytes identical");
            rc = lc_inflate(b.p, b.len, big, en - 1, &outlen);
            ck(rc == LC_INFLATE_FULL, "fixed stream with dstcap==size-1 -> FULL");
            rc = lc_inflate(b.p, b.len / 2, big, en, &outlen);
            ck(rc != LC_INFLATE_OK, "truncated fixed-Huffman stream rejected");
            free(big);
        }
        printf("    fixed stream: %llu bytes deflate -> %llu bytes output\n",
               (unsigned long long)b.len, (unsigned long long)en);
        free(expect);
        bw_free(&b);
    }

    /* (j) 固定 Huffman：非法距离 */
    {
        BW b;
        bw_init(&b);
        bw_bits(&b, 1, 1);
        bw_bits(&b, 1, 2);
        bw_fixed(&b, 'A');
        bw_fixed(&b, 257);
        bw_fixed_dist(&b, 30);          /* 距离码 30 非法 */
        bw_flush(&b);
        rc = lc_inflate(b.p, b.len, out, sizeof out, &n);
        ck(rc != LC_INFLATE_OK, "distance code 30 rejected");
        bw_free(&b);

        bw_init(&b);
        bw_bits(&b, 1, 1);
        bw_bits(&b, 1, 2);
        bw_fixed(&b, 'A');
        bw_fixed(&b, 257);
        bw_fixed_dist(&b, 4);           /* 距离 5 > 已产生 1 字节 */
        bw_flush(&b);
        rc = lc_inflate(b.p, b.len, out, sizeof out, &n);
        ck(rc != LC_INFLATE_OK, "distance beyond produced bytes rejected");
        bw_free(&b);
    }

    /* (k) 重叠复制（LZ77 语义）：RLE，距离 1，长度 258 */
    {
        BW b;
        unsigned char *expect = (unsigned char *)malloc(4096);
        unsigned char *big = (unsigned char *)malloc(4096);
        size_t en = 0, outlen = 0;
        int i;
        bw_init(&b);
        bw_bits(&b, 1, 1);
        bw_bits(&b, 1, 2);
        bw_fixed(&b, 'Z');
        expect[en++] = 'Z';
        for (i = 0; i < 10; i++)
            emit_pair(&b, 28, 0, expect, &en);   /* 长度 258 @ 距离 1 */
        bw_fixed(&b, 256);
        bw_flush(&b);
        rc = lc_inflate(b.p, b.len, big, 4096, &outlen);
        ck(rc == LC_INFLATE_OK && outlen == en && memcmp(big, expect, en) == 0,
           "overlapping copy (dist 1, len 258 x10)");
        free(expect);
        free(big);
        bw_free(&b);
    }

    section_end("inflate edge cases");
}

/* ====================================== 3. 外部工具生成的真实 deflate zip */

static void test_real_zip(void) {
    size_t zlen = 0;
    unsigned char *zbytes = slurp_rel("real.zip", &zlen);
    LcZip *z;
    wchar_t wpath[512];

    section("real.zip (Compress-Archive, deflate)");

    if (!zbytes) {
        printf("    [SKIP] build\\\\zipwork\\\\real.zip \xe4\xb8\x8d\xe5\xad\x98\xe5\x9c\xa8\xef\xbc\x88\xe5\x85\x88\xe8\xb7\x91 build\\\\build.ps1 -Tests \xe7\x94\x9f\xe6\x88\x90\xe8\xaf\xa5\xe5\xa4\xb9\xe6\x96\x87\xe4\xbb\xb6\xef\xbc\x89\\n");
        g_fail++;
        g_section_fail++;
        section_end("real.zip");
        return;
    }
    printf("    real.zip = %llu bytes\n", (unsigned long long)zlen);

    MultiByteToWideChar(CP_ACP, 0, "build\\zipwork\\real.zip", -1, wpath, 512);
    z = zip_open(wpath);
    ck(z != NULL, "zip_open(real.zip)");
    if (!z) { free(zbytes); section_end("real.zip"); return; }

    printf("    entries = %llu\n", (unsigned long long)zip_count(z));
    {
        size_t i;
        for (i = 0; i < zip_count(z); i++)
            printf("      [%llu] %-16s method=%d size=%lld\n",
                   (unsigned long long)i, zip_name_at(z, i),
                   zip_method_at(z, i), zip_size_at(z, i));
    }

    /* a.txt: PowerShell Set-Content -NoNewline 写入，无换行 */
    {
        size_t n = 0;
        unsigned char *b = rd_name(z, "a.txt", &n);
        ck(b != NULL, "read a.txt");
        if (b) {
            ck(n == 9 && memcmp(b, "hello zip", 9) == 0,
               "a.txt content == \"hello zip\"");
            if (!(n == 9 && memcmp(b, "hello zip", 9) == 0)) {
                printf("      got %llu bytes: ", (unsigned long long)n);
                hexdump(b, n > 32 ? 32 : n);
            }
            free(b);
        }
    }

    /* b.txt: 100000 个 'X' */
    {
        size_t n = 0, i;
        unsigned char *b = rd_name(z, "b.txt", &n);
        int ok = 1;
        ck(b != NULL, "read b.txt");
        if (b) {
            if (n != 100000) ok = 0;
            for (i = 0; ok && i < n; i++) if (b[i] != 'X') ok = 0;
            ck(ok, "b.txt == 100000 x 'X'");
            free(b);
        }
    }

    /* c.txt: 1MB 高度可压缩数据（RLE 长匹配） */
    {
        size_t n = 0, i;
        unsigned char *b = rd_name(z, "c.txt", &n);
        int ok = 1;
        ck(b != NULL, "read c.txt (1MB, highly compressible)");
        if (b) {
            if (n != 1024u * 1024u) ok = 0;
            for (i = 0; ok && i < n; i++) if (b[i] != 'A') ok = 0;
            ck(ok, "c.txt == 1MB x 'A'");
            free(b);
        }
    }

    /* 不存在的名字 */
    {
        size_t n = 0;
        unsigned char *b = NULL;
        ck(zip_read_by_name(z, "nope.txt", &b, &n) != 0 && b == NULL,
           "missing name returns failure + NULL");
    }

    /* 按下标读取 + 越界 */
    {
        size_t n = 0;
        unsigned char *b = rd_at(z, 0, &n);
        ck(b != NULL && n > 0, "zip_read_at(index 0) works");
        free(b);
        b = NULL;
        n = 0;
        ck(zip_read_at(z, zip_count(z) + 5, &b, &n) != 0 && b == NULL && n == 0,
           "out-of-range index -> failure + NULL");
        ck(zip_name_at(z, zip_count(z) + 5) == NULL,
           "zip_name_at out-of-range -> NULL");
    }

    /* 用内存接口再读一遍同一份数据 */
    {
        LcZip *z2 = zip_open_memory(zbytes, zlen);
        size_t n = 0;
        unsigned char *b = NULL;
        ck(z2 != NULL, "zip_open_memory(real.zip)");
        if (z2 && zip_read_by_name(z2, "a.txt", &b, &n) == 0)
            ck(n == 9 && memcmp(b, "hello zip", 9) == 0,
               "memory reader a.txt == \"hello zip\"");
        else
            ck(0, "memory reader a.txt");
        free(b);
        zip_close(z2);
    }

    zip_close(z);
    free(zbytes);
    section_end("real.zip");
}

/* ============================================ 4. 真实 OOXML (.docx) */

static void test_real_docx(void) {
    size_t len = 0;
    unsigned char *bytes = slurp_rel("real_word.docx", &len);
    LcZip *z;

    section("real .docx (OOXML)");
    if (!bytes) {
        printf("    [SKIP] build\\zipwork\\real_word.docx 不存在\n");
        section_end("real .docx (OOXML)");
        return;
    }
    printf("    real_word.docx = %llu bytes\n", (unsigned long long)len);

    z = zip_open_memory(bytes, len);
    if (!z) {
        ck(0, "zip_open_memory(real_word.docx)");
        free(bytes);
        section_end("real .docx (OOXML)");
        return;
    }
    printf("    entries = %llu\n", (unsigned long long)zip_count(z));
    {
        size_t i;
        for (i = 0; i < zip_count(z); i++)
            printf("      [%llu] %-40s method=%d size=%lld\n",
                   (unsigned long long)i, zip_name_at(z, i),
                   zip_method_at(z, i), zip_size_at(z, i));
    }
    ck(zip_find(z, "[Content_Types].xml") >= 0, "has [Content_Types].xml");
    ck(zip_find(z, "_rels/.rels") >= 0, "has _rels/.rels");
    ck(zip_find(z, "word/document.xml") >= 0, "has word/document.xml");

    {
        size_t n = 0;
        unsigned char *d = rd_name(z, "word/document.xml", &n);
        ck(d != NULL && n > 0, "read word/document.xml");
        if (d) {
            char *txt = (char *)malloc(n + 1);
            memcpy(txt, d, n);
            txt[n] = 0;
            ck(strstr(txt, "<w:document") != NULL, "document.xml contains <w:document");
            ck(strstr(txt, "<?xml") != NULL, "document.xml contains <?xml");
            printf("    document.xml = %llu bytes, starts: %.40s\n",
                   (unsigned long long)n, txt);
            free(txt);
            free(d);
        }
        d = rd_name(z, "[Content_Types].xml", &n);
        ck(d != NULL && n > 0, "read [Content_Types].xml");
        free(d);
    }
    zip_close(z);
    free(bytes);
    section_end("real .docx (OOXML)");
}

/* ======================================== 5. 自己的 writer -> reader 往返 */

static void test_roundtrip(void) {
    LcZipWriter *w;
    const char *big_name = "word/media/big.bin";
    unsigned char *big = (unsigned char *)malloc(200u * 1024u);
    size_t i;
    unsigned char *zipbuf = NULL;
    size_t ziplen = 0;
    wchar_t outpath[512];

    section("writer -> reader round-trip");

    for (i = 0; i < 200u * 1024u; i++)
        big[i] = (unsigned char)((i * 31u + (i >> 8) * 7u) & 0xFFu);

    w = zip_writer_create();
    ck(w != NULL, "zip_writer_create");

    ck(zip_writer_add(w, "[Content_Types].xml", "<Types/>", 8) == 0,
       "add ASCII entry");
    ck(zip_writer_add(w, "word/document.xml", "<w:document/>", 13) == 0,
       "add word/document.xml");
    ck(zip_writer_add(w, "\xe5\x9b\xbe\xe7\x89\x87/", NULL, 0) == 0,
       "add non-ASCII directory entry");
    ck(zip_writer_add(w, "\xe5\x9b\xbe\xe7\x89\x87/\xe8\xaf\xb4\xe6\x98\x8e.txt",
                      "\xe4\xbd\xa0\xe5\xa5\xbd\xef\xbc\x8c\xe4\xb8\x96\xe7\x95\x8c", 15) == 0,
       "add non-ASCII file entry (UTF-8 content)");
    ck(zip_writer_add(w, "empty.txt", NULL, 0) == 0, "add empty file");
    ck(zip_writer_add(w, big_name, big, 200u * 1024u) == 0, "add 200KB entry");
    ck(zip_writer_add(w, "dir\\sub\\slash.txt", "s", 1) == 0,
       "add entry with backslashes");
    ck(zip_writer_add_w(w, L"\u5bbd\u5b57\u7b26.txt", "wide", 4) == 0,
       "add entry via wide-char name API");

    ck(zip_writer_data(w, &zipbuf, &ziplen) == 0 && zipbuf && ziplen > 0,
       "zip_writer_data");
    printf("    written zip = %llu bytes\n", (unsigned long long)ziplen);
    ck(ziplen >= 200u * 1024u, "zip contains the 200KB payload");

    /* (a) 内存往返 */
    {
        LcZip *z = zip_open_memory(zipbuf, ziplen);
        size_t n = 0;
        unsigned char *b;
        ck(z != NULL, "reader opens our own archive (memory)");
        ck(z && zip_count(z) == 8, "8 entries");
        if (z) {
            b = rd_name(z, "[Content_Types].xml", &n);
            ck(b && n == 8 && memcmp(b, "<Types/>", 8) == 0,
               "round-trip [Content_Types].xml");
            free(b);

            b = rd_name(z, "word/document.xml", &n);
            ck(b && n == 13 && memcmp(b, "<w:document/>", 13) == 0,
               "round-trip word/document.xml");
            free(b);

            b = rd_name(z, "\xe5\x9b\xbe\xe7\x89\x87/", &n);
            ck(b != NULL && n == 0, "non-ASCII directory entry readable, 0 bytes");
            ck(zip_is_dir_at(z, (size_t)zip_find(z, "\xe5\x9b\xbe\xe7\x89\x87/")) == 1,
               "directory flag reported");
            free(b);

            b = rd_name(z, "\xe5\x9b\xbe\xe7\x89\x87/\xe8\xaf\xb4\xe6\x98\x8e.txt", &n);
            ck(b && n == 15 && memcmp(b, "\xe4\xbd\xa0\xe5\xa5\xbd\xef\xbc\x8c\xe4\xb8\x96\xe7\x95\x8c", 15) == 0,
               "non-ASCII name + UTF-8 content round-trip");
            if (!b) {
                printf("      names present:");
                for (i = 0; i < zip_count(z); i++) printf(" %s |", zip_name_at(z, i));
                printf("\n");
            }
            free(b);

            b = rd_name(z, "empty.txt", &n);
            ck(b != NULL && n == 0, "empty entry -> 0 bytes (non-NULL)");
            free(b);

            b = rd_name(z, big_name, &n);
            ck(b && n == 200u * 1024u && memcmp(b, big, n) == 0,
               "200KB entry byte-identical");
            free(b);

            b = rd_name(z, "dir/sub/slash.txt", &n);
            ck(b && n == 1 && b[0] == 's', "backslash name normalized to '/'");
            free(b);

            b = rd_name(z, "\xe5\xae\xbd\xe5\xad\x97\xe7\xac\xa6.txt", &n);
            ck(b && n == 4 && memcmp(b, "wide", 4) == 0, "wide-name entry round-trip");
            free(b);

            zip_close(z);
        }
    }

    /* (b) 落盘往返（外部工具随后会用 .NET / Python 校验这个文件） */
    {
        LcZip *z;
        CreateDirectoryW(L"build", NULL);
        CreateDirectoryW(L"build\\zipwork", NULL);
        MultiByteToWideChar(CP_ACP, 0, "build\\zipwork\\roundtrip.zip", -1, outpath, 512);
        ck(zip_writer_save(w, outpath) == 0, "zip_writer_save");
        z = zip_open(outpath);
        ck(z != NULL, "reader opens saved file");
        if (z) {
            size_t n = 0;
            unsigned char *b = rd_name(z, big_name, &n);
            ck(b && n == 200u * 1024u && memcmp(b, big, n) == 0,
               "saved file: 200KB byte-identical");
            free(b);
            zip_close(z);
        }
    }

    free(zipbuf);
    zip_writer_free(w);
    free(big);
    section_end("writer -> reader round-trip");
}

/* ================================================== 6. 损坏/截断输入 */

static void test_fuzz(void) {
    size_t zlen = 0, i;
    unsigned char *orig = slurp_rel("real.zip", &zlen);
    unsigned char *buf;
    unsigned long seed = 12345u;
    int absurd = 0, opened = 0;

    section("corrupt / truncated input");
    if (!orig) {
        printf("    [SKIP] real.zip 不存在\n");
        section_end("corrupt / truncated input");
        return;
    }

    buf = (unsigned char *)malloc(zlen > 8192 ? zlen + 64 : 8192);

    /* 截断：均匀取样 */
    for (i = 0; i <= zlen; i += (zlen / 64) + 1) {
        LcZip *z;
        memcpy(buf, orig, i);
        z = zip_open_memory(buf, i);
        if (z) {
            size_t k, c = zip_count(z);
            opened++;
            for (k = 0; k < c && k < 64; k++) {
                unsigned char *b = NULL;
                size_t n = 0;
                if (zip_read_at(z, k, &b, &n) == 0) free(b);
            }
            zip_close(z);
        }
    }
    /* 尾部逐字节截断（最容易踩到 EOCD 边界） */
    {
        size_t lo = zlen > 80 ? zlen - 80 : 0;
        for (i = lo; i < zlen; i++) {
            LcZip *z;
            memcpy(buf, orig, i);
            z = zip_open_memory(buf, i);
            if (z) { zip_close(z); opened++; }
        }
    }

    /* 随机字节突变 */
    for (i = 0; i < 4000; i++) {
        LcZip *z;
        size_t j, muts;
        memcpy(buf, orig, zlen);
        muts = 1 + (size_t)((seed >> 3) % 4);
        for (j = 0; j < muts; j++) {
            size_t p;
            seed = seed * 1103515245u + 12345u;
            p = (size_t)((seed >> 8) % zlen);
            seed = seed * 1103515245u + 12345u;
            buf[p] = (unsigned char)(seed >> 16);
        }
        z = zip_open_memory(buf, zlen);
        if (z) {
            size_t k, c = zip_count(z);
            opened++;
            for (k = 0; k < c && k < 64; k++) {
                unsigned char *b = NULL;
                size_t n = 0;
                if (zip_read_at(z, k, &b, &n) == 0) {
                    /* real.zip 里最大的条目是 1MB；超过 4MB 说明解压失控 */
                    if (n > 4u * 1024u * 1024u) absurd++;
                    free(b);
                }
            }
            zip_close(z);
        }
    }

    /* 纯随机数据 */
    for (i = 0; i < 2000; i++) {
        LcZip *z;
        size_t j, n = 1 + (i % 4096);
        for (j = 0; j < n; j++) {
            seed = seed * 1103515245u + 12345u;
            buf[j] = (unsigned char)(seed >> 16);
        }
        z = zip_open_memory(buf, n);
        if (z) { zip_close(z); opened++; }
    }

    printf("    survived; archives that happened to open = %d\n", opened);
    ck(absurd == 0, "no absurd decompression (zip-bomb guard)");
    ck(1, "no crash / no hang on truncated & mutated input");
    free(buf);
    free(orig);
    section_end("corrupt / truncated input");
}

/* ============================================================ 7. 大输入 */

static void test_large(void) {
    size_t len = 0;
    unsigned char *raw = slurp_rel("repeat1m.deflate", &len);
    unsigned char *out;
    size_t outlen = 0;
    int rc;
    DWORD t0, t1;

    section("large highly-compressible input (1MB)");
    if (!raw) {
        printf("    [SKIP] repeat1m.deflate 不存在\n");
        section_end("large input");
        return;
    }
    printf("    deflate stream = %llu bytes\n", (unsigned long long)len);
    out = (unsigned char *)malloc(1024u * 1024u + 16);
    t0 = GetTickCount();
    rc = lc_inflate(raw, len, out, 1024u * 1024u, &outlen);
    t1 = GetTickCount();
    ck(rc == LC_INFLATE_OK, "1MB inflate rc == OK");
    ck(outlen == 1024u * 1024u, "1MB output length");
    {
        size_t i;
        int ok = 1;
        for (i = 0; i < outlen; i++) if (out[i] != 'A') { ok = 0; break; }
        ck(ok, "all 1MB bytes == 'A'");
    }
    printf("    inflate took %lu ms\n", (unsigned long)(t1 - t0));
    ck((t1 - t0) < 3000, "inflate finished in < 3s");
    rc = lc_inflate(raw, len, out, 1024u * 1024u - 1u, &outlen);
    ck(rc == LC_INFLATE_FULL, "1MB with dstcap-1 -> FULL");
    free(out);
    free(raw);
    section_end("large input");
}

/* ================================ 8. 流式写入的 zip（本地头尺寸全为 0） */

static void p16(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
}
static void p32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

/* 本地文件头的 CRC/尺寸全为 0、真正的值只在中央目录 + data descriptor 里。
 * 这正是"必须优先用中央目录"的场景（流式写出的 zip 都长这样）。 */
static void test_streamed_zip(void) {
    static const char payload[] = "streamed entry: local sizes are zeroed out";
    const size_t dlen = sizeof payload - 1;
    const char *name = "stream.bin";
    const size_t nl = sizeof "stream.bin" - 1;
    BW b;
    unsigned char *zip;
    size_t dl, pos, cd_off, zlen, i;
    uint32_t crc;
    LcZip *z;
    unsigned char *got = NULL;
    size_t gotlen = 0;

    section("streamed zip (zeroed local sizes + data descriptor)");

    bw_init(&b);
    bw_bits(&b, 1, 1);
    bw_bits(&b, 1, 2);
    for (i = 0; i < dlen; i++) bw_fixed(&b, (unsigned char)payload[i]);
    bw_fixed(&b, 256);
    bw_flush(&b);
    dl = b.len;
    crc = lc_crc32(payload, dlen);

    zip = (unsigned char *)malloc(30 + nl + dl + 16 + 46 + nl + 22 + 32);
    /* --- 本地文件头：crc/sizes = 0，flags 带 bit3 --- */
    p32(zip + 0, 0x04034b50u);
    p16(zip + 4, 20);
    p16(zip + 6, 0x0008);      /* data descriptor */
    p16(zip + 8, 8);           /* method = deflate */
    p16(zip + 10, 0);
    p16(zip + 12, 0);
    p32(zip + 14, 0);          /* crc = 0 */
    p32(zip + 18, 0);          /* comp = 0 */
    p32(zip + 22, 0);          /* uncomp = 0 */
    p16(zip + 26, (unsigned)nl);
    p16(zip + 28, 0);
    memcpy(zip + 30, name, nl);
    memcpy(zip + 30 + nl, b.p, dl);
    pos = 30 + nl + dl;
    /* --- data descriptor --- */
    p32(zip + pos, 0x08074b50u);
    p32(zip + pos + 4, crc);
    p32(zip + pos + 8, (uint32_t)dl);
    p32(zip + pos + 12, (uint32_t)dlen);
    pos += 16;
    /* --- 中央目录 --- */
    cd_off = pos;
    p32(zip + pos + 0, 0x02014b50u);
    p16(zip + pos + 4, 20);
    p16(zip + pos + 6, 20);
    p16(zip + pos + 8, 0x0008);
    p16(zip + pos + 10, 8);
    p16(zip + pos + 12, 0);
    p16(zip + pos + 14, 0);
    p32(zip + pos + 16, crc);
    p32(zip + pos + 20, (uint32_t)dl);
    p32(zip + pos + 24, (uint32_t)dlen);
    p16(zip + pos + 28, (unsigned)nl);
    p16(zip + pos + 30, 0);
    p16(zip + pos + 32, 0);
    p16(zip + pos + 34, 0);
    p16(zip + pos + 36, 0);
    p32(zip + pos + 38, 0);
    p32(zip + pos + 42, 0);    /* 本地头偏移 = 0 */
    memcpy(zip + pos + 46, name, nl);
    pos += 46 + nl;
    /* --- EOCD --- */
    p32(zip + pos + 0, 0x06054b50u);
    p16(zip + pos + 4, 0);
    p16(zip + pos + 6, 0);
    p16(zip + pos + 8, 1);
    p16(zip + pos + 10, 1);
    p32(zip + pos + 12, (uint32_t)(pos - cd_off));
    p32(zip + pos + 16, (uint32_t)cd_off);
    p16(zip + pos + 20, 0);
    pos += 22;
    zlen = pos;

    z = zip_open_memory(zip, zlen);
    ck(z != NULL, "streamed zip opens");
    if (z) {
        ck(zip_count(z) == 1, "1 entry");
        ck(zip_name_at(z, 0) && strcmp(zip_name_at(z, 0), "stream.bin") == 0,
           "entry name from central directory");
        ck(zip_size_at(z, 0) == (long long)dlen, "size from central directory");
        got = rd_at(z, 0, &gotlen);
        ck(got && gotlen == dlen && memcmp(got, payload, dlen) == 0,
           "streamed entry decompressed via central directory");
        free(got);
        zip_close(z);
    }
    free(zip);
    bw_free(&b);
    section_end("streamed zip");
}

/* ============================ 9. 机器上其它真实 docx（走 zip_open 文件路径） */

static void test_docx_samples(void) {
    WIN32_FIND_DATAW fd;
    HANDLE h;
    wchar_t pat[512];
    int n = 0, bad = 0;

    section("real-world .docx samples (build\\zipwork\\samples)");
    MultiByteToWideChar(CP_ACP, 0, "build\\zipwork\\samples\\*.docx", -1, pat, 512);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        printf("    [SKIP] no samples directory\n");
        section_end("real-world .docx samples");
        return;
    }
    do {
        wchar_t wpath[512];
        char rel[512], nm[256];
        unsigned char *bytes = NULL;
        size_t len = 0;
        LcZip *z;
        int ok = 1;

        WideCharToMultiByte(CP_ACP, 0, fd.cFileName, -1, nm, 256, NULL, NULL);
        sprintf(rel, "build\\zipwork\\samples\\%s", nm);
        MultiByteToWideChar(CP_ACP, 0, rel, -1, wpath, 512);

        z = zip_open(wpath);                 /* 文件路径接口 */
        if (!z) {
            printf("      %-28s OPEN FAILED\n", nm);
            bad++;
            continue;
        }
        bytes = rd_name(z, "word/document.xml", &len);
        if (!bytes || len == 0) ok = 0;
        if (ok) {
            char *txt = (char *)malloc(len + 1);
            memcpy(txt, bytes, len);
            txt[len] = 0;
            if (!strstr(txt, "<w:document")) ok = 0;
            free(txt);
        }
        printf("      %-28s %s (%llu entries, document.xml %llu bytes)\n", nm,
               ok ? "OK  " : "BAD ", (unsigned long long)zip_count(z),
               (unsigned long long)len);
        if (!ok) bad++;
        free(bytes);
        zip_close(z);
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    printf("    %d sample(s), %d bad\n", n, bad);
    ck(n > 0, "at least one real .docx sample present");
    ck(bad == 0, "every real .docx sample readable");
    section_end("real-world .docx samples");
}

/* ==================== 10. 用我们的 writer 写出一个真的 .docx 包 */
/* 生成 build\zipwork\minimal.docx，随后由外部工具（python-docx / .NET）打开，
   用来证明"我们写的 zip 能被 OOXML 消费者接受"。 */

static const char DOCX_CT[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
    "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
    "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
    "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
    "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
    "</Types>";

static const char DOCX_RELS[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
    "</Relationships>";

static const char DOCX_DOCRELS[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
    "</Relationships>";

static const char DOCX_STYLES[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
    "<w:docDefaults/>"
    "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
    "<w:name w:val=\"Normal\"/></w:style></w:styles>";

/* 文字里带中文，顺便验证 UTF-8 内容 + 非 ASCII 名字 */
static const char DOCX_BODY[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
    "<w:body><w:p><w:r><w:t>LocalCraft ZIP writer: hello, \xe4\xb8\x96\xe7\x95\x8c</w:t></w:r></w:p>"
    "</w:body></w:document>";

static void test_write_docx(void) {
    LcZipWriter *w;
    wchar_t out[512];
    LcZip *z;

    section("write a real .docx package with our writer");
    CreateDirectoryW(L"build\\zipwork", NULL);

    w = zip_writer_create();
    ck(w != NULL, "writer created");
    ck(zip_writer_add(w, "[Content_Types].xml", DOCX_CT, strlen(DOCX_CT)) == 0,
       "add [Content_Types].xml");
    ck(zip_writer_add(w, "_rels/.rels", DOCX_RELS, strlen(DOCX_RELS)) == 0,
       "add _rels/.rels");
    ck(zip_writer_add(w, "word/_rels/document.xml.rels", DOCX_DOCRELS,
                      strlen(DOCX_DOCRELS)) == 0, "add word/_rels/document.xml.rels");
    ck(zip_writer_add(w, "word/document.xml", DOCX_BODY, strlen(DOCX_BODY)) == 0,
       "add word/document.xml");
    ck(zip_writer_add(w, "word/styles.xml", DOCX_STYLES, strlen(DOCX_STYLES)) == 0,
       "add word/styles.xml");

    MultiByteToWideChar(CP_ACP, 0, "build\\zipwork\\minimal.docx", -1, out, 512);
    ck(zip_writer_save(w, out) == 0, "save minimal.docx");
    zip_writer_free(w);

    z = zip_open(out);
    ck(z != NULL, "reopen minimal.docx");
    if (z) {
        size_t n = 0;
        unsigned char *d = rd_name(z, "word/document.xml", &n);
        ck(d && n == strlen(DOCX_BODY) && memcmp(d, DOCX_BODY, n) == 0,
           "minimal.docx document.xml intact");
        free(d);
        zip_close(z);
    }
    printf("    wrote build\\zipwork\\minimal.docx (5 parts)\n");
    section_end("write real .docx");
}

/* ==================== 11. 旧式文件名（没有 UTF-8 标记位） */

/* 造一个只有 1 个 STORED 条目、且**不设** UTF-8 标记位的 zip */
static unsigned char *make_simple_zip(const unsigned char *name, size_t nl,
                                      const unsigned char *data, size_t dl,
                                      size_t *zlen) {
    unsigned char *zip = (unsigned char *)malloc(30 + nl + dl + 46 + nl + 22 + 16);
    uint32_t crc = lc_crc32(data, dl);
    size_t pos;

    p32(zip + 0, 0x04034b50u);
    p16(zip + 4, 20);
    p16(zip + 6, 0);                  /* flags = 0：没有 UTF-8 标记位 */
    p16(zip + 8, 0);                  /* method = stored */
    p16(zip + 10, 0);
    p16(zip + 12, 0);
    p32(zip + 14, crc);
    p32(zip + 18, (uint32_t)dl);
    p32(zip + 22, (uint32_t)dl);
    p16(zip + 26, (unsigned)nl);
    p16(zip + 28, 0);
    memcpy(zip + 30, name, nl);
    memcpy(zip + 30 + nl, data, dl);
    pos = 30 + nl + dl;

    p32(zip + pos + 0, 0x02014b50u);
    p16(zip + pos + 4, 20);
    p16(zip + pos + 6, 20);
    p16(zip + pos + 8, 0);            /* flags = 0 */
    p16(zip + pos + 10, 0);
    p16(zip + pos + 12, 0);
    p16(zip + pos + 14, 0);
    p32(zip + pos + 16, crc);
    p32(zip + pos + 20, (uint32_t)dl);
    p32(zip + pos + 24, (uint32_t)dl);
    p16(zip + pos + 28, (unsigned)nl);
    p16(zip + pos + 30, 0);
    p16(zip + pos + 32, 0);
    p16(zip + pos + 34, 0);
    p16(zip + pos + 36, 0);
    p32(zip + pos + 38, 0);
    p32(zip + pos + 42, 0);
    memcpy(zip + pos + 46, name, nl);
    pos += 46 + nl;

    p32(zip + pos + 0, 0x06054b50u);
    p16(zip + pos + 4, 0);
    p16(zip + pos + 6, 0);
    p16(zip + pos + 8, 1);
    p16(zip + pos + 10, 1);
    p32(zip + pos + 12, (uint32_t)(pos - (30 + nl + dl)));
    p32(zip + pos + 16, (uint32_t)(30 + nl + dl));
    p16(zip + pos + 20, 0);
    pos += 22;
    *zlen = pos;
    return zip;
}

static void test_legacy_names(void) {
    UINT acp = GetACP();

    section("legacy filenames (no UTF-8 flag bit)");
    printf("    system ANSI code page = %u\n", (unsigned)acp);

    /* GBK 的"图片.txt"：CD BC C6 AC 2E 74 78 74。
       这段字节**同时是合法 UTF-8**，所以"合法就用 UTF-8"的实现会解错。 */
    {
        static const unsigned char gbk_name[] = {
            0xCD, 0xBC, 0xC6, 0xAC, 0x2E, 0x74, 0x78, 0x74
        };
        size_t zl = 0;
        unsigned char *zip = make_simple_zip(gbk_name, sizeof gbk_name,
                                             (const unsigned char *)"hi", 2, &zl);
        LcZip *z = zip_open_memory(zip, zl);
        ck(z != NULL, "GBK-named zip opens");
        if (z) {
            const char *nm = zip_name_at(z, 0);
            if (acp == 936) {
                ck(nm && strcmp(nm, "\xe5\x9b\xbe\xe7\x89\x87.txt") == 0,
                   "GBK name decoded through CP_ACP (图片.txt)");
                printf("    GBK  name -> %s\n", nm ? nm : "(null)");
            } else {
                printf("    GBK  name -> %s (ACP != 936, not asserted)\n",
                       nm ? nm : "(null)");
            }
            zip_close(z);
        }
        free(zip);
    }

    /* CP437 的"é.txt"：0x82 在 GBK 里是非法序列，应回退到 CP437。 */
    {
        static const unsigned char cp437_name[] = {
            0x82, 0x2E, 0x74, 0x78, 0x74
        };
        size_t zl = 0;
        unsigned char *zip = make_simple_zip(cp437_name, sizeof cp437_name,
                                             (const unsigned char *)"hi", 2, &zl);
        LcZip *z = zip_open_memory(zip, zl);
        ck(z != NULL, "CP437-named zip opens");
        if (z) {
            const char *nm = zip_name_at(z, 0);
            if (acp == 936) {
                ck(nm && strcmp(nm, "\xc3\xa9.txt") == 0,
                   "CP437 name decoded as \xc3\xa9.txt");
                printf("    CP437 name -> %s\n", nm ? nm : "(null)");
            } else {
                printf("    CP437 name -> %s (ACP != 936, not asserted)\n",
                       nm ? nm : "(null)");
            }
            zip_close(z);
        }
        free(zip);
    }

    /* 带 UTF-8 标记位的非 ASCII 名字仍必须走 UTF-8 */
    {
        static const unsigned char u8_name[] = {
            0xE5, 0x9B, 0xBE, 0xE7, 0x89, 0x87, 0x2E, 0x74, 0x78, 0x74
        };
        size_t zl = 0;
        unsigned char *zip = make_simple_zip(u8_name, sizeof u8_name,
                                             (const unsigned char *)"hi", 2, &zl);
        LcZip *z;
        /* 把本地头与中央目录的 flags 都改成带 bit11（30 + namelen + datalen + 8） */
        p16(zip + 6, 0x0800);
        p16(zip + 30 + sizeof u8_name + 2 + 8, 0x0800);
        z = zip_open_memory(zip, zl);
        ck(z != NULL, "UTF-8-flagged zip opens");
        if (z) {
            const char *nm = zip_name_at(z, 0);
            ck(nm && strcmp(nm, "\xe5\x9b\xbe\xe7\x89\x87.txt") == 0,
               "UTF-8 flag honoured");
            zip_close(z);
        }
        free(zip);
    }
    section_end("legacy filenames");
}

/* ============================================================ main */
int main(void) {
    DWORD t0 = GetTickCount();
    setvbuf(stdout, NULL, _IONBF, 0);   /* 崩溃时也能看到进度 */
    printf("LocalCraft test_zip — ZIP reader/writer + inflate\n");
    printf("(test data in build\\zipwork\\)\n");

    test_crc();
    test_inflate_edge();
    test_roundtrip();
    test_real_zip();
    test_real_docx();
    test_streamed_zip();
    test_legacy_names();
    test_docx_samples();
    test_write_docx();
    test_fuzz();
    test_large();

    printf("\n======================== SUMMARY ========================\n");
    printf("  checks passed : %d\n", g_pass);
    printf("  checks failed : %d\n", g_fail);
    printf("  elapsed       : %lu ms\n", (unsigned long)(GetTickCount() - t0));
    printf("  result        : %s\n", g_fail ? "FAIL" : "ALL PASS");
    printf("=========================================================\n");
    return g_fail ? 1 : 0;
}
