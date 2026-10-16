/* ==========================================================================
 * LocalCraft — 正则引擎测试 / regex engine test suite
 * tests/test_regex.c
 *
 * 编译 build：
 *   gcc -std=c11 -D__USE_MINGW_ANSI_STDIO=1 -DUNICODE -D_UNICODE -Wall -Wextra
 *       -I src src\regex.c src\util.c tests\test_regex.c -o build\test_regex.exe
 *
 * 说明：测试名一律用 ASCII，避免控制台编码干扰；模式里的中文按 UTF-8
 *       源文件直接写（gcc 按 UTF-8 解释宽字符字面量）。
 * ========================================================================== */
#include "lc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <locale.h>
#include <wchar.h>

static int g_pass = 0, g_fail = 0, g_section = 0;

static void note(const wchar_t *fmt, ...) {
    va_list ap;
    wchar_t buf[512];
    va_start(ap, fmt);
    _vsnwprintf(buf, 511, fmt, ap);
    va_end(ap);
    buf[511] = 0;
    wprintf(L"%ls\n", buf);
    fflush(NULL);
}

static void ok(int cond, const wchar_t *what) {
    if (cond) { g_pass++; return; }
    g_fail++;
    wprintf(L"  [FAIL] sec%d %ls\n", g_section, what);
    fflush(NULL);
}

#define SECTION(n) do {                                        \
        g_section = (n);                                       \
        note(L"-- section %d --", g_section);                  \
    } while (0)

/* ---------------------------------------------------------------- helpers */

static LcRegex *mkre(const wchar_t *pat, int icase) {
    wchar_t err[256];
    LcRegex *re;
    err[0] = 0;
    re = regex_compile(pat, icase, err, 256);
    if (!re) note(L"         (compile failed: %ls -> %ls)", pat, err);
    return re;
}

static int m(const wchar_t *pat, const wchar_t *s, int icase) {
    LcRegex *re = mkre(pat, icase);
    int r;
    if (!re) return -1;
    r = regex_matches(re, s);
    regex_free(re);
    return r;
}

static wchar_t *rep(const wchar_t *pat, const wchar_t *s, const wchar_t *r,
                    int icase) {
    LcRegex *re = mkre(pat, icase);
    WStr out;
    wchar_t *res;
    if (!re) return NULL;
    wstr_init(&out);
    if (regex_replace_all(re, s, r, &out) != 0) {
        regex_free(re); wstr_free(&out); return NULL;
    }
    regex_free(re);
    res = wstr_steal(&out);
    if (!res) res = _wcsdup(L"");
    return res;
}

static void chk_rep(const wchar_t *pat, const wchar_t *s, const wchar_t *r,
                    const wchar_t *want, int icase, const wchar_t *what) {
    wchar_t *got = rep(pat, s, r, icase);
    if (!got) {
        g_fail++;
        wprintf(L"  [FAIL] sec%d %ls : compile/replace failed\n",
                g_section, what);
        fflush(NULL);
        return;
    }
    if (wcscmp(got, want) != 0) {
        g_fail++;
        wprintf(L"  [FAIL] sec%d %ls\n", g_section, what);
        wprintf(L"         pat=[%ls] in=[%ls] repl=[%ls]\n", pat, s, r);
        wprintf(L"         want=[%ls] got=[%ls]\n", want, got);
        fflush(NULL);
    } else {
        g_pass++;
    }
    free(got);
}

static void chk_m(const wchar_t *pat, const wchar_t *s, int want,
                  const wchar_t *what) {
    int got = m(pat, s, 0);
    if (got != want) {
        g_fail++;
        wprintf(L"  [FAIL] sec%d %ls | pat=[%ls] in=[%ls] want=%d got=%d\n",
                g_section, what, pat, s, want, got);
        fflush(NULL);
    } else {
        g_pass++;
    }
}

static void chk_m_i(const wchar_t *pat, const wchar_t *s, int want,
                    const wchar_t *what) {
    int got = m(pat, s, 1);
    if (got != want) {
        g_fail++;
        wprintf(L"  [FAIL] sec%d %ls | pat=[%ls] in=[%ls] want=%d got=%d\n",
                g_section, what, pat, s, want, got);
        fflush(NULL);
    } else {
        g_pass++;
    }
}

/* ==================================================================== main */

int main(void)
{
    wchar_t err[256];

    setlocale(LC_ALL, "");
    note(L"==== LocalCraft regex engine test suite ====");

    /* ------------------------------------------------- 1 literals ------- */
    SECTION(1);
    chk_m(L"abc", L"xxabcxx", 1, L"literal unanchored search");
    chk_m(L"abc", L"abx", 0, L"literal no match");
    chk_m(L"", L"", 1, L"empty pattern matches empty string");
    chk_m(L"", L"xyz", 1, L"empty pattern matches any string");
    chk_m(L"a", L"a", 1, L"single char");
    chk_m(L"ab", L"ab", 1, L"two chars");
    chk_m(L"abc", L"abcd", 1, L"unanchored prefix");
    ok(mkre(L"abc", 0) != NULL, L"simple pattern compiles");

    /* ---------------------------------------------- 2 dot / ^ / $ ------ */
    SECTION(2);
    chk_m(L"a.c", L"abc", 1, L"dot matches any char");
    chk_m(L"a.c", L"a\nc", 0, L"dot does not match newline");
    chk_m(L"a.c", L"a\u4e2dc", 1, L"dot matches a CJK char");
    chk_m(L"^abc$", L"abc", 1, L"anchors ^ $");
    chk_m(L"^abc$", L"xabc", 0, L"^ anchor fails");
    chk_m(L"^abc$", L"abcx", 0, L"$ anchor fails");
    chk_m(L"^$", L"", 1, L"^$ matches empty");
    chk_m(L"^$", L"a", 0, L"^$ rejects non-empty");
    chk_m(L"^a", L"ba", 0, L"^ only matches at start");
    chk_m(L"c$", L"abc", 1, L"$ only matches at end");
    chk_m(L".", L"a", 1, L"single dot");

    /* ------------------------------------------------- 3 quantifiers ---- */
    SECTION(3);
    chk_m(L"ab*", L"a", 1, L"star zero times");
    chk_m(L"ab*", L"abbb", 1, L"star many times");
    chk_m(L"ab+", L"a", 0, L"plus needs one");
    chk_m(L"ab+", L"ab", 1, L"plus once");
    chk_m(L"ab?c", L"ac", 1, L"question zero");
    chk_m(L"ab?c", L"abc", 1, L"question once");
    chk_m(L"ab?c", L"abbc", 0, L"question rejects two");
    chk_m(L"^a*$", L"", 1, L"a* matches empty");
    chk_m(L"^a+$", L"", 0, L"a+ rejects empty");

    chk_rep(L"a+?", L"aaa", L"X", L"XXX", 0, L"lazy a+? one at a time");
    chk_rep(L"a+", L"aaa", L"X", L"X", 0, L"greedy a+ whole run");
    chk_rep(L"<.+?>", L"<a><b>", L"[\\0]", L"[<a>][<b>]", 0, L"lazy .+?");
    chk_rep(L"<.+>", L"<a><b>", L"[\\0]", L"[<a><b>]", 0, L"greedy .+");
    chk_rep(L"ab??c", L"abc", L"Y", L"Y", 0, L"lazy ?? still must match c");
    chk_rep(L"a*?", L"aaa", L"X", L"XaXaXaX", 0, L"lazy a*? prefers empty match");

    /* ------------------------------------- 4 bounded repetition {n,m} --- */
    SECTION(4);
    chk_m(L"^a{3}$", L"aaa", 1, L"{3} exact");
    chk_m(L"^a{3}$", L"aa", 0, L"{3} too few");
    chk_m(L"^a{3}$", L"aaaa", 0, L"{3} too many");
    chk_m(L"^a{2,}$", L"aa", 1, L"{2,} lower bound");
    chk_m(L"^a{2,}$", L"a", 0, L"{2,} too few");
    chk_m(L"^a{2,}$", L"aaaaa", 1, L"{2,} more");
    chk_m(L"^a{2,3}$", L"aa", 1, L"{2,3} lower bound");
    chk_m(L"^a{2,3}$", L"aaa", 1, L"{2,3} upper bound");
    chk_m(L"^a{2,3}$", L"aaaa", 0, L"{2,3} too many");
    chk_m(L"^a{0,1}$", L"", 1, L"{0,1} empty");
    chk_m(L"^a{1}$", L"a", 1, L"{1}");
    chk_m(L"^a{0}$", L"", 1, L"{0} matches empty only");
    chk_rep(L"a{2,3}", L"aaaaa", L"X", L"XX", 0, L"{2,3} greedy takes more");
    chk_rep(L"a{2,3}?", L"aaaaa", L"X", L"XXa", 0, L"{2,3}? lazy takes fewer");
    chk_rep(L"a{2,}", L"aaaaa", L"X", L"X", 0, L"{2,} greedy");
    chk_rep(L"a{2,}?", L"aaaaa", L"X", L"XXa", 0, L"{2,}? lazy");
    chk_rep(L"^\\d{3}-\\d{4}$", L"123-4567", L"OK", L"OK", 0, L"phone pattern");
    chk_m(L"^(ab){2}$", L"abab", 1, L"group repeated {2}");

    /* --------------------------------------------- 5 character classes -- */
    SECTION(5);
    chk_m(L"^[a-z]+$", L"hello", 1, L"[a-z]");
    chk_m(L"^[a-z]+$", L"Hello", 0, L"[a-z] rejects uppercase");
    chk_m(L"^[^0-9]+$", L"abc", 1, L"[^0-9]");
    chk_m(L"^[^0-9]+$", L"a1c", 0, L"[^0-9] rejects digits");
    chk_m(L"^[abc]+$", L"cab", 1, L"[abc] enumeration");
    chk_m(L"^[a-cx-z]+$", L"axcz", 1, L"[a-cx-z] multiple ranges");
    chk_m(L"^[a-cx-z]+$", L"aw", 0, L"[a-cx-z] rejects outside");
    chk_m(L"^[\\d]+$", L"1234", 1, L"class \\d");
    chk_m(L"^[\\d]+$", L"12a4", 0, L"class \\d rejects letter");
    chk_m(L"^[\\w]+$", L"a_1", 1, L"class \\w");
    chk_m(L"^[\\s]+$", L" \t\n", 1, L"class \\s");
    chk_m(L"^[\\D]+$", L"abc", 1, L"class \\D");
    chk_m(L"^[\\D]+$", L"ab1", 0, L"class \\D rejects digit");
    chk_m(L"^[\\S]+$", L"abc", 1, L"class \\S");
    chk_m(L"^[^\\d]+$", L"abc", 1, L"[^\\d]");
    chk_m(L"^[^\\d]+$", L"a1", 0, L"[^\\d] rejects digit");
    chk_m(L"^[a-c-]+$", L"a-c", 1, L"literal dash inside class");
    chk_m(L"^-$", L"-", 1, L"dash outside class is literal");
    chk_m(L"^[]]$", L"]", 1, L"']' first in class is literal");
    chk_m(L"^[\\]x]+$", L"]x", 1, L"\\] inside class");
    chk_m(L"^[abc]+$", L"abd", 0, L"anchored class rejects outside char");
    chk_m(L"[abc]+", L"abd", 1, L"unanchored class finds a substring");
    chk_m(L"^[^a]$", L"b", 1, L"single negated class");

    /* ------------------------------------------------------ 6 escapes --- */
    SECTION(6);
    chk_m(L"^\\d+$", L"2024", 1, L"\\d+");
    chk_m(L"^\\d+$", L"20a4", 0, L"\\d+ rejects letter");
    chk_m(L"^\\w+$", L"a_1", 1, L"\\w+");
    chk_m(L"^\\w+$", L"a b", 0, L"\\w+ rejects space");
    chk_m(L"^\\s+$", L"  \t", 1, L"\\s+");
    chk_m(L"^\\s+$", L"abc", 0, L"\\s+ rejects letters");
    chk_m(L"^\\S+$", L"abc", 1, L"\\S+");
    chk_m(L"^\\S+$", L"a b", 0, L"\\S+ rejects space");
    chk_m(L"^\\D+$", L"abc", 1, L"\\D+");
    chk_m(L"^\\W+$", L"!!", 1, L"\\W+");
    chk_m(L"a\\.c", L"a.c", 1, L"escaped dot literal");
    chk_m(L"^a\\.c$", L"abc", 0, L"escaped dot not any char");
    chk_m(L"^a\\*b$", L"a*b", 1, L"escaped star");
    chk_m(L"^\\$\\^$", L"$^", 1, L"escaped $ ^");
    chk_m(L"^\\[\\]$", L"[]", 1, L"escaped [ ]");
    chk_m(L"^\\\\$", L"\\", 1, L"escaped backslash");
    chk_m(L"^a\\tb$", L"a\tb", 1, L"\\t");
    chk_m(L"^a\\nb$", L"a\nb", 1, L"\\n");
    chk_m(L"^a\\rb$", L"a\rb", 1, L"\\r");
    chk_m(L"^\\|$", L"|", 1, L"escaped pipe");
    chk_m(L"^\\+\\?$", L"+?", 1, L"escaped + ?");
    chk_m(L"^\\{\\}$", L"{}", 1, L"escaped { }");

    /* -------------------------------------------- 7 groups / alternation */
    SECTION(7);
    chk_m(L"^(ab)+$", L"abab", 1, L"group + quantifier");
    chk_m(L"^(ab)+$", L"aba", 0, L"group + quantifier fails");
    chk_m(L"^cat|dog$", L"cat", 1, L"alternation left");
    chk_m(L"^cat|dog$", L"dog", 1, L"alternation right");
    chk_m(L"^(cat|dog)$", L"dog", 1, L"alternation in group");
    chk_m(L"^(cat|dog)$", L"cow", 0, L"alternation in group fails");
    chk_m(L"^(a|b|c)+$", L"abcab", 1, L"multi-branch");
    chk_m(L"^(a|b|c)+$", L"abcx", 0, L"anchored multi-branch rejects x");
    chk_m(L"(a|b|c)+", L"abcx", 1, L"unanchored multi-branch matches abc");
    chk_m(L"^(?:ab)+$", L"abab", 1, L"non-capturing group");
    chk_m(L"^(?:ab)+$", L"aba", 0, L"non-capturing group fails");
    chk_m(L"^(a|)$", L"", 1, L"empty branch");
    chk_m(L"^(a|)$", L"a", 1, L"empty branch left");
    chk_m(L"^(foo|foobar)$", L"foobar", 1, L"branch prefix competition");
    chk_m(L"^(a|ab)c$", L"abc", 1, L"backtrack into second branch");
    chk_m(L"^(a|ab)c$", L"ac", 1, L"first branch suffices");

    /* ------------------------------------------------- 8 backreferences -- */
    SECTION(8);
    chk_rep(L"(\\d+)-(\\d+)", L"12-34", L"\\2/\\1", L"34/12", 0, L"swap two groups");
    chk_rep(L"(\\w+)@(\\w+)", L"a@b", L"\\2@\\1", L"b@a", 0, L"swap at @");
    chk_rep(L"(\\d+)", L"a1b22c", L"<\\1>", L"a<1>b<22>c", 0, L"repeated \\1");
    chk_rep(L"(a)(b)(c)", L"abc", L"\\3\\2\\1", L"cba", 0, L"three groups reversed");
    chk_rep(L"\\d+", L"a1b22c", L"<\\0>", L"a<1>b<22>c", 0, L"\\0 whole match");
    chk_rep(L"\\d+", L"a1b", L"[\\&]", L"a[1]b", 0, L"\\& whole match");
    chk_rep(L"(a)", L"a", L"\\1\\9", L"a", 0, L"missing group becomes empty");
    chk_rep(L"(a)", L"a", L"\\\\", L"\\", 0, L"\\\\ emits backslash");
    chk_rep(L"x", L"axb", L"$1", L"a$1b", 0, L"non-backslash copied verbatim");
    chk_rep(L"(\\d{4})-(\\d{2})-(\\d{2})", L"2024-01-02",
            L"\\1\u5e74\\2\u6708\\3\u65e5",
            L"2024\u5e7401\u670802\u65e5", 0, L"date regrouping (CJK literals)");
    chk_rep(L"(a)(b)", L"ab", L"\\0-\\1-\\2", L"ab-a-b", 0, L"whole plus groups");
    chk_rep(L"(\\d+)", L"a1b22", L"", L"ab", 0, L"replace with empty");
    chk_rep(L"(\\d+)", L"a1b22", NULL, L"ab", 0, L"NULL repl == empty");

    /* -------------------------------------------------------- 9 CJK ----- */
    SECTION(9);
    chk_rep(L"IMG_(\\d+)", L"IMG_0001.JPG", L"P\\1", L"P0001.JPG", 0,
            L"IMG_(\\d+) -> P\\1  [REQUIRED]");
    chk_rep(L"\u7167\u7247-(\\d+)", L"\u7167\u7247-001.jpg",
            L"\u65c5\u884c_\\1", L"\u65c5\u884c_001.jpg", 0,
            L"CJK pattern 照片-(\\d+) -> 旅行_\\1  [REQUIRED]");
    chk_m(L"^\u7167\u7247-(\\d+)$", L"\u7167\u7247-001", 1, L"CJK pattern matches");
    chk_m(L"^\u7167\u7247-(\\d+)$", L"\u7167\u7247-abc", 0, L"CJK pattern rejects");
    chk_rep(L"^\u7167\u7247-", L"\u7167\u7247-001.jpg", L"\u56fe\u7247_",
            L"\u56fe\u7247_001.jpg", 0, L"CJK prefix replace");
    chk_m(L"^\u7b2c\\d+\u7ae0$", L"\u7b2c12\u7ae0", 1, L"CJK + ASCII mixed");
    chk_rep(L"(\u7b2c)(\\d+)(\u7ae0)", L"\u7b2c12\u7ae0", L"\\3\\2\\1",
            L"\u7ae012\u7b2c", 0, L"CJK group references");
    chk_m(L"^\\w+$", L"\u4e2d\u6587\u6d4b\u8bd5", 0,
          L"\\w is ASCII-only (CJK is not a word char)");
    chk_m(L"^[\\W]+$", L"\u4e2d\u6587\u6d4b\u8bd5", 1, L"\\W matches CJK");
    chk_m(L"^[\u4e00-\u9fa5]+$", L"\u4e2d\u6587\u5b57", 1, L"CJK range class");
    chk_m(L"^[\u4e00-\u9fa5]+$", L"abc", 0, L"CJK range rejects ASCII");
    chk_rep(L"\u6587\u4ef6", L"a\u6587\u4ef6b\u6587\u4ef6c", L"FILE",
            L"aFILEbFILEc", 0, L"CJK repeated replace");
    chk_rep(L"(.)(.)", L"\u4e2d\u6587", L"\\2\\1", L"\u6587\u4e2d", 0,
            L"CJK counted as single wchar_t");
    chk_rep(L"(.)", L"\u7167\u7247", L"\\1\\1", L"\u7167\u7167\u7247\u7247", 0,
            L"CJK per-char duplication");
    chk_m(L"^.{2}$", L"\u4e2d\u6587", 1, L"dot counts wchar_t");

    /* ------------------------------------------------ 10 ignore_case ----- */
    SECTION(10);
    {
        LcRegex *re = mkre(L"hello", 1);
        ok(re != NULL, L"compile with ignore_case");
        if (re) {
            ok(regex_matches(re, L"HELLO") == 1, L"HELLO matches hello");
            ok(regex_matches(re, L"HeLLo") == 1, L"HeLLo matches hello");
            ok(regex_matches(re, L"world") == 0, L"world does not match hello");
            regex_free(re);
        }
    }
    chk_m_i(L"abc", L"ABC", 1, L"ignore_case basic");
    chk_m_i(L"IMG_(\\d+)", L"img_0001.jpg", 1, L"ignore_case + group");
    chk_rep(L"img_(\\d+)", L"IMG_0001.JPG", L"p\\1", L"p0001.JPG", 1,
            L"ignore_case replace");
    chk_rep(L"[a-z]", L"ABC", L"x", L"xxx", 1, L"ignore_case class");
    {
        LcRegex *re = mkre(L"hello", 0);
        ok(re && regex_matches(re, L"HELLO") == 0,
           L"case-sensitive: HELLO does not match");
        if (re) regex_free(re);
    }
    chk_m_i(L"^[a-z]+$", L"ABC", 1, L"ignore_case ^[a-z]+$ matches ABC");
    chk_m(L"^[a-z]+$", L"ABC", 0, L"case-sensitive ^[a-z]+$ rejects ABC");

    /* ------------------------------------------------- 11 bad patterns --- */
    SECTION(11);
    {
        static const wchar_t *bad[] = {
            L"(", L"a)", L"((a)", L"[a", L"[^", L"\\", L"a**", L"a*+",
            L"*a", L"+a", L"?a", L"a{2,1}", L"(?", L"(?=", L"(?<name>a)",
            L"(?i)a", L"[z-a]", L"[]"
        };
        size_t i;
        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            LcRegex *re;
            err[0] = 0;
            re = regex_compile(bad[i], 0, err, 256);
            if (re) {
                g_fail++;
                wprintf(L"  [FAIL] sec11 bad pattern [%ls] compiled!\n", bad[i]);
                fflush(NULL);
                regex_free(re);
            } else if (err[0] == 0) {
                g_fail++;
                wprintf(L"  [FAIL] sec11 bad pattern [%ls] no error text\n", bad[i]);
                fflush(NULL);
            } else {
                g_pass++;
                note(L"        rejected %-12ls : %ls", bad[i], err);
            }
        }
    }
    {
        LcRegex *re;
        err[0] = 0;
        re = regex_compile(NULL, 0, err, 256);
        ok(re == NULL, L"NULL pattern returns NULL");
        ok(err[0] != 0, L"NULL pattern writes error text");
    }
    {
        wchar_t small[8];
        LcRegex *re;
        int k;
        for (k = 0; k < 8; k++) small[k] = L'#';
        re = regex_compile(L"(", 0, small, 4);
        ok(re == NULL, L"tiny err buffer does not overflow");
    }
    {
        LcRegex *re = regex_compile(L"abc", 0, NULL, 0);
        ok(re != NULL, L"err == NULL still compiles");
        if (re) regex_free(re);
    }
    chk_m(L"^a\\{b$", L"a{b", 1, L"escaped { literal");
    chk_m(L"^a{xy$", L"a{xy", 1, L"invalid {n,m} degrades to literal");
    chk_m(L"^a{,3}$", L"a{,3}", 1, L"{,3} degrades to literal");

    /* --------------------------------------- 12 NULL / empty robustness -- */
    SECTION(12);
    {
        LcRegex *re = mkre(L"a", 0);
        WStr out;
        ok(regex_matches(NULL, L"abc") == 0, L"regex_matches(NULL) == 0");
        ok(regex_matches(re, NULL) == 0, L"regex_matches(re,NULL) == 0");
        wstr_init(&out);
        ok(regex_replace_all(NULL, L"a", L"b", &out) == -1,
           L"regex_replace_all(NULL re) == -1");
        ok(regex_replace_all(re, L"a", L"b", NULL) == -1,
           L"regex_replace_all(NULL out) == -1");
        ok(regex_replace_all(re, NULL, L"b", &out) == 0,
           L"NULL input treated as empty");
        wstr_free(&out);
        regex_free(NULL);
        g_pass++;
        regex_free(re);
    }
    {
        /* 模式 "a" 在空串上不匹配：out 必须保持原样（追加，不覆盖） */
        LcRegex *re = mkre(L"a", 0);
        WStr out;
        wstr_init(&out);
        wstr_puts(&out, L"prefix-");
        ok(regex_replace_all(re, L"", L"X", &out) == 0, L"empty input returns 0");
        ok(out.data && wcscmp(out.data, L"prefix-") == 0,
           L"no match on empty input leaves out untouched (appends, no overwrite)");
        wstr_free(&out);

        /* 空模式在空串上有一个空匹配 -> 追加一次替换 */
        {
            LcRegex *re2 = mkre(L"", 0);
            WStr out2;
            wstr_init(&out2);
            wstr_puts(&out2, L"prefix-");
            ok(regex_replace_all(re2, L"", L"X", &out2) == 0,
               L"empty pattern on empty input returns 0");
            ok(out2.data && wcscmp(out2.data, L"prefix-X") == 0,
               L"empty pattern appends one replacement to existing content");
            wstr_free(&out2);
            regex_free(re2);
        }

        /* 模式是 "a"，在 "abc" 上确实有一个匹配 -> "Zbc"（不是"原样拷贝"） */
        wstr_init(&out);
        ok(regex_replace_all(re, L"abc", L"Z", &out) == 0, L"single match returns 0");
        ok(out.data && wcscmp(out.data, L"Zbc") == 0,
           L"pattern 'a' on 'abc' -> 'Zbc'");
        wstr_free(&out);
        regex_free(re);
    }
    {
        LcRegex *re = mkre(L"b", 0);
        WStr out;
        wstr_init(&out);
        wstr_puts(&out, L"existing:");
        regex_replace_all(re, L"abc", L"B", &out);
        ok(out.data && wcscmp(out.data, L"existing:aBc") == 0,
           L"appends rather than overwrites");
        wstr_free(&out);
        regex_free(re);
    }
    chk_rep(L"a*", L"", L"-", L"-", 0, L"a* on empty gives one empty match");
    chk_rep(L"x*", L"abc", L"-", L"-a-b-c-", 0, L"empty matches advance, no hang");
    chk_rep(L"", L"ab", L"-", L"-a-b-", 0, L"empty pattern matches everywhere");

    /* ------------------------------------------------ 13 combinations ---- */
    SECTION(13);
    chk_m(L"^(\\d{4})-(\\d{2})-(\\d{2})$", L"2024-01-02", 1, L"date anchored");
    chk_m(L"^(\\d{4})-(\\d{2})-(\\d{2})$", L"2024-1-02", 0, L"date incomplete");
    chk_m(L"^[A-Za-z_][A-Za-z0-9_]*$", L"_id9", 1, L"identifier");
    chk_m(L"^[A-Za-z_][A-Za-z0-9_]*$", L"9id", 0, L"identifier cannot start digit");
    chk_rep(L"\\s+", L"  a   b  ", L" ", L" a b ", 0, L"whitespace normalize");
    chk_rep(L"^\\s+|\\s+$", L"  a  ", L"", L"a", 0, L"trim whitespace");
    chk_rep(L"(\\w+)@(\\w+)\\.(\\w+)", L"mail a@b.com x", L"\\3.\\2@\\1",
            L"mail com.b@a x", 0, L"email regroup");
    chk_rep(L"a(bc|de)f", L"abcf adef", L"<\\1>", L"<bc> <de>", 0,
            L"branch capture");
    chk_rep(L"(a+)(b+)", L"aabbb", L"\\2\\1", L"bbbaa", 0, L"adjacent groups");
    chk_rep(L"^(.*?)\\.(.*)$", L"a.b.c", L"\\2#\\1", L"b.c#a", 0,
            L"lazy + greedy mixed");
    chk_rep(L"\\d{2,4}", L"123456789", L"<\\0>", L"<1234><5678>9", 0,
            L"digit blocks leave a lone digit");
    chk_m(L"^(a*)*$", L"aaa", 1, L"nested star terminates");
    chk_m(L"^(a*)*$", L"", 1, L"nested star empty");
    chk_rep(L"^(a*)*$", L"aaa", L"M", L"M", 0, L"nested star replace");
    chk_m(L"^(a+)+$", L"aaaa", 1, L"nested plus");
    chk_m(L"^(a|b)*c$", L"ababc", 1, L"branch star then literal");
    chk_m(L"^(a|b)*c$", L"abab", 0, L"branch star missing tail");
    chk_m(L"^(a|a)*b$", L"aaab", 1, L"ambiguous branch star");

    /* ------------------------------- 14 long input (2,000,000 chars) ----- */
    SECTION(14);
    {
        const size_t N = 2000000;
        wchar_t *big = (wchar_t *)malloc((N + 1) * sizeof(wchar_t));
        if (!big) {
            g_fail++;
            wprintf(L"  [FAIL] sec14 cannot allocate 2MB input\n");
        } else {
            size_t i;
            clock_t t0, t1;
            double secs;

            for (i = 0; i < N; i++) big[i] = (i % 7 == 0) ? L'x' : L'a';
            big[N] = 0;

            {
                LcRegex *re = mkre(L"ZZZZ", 0);
                ok(re != NULL, L"long input: compile ZZZZ");
                t0 = clock();
                ok(re && regex_matches(re, big) == 0, L"long input: ZZZZ no match");
                t1 = clock();
                secs = (double)(t1 - t0) / CLOCKS_PER_SEC;
                note(L"      long(2e6) no-match ZZZZ : %.3f s", secs);
                ok(secs < 5.0, L"long no-match finishes fast");
                if (re) regex_free(re);
            }
            {
                LcRegex *re = mkre(L"x", 0);
                t0 = clock();
                ok(re && regex_matches(re, big) == 1, L"long input: finds x");
                t1 = clock();
                note(L"      long(2e6) match x : %.3f s",
                     (double)(t1 - t0) / CLOCKS_PER_SEC);
                if (re) regex_free(re);
            }
            {
                LcRegex *re = mkre(L"^[ax]+$", 0);
                t0 = clock();
                ok(re && regex_matches(re, big) == 1, L"long input: ^[ax]+$");
                t1 = clock();
                note(L"      long(2e6) ^[ax]+$ : %.3f s",
                     (double)(t1 - t0) / CLOCKS_PER_SEC);
                if (re) regex_free(re);
            }
            {
                LcRegex *re = mkre(L"xa+", 0);
                WStr out;
                wstr_init(&out);
                t0 = clock();
                ok(re && regex_replace_all(re, big, L"#", &out) == 0,
                   L"long input: replace works");
                t1 = clock();
                note(L"      long(2e6) replace xa+ : %.3f s, out %u chars",
                     (double)(t1 - t0) / CLOCKS_PER_SEC, (unsigned)out.len);
                ok(out.len > 0, L"long replace produced output");
                wstr_free(&out);
                if (re) regex_free(re);
            }
            {
                LcRegex *re = mkre(L"(a+)+b", 0);
                if (re) {
                    wchar_t *evil = (wchar_t *)malloc(41 * sizeof(wchar_t));
                    if (evil) {
                        int k;
                        for (k = 0; k < 40; k++) evil[k] = L'a';
                        evil[40] = 0;
                        t0 = clock();
                        regex_matches(re, evil);  /* result irrelevant */
                        t1 = clock();
                        secs = (double)(t1 - t0) / CLOCKS_PER_SEC;
                        note(L"      catastrophic (a+)+b capped at %.3f s", secs);
                        ok(secs < 30.0, L"catastrophic backtracking is capped");
                        free(evil);
                    }
                    regex_free(re);
                }
            }
            free(big);
        }
    }

    /* ----------------------------------- 15 deep nesting / large patterns */
    SECTION(15);
    {
        /* 嵌套在支持范围内：正常编译 + 匹配 */
        wchar_t pat[600];
        int i, n = 30;   /* 捕获组上限是 31，30 层应当可用 */
        LcRegex *re;
        for (i = 0; i < n; i++) pat[i] = L'(';
        pat[n] = L'a';
        for (i = 0; i < n; i++) pat[n + 1 + i] = L')';
        pat[2 * n + 1] = 0;
        err[0] = 0;
        re = regex_compile(pat, 0, err, 256);
        ok(re != NULL, L"30 nested parens compile");
        if (re) {
            ok(regex_matches(re, L"a") == 1, L"30 nested parens match");
            regex_free(re);
        }
    }
    {
        /* 超出嵌套上限：必须干净地报错返回 NULL，绝不爆栈 */
        wchar_t pat[1400];
        int i, n = 500;
        LcRegex *re;
        for (i = 0; i < n; i++) pat[i] = L'(';
        pat[n] = L'a';
        for (i = 0; i < n; i++) pat[n + 1 + i] = L')';
        pat[2 * n + 1] = 0;
        err[0] = 0;
        re = regex_compile(pat, 0, err, 256);
        ok(re == NULL, L"500 nested parens rejected without stack overflow");
        ok(err[0] != 0, L"depth rejection writes an error message");
        if (re) regex_free(re);
    }
    {
        wchar_t pat[8192];
        int i;
        size_t p = 0;
        LcRegex *re;
        for (i = 0; i < 400; i++) {
            if (i) pat[p++] = L'|';
            pat[p++] = L'a';
            pat[p++] = L'b';
        }
        pat[p] = 0;
        re = mkre(pat, 0);
        ok(re != NULL, L"400 alternations compile");
        if (re) {
            ok(regex_matches(re, L"ab") == 1, L"multi-branch match");
            regex_free(re);
        }
    }
    {
        wchar_t pat[8192];
        int i;
        size_t q = 0;
        LcRegex *re;
        for (i = 0; i < 300; i++) {
            if (i) pat[q++] = L'|';
            pat[q++] = L'a';
            pat[q++] = L'b';
        }
        pat[q++] = L'|';
        pat[q++] = L'z';
        pat[q] = 0;
        re = mkre(pat, 0);
        ok(re != NULL, L"branch-backtrack pattern compiles");
        if (re) {
            clock_t t0 = clock();
            ok(regex_matches(re, L"z") == 1, L"backtracks to last branch");
            note(L"      300-branch backtrack : %.3f s",
                 (double)(clock() - t0) / CLOCKS_PER_SEC);
            regex_free(re);
        }
    }
    {
        const size_t N = 100000;
        wchar_t *s = (wchar_t *)malloc((N + 2) * sizeof(wchar_t));
        if (!s) {
            g_fail++;
            wprintf(L"  [FAIL] sec15 cannot allocate test string\n");
        } else {
            size_t i;
            LcRegex *re;
            for (i = 0; i < N; i++) s[i] = L'a';
            s[N] = L'b';
            s[N + 1] = 0;

            re = mkre(L"^a*b", 0);
            {
                clock_t t0 = clock();
                ok(re && regex_matches(re, s) == 1, L"100k a*b greedy");
                note(L"      100k ^a*b : %.3f s",
                     (double)(clock() - t0) / CLOCKS_PER_SEC);
            }
            if (re) regex_free(re);

            re = mkre(L"^a*$", 0);
            ok(re && regex_matches(re, s) == 0, L"^a*$ rejects a...b");
            if (re) regex_free(re);

            re = mkre(L"^(a|a)*b$", 0);
            ok(re && regex_matches(re, s) == 1, L"^(a|a)*b$ branch loop");
            if (re) regex_free(re);

            free(s);
        }
    }

    /* --------------------------------------------------------- summary -- */
    wprintf(L"\n==== RESULT: %d passed, %d failed ====\n", g_pass, g_fail);
    wprintf(L"%ls\n", g_fail == 0 ? L"ALL TESTS PASSED." : L"THERE WERE FAILURES.");
    fflush(NULL);
    return g_fail == 0 ? 0 : 1;
}
