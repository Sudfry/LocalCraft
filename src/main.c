/* ==========================================================================
 * main.c —— LocalCraft 入口
 *
 *   无参数 / gui      启动本地服务 + 打开无边框应用窗口（默认）
 *   --headless        只启动服务，自己用浏览器打开（方便调试界面）
 *   --browser <路径>  指定浏览器可执行文件
 *   --port <n>        固定端口（默认自动分配）
 *   --no-app-mode     用普通浏览器标签页打开
 *   --console         保留控制台输出
 *   --version / --help
 *
 * 子系统是 WINDOWS（双击不闪黑框），需要控制台时用 AttachConsole 挂到
 * 父进程的控制台上 —— 这样 GUI 与控制台两种用法互不干扰。
 * 服务只监听 127.0.0.1，令牌一次一换，绝不发起任何出站连接。
 * ========================================================================== */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <io.h>
#include <shellapi.h>

#include "lc.h"

/* 挂到父进程的控制台（cmd / PowerShell 里跑时才有效） */
static void attach_console(void) {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    FILE *f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONIN$", "r", stdin);
    /* 控制台按 UTF-8 解释我们写出去的字节 */
    SetConsoleOutputCP(CP_UTF8);
}

/*
 * 输出一段宽字符串。
 *
 * 不要用 fputws：C 运行时默认处于 "C" locale，宽 -> 多字节的转换只认 ASCII，
 * 碰到 "——" 这类字符会静默截断（--help 就只打印出第一行）。
 * 这里自己转成 UTF-8 再按字节写，和控制台代码页一致，重定向到文件也是干净的 UTF-8。
 */
static void put_wide_line(const wchar_t *s) {
    if (!s) return;
    char *u8 = lc_wcs_to_utf8(s);
    if (u8) {
        fwrite(u8, 1, strlen(u8), stdout);
        free(u8);
    } else {
        /* 极端情况下退化成逐字符 ASCII 输出，至少有内容 */
        for (; *s; ++s) fputc(*s < 0x80 ? (char)*s : '?', stdout);
    }
    fflush(stdout);
}

/* 命令行里非 ASCII 很少见，直接用宽字符比较 */
static int arg_is(const wchar_t *arg, const wchar_t *name) {
    return _wcsicmp(arg, name) == 0;
}

static void print_usage(void) {
    put_wide_line(
        L"LocalCraft " LC_APP_VERSION L" —— 完全离线的本地文件处理工具\n"
        L"\n"
        L"用法：\n"
        L"  LocalCraft.exe                启动图形界面（默认）\n"
        L"  LocalCraft.exe --headless     只启动本地服务，不打开窗口\n"
        L"  LocalCraft.exe --port 8080    固定端口\n"
        L"  LocalCraft.exe --no-app-mode  用普通浏览器标签页打开\n"
        L"  LocalCraft.exe --browser PATH 指定浏览器\n"
        L"  LocalCraft.exe --console      额外挂一个控制台窗口看日志\n"
        L"  LocalCraft.exe --version      显示版本\n"
        L"  LocalCraft.exe --help         显示本帮助\n"
        L"\n"
        L"所有批量操作都遵循：先预览 → 再执行 → 可随时回退。\n"
        L"程序只监听 127.0.0.1，不联网，不上传任何文件。\n"
        L"把 pandoc.exe / ffmpeg.exe / LibreOffice 放进 exe 同级的 tools\\ 可启用扩展格式。\n");
}

/* 服务运行期间的看门狗：窗口关掉后自动退出，但有任务在跑时不退 */
typedef struct { ApiState *api; volatile LONG *stop; } WatchCtx;

static DWORD WINAPI watchdog_thread(LPVOID param) {
    WatchCtx *ctx = (WatchCtx *)param;
    while (!*ctx->stop) {
        Sleep(2000);
        if (api_watchdog_tick(ctx->api)) {
            InterlockedExchange(ctx->stop, 1);
            return 0;
        }
    }
    return 0;
}

static int run_app(int argc, wchar_t **argv);

/* 声明 DPI 感知。
   不声明的话，高 DPI 屏上 Windows 会把我们算好的窗口坐标再乘一遍缩放倍数，
   窗口会被推到屏幕外面去；字体也会被系统拉伸得发虚。
   必须在创建任何窗口之前调用。 */
static void enable_dpi_awareness(void) {
    /* Windows 10 1703+ 用这个更精确；老系统退到 SetProcessDPIAware */
    typedef BOOL (WINAPI *SetCtxFn)(HANDLE);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        SetCtxFn f = (SetCtxFn)(void *)GetProcAddress(u, "SetProcessDpiAwarenessContext");
        if (f && f((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */)) return;
    }
    SetProcessDPIAware();
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int show) {
    enable_dpi_awareness();
    (void)hInst; (void)hPrev; (void)show;

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int rc = run_app(argc, argv);
    if (argv) LocalFree(argv);
    (void)cmd;
    return rc;
}

static int run_app(int argc, wchar_t **argv) {
    int headless = 0;
    int strategy = 0;   /* 0=私有配置 1=默认配置 2=默认浏览器 */
    int web_mode = 0;   /* --web：走浏览器界面（保留给调试/兼容） */
    int native_ui = 0;  /* --native：走纯 Win32 控件界面（不用 WebView2） */
    int console = 0;
    int port = 0;
    int want_version = 0;
    int want_help = 0;
    wchar_t browser[MAX_PATH * 2];
    browser[0] = 0;

    for (int i = 1; i < argc; ++i) {
        const wchar_t *a = argv[i];
        if (arg_is(a, L"--headless")) headless = 1;
        else if (arg_is(a, L"--console")) console = 1;
        else if (arg_is(a, L"--web")) web_mode = 1;
        else if (arg_is(a, L"--native")) native_ui = 1;
        else if (arg_is(a, L"--no-app-mode")) strategy = 2;  /* 直接用默认浏览器 */
        else if (arg_is(a, L"--version") || arg_is(a, L"-v")) want_version = 1;
        else if (arg_is(a, L"--help") || arg_is(a, L"-h") || arg_is(a, L"/?")) want_help = 1;
        else if (arg_is(a, L"--port") && i + 1 < argc) port = _wtoi(argv[++i]);
        else if (arg_is(a, L"--browser") && i + 1 < argc) {
            wcsncpy(browser, argv[++i], MAX_PATH * 2 - 1);
            browser[MAX_PATH * 2 - 1] = 0;
        }
    }

    if (want_version || want_help || headless || console) {
        attach_console();
    }
    if (want_version) {
        put_wide_line(L"LocalCraft " LC_APP_VERSION L"\n");
        return 0;
    }
    if (want_help) {
        print_usage();
        return 0;
    }

    /* 界面资源：开发时优先读磁盘（改完刷新即可见效），
       没有磁盘目录时用内嵌资源（打包后单文件即可运行） */
    wchar_t web_dir[MAX_PATH * 2];
    web_dir[0] = 0;
    {
        wchar_t *appdir = lc_app_dir();
        WStr s;
        wstr_init(&s);
        wstr_puts(&s, appdir);
        wstr_puts(&s, L"\\webui");
        if (lc_is_dir(s.data)) wcsncpy(web_dir, s.data, MAX_PATH * 2 - 1);
        wstr_free(&s);

        if (!web_dir[0]) {
            wstr_init(&s);
            wstr_puts(&s, appdir);
            wstr_puts(&s, L"\\..\\webui");
            wchar_t *abs = lc_abs_path(s.data);
            if (abs && lc_is_dir(abs)) wcsncpy(web_dir, abs, MAX_PATH * 2 - 1);
            free(abs);
            wstr_free(&s);
        }
        free(appdir);
    }

    ApiState *api = api_create(web_dir[0] ? web_dir : NULL);
    if (!api) {
        MessageBoxW(NULL, L"内存不足，无法启动。", L"LocalCraft", MB_ICONERROR);
        return 1;
    }

    HttpConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.handler = api_dispatch;
    cfg.ud = api;
    cfg.web_dir = web_dir[0] ? web_dir : NULL;

    wchar_t err[512];
    err[0] = 0;
    HttpServer *srv = http_start(&cfg, port, err, 512);
    if (!srv) {
        wchar_t msg[768];
        swprintf(msg, 768, L"无法启动本地服务：\n%ls", err);
        MessageBoxW(NULL, msg, L"LocalCraft", MB_ICONERROR);
        api_destroy(api);
        return 1;
    }

    char token[64];
    snprintf(token, sizeof(token), "%s", http_token(srv));
    WStr url;
    wstr_init(&url);
    wstr_printf(&url, L"http://127.0.0.1:%d/?token=", http_port(srv));
    {
        wchar_t wtok[64];
        MultiByteToWideChar(CP_UTF8, 0, token, -1, wtok, 64);
        wstr_puts(&url, wtok);
    }

    if (headless) {
        char *u8 = lc_wcs_to_utf8(url.data);
        printf("LocalCraft 服务已启动：%s\n", u8 ? u8 : "");
        printf("按 Ctrl+C 退出。\n");
        free(u8);
        fflush(stdout);
    }

    volatile LONG stop = 0;
    WatchCtx wctx;
    wctx.api = api;
    wctx.stop = &stop;
    HANDLE wt = CreateThread(NULL, 0, watchdog_thread, &wctx, 0, NULL);

    /*
     * 默认走**原生界面**：不监听任何端口、不启动浏览器。
     *   --headless  只起本地服务（调试用）
     *   --web       起服务并用浏览器打开（保留的兼容路径）
     */
    if (!headless && !web_mode && !native_ui) {
        /*
         * 默认：**原生窗口 + WebView2 渲染内嵌的 HTML 界面**。
         *   · 不启动外部浏览器 —— WebView2 是进程内的渲染控件
         *   · 不监听任何端口 —— 页面资源由本进程通过请求拦截直接应答
         *   · HTML/CSS/JS 一行都不用改：页面走虚拟域 https://localcraft/，
         *     请求被拦下来交给同一个 api_dispatch
         * WebView2 不可用时自动退回纯 Win32 控件界面（不是留个白屏），
         * 并把原因写清楚。
         */
        char tok[64];
        snprintf(tok, sizeof(tok), "%s", http_token(srv));
        wchar_t wtok[64];
        MultiByteToWideChar(CP_UTF8, 0, tok, -1, wtok, 64);

        int rc = lc_webview_run(api, wtok);
        if (rc != 0) {
            wchar_t msg[1024];
            swprintf(msg, 1024,
                L"%ls\n\n"
                L"已自动切换到纯 Win32 控件界面，功能完全一样。\n\n"
                L"想用 HTML 界面的话：把 WebView2Loader.dll 放到 exe 同目录，"
                L"或安装「Microsoft Edge WebView2 Runtime」。",
                lc_webview_note());
            MessageBoxW(NULL, msg, L"LocalCraft", MB_ICONINFORMATION | MB_OK);
            rc = lc_app_run(api);
        }
        http_stop(srv);
        api_destroy(api);
        wstr_free(&url);
        return rc;
    }

    if (!headless) {
        /* --web：起服务并用浏览器打开（保留的兼容路径，默认不用） */
        wchar_t how[256];
        how[0] = 0;
        DWORD pid = 0;
        int lrc = launcher_open_window(url.data, browser[0] ? browser : NULL,
                                       strategy, how, 256, &pid);
        wchar_t probe[256];
        probe[0] = 0;
        int self_ok = lc_probe_loopback(http_port(srv), probe, 256);
        wchar_t how2[560];
        swprintf(how2, 560, L"%ls（自检：%ls）", how,
                 self_ok ? L"回环连接正常" : (probe[0] ? probe : L"回环连接失败"));
        lc_write_startup_note(url.data, lrc == 0, how2);
        if (lrc != 0) {
            wchar_t *root = lc_data_root();
            lc_copy_to_clipboard(url.data);
            wchar_t msg[1600];
            swprintf(msg, 1600,
                L"没能自动打开浏览器。\n\n%ls\n\n"
                L"地址已复制到剪贴板，切到浏览器按 Ctrl+V 即可。\n\n"
                L"地址也存在：\n%ls\\启动信息.txt",
                url.data, root ? root : L"数据目录");
            free(root);
            MessageBoxW(NULL, msg, L"LocalCraft",
                        MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
        }
    }

    /* 主线程等待：看门狗置位（窗口关闭或用户点了退出） */
    while (!stop) Sleep(300);

    if (wt) { WaitForSingleObject(wt, 3000); CloseHandle(wt); }
    http_stop(srv);
    api_destroy(api);
    wstr_free(&url);
    return 0;
}
