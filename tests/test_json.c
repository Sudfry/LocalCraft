/* ==========================================================================
 * LocalCraft — tests/test_json.c
 * json.c 的自检程序：全部断言 + 汇总。
 * 编译：
 *   gcc -std=c11 -D__USE_MINGW_ANSI_STDIO=1 -DUNICODE -D_UNICODE -Wall -Wextra
 *       -I src src\json.c src\util.c tests\test_json.c -o build\test_json.exe
 * ========================================================================== */
#include "lc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        if (cond) {                                                       \
            g_pass++;                                                     \
        } else {                                                          \
            g_fail++;                                                     \
            printf("  FAIL [%s:%d] %s\n", __FILE__, __LINE__, (msg));     \
        }                                                                 \
    } while (0)

/* ---------------------------------------------------------- 1. 往返 ---- */
static void test_roundtrip(void) {
    const char *src =
        "{\"name\":\"批量重命名\",\"n\":3,\"ok\":true,\"list\":[1,2,3],"
        "\"nested\":{\"a\":null}}";
    Arena *a = arena_new();
    JsonValue *v = json_parse(src, a);
    JsonValue *list;
    JsonValue *nested;

    printf("[1] 基本往返 / 访问器\n");
    CHECK(v != NULL, "解析手写 JSON");
    CHECK(json_is_object(v), "根是对象");
    CHECK(strcmp(json_string(json_get(v, "name"), ""), "批量重命名") == 0,
          "中文 name");
    CHECK(json_int(json_get(v, "n"), -1) == 3, "n == 3");
    CHECK(json_llong(json_get(v, "n"), -1) == 3, "n(LL) == 3");
    CHECK(json_double(json_get(v, "n"), -1) == 3.0, "n(double) == 3");
    CHECK(json_bool(json_get(v, "ok"), 0) == 1, "ok == true");

    list = json_get(v, "list");
    CHECK(json_is_array(list), "list 是数组");
    CHECK(json_array_len(list) == 3, "list 长度 3");
    CHECK(json_int(json_at(list, 0), -1) == 1, "list[0]");
    CHECK(json_int(json_at(list, 1), -1) == 2, "list[1]");
    CHECK(json_int(json_at(list, 2), -1) == 3, "list[2]");
    CHECK(json_at(list, 3) == NULL, "list[3] 越界为 NULL");
    CHECK(json_at(NULL, 0) == NULL, "json_at(NULL) 安全");

    nested = json_get(v, "nested");
    CHECK(json_is_object(nested), "nested 是对象");
    CHECK(json_is_null(json_get(nested, "a")), "nested.a 是 null");
    CHECK(json_get(nested, "missing") == NULL, "缺键返回 NULL");
    CHECK(json_get(v, "list") != NULL && !json_is_object(json_get(v, "list")),
          "非对象上 json_get 语义");
    CHECK(json_get(list, "x") == NULL, "数组上 json_get 返回 NULL");

    CHECK(json_string(json_get(v, "n"), "fb") != NULL &&
              strcmp(json_string(json_get(v, "n"), "fb"), "fb") == 0,
          "非字符串 json_string 用 fallback");
    CHECK(json_int(NULL, 7) == 7, "json_int(NULL) fallback");
    CHECK(json_llong(NULL, 8) == 8, "json_llong(NULL) fallback");
    CHECK(json_double(NULL, 1.5) == 1.5, "json_double(NULL) fallback");
    CHECK(json_bool(NULL, 2) == 2, "json_bool(NULL) fallback");
    CHECK(json_array_len(NULL) == 0, "json_array_len(NULL) == 0");
    CHECK(!json_is_array(NULL) && !json_is_object(NULL) && !json_is_null(NULL),
          "NULL 类型判断");

    json_free(v);
    json_free(NULL);   /* 必须安全 */
    arena_free(a);
}

/* ------------------------------------------------------- 2. \u 转义 ---- */
static void test_unicode_escapes(void) {
    const char *src =
        "{\"a\":\"\\u4e2d\\u6587\","
        "\"e\":\"\\uD83D\\uDE00\","
        "\"q\":\"a\\\"b\\\\c\\/d\","
        "\"ctl\":\"\\u0001\\n\\t\\b\\f\\r\","
        "\"nul\":\"x\\u0000y\"}";
    Arena *a = arena_new();
    JsonValue *v = json_parse(src, a);
    const char *emoji;
    const char *ctl;

    printf("[2] \\uXXXX 转义 / 代理对\n");
    CHECK(v != NULL, "解析带转义的文档");
    CHECK(strcmp(json_string(json_get(v, "a"), ""), "中文") == 0,
          "\\u4e2d\\u6587 -> 中文");
    emoji = json_string(json_get(v, "e"), "");
    CHECK(emoji && (unsigned char)emoji[0] == 0xF0 &&
              (unsigned char)emoji[1] == 0x9F &&
              (unsigned char)emoji[2] == 0x98 &&
              (unsigned char)emoji[3] == 0x80 && emoji[4] == 0,
          "代理对 -> U+1F600 的 4 字节 UTF-8");
    CHECK(strcmp(json_string(json_get(v, "q"), ""), "a\"b\\c/d") == 0,
          "引号/反斜杠/斜杠");
    ctl = json_string(json_get(v, "ctl"), "");
    CHECK(ctl && ctl[0] == 0x01 && ctl[1] == '\n' && ctl[2] == '\t' &&
              ctl[3] == '\b' && ctl[4] == '\f' && ctl[5] == '\r',
          "控制字符转义");
    CHECK(strcmp(json_string(json_get(v, "nul"), ""), "x") == 0,
          "内嵌 \\u0000：NUL 结尾访问器可见前缀");

    /* 原始 UTF-8（非 \u 形式）也要原样保留 */
    {
        JsonValue *w = json_parse("{\"k\":\"中文😀\"}", a);
        CHECK(w && strcmp(json_string(json_get(w, "k"), ""), "中文😀") == 0,
              "裸 UTF-8 原样保留");
    }
    json_free(v);
    arena_free(a);
}

/* ----------------------------------------------------------- 3. 数字 ---- */
static void test_numbers(void) {
    const char *src =
        "{\"neg\":-5,\"f\":3.5,\"e\":1.5e3,\"E\":-2E-2,\"big\":1234567890123,"
        "\"si\":\"42\",\"sf\":\"2.75\",\"sz\":\" 7 \",\"zero\":0,\"neg0\":-0,"
        "\"tr\":3.9,\"bad\":\"abc\"}";
    Arena *a = arena_new();
    JsonValue *v = json_parse(src, a);
    double e;

    printf("[3] 数字：负数 / 浮点 / 指数 / 数字串\n");
    CHECK(v != NULL, "解析数字文档");
    CHECK(json_int(json_get(v, "neg"), 0) == -5, "负整数");
    CHECK(json_double(json_get(v, "f"), 0) == 3.5, "浮点");
    e = json_double(json_get(v, "e"), 0);
    CHECK(e == 1500.0, "1.5e3");
    CHECK(fabs(json_double(json_get(v, "E"), 0) - (-0.02)) < 1e-12, "-2E-2");
    CHECK(json_llong(json_get(v, "big"), 0) == 1234567890123LL, "大整数精确");
    CHECK(json_int(json_get(v, "si"), -1) == 42, "数字串 \"42\"");
    CHECK(json_double(json_get(v, "sf"), -1) == 2.75, "数字串 \"2.75\"");
    CHECK(json_int(json_get(v, "sz"), -1) == 7, "带空白的数字串");
    CHECK(json_int(json_get(v, "zero"), -1) == 0, "0");
    CHECK(json_int(json_get(v, "neg0"), -1) == 0, "-0");
    CHECK(json_int(json_get(v, "tr"), -1) == 3, "浮点截断 3.9 -> 3");
    CHECK(json_int(json_get(v, "bad"), -1) == -1, "非数字串用 fallback");
    CHECK(json_llong(json_get(v, "f"), -1) == 3, "json_llong 截断");
    CHECK(json_double(json_get(v, "si"), -1) == 42.0, "数字串给 json_double");

    json_free(v);

    /* 单独的字面量 */
    {
        const char *ok[] = { "0", "-0", "0.5", "-0.5", "1e3", "1E+3", "1e-3",
                             "12.25e2", "123456789012345678", "1e999" };
        size_t i;
        for (i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
            JsonValue *x = json_parse(ok[i], a);
            CHECK(x != NULL, ok[i]);
        }
    }
    /* 布尔与数字互通 */
    {
        JsonValue *x = json_parse("{\"t\":1,\"f\":0}", a);
        CHECK(json_bool(json_get(x, "t"), -1) == 1, "数字 1 为真");
        CHECK(json_bool(json_get(x, "f"), -1) == 0, "数字 0 为假");
    }
    /* 重复键：后者胜 */
    {
        JsonValue *x = json_parse("{\"a\":1,\"a\":2,\"b\":3,\"a\":4}", a);
        CHECK(json_int(json_get(x, "a"), -1) == 4, "重复键后者胜");
        CHECK(json_int(json_get(x, "b"), -1) == 3, "重复键不影响其它键");
    }
    arena_free(a);
}

/* ----------------------------------------------------- 4. 转义写出 ---- */
static void test_escape_writers(void) {
    Str out;
    Str doc;
    Arena *a = arena_new();
    JsonValue *v;
    char *expect;
    size_t before;

    printf("[4] 转义写出函数\n");

    /* 4.1 中文宽字符串 -> 原样 UTF-8（不转义） */
    str_init(&out);
    json_escape_to(&out, L"批量重命名");
    CHECK(out.len > 2 && out.data[0] == '"' && out.data[out.len - 1] == '"',
          "带首尾引号");
    CHECK(strstr(out.data, "\\u") == NULL, "非 ASCII 不做 \\u 转义");
    expect = lc_wcs_to_utf8(L"批量重命名");
    CHECK(expect && strncmp(out.data + 1, expect, strlen(expect)) == 0,
          "中文原样 UTF-8 字节与宽字符转换一致");

    /* 4.2 追加语义：不清空已有内容 */
    before = out.len;
    json_escape_char_to(&out, 'x');
    CHECK(out.len == before + 3, "json_escape_char_to 追加 \"x\"");
    json_escape_to(&out, L"N");
    CHECK(out.len == before + 3 + 3, "json_escape_to 追加 \"N\"");

    /* 4.3 json_escape_to 的输出直接当 JSON 值用，能解析回原串 */
    str_init(&doc);
    str_puts(&doc, "{\"k\":");
    {
        Str one;
        str_init(&one);
        json_escape_to(&one, L"批量重命名");
        str_putn(&doc, one.data, one.len);
        str_free(&one);
    }
    str_puts(&doc, "}");
    v = json_parse(doc.data, a);
    CHECK(v && strcmp(json_string(json_get(v, "k"), ""), expect) == 0,
          "json_escape_to 的输出可作为 JSON 值解析回原串");
    str_free(&doc);

    /* 4.4 单个字符的转义表 */
    {
        struct { char c; const char *want; } cases[] = {
            { '"',  "\"\\\"\"" },
            { '\\', "\"\\\\\"" },
            { '\n', "\"\\n\"" },
            { '\r', "\"\\r\"" },
            { '\t', "\"\\t\"" },
            { '\b', "\"\\b\"" },
            { '\f', "\"\\f\"" },
            { (char)0x01, "\"\\u0001\"" },
            { (char)0x1f, "\"\\u001f\"" },
            { 'A',  "\"A\"" },
            { (char)0xE4, "\"\xE4\"" }   /* 高位字节原样输出 */
        };
        size_t i;
        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            Str s;
            str_init(&s);
            json_escape_char_to(&s, cases[i].c);
            CHECK(strcmp(s.data, cases[i].want) == 0, cases[i].want);
            str_free(&s);
        }
    }

    /* 4.5 UTF-8 输入版 */
    {
        Str s;
        str_init(&s);
        json_escape_utf8_to(&s, "中文\"x\"");
        CHECK(strcmp(s.data, "\"中文\\\"x\\\"\"") == 0, "json_escape_utf8_to");
        str_clear(&s);
        json_escape_utf8_to(&s, NULL);
        CHECK(strcmp(s.data, "\"\"") == 0, "json_escape_utf8_to(NULL)");
        str_free(&s);
    }

    /* 4.6 宽字符版 + 宽字符串 NULL */
    {
        Str s;
        str_init(&s);
        json_escape_wchar_to(&s, L'中');
        CHECK(s.len == 5 && (unsigned char)s.data[1] == 0xE4, "json_escape_wchar_to 中文");
        str_clear(&s);
        json_escape_wchar_to(&s, L'\n');
        CHECK(strcmp(s.data, "\"\\n\"") == 0, "json_escape_wchar_to 换行");
        str_clear(&s);
        json_escape_to(&s, NULL);
        CHECK(strcmp(s.data, "\"\"") == 0, "json_escape_to(NULL)");
        str_clear(&s);
        json_escape_to(&s, L"a\"b\\c");
        CHECK(strcmp(s.data, "\"a\\\"b\\\\c\"") == 0, "宽串中的引号与反斜杠");
        str_free(&s);
    }

    /* 4.7 代理对（宽字符 -> 4 字节 UTF-8） */
    {
        Str s;
        wchar_t pair[3];
        pair[0] = (wchar_t)0xD83D;
        pair[1] = (wchar_t)0xDE00;
        pair[2] = 0;
        str_init(&s);
        json_escape_to(&s, pair);
        CHECK(s.len == 6 && (unsigned char)s.data[1] == 0xF0 &&
                  (unsigned char)s.data[2] == 0x9F,
              "宽字符代理对 -> 4 字节 UTF-8");
        str_free(&s);
    }

    /* 4.8 转义结果能被自己解析回来（含控制字符） */
    {
        Str s;
        str_init(&s);
        json_escape_to(&s, L"tab\there \"q\" \\ back \\n 中文");
        str_puts(&s, "");
        v = json_parse(s.data, a);
        CHECK(v && strcmp(json_string(v, ""),
                          "tab\there \"q\" \\ back \\n 中文") == 0,
              "转义后的串能被解析回原值");
        str_free(&s);
    }

    str_free(&out);
    lc_free(expect);
    json_free(v);
    arena_free(a);
}

/* ------------------------------------------------------- 5. 非法输入 ---- */
static void test_malformed(void) {
    const char *bad[] = {
        "{", "[1,]", "{\"a\":}", "\"unterminated", "01", "nul",
        "{\"a\":1}}", "", "   ", "\t\n\r ", "tru", "fals", "nulll",
        "{\"a\" 1}", "[1 2]", "{\"a\":1,}", "{\"a\":1,,}", "[,1]",
        "\"\\x\"", "\"\\u12\"", "\"\\uZZZZ\"", "\"a", "\"a\\", "\\",
        ",", "+1", ".5", "1.", "1e", "1e+", "-", "--1", "0x10", "00", "-01",
        "[", "{]", "{,}", "]", "}", "[1,2", "{\"a\":1", "TRUE", "True",
        "NaN", "Infinity", "-Infinity", "undefined", "'single'",
        "{\"a\":\"x\ny\"}",     /* 串里裸换行 */
        "{\"a\":\"x\ty\"}",     /* 串里裸 TAB */
        "[1,2,3]x", "nulll", "{\"a\":1}x", "1 2", "\"a\" \"b\"",
        "{\"a\":1}{\"b\":2}", NULL
    };
    Arena *a = arena_new();
    size_t i;
    int n = 0;

    printf("[5] 非法输入必须返回 NULL\n");
    for (i = 0; bad[i]; i++) {
        JsonValue *v = json_parse(bad[i], a);
        n++;
        if (v) {
            g_fail++;
            printf("  FAIL 非法输入被接受: <<%s>>\n", bad[i]);
        } else {
            g_pass++;
        }
    }
    printf("  非法用例 %d 个\n", n);

    /* 允许的边界情况 */
    {
        const char *good[] = {
            "0", "1", "[]", "{}", "true", "false", "null",
            "\"\"", " \t\r\n 42 \n", "[[]]", "{\"\":0}", "{\"a\":[]}",
            "  {\"a\" : [ 1 , 2 ] }  ", "-0.0e0"
        };
        size_t k;
        for (k = 0; k < sizeof(good) / sizeof(good[0]); k++) {
            JsonValue *v = json_parse(good[k], a);
            CHECK(v != NULL, good[k]);
        }
    }
    CHECK(json_parse(NULL, a) == NULL, "NULL 输入");
    CHECK(json_parse("{}", NULL) == NULL, "NULL arena");
    arena_free(a);
}

/* --------------------------------------------------------- 6. 深度 ---- */
static void test_depth(void) {
    Arena *a = arena_new();
    size_t n;
    Str s;
    int depths[] = { 100, 200, 201, 300, 1000 };
    size_t k;

    printf("[6] 嵌套深度限制（上限 200）\n");
    for (k = 0; k < sizeof(depths) / sizeof(depths[0]); k++) {
        int d = depths[k];
        int want = (d <= 200);
        int i;
        str_init(&s);
        for (i = 0; i < d; i++) str_putc(&s, '[');
        for (i = 0; i < d; i++) str_putc(&s, ']');
        {
            JsonValue *v = json_parse(s.data, a);
            CHECK((v != NULL) == want, want ? "<=200 接受" : ">200 拒绝");
        }
        str_free(&s);
    }
    /* 对象同样限制 */
    str_init(&s);
    for (n = 0; n < 300; n++) str_puts(&s, "{\"a\":");
    for (n = 0; n < 300; n++) str_putc(&s, '}');
    CHECK(json_parse(s.data, a) == NULL, "对象嵌套 300 拒绝");
    str_free(&s);

    str_init(&s);
    for (n = 0; n < 150; n++) str_puts(&s, "{\"a\":");
    str_puts(&s, "1");
    for (n = 0; n < 150; n++) str_putc(&s, '}');
    CHECK(json_parse(s.data, a) != NULL, "对象嵌套 150 接受");
    str_free(&s);
    arena_free(a);
}

/* ------------------------------------------------------- 7. 大输入 ---- */
static void test_large(void) {
    Arena *a = arena_new();
    size_t n = 4u * 1024u * 1024u;   /* 4 MB */
    char *big = (char *)malloc(n + 64);
    clock_t t0;
    double secs;
    JsonValue *v;

    printf("[7] 大输入（4 MB 字符串 / 20 万元素数组 / 对象索引）\n");
    CHECK(big != NULL, "malloc 4MB");
    if (!big) { arena_free(a); return; }

    memcpy(big, "{\"s\":\"", 6);
    memset(big + 6, 'x', n);
    big[6 + n] = '"';
    big[6 + n + 1] = '}';
    big[6 + n + 2] = 0;

    t0 = clock();
    v = json_parse(big, a);
    secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    CHECK(v != NULL, "解析 4MB 文档");
    CHECK(json_string(json_get(v, "s"), NULL) != NULL &&
              strlen(json_string(json_get(v, "s"), "")) == n,
          "4MB 字符串长度正确");
    printf("  4 MB 字符串: %.3f s\n", secs);
    CHECK(secs < 5.0, "4MB 字符串不超时");

    /* 4MB 里全是转义（最坏情况：每个字节都要走转义分支） */
    {
        char *esc = (char *)malloc(2u * 1024u * 1024u + 64);
        size_t m = 2u * 1024u * 1024u;
        CHECK(esc != NULL, "malloc 转义串");
        if (esc) {
            memcpy(esc, "{\"s\":\"", 6);
            memset(esc + 6, '\\', m);
            esc[6 + m] = '"';
            esc[6 + m + 1] = '}';
            esc[6 + m + 2] = 0;
            t0 = clock();
            v = json_parse(esc, a);
            secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
            /* 全是合法的 \\ 转义对，解码后长度应为 m/2 */
            CHECK(v != NULL, "解析 2MB 反斜杠转义");
            CHECK(v && strlen(json_string(json_get(v, "s"), "")) == m / 2,
                  "转义解码长度正确");
            printf("  2 MB 反斜杠转义: %.3f s\n", secs);
            CHECK(secs < 5.0, "大量转义不超时");
            free(esc);
        }
    }

    /* 20 万数字的数组 */
    {
        size_t cap = 200000u * 12u + 32u;
        char *arr = (char *)malloc(cap);
        size_t len = 0;
        size_t i;
        CHECK(arr != NULL, "malloc 大数组");
        if (arr) {
            arr[len++] = '[';
            for (i = 0; i < 200000; i++) {
                len += (size_t)snprintf(arr + len, cap - len,
                                        i ? ",%llu" : "%llu",
                                        (unsigned long long)(i % 1000));
            }
            arr[len++] = ']';
            arr[len] = 0;
            t0 = clock();
            v = json_parse(arr, a);
            secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
            CHECK(v != NULL, "解析 20 万元素数组");
            CHECK(json_array_len(v) == 200000, "数组长度 200000");
            CHECK(json_int(json_at(v, 199999), -1) == 999, "最后一个元素");
            CHECK(json_int(json_at(v, 0), -1) == 0, "第一个元素");
            CHECK(json_at(v, 200000) == NULL, "越界");
            printf("  20 万元素数组: %.3f s\n", secs);
            CHECK(secs < 5.0, "大数组不超时");
            free(arr);
        }
    }

    /* 2 万个键的对象：走哈希索引路径 */
    {
        size_t cap = 20000u * 24u + 32u;
        char *ob = (char *)malloc(cap);
        size_t len = 0;
        size_t i;
        CHECK(ob != NULL, "malloc 大对象");
        if (ob) {
            Str q;
            len += (size_t)snprintf(ob + len, cap - len, "{");
            for (i = 0; i < 20000; i++) {
                len += (size_t)snprintf(ob + len, cap - len,
                                        i ? ",\"k%llu\":%llu" : "\"k%llu\":%llu",
                                        (unsigned long long)i,
                                        (unsigned long long)i);
            }
            len += (size_t)snprintf(ob + len, cap - len, "}");
            t0 = clock();
            v = json_parse(ob, a);
            secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
            CHECK(v != NULL, "解析 2 万键对象");
            CHECK(json_int(json_get(v, "k0"), -1) == 0, "k0");
            CHECK(json_int(json_get(v, "k19999"), -1) == 19999, "k19999");
            CHECK(json_get(v, "k20000") == NULL, "不存在的键");
            printf("  2 万键对象: %.3f s\n", secs);
            CHECK(secs < 5.0, "大对象不超时");

            /* 大量查询也不应二次复杂度 */
            str_init(&q);
            t0 = clock();
            for (i = 0; i < 20000; i++) {
                str_clear(&q);
                str_printf(&q, "k%llu", (unsigned long long)i);
                if (json_int(json_get(v, q.data), -1) != (int)i) {
                    g_fail++;
                    printf("  FAIL 查询 %s\n", q.data);
                    break;
                }
            }
            g_pass++;
            secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
            printf("  2 万次查询: %.3f s\n", secs);
            CHECK(secs < 5.0, "查询不超时");
            str_free(&q);
            free(ob);
        }
    }

    /* 大量重复键：后者胜（压力测试去重路径） */
    {
        size_t cap = 20000u * 16u + 32u;
        char *ob = (char *)malloc(cap);
        size_t len = 0;
        size_t i;
        if (ob) {
            len += (size_t)snprintf(ob + len, cap - len, "{");
            for (i = 0; i < 20000; i++) {
                len += (size_t)snprintf(ob + len, cap - len,
                                        i ? ",\"dup\":%llu" : "\"dup\":%llu",
                                        (unsigned long long)i);
            }
            len += (size_t)snprintf(ob + len, cap - len, "}");
            v = json_parse(ob, a);
            CHECK(v != NULL, "2 万重复键可解析");
            CHECK(json_int(json_get(v, "dup"), -1) == 19999, "重复键后者胜");
            free(ob);
        }
    }

    /* BOM + 中文大串 */
    {
        char bomdoc[] = "\xEF\xBB\xBF{\"名字\":\"批量重命名\",\"n\":3}";
        v = json_parse(bomdoc, a);
        CHECK(v != NULL, "接受 UTF-8 BOM");
        CHECK(strcmp(json_string(json_get(v, "名字"), ""), "批量重命名") == 0,
              "非 ASCII 键查找");
    }

    free(big);
    arena_free(a);
}

/* ------------------------------------------- 8. 截断 / 变异模糊测试 ---- */
static void test_fuzz(void) {
    const char *base =
        "{\"a\":[1,2.5,true,null,\"\\u4e2d\\u6587\\uD83D\\uDE00\"],"
        "\"b\":{\"c\":-3e2},\"名\":\"值\"}";
    size_t L = strlen(base);
    char *buf = (char *)malloc(L + 1);
    Arena *a = arena_new();
    size_t i;
    unsigned seed = 12345u;
    int n_ok = 0, n_null = 0, n_mut_ok = 0;

    printf("[8] 截断 / 变异模糊测试（不得崩溃、不得越界）\n");
    CHECK(buf != NULL, "malloc fuzz buffer");
    if (!buf) { arena_free(a); return; }

    /* 8.1 所有前缀：任何截断都必须安全返回 */
    for (i = 0; i <= L; i++) {
        JsonValue *v;
        memcpy(buf, base, i);
        buf[i] = 0;
        v = json_parse(buf, a);
        if (v) n_ok++; else n_null++;
    }
    CHECK((size_t)(n_ok + n_null) == L + 1, "全部前缀都得到确定结果");
    CHECK(n_null > 0, "截断的文档确实被拒绝");
    g_pass++;

    /* 8.2 随机单字节变异 5000 次 */
    for (i = 0; i < 5000; i++) {
        size_t pos;
        JsonValue *v;
        seed = seed * 1103515245u + 12345u;
        pos = (size_t)((seed >> 8) % L);
        seed = seed * 1103515245u + 12345u;
        memcpy(buf, base, L);
        buf[L] = 0;
        buf[pos] = (char)(unsigned char)(seed >> 8);
        v = json_parse(buf, a);
        if (v) n_mut_ok++;
    }
    g_pass++;

    /* 8.3 深层截断：把 \u 转义切在不同的位置 */
    for (i = 0; i < L; i++) {
        if (json_parse(base + i, a)) n_mut_ok++;   /* 从中间开始解析也要安全 */
    }
    g_pass++;

    printf("  前缀 %llu 个（接受 %d / 拒绝 %d）；变异 5000 次（接受 %d）\n",
           (unsigned long long)(L + 1), n_ok, n_null, n_mut_ok);
    free(buf);
    arena_free(a);
}

/* ----------------------------------- 9. 对象索引边界（1..40 个键） ---- */
static void test_obj_index_boundary(void) {
    Arena *a = arena_new();
    int n;
    printf("[9] 对象键数 1..40（线性/哈希索引切换边界）\n");
    for (n = 1; n <= 40; n++) {
        Str s;
        JsonValue *v;
        int i;
        int all_ok = 1;
        str_init(&s);
        str_putc(&s, '{');
        for (i = 0; i < n; i++) {
            if (i) str_putc(&s, ',');
            str_printf(&s, "\"k%d\":%d", i, i);
        }
        str_putc(&s, '}');
        v = json_parse(s.data, a);
        if (!v || !json_is_object(v)) { all_ok = 0; }
        for (i = 0; all_ok && i < n; i++) {
            char key[16];
            snprintf(key, sizeof key, "k%d", i);
            if (json_int(json_get(v, key), -1) != i) all_ok = 0;
        }
        if (all_ok) {
            char miss[16];
            snprintf(miss, sizeof miss, "k%d", n);
            if (json_get(v, miss) != NULL) all_ok = 0;
            if (json_get(v, "") != NULL) all_ok = 0;
        }
        str_free(&s);
        if (!all_ok) {
            g_fail++;
            printf("  FAIL 键数 %d 的查询不一致\n", n);
        } else {
            g_pass++;
        }
    }
    /* 索引建立之后再出现重复键 */
    {
        const char *dup8 =
            "{\"k0\":0,\"k1\":1,\"k2\":2,\"k3\":3,\"k4\":4,\"k5\":5,"
            "\"k6\":6,\"k0\":100,\"k7\":7,\"k2\":200}";
        JsonValue *v = json_parse(dup8, a);
        CHECK(v != NULL, "8 键以上含重复键可解析");
        CHECK(json_int(json_get(v, "k0"), -1) == 100, "索引期重复键后者胜(k0)");
        CHECK(json_int(json_get(v, "k2"), -1) == 200, "索引期重复键后者胜(k2)");
        CHECK(json_int(json_get(v, "k7"), -1) == 7, "后续新键仍可插入");
        CHECK(json_int(json_get(v, "k6"), -1) == 6, "旧键仍可查到");
        CHECK(json_get(v, "k8") == NULL, "不存在的键");
    }
    /* 触发扩容：40 键时索引会 rehash 多次 */
    {
        Str s;
        JsonValue *v;
        int i, ok = 1;
        str_init(&s);
        str_putc(&s, '{');
        for (i = 0; i < 40; i++) {
            if (i) str_putc(&s, ',');
            str_printf(&s, "\"key_%d\":%d", i, i * 3);
        }
        str_putc(&s, '}');
        v = json_parse(s.data, a);
        for (i = 0; ok && i < 40; i++) {
            char key[24];
            snprintf(key, sizeof key, "key_%d", i);
            if (json_int(json_get(v, key), -1) != i * 3) ok = 0;
        }
        CHECK(ok, "40 键（多次 rehash）全部可查");
        str_free(&s);
    }
    arena_free(a);
}

int main(void) {
    printf("=== LocalCraft json.c 自检 ===\n");
    test_roundtrip();
    test_unicode_escapes();
    test_numbers();
    test_escape_writers();
    test_malformed();
    test_depth();
    test_large();
    test_fuzz();
    test_obj_index_boundary();
    printf("=== 通过 %d / 失败 %d ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
