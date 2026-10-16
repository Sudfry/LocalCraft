/* ==========================================================================
 * embed.c —— 构建期工具：把 webui/ 下的界面文件打包成 C 源码
 *
 * 用法：  embed.exe <webui目录> <输出.c>
 *
 * 之所以自己写而不是用 xxd/第三方工具：让整个构建过程只依赖 gcc，
 * 换一台没装任何东西的 Windows 也能编译出来。
 * ========================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef struct {
    char  *name;            /* 文件名，如 index.html */
    char  *symbol;          /* C 标识符片段 */
    unsigned char *data;
    size_t len;
} Asset;

static char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    *len_out = got;
    return buf;
}

static void make_symbol(const char *name, char *out, size_t cch) {
    size_t j = 0;
    for (size_t i = 0; name[i] && j + 1 < cch; ++i) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9'))
            out[j++] = c;
        else
            out[j++] = '_';
    }
    out[j] = 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "用法: embed <webui目录> <输出文件.c>\n");
        return 2;
    }
    const char *dir = argv[1];
    const char *out_path = argv[2];

    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "无法读取目录：%s\n", dir);
        return 1;
    }

    Asset *assets = NULL;
    size_t n = 0, cap = 0;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const char *dot = strrchr(fd.cFileName, '.');
        if (!dot) continue;
        if (_stricmp(dot, ".html") != 0 && _stricmp(dot, ".css") != 0 &&
            _stricmp(dot, ".js") != 0 && _stricmp(dot, ".svg") != 0 &&
            _stricmp(dot, ".ico") != 0 && _stricmp(dot, ".png") != 0)
            continue;

        char full[MAX_PATH * 2];
        snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);

        size_t len = 0;
        char *data = read_file(full, &len);
        if (!data) continue;

        if (n + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 8;
            Asset *na = (Asset *)realloc(assets, ncap * sizeof(Asset));
            if (!na) { free(data); break; }
            assets = na;
            cap = ncap;
        }
        assets[n].name = _strdup(fd.cFileName);
        assets[n].symbol = (char *)malloc(strlen(fd.cFileName) + 8);
        make_symbol(fd.cFileName, assets[n].symbol, strlen(fd.cFileName) + 8);
        assets[n].data = (unsigned char *)data;
        assets[n].len = len;
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    if (n == 0) {
        fprintf(stderr, "目录里没有可打包的界面文件：%s\n", dir);
        return 1;
    }

    FILE *o = fopen(out_path, "wb");
    if (!o) { fprintf(stderr, "无法写出：%s\n", out_path); return 1; }

    fprintf(o,
        "/* 本文件由 tools/embed.c 自动生成，请勿手工修改。\n"
        "   内容来源：webui/ 下的界面文件。 */\n"
        "#include \"lc.h\"\n"
        "#include <string.h>\n"
        "#include <stddef.h>\n\n");

    for (size_t i = 0; i < n; ++i) {
        fprintf(o, "static const unsigned char asset_%s[] = {", assets[i].symbol);
        for (size_t k = 0; k < assets[i].len; ++k) {
            if (k % 24 == 0) fprintf(o, "\n    ");
            fprintf(o, "%u,", (unsigned)assets[i].data[k]);
        }
        fprintf(o, "\n};\n");
        fprintf(o, "static const size_t asset_%s_len = %llu;\n\n",
                assets[i].symbol, (unsigned long long)assets[i].len);
    }

    fprintf(o, "typedef struct { const char *name; const unsigned char *data;"
               " size_t len; } LcAsset;\n");
    fprintf(o, "static const LcAsset LC_ASSETS[] = {\n");
    for (size_t i = 0; i < n; ++i)
        fprintf(o, "    { \"%s\", asset_%s, asset_%s_len },\n",
                assets[i].name, assets[i].symbol, assets[i].symbol);
    fprintf(o, "};\n");
    fprintf(o, "static const size_t LC_ASSET_COUNT = %llu;\n\n", (unsigned long long)n);

    fprintf(o,
        "const char *webui_asset(const char *name, size_t *len_out) {\n"
        "    if (!name) return NULL;\n"
        "    for (size_t i = 0; i < LC_ASSET_COUNT; ++i) {\n"
        "        if (_stricmp(LC_ASSETS[i].name, name) == 0) {\n"
        "            if (len_out) *len_out = LC_ASSETS[i].len;\n"
        "            return (const char *)LC_ASSETS[i].data;\n"
        "        }\n"
        "    }\n"
        "    if (len_out) *len_out = 0;\n"
        "    return NULL;\n"
        "}\n");

    fclose(o);
    printf("已打包 %llu 个界面文件 -> %s\n", (unsigned long long)n, out_path);

    for (size_t i = 0; i < n; ++i) { free(assets[i].name); free(assets[i].symbol); free(assets[i].data); }
    free(assets);
    return 0;
}
