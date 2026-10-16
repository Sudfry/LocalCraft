/* ==========================================================================
 * convert.c —— 模块 2：批量格式转换
 *
 * 分两层，界限很清楚：
 *
 *   【内置】不依赖任何外部程序，断网可用
 *     · 文本互转：txt / md / html / csv / json 之间
 *     · Office 读取：docx / xlsx / pptx -> txt / md / html / csv
 *     · 图片：bmp / png / jpg 互转（走 Windows 自带的 GDI+）
 *
 *   【外部引擎】找到就用，找不到就如实说"未接入"
 *     · Pandoc、LibreOffice、FFmpeg —— 放进 tools\ 即可启用
 *
 * 所有产物都通过 lc_generate_artifact 生成，由 executor 的 CREATE 步骤调用，
 * 因此**转换产物同样进回退日志**：回退时产物进回收站，源文件原样保留。
 * ========================================================================== */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "lc.h"

/* ==================================================== 引擎探测 ========= */

typedef struct { const wchar_t *key; const wchar_t *exe; const wchar_t *hint; } EngineSpec;

static const EngineSpec ENGINES[] = {
    { L"pandoc", L"pandoc.exe",
      L"未找到。把 pandoc.exe 放进程序目录的 tools\\ 即可启用（约 30 MB）" },
    { L"soffice", L"soffice.exe",
      L"未找到。把 LibreOffice 的 program\\ 目录放进 tools\\soffice\\ 即可启用" },
    { L"ffmpeg", L"ffmpeg.exe",
      L"未找到。把 ffmpeg.exe 放进程序目录的 tools\\ 即可启用（约 80 MB）" },
    { L"libraw", L"dcraw.exe",
      L"未找到。RAW 解码需要 dcraw.exe 或 LibRaw，放进 tools\\ 即可" },
};
#define ENGINE_COUNT (sizeof(ENGINES) / sizeof(ENGINES[0]))

/* 在 tools\、程序目录、以及系统 PATH 里找一个可执行文件 */
static wchar_t *find_exe(const wchar_t *exe) {
    wchar_t *tools = lc_tools_dir();
    wchar_t *appdir = lc_app_dir();

    const wchar_t *dirs[3];
    dirs[0] = tools;
    dirs[1] = appdir;
    dirs[2] = NULL;

    wchar_t *found = NULL;
    for (int i = 0; i < 2 && !found; ++i) {
        WStr s;
        wstr_init(&s);
        wstr_puts(&s, dirs[i]);
        wstr_putc(&s, L'\\');
        wstr_puts(&s, exe);
        if (lc_is_file(s.data)) found = wcsdup(s.data);
        wstr_free(&s);
    }

    /* 常见的嵌套目录：tools\soffice\program\soffice.exe */
    if (!found) {
        static const wchar_t *subs[] = {
            L"\\soffice\\program\\", L"\\LibreOffice\\program\\",
            L"\\bin\\", L"\\pandoc\\", L"\\ffmpeg\\bin\\", NULL
        };
        for (int i = 0; subs[i] && !found; ++i) {
            WStr s;
            wstr_init(&s);
            wstr_puts(&s, tools);
            wstr_puts(&s, subs[i]);
            wstr_puts(&s, exe);
            if (lc_is_file(s.data)) found = wcsdup(s.data);
            wstr_free(&s);
        }
    }

    /* PATH（自己按分号切，MinGW 的 wcstok 是不可重入的两参数版本） */
    if (!found) {
        wchar_t buf[32768];
        DWORD n = GetEnvironmentVariableW(L"PATH", buf, 32767);
        if (n > 0 && n < 32767) {
            buf[n] = 0;
            wchar_t *seg = buf;
            while (seg && *seg && !found) {
                wchar_t *semi = wcschr(seg, L';');
                if (semi) *semi = 0;
                if (*seg) {
                    WStr s;
                    wstr_init(&s);
                    wstr_puts(&s, seg);
                    if (s.len && !lc_is_sep(s.data[s.len - 1])) wstr_putc(&s, L'\\');
                    wstr_puts(&s, exe);
                    if (lc_is_file(s.data)) found = wcsdup(s.data);
                    wstr_free(&s);
                }
                seg = semi ? semi + 1 : NULL;
            }
        }
    }

    free(tools);
    free(appdir);
    return found;
}

void convert_probe_engines(ConvertEngine *out, size_t n_out) {
    for (size_t i = 0; i < n_out && i < ENGINE_COUNT; ++i) {
        out[i].key = wcsdup(ENGINES[i].key);
        out[i].path = find_exe(ENGINES[i].exe);
        out[i].found = out[i].path != NULL;
        if (!out[i].path) out[i].path = wcsdup(ENGINES[i].hint);
    }
}

void convert_engine_free(ConvertEngine *e) {
    if (!e) return;
    free(e->key); free(e->path);
    e->key = e->path = NULL;
}

/* ==================================================== 跑外部程序 ======= */

/* 静默执行，等待结束，返回退出码（-1 表示启动失败）。绝不弹出窗口。 */
static int run_quiet(const wchar_t *cmdline, DWORD timeout_ms) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    memset(&pi, 0, sizeof(pi));

    wchar_t *cmd = _wcsdup(cmdline);
    if (!cmd) return -1;

    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        free(cmd);
        return -1;
    }
    free(cmd);

    DWORD r = WaitForSingleObject(pi.hProcess, timeout_ms);
    int code = -1;
    if (r == WAIT_OBJECT_0) {
        DWORD ec = 0;
        GetExitCodeProcess(pi.hProcess, &ec);
        code = (int)ec;
    } else {
        TerminateProcess(pi.hProcess, 1);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code;
}

/* ==================================================== 文本写出 ========= */

static int write_text_file(const wchar_t *path, const wchar_t *text) {
    char *u8 = lc_wcs_to_utf8(text ? text : L"");
    if (!u8) return -1;
    wchar_t *lp = lc_long_path(path);
    if (!lp) { free(u8); return -1; }
    HANDLE h = CreateFileW(lp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) { free(u8); return -1; }
    /* 带 UTF-8 BOM：Windows 记事本 / Excel 打开中文才不乱码 */
    DWORD w = 0;
    const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
    WriteFile(h, bom, 3, &w, NULL);
    WriteFile(h, u8, (DWORD)strlen(u8), &w, NULL);
    CloseHandle(h);
    free(u8);
    return 0;
}

/* 把抽取出来的纯文本（段落用 \n 分隔）转成 Markdown */
static wchar_t *text_to_markdown(const wchar_t *text, const wchar_t *title) {
    WStr out;
    wstr_init(&out);
    if (title && title[0]) {
        wstr_printf(&out, L"# %ls\n\n", title);
    }
    const wchar_t *p = text;
    while (*p) {
        const wchar_t *eol = wcschr(p, L'\n');
        size_t len = eol ? (size_t)(eol - p) : wcslen(p);
        while (len > 0 && (p[len - 1] == L'\r' || p[len - 1] == L' ')) len--;
        if (len > 0) {
            wstr_putn(&out, p, len);
            wstr_puts(&out, L"\n\n");
        }
        p = eol ? eol + 1 : p + len;
    }
    return wstr_steal(&out);
}

/* 转义 HTML */
static void html_escape(WStr *out, const wchar_t *s, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        switch (s[i]) {
            case L'&': wstr_puts(out, L"&amp;"); break;
            case L'<': wstr_puts(out, L"&lt;");  break;
            case L'>': wstr_puts(out, L"&gt;");  break;
            default:   wstr_putc(out, s[i]);
        }
    }
}

static wchar_t *text_to_html(const wchar_t *text, const wchar_t *title) {
    WStr out;
    wstr_init(&out);
    wstr_puts(&out,
        L"<!doctype html>\n<html lang=\"zh-CN\">\n<head>\n"
        L"<meta charset=\"utf-8\">\n<title>");
    html_escape(&out, title ? title : L"", title ? wcslen(title) : 0);
    wstr_puts(&out,
        L"</title>\n<style>\n"
        L"body{font-family:\"Microsoft YaHei\",\"Segoe UI\",sans-serif;"
        L"max-width:46em;margin:3em auto;padding:0 1.2em;line-height:1.8;"
        L"color:#1f2328;}\n"
        L"h1{font-size:1.7em;border-bottom:2px solid #e4e7ec;padding-bottom:.3em;}\n"
        L"p{margin:.9em 0;}\n</style>\n</head>\n<body>\n");
    if (title && title[0]) {
        wstr_puts(&out, L"<h1>");
        html_escape(&out, title, wcslen(title));
        wstr_puts(&out, L"</h1>\n");
    }
    const wchar_t *p = text;
    while (*p) {
        const wchar_t *eol = wcschr(p, L'\n');
        size_t len = eol ? (size_t)(eol - p) : wcslen(p);
        while (len > 0 && (p[len - 1] == L'\r' || p[len - 1] == L' ')) len--;
        if (len > 0) {
            wstr_puts(&out, L"<p>");
            html_escape(&out, p, len);
            wstr_puts(&out, L"</p>\n");
        }
        p = eol ? eol + 1 : p + len;
    }
    wstr_puts(&out, L"</body>\n</html>\n");
    return wstr_steal(&out);
}

/* Markdown -> 纯文本（剥掉标记） */
static wchar_t *markdown_to_text(const wchar_t *md) {
    WStr out;
    wstr_init(&out);
    const wchar_t *p = md;
    int in_code = 0;
    while (*p) {
        const wchar_t *eol = wcschr(p, L'\n');
        size_t len = eol ? (size_t)(eol - p) : wcslen(p);
        if (len >= 3 && wcsncmp(p, L"```", 3) == 0) { in_code = !in_code; p = eol ? eol + 1 : p + len; continue; }
        size_t s = 0;
        if (!in_code) {
            /* 去掉行首标记 */
            while (s < len && p[s] == L'#') s++;
            while (s < len && p[s] == L' ') s++;
            if (s < len && p[s] == L'>') { s++; while (s < len && p[s] == L' ') s++; }
            if (s + 1 < len && (p[s] == L'-' || p[s] == L'*') && p[s+1] == L' ') {
                wstr_puts(&out, L"  · ");
                s += 2;
            }
        }
        size_t e = len;
        while (e > s && (p[e-1] == L' ' || p[e-1] == L'\r')) e--;
        if (e > s) {
            /* 去掉行内标记 */
            for (size_t i = s; i < e; ++i) {
                wchar_t c = p[i];
                if (c == L'*' || c == L'`' || c == L'~' || c == L'^') continue;
                if (c == L'[') { /* 只留文字 */ while (i < e && p[i] != L']') wstr_putc(&out, p[i++]); 
                                  while (i < e && p[i] != L')') i++;
                                  continue; }
                wstr_putc(&out, c);
            }
            wstr_puts(&out, L"\n");
        }
        p = eol ? eol + 1 : p + len;
    }
    return wstr_steal(&out);
}

/* ==================================================== CSV 写出 ========= */

static int write_csv_from_text(const wchar_t *path, const wchar_t *text) {
    WStr out;
    wstr_init(&out);
    const wchar_t *p = text;
    while (*p) {
        const wchar_t *eol = wcschr(p, L'\n');
        size_t len = eol ? (size_t)(eol - p) : wcslen(p);
        while (len > 0 && p[len-1] == L'\r') len--;
        if (len > 0) {
            for (size_t i = 0; i < len; ++i) {
                wchar_t c = p[i];
                if (c == L'\t') wstr_putc(&out, L',');
                else if (c == L',' || c == L'"') { wstr_putc(&out, L'"'); wstr_putc(&out, c); wstr_putc(&out, L'"'); }
                else wstr_putc(&out, c);
            }
            wstr_puts(&out, L"\r\n");
        }
        p = eol ? eol + 1 : p + len;
    }
    int rc = write_text_file(path, out.data);
    wstr_free(&out);
    return rc;
}

/* ==================================================== 图片转换 ========= */
/* 用 GDI+ 做，Windows 自带，不需要任何第三方库。
   注意：MinGW 的 gdiplus.h 提供的是**扁平 C API**（Gdip* 系列），
   不是 MSVC 那套 C++ 包装类，所以这里不能用 Bitmap::FromFile。 */

#include <gdiplus.h>

/* EncoderQuality 在头文件里只是 extern 声明，MinGW 不提供定义，自己写一份 */
static const GUID LC_EncoderQuality =
    { 0x1d5be4b5, 0xfa4a, 0x452d,
      { 0x9c, 0xdd, 0x5d, 0xb3, 0x51, 0x05, 0xe7, 0xeb } };

static const wchar_t *image_mime_for(const wchar_t *ext) {
    if (lc_wcs_ieq(ext, L".png"))  return L"image/png";
    if (lc_wcs_ieq(ext, L".jpg") || lc_wcs_ieq(ext, L".jpeg")) return L"image/jpeg";
    if (lc_wcs_ieq(ext, L".bmp"))  return L"image/bmp";
    if (lc_wcs_ieq(ext, L".gif"))  return L"image/gif";
    if (lc_wcs_ieq(ext, L".tif") || lc_wcs_ieq(ext, L".tiff")) return L"image/tiff";
    return NULL;
}

static int image_convert(const wchar_t *src, const wchar_t *dst,
                         wchar_t *err, size_t errcch) {
    const wchar_t *mime = image_mime_for(lc_extname(dst));
    if (!mime) {
        swprintf(err, errcch, L"不支持的图片目标格式：%ls", lc_extname(dst));
        return -1;
    }

    GdiplusStartupInput gsi;
    memset(&gsi, 0, sizeof(gsi));
    gsi.GdiplusVersion = 1;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &gsi, NULL) != 0) {
        swprintf(err, errcch, L"无法初始化 GDI+");
        return -1;
    }

    int rc = -1;
    GpBitmap *bmp = NULL;
    if (GdipCreateBitmapFromFile(src, &bmp) == Ok && bmp) {
        /* 找目标格式的编码器 CLSID */
        UINT num = 0, size = 0;
        GdipGetImageEncodersSize(&num, &size);
        CLSID clsid;
        int found = 0;
        if (size > 0) {
            ImageCodecInfo *info = (ImageCodecInfo *)malloc(size);
            if (info && GdipGetImageEncoders(num, size, info) == Ok) {
                for (UINT i = 0; i < num; ++i) {
                    if (info[i].MimeType && wcscmp(info[i].MimeType, mime) == 0) {
                        clsid = info[i].Clsid;
                        found = 1;
                        break;
                    }
                }
            }
            free(info);
        }
        if (!found) {
            swprintf(err, errcch, L"系统缺少 %ls 编码器", mime);
        } else {
            EncoderParameters params;
            memset(&params, 0, sizeof(params));
            ULONG quality = 92;                 /* JPEG 用较高质量，中文截图不糊 */
            params.Count = 1;
            params.Parameter[0].Guid = LC_EncoderQuality;
            params.Parameter[0].Type = EncoderParameterValueTypeLong;
            params.Parameter[0].NumberOfValues = 1;
            params.Parameter[0].Value = &quality;

            GpStatus st = (wcscmp(mime, L"image/jpeg") == 0)
                        ? GdipSaveImageToFile(bmp, dst, &clsid, &params)
                        : GdipSaveImageToFile(bmp, dst, &clsid, NULL);
            if (st == Ok) rc = 0;
            else swprintf(err, errcch, L"图片保存失败（GDI+ 状态 %d）", (int)st);
        }
        GdipDisposeImage((GpImage *)bmp);
    } else {
        swprintf(err, errcch,
                 L"无法解码源图片（HEIC 等格式需要额外组件，PNG/JPG/BMP/GIF/TIFF 可直接互转）");
    }

    GdiplusShutdown(token);
    return rc;
}

/* ==================================================== 内置支持判定 ===== */

static int is_office(const wchar_t *p) {
    return lc_wcs_ieq(lc_extname(p), L".docx") ||
           lc_wcs_ieq(lc_extname(p), L".docm") ||
           lc_wcs_ieq(lc_extname(p), L".xlsx") ||
           lc_wcs_ieq(lc_extname(p), L".xlsm") ||
           lc_wcs_ieq(lc_extname(p), L".pptx") ||
           lc_wcs_ieq(lc_extname(p), L".pptm");
}

static int is_image_ext(const wchar_t *e) {
    return lc_wcs_ieq(e, L".png") || lc_wcs_ieq(e, L".jpg") ||
           lc_wcs_ieq(e, L".jpeg") || lc_wcs_ieq(e, L".bmp") ||
           lc_wcs_ieq(e, L".gif") || lc_wcs_ieq(e, L".tif") ||
           lc_wcs_ieq(e, L".tiff");
}

int convert_builtin_supported(const wchar_t *src_path, const wchar_t *dst_path) {
    const wchar_t *se = lc_extname(src_path);
    const wchar_t *de = lc_extname(dst_path);

    if (is_image_ext(se) && is_image_ext(de)) return 1;

    /* 文本类目标 */
    int dst_text = lc_wcs_ieq(de, L".txt") || lc_wcs_ieq(de, L".md") ||
                   lc_wcs_ieq(de, L".markdown") || lc_wcs_ieq(de, L".html") ||
                   lc_wcs_ieq(de, L".htm") || lc_wcs_ieq(de, L".csv");
    if (!dst_text) return 0;

    if (is_office(src_path)) return 1;
    if (extract_supported(src_path)) return 1;
    return 0;
}

/*
 * 判断一次转换到底可不可行。
 *
 * 返回：
 *   2 = 内置就能完成（图片互转、文档抽取成 txt/md/html/csv）
 *   1 = 内置做不了，但外部引擎有可能完成（docx→pdf、音频转码之类）
 *   0 = **这种组合根本不成立**，装什么引擎都没用
 *
 * 为什么需要这个函数
 * ------------------
 * 之前界面这里是这样写的：
 *     if (!convert_builtin_supported(src, dst)) {
 *         // 内置不支持也先列出来，执行时会走外部引擎
 *     }
 * 花括号里是空的 —— 判断算了，结果丢了。后果是选"转成 PNG"时，
 * 同一文件夹里的 .txt / .md 也被列进预览表，而且标成"正常、可执行"，
 * 点下去只会失败。测试数据一跑就露出来了。
 *
 * 关键判断是图片和非图片之间那道墙：把 txt 变成 png 需要先"看懂"内容，
 * 那是 OCR / 理解类的工作，本项目明确不做，所以直接判 0，
 * 而不是留给执行阶段去报一个含糊的错。
 */
int convert_feasibility(const wchar_t *src_path, const wchar_t *dst_path) {
    if (convert_builtin_supported(src_path, dst_path)) return 2;

    const wchar_t *se = lc_extname(src_path);
    const wchar_t *de = lc_extname(dst_path);

    int src_img = is_image_ext(se);
    int dst_img = is_image_ext(de);

    /* 非图片 → 图片：要先理解内容才能画出来，做不到 */
    if (dst_img && !src_img) return 0;
    /* 图片 → 非图片：提取图片里的文字属于 OCR，本项目不做 */
    if (src_img && !dst_img) return 0;

    /* 其余情况交给外部引擎去试（Pandoc / LibreOffice / FFmpeg） */
    return 1;
}

/* ==================================================== 外部引擎兜底 ===== */

/* 源文件与目标格式都不在"内置"覆盖范围内时，交给外部引擎。
   LibreOffice 会把结果写进 --outdir，文件名由它自己决定，所以要再搬一次。 */
static int run_engine_convert(const wchar_t *src, const wchar_t *dst,
                              wchar_t *err, size_t errcch) {
    const wchar_t *de = lc_extname(dst);
    wchar_t ext_no_dot[64];
    wcsncpy(ext_no_dot, (*de == L'.') ? de + 1 : de, 63);
    ext_no_dot[63] = 0;

    /* ---- 1) Pandoc：文档类互转的首选 ---- */
    wchar_t *pandoc = find_exe(L"pandoc.exe");
    if (pandoc) {
        WStr cmd;
        wstr_init(&cmd);
        wstr_printf(&cmd, L"\"%ls\" \"%ls\" -o \"%ls\" --standalone", pandoc, src, dst);
        int code = run_quiet(cmd.data, 120000);
        wstr_free(&cmd);
        free(pandoc);
        if (code == 0 && lc_is_file(dst)) return 0;
    }

    /* ---- 2) LibreOffice：Office / PDF 互转，版式保真度最高 ---- */
    wchar_t *soffice = find_exe(L"soffice.exe");
    if (soffice) {
        wchar_t *outdir = lc_dirname(dst);
        WStr cmd;
        wstr_init(&cmd);
        wstr_printf(&cmd,
            L"\"%ls\" --headless --norestore --convert-to %ls --outdir \"%ls\" \"%ls\"",
            soffice, ext_no_dot, outdir, src);
        int code = run_quiet(cmd.data, 180000);
        wstr_free(&cmd);
        free(soffice);
        if (code == 0) {
            /* LibreOffice 产出的文件名是"源主名.目标扩展名" */
            wchar_t *stem = lc_stem(src);
            WStr made;
            wstr_init(&made);
            wstr_puts(&made, outdir);
            if (made.len && !lc_is_sep(made.data[made.len - 1])) wstr_putc(&made, L'\\');
            wstr_puts(&made, stem);
            wstr_puts(&made, L".");
            wstr_puts(&made, ext_no_dot);
            free(stem);

            if (lc_is_file(made.data)) {
                int moved = 0;
                if (lc_same_path(made.data, dst)) {
                    moved = 1;
                } else {
                    wchar_t *ls = lc_long_path(made.data);
                    wchar_t *ld = lc_long_path(dst);
                    if (ls && ld && MoveFileExW(ls, ld, MOVEFILE_REPLACE_EXISTING))
                        moved = 1;
                    free(ls); free(ld);
                }
                if (moved) { wstr_free(&made); free(outdir); return 0; }
            }
            wstr_free(&made);
        }
        free(outdir);
    }

    /* ---- 3) FFmpeg：音视频 ---- */
    wchar_t *ffmpeg = find_exe(L"ffmpeg.exe");
    if (ffmpeg) {
        WStr cmd;
        wstr_init(&cmd);
        wstr_printf(&cmd, L"\"%ls\" -y -i \"%ls\" \"%ls\"", ffmpeg, src, dst);
        int code = run_quiet(cmd.data, 600000);
        wstr_free(&cmd);
        free(ffmpeg);
        if (code == 0 && lc_is_file(dst)) return 0;
    }

    /* ---- 都没成功：如实说明，并给出可操作的建议 ---- */
    swprintf(err, errcch,
             L"内置转换不支持 %ls → %ls。"
             L"把 pandoc.exe（文档互转）或 LibreOffice 的 program\\ 目录放进程序目录的 "
             L"tools\\ 文件夹后重启即可启用；libraw/dcraw 用于相机 RAW。",
             lc_extname(src), de);
    return -1;
}

/* ==================================================== 产物生成 ========= */

int lc_generate_artifact(const wchar_t *generator, const wchar_t *src,
                         const wchar_t *dst, wchar_t *err, size_t errcch) {
    if (err && errcch) err[0] = 0;
    if (!generator || !generator[0]) {
        if (err) swprintf(err, errcch, L"缺少生成规格");
        return -1;
    }

    /*
     * "edit:<flags>:<hexfind>:<hexreplace>" —— 文本内容批量查找替换。
     *
     * 这里传进来的 src 是**回收站里的原始副本**，dst 是原路径。
     * 由 planner 在拆解步骤时安排成 MOVE(原文件→回收站) + CREATE(原路径)，
     * 所以逆序回退会先删掉改过的版本、再把原件搬回来 —— 内容能完整还原。
     *
     * 放在最前面判断，因为它和下面的导出/转换没有重叠。
     */
    if (lc_wcs_starts_ci(generator, L"edit:")) {
        return findrep_apply(generator, src, dst, err, errcch);
    }

    /* "tbl:<hex>" —— 表格处理（筛选 / 排序 / 选列 / 合并）产出的 CSV */
    if (lc_wcs_starts_ci(generator, L"tbl:")) {
        return table_generate(generator, src, dst, err, errcch);
    }

    /* "export:docx" —— 由 Markdown 源文件导出 */
    if (lc_wcs_starts_ci(generator, L"export:")) {
        const wchar_t *fmt = generator + 7;
        size_t len = 0;
        unsigned char *data = NULL;
        wchar_t *lp = lc_long_path(src);
        if (lp) {
            HANDLE h = CreateFileW(lp, GENERIC_READ, FILE_SHARE_READ, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER sz;
                if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 &&
                    sz.QuadPart < (LONGLONG)(64ll << 20)) {
                    len = (size_t)sz.QuadPart;
                    data = (unsigned char *)malloc(len + 1);
                    if (data) {
                        DWORD got = 0;
                        if (!ReadFile(h, data, (DWORD)len, &got, NULL)) got = 0;
                        len = got;
                        data[len] = 0;
                    }
                }
                CloseHandle(h);
            }
            free(lp);
        }
        if (!data) {
            if (err) swprintf(err, errcch, L"无法读取源文件：%ls", src);
            return -1;
        }
        wchar_t *md = decode_text_bytes(data, len);
        free(data);
        if (!md) {
            if (err) swprintf(err, errcch, L"无法识别源文件编码");
            return -1;
        }

        int rc;
        if (lc_wcs_ieq(fmt, L"docx"))
            rc = export_docx_file(md, dst, err, errcch);
        else if (lc_wcs_ieq(fmt, L"pdf"))
            rc = export_pdf_file(md, dst, err, errcch);
        else if (lc_wcs_ieq(fmt, L"md"))
            rc = export_markdown_file(md, dst, err, errcch);
        else {
            if (err) swprintf(err, errcch, L"不支持的导出格式：%ls", fmt);
            rc = -1;
        }
        free(md);
        return rc;
    }

    /* "convert:<ext>" —— 先内置，内置不行再交给外部引擎 */
    if (lc_wcs_starts_ci(generator, L"convert:")) {
        const wchar_t *de = lc_extname(dst);
        const wchar_t *se = lc_extname(src);

        /* 图片走 GDI+ */
        if (is_image_ext(se) && is_image_ext(de))
            return image_convert(src, dst, err, errcch);

        /* 能抽取文本的源，用内置路径 */
        if (!extract_supported(src)) {
            return run_engine_convert(src, dst, err, errcch);
        }

        wchar_t ex[512];
        ex[0] = 0;
        wchar_t *text = extract_text(src, 4u << 20, ex, 512);
        if (!text) {
            /* 抽取失败不代表要放弃：可能外部引擎能处理 */
            return run_engine_convert(src, dst, err, errcch);
        }

        wchar_t *title = lc_stem(src);
        int rc;
        if (lc_wcs_ieq(de, L".md") || lc_wcs_ieq(de, L".markdown")) {
            wchar_t *md = text_to_markdown(text, title);
            rc = md ? write_text_file(dst, md) : -1;
            free(md);
        } else if (lc_wcs_ieq(de, L".html") || lc_wcs_ieq(de, L".htm")) {
            wchar_t *html = text_to_html(text, title);
            rc = html ? write_text_file(dst, html) : -1;
            free(html);
        } else if (lc_wcs_ieq(de, L".csv")) {
            rc = write_csv_from_text(dst, text);
        } else if (lc_wcs_ieq(de, L".txt")) {
            /* Markdown 源要剥标记；其它原样 */
            if (lc_wcs_ieq(se, L".md") || lc_wcs_ieq(se, L".markdown")) {
                wchar_t *plain = markdown_to_text(text);
                rc = plain ? write_text_file(dst, plain) : -1;
                free(plain);
            } else {
                rc = write_text_file(dst, text);
            }
        } else {
            if (err) swprintf(err, errcch, L"不支持的转换目标：%ls", de);
            rc = -1;
        }
        free(title);
        free(text);
        if (rc != 0 && err && !err[0])
            swprintf(err, errcch, L"写入目标文件失败：%ls", dst);
        return rc;
    }

    if (err) swprintf(err, errcch, L"无法识别的生成规格：%ls", generator);
    return -1;
}
