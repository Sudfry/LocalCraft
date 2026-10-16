/* ==========================================================================
 * webview.c —— 用 WebView2 在原生窗口里显示 HTML 界面
 *
 * 为什么是这个方案
 * ----------------
 *   外部浏览器 + HTTP 服务：界面好看，但"程序替用户开浏览器"在真实机器上不可靠
 *     （--app 白屏、代理拦截、残留实例冲突……）
 *   纯原生控件：一定出得来，但界面是传统 Windows 样子
 *   **WebView2**：HTML 界面 + 原生窗口，两边的好处都要
 *     · 不启动外部浏览器 —— WebView2 是进程内的渲染控件
 *     · 不监听任何端口 —— 页面资源由本进程通过 WebResourceRequested 直接喂给渲染器
 *     · 断网可用 —— 页面只请求一个虚拟域 https://localcraft/，由我们自己应答
 *
 * 桥接方式（关键设计）
 * --------------------
 * 页面里的 app.js **一行都不用改**。它在 http 时代是这样发请求的：
 *     fetch('/api/call?token=...', {method:'POST', body: JSON.stringify({method,params})})
 * 现在页面挂在虚拟域 https://localcraft/ 下，同一个相对地址变成
 *     https://localcraft/api/call?token=...
 * 我们在 WebResourceRequested 里拦下它，把 body 交给 api_dispatch，
 * 再把返回的 JSON 包成 HTTP 响应塞回去。对 JS 来说完全透明。
 * ========================================================================== */
#include <winsock2.h>
#include <windows.h>
#include <objidl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lc.h"
#include "webview2.h"

#define LCWV_HOST   L"https://localcraft/"
#define LCWV_CLASS  L"LocalCraftWebView"

/* ==================================================== 全局 ============== */

static PFN_CreateCoreWebView2EnvironmentWithOptions pCreateEnv = NULL;
static PFN_GetAvailableCoreWebView2BrowserVersionString pGetVer = NULL;

static ICoreWebView2Environment  *g_env = NULL;
static ICoreWebView2Controller   *g_ctrl = NULL;
static ICoreWebView2             *g_web = NULL;
static HWND                       g_hwnd = NULL;
static ApiState                  *g_api = NULL;
static volatile LONG              g_closed = 0;
static wchar_t                    g_token[64];
static wchar_t                    g_note[512];   /* 启动诊断信息 */

/* ==================================================== 加载器 ============ */

static HMODULE try_load(const wchar_t *path) {
    if (!path || !path[0]) return NULL;
    return LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
}

/*
 * 找 WebView2Loader.dll。
 * 优先 exe 同目录（发布时可以把它放一起），找不到再试系统里几个常见位置。
 * 这个 dll 只负责"把 WebView2 运行库拉起来"，真正的渲染引擎是系统里装好的
 * Microsoft Edge WebView2 Runtime。
 */
static HMODULE load_loader(void) {
    wchar_t path[MAX_PATH * 2];

    /* 1) exe 同目录 */
    wchar_t *dir = lc_app_dir();
    if (dir) {
        swprintf(path, MAX_PATH * 2, L"%ls\\WebView2Loader.dll", dir);
        HMODULE m = try_load(path);
        free(dir);
        if (m) return m;
    }

    /* 2) 系统里已知的若干位置（别的软件带来的一份也能用） */
    static const wchar_t *cands[] = {
        L"%ProgramFiles(x86)%\\Microsoft\\EdgeWebView\\Application\\WebView2Loader.dll",
        L"%ProgramFiles%\\Microsoft\\EdgeWebView\\Application\\WebView2Loader.dll",
        L"%ProgramFiles%\\Common Files\\Microsoft\\EdgeWebView\\WebView2Loader.dll",
        L"%ProgramFiles(x86)%\\Common Files\\Microsoft\\EdgeWebView\\WebView2Loader.dll",
        NULL
    };
    for (int i = 0; cands[i]; ++i) {
        wchar_t ex[MAX_PATH * 2];
        DWORD n = ExpandEnvironmentStringsW(cands[i], ex, MAX_PATH * 2);
        if (n == 0 || n > MAX_PATH * 2) continue;
        HMODULE m = try_load(ex);
        if (m) return m;
    }
    return NULL;
}

/* ==================================================== 小工具 ============ */

static void release_i(void *p) {
    if (p) ((IUnknown *)p)->lpVtbl->Release((IUnknown *)p);
}

/* 把内存里的字节变成一个 IStream */
static IStream *stream_from(const void *data, size_t len) {
    IStream *s = NULL;
    if (!data || !len) return NULL;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, len);
    if (!h) return NULL;
    void *p = GlobalLock(h);
    if (!p) { GlobalFree(h); return NULL; }
    memcpy(p, data, len);
    GlobalUnlock(h);

    if (FAILED(CreateStreamOnHGlobal(h, TRUE, &s))) { GlobalFree(h); return NULL; }
    return s;
}

/* 读光一个 IStream */
static char *stream_read_all(IStream *s, size_t *len_out) {
    *len_out = 0;
    if (!s) return NULL;

    STATSTG st;
    memset(&st, 0, sizeof(st));
    if (FAILED(s->lpVtbl->Stat(s, &st, STATFLAG_NONAME))) return NULL;
    ULONGLONG total = st.cbSize.QuadPart;
    if (total == 0 || total > (64u << 20)) return NULL;

    char *buf = (char *)malloc((size_t)total + 1);
    if (!buf) return NULL;

    size_t got = 0;
    while (got < (size_t)total) {
        ULONG n = 0;
        HRESULT hr = s->lpVtbl->Read(s, buf + got, (ULONG)((size_t)total - got), &n);
        if (FAILED(hr) || n == 0) break;
        got += n;
    }
    buf[got] = 0;
    *len_out = got;
    return buf;
}

/* ==================================================== 应答请求 ========== */

/*
 * 把一个 HTTP 响应塞回去。
 * headers 形如 L"Content-Type: text/html; charset=utf-8\r\n"。
 */
static void respond(ICoreWebView2WebResourceRequestedEventArgs *args,
                    const void *body, size_t len, int status,
                    const wchar_t *reason, const wchar_t *headers) {
    if (!g_env || !args) return;
    IStream *s = stream_from(body, len);
    ICoreWebView2WebResourceResponse *resp = NULL;
    HRESULT hr = g_env->lpVtbl->CreateWebResourceResponse(
        g_env, s, status, reason, headers, &resp);
    if (SUCCEEDED(hr) && resp) {
        args->lpVtbl->put_Response(args, resp);
        resp->lpVtbl->Release(resp);
    }
    if (s) s->lpVtbl->Release(s);
}

/* 路径 -> 内嵌资源名 */
static int asset_name_from(const wchar_t *path, char *out, size_t cch) {
    /* path 形如 "https://localcraft/static/app.js" 或 "https://localcraft/" */
    const wchar_t *p = path;
    size_t plen = wcslen(LCWV_HOST);
    if (wcsncmp(p, LCWV_HOST, plen - 1) == 0) p += (plen - 1);
    else if (p[0] == L'/') ++p;
    else return 0;

    /* 去掉查询串 */
    wchar_t clean[512];
    wcsncpy(clean, p, 511);
    clean[511] = 0;
    wchar_t *q = wcschr(clean, L'?');
    if (q) *q = 0;

    /* 去掉开头的 / */
    const wchar_t *name = clean;
    while (*name == L'/') ++name;
    if (*name == 0) name = L"index.html";

    char *u8 = lc_wcs_to_utf8(name);
    if (!u8) return 0;
    /* 只保留文件名部分（内嵌资源是平铺的） */
    char *slash = strrchr(u8, '/');
    const char *base = slash ? slash + 1 : u8;
    snprintf(out, cch, "%s", base);
    free(u8);
    return 1;
}

static const wchar_t *mime_for_asset(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return L"application/octet-stream";
    if (_stricmp(dot, ".html") == 0) return L"text/html; charset=utf-8";
    if (_stricmp(dot, ".css") == 0)  return L"text/css; charset=utf-8";
    if (_stricmp(dot, ".js") == 0)   return L"application/javascript; charset=utf-8";
    if (_stricmp(dot, ".svg") == 0)  return L"image/svg+xml";
    if (_stricmp(dot, ".png") == 0)  return L"image/png";
    if (_stricmp(dot, ".ico") == 0)  return L"image/x-icon";
    return L"application/octet-stream";
}

/*
 * 虚拟域内的请求全部由本进程应答：
 *   /api/call  -> 交给 api_dispatch（和浏览器时代完全同一条后端路径）
 *   其它       -> 内嵌的界面资源
 */
/*
 * 诊断：把每一次被拦截的请求写进日志文件。
 * 这是判断"页面到底加载到哪一步"最可靠的手段 ——
 * 截图可能因为 DirectComposition 抓不到，但请求日志骗不了人。
 * 正常运行时这个文件只在数据目录里留一份，不影响使用。
 */
static void wv_log(const wchar_t *fmt, ...) {
    static int inited = 0;
    static wchar_t path[MAX_PATH * 2];

    if (!inited) {
        inited = 1;
        wchar_t *root = lc_data_root();
        if (root) {
            swprintf(path, MAX_PATH * 2, L"%ls\\请求日志.txt", root);
            free(root);
        }
    }
    if (!path[0]) return;

    wchar_t line[1200];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(line, 1199, fmt, ap);
    va_end(ap);
    line[1199] = 0;

    FILE *f = _wfopen(path, L"a, ccs=UTF-8");
    if (!f) return;
    fputws(line, f);
    fputws(L"\n", f);
    fclose(f);
}
static void serve(ICoreWebView2WebResourceRequestedEventArgs *args) {
    ICoreWebView2WebResourceRequest *req = NULL;
    if (FAILED(args->lpVtbl->get_Request(args, &req)) || !req) return;

    LPWSTR uri = NULL;
    req->lpVtbl->get_Uri(req, &uri);
    if (!uri) { release_i(req); return; }

    wv_log(L"[请求] %ls", uri);

    /* 不是我们的虚拟域就不管，让它自己走 */
    if (_wcsnicmp(uri, LCWV_HOST, wcslen(LCWV_HOST)) != 0) {
        CoTaskMemFree(uri);
        release_i(req);
        return;
    }

    /* ---- /api/call ---- */
    if (wcsstr(uri, L"/api/call")) {
        IStream *body = NULL;
        req->lpVtbl->get_Content(req, &body);
        size_t blen = 0;
        char *json = stream_read_all(body, &blen);
        if (body) body->lpVtbl->Release(body);

        Str out, err, wrap;
        str_init(&out); str_init(&err); str_init(&wrap);

        if (json) {
            /* body 形如 {"method":"x.y","params":{...}} */
            Arena *a = arena_new();
            JsonValue *payload = json_parse(json, a);
            const char *method = payload ? json_string(json_get(payload, "method"), "") : "";
            const JsonValue *params = payload ? json_get(payload, "params") : NULL;

            int rc = -1;
            if (method[0]) rc = api_dispatch(g_api, method, params, &out, &err);
            else           str_puts(&err, "非法请求");

            if (rc == 0) {
                str_puts(&wrap, "{\"ok\":true,\"result\":");
                str_puts(&wrap, out.data ? out.data : "null");
                str_puts(&wrap, "}");
            } else {
                str_puts(&wrap, "{\"ok\":false,\"error\":");
                {
                    Str esc; str_init(&esc);
                    json_escape_utf8_to(&esc, err.data ? err.data : "unknown error");
                    str_puts(&wrap, esc.data);
                    str_free(&esc);
                }
                str_puts(&wrap, "}");
            }
            arena_free(a);
            free(json);
        } else {
            str_puts(&wrap, "{\"ok\":false,\"error\":\"请求体读取失败\"}");
        }

        respond(args, wrap.data, wrap.len, 200, L"OK",
                L"Content-Type: application/json; charset=utf-8\r\n"
                L"Cache-Control: no-store\r\n");
        wv_log(L"  -> JSON %d 字节 %ls", (int)wrap.len,
               strstr(wrap.data ? wrap.data : "", "\"ok\":false") ? L"(业务错误)" : L"(ok)");
        str_free(&out); str_free(&err); str_free(&wrap);
        CoTaskMemFree(uri);
        release_i(req);
        return;
    }

    /* ---- 界面资源 ---- */
    char name[256];
    if (asset_name_from(uri, name, sizeof(name))) {
        size_t len = 0;
        const char *data = webui_asset(name, &len);
        /*
         * 只有"像页面导航"的请求才回退到 index.html。
         * favicon.ico 这类小文件不加判断的话，会被替换成整份网页发回去 ——
         * 功能上没影响，但每个图标请求都白白传 34KB。
         */
        if (!data && _stricmp(name, "index.html") != 0 &&
            _stricmp(name, "favicon.ico") != 0 && !strchr(name, '.')) {
            data = webui_asset("index.html", &len);
        }
        if (data) {
            WStr h; wstr_init(&h);
            wstr_puts(&h, L"Content-Type: ");
            wstr_puts(&h, mime_for_asset(name));
            wstr_puts(&h, L"\r\nCache-Control: no-store\r\n");
            respond(args, data, len, 200, L"OK", h.data);
            wv_log(L"  -> 资源 %hs %d 字节", name, (int)len);
            wstr_free(&h);
        } else {
            respond(args, "not found", 9, 404, L"Not Found",
                    L"Content-Type: text/plain\r\n");
            wv_log(L"  -> 404 找不到资源 %hs", name);
        }
    } else {
        respond(args, "bad request", 11, 400, L"Bad Request",
                L"Content-Type: text/plain\r\n");
    }

    CoTaskMemFree(uri);
    release_i(req);
}

/* ==================================================== 回调实现 ========== */

static HRESULT STDMETHODCALLTYPE qi_noop(void *self, REFIID riid, void **ppv) {
    (void)riid;
    if (ppv) *ppv = self;
    return S_OK;
}
static ULONG STDMETHODCALLTYPE addref1(void *self) { (void)self; return 2; }
static ULONG STDMETHODCALLTYPE release1(void *self) { (void)self; return 1; }

static LRESULT CALLBACK wv_proc(HWND h, UINT m, WPARAM w, LPARAM l);

/* ---- 环境创建完成 ---- */


/* 控制器创建完成：拿到 ICoreWebView2，挂载拦截器，导航到界面 */
static HRESULT STDMETHODCALLTYPE on_controller_created(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *self,
    HRESULT result, ICoreWebView2Controller *ctrl);

static ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl g_ctrlVtbl = {
    (void *)qi_noop, (void *)addref1, (void *)release1,
    (void *)on_controller_created
};
static ICoreWebView2CreateCoreWebView2ControllerCompletedHandler g_ctrlHandler = {
    &g_ctrlVtbl
};

static HRESULT STDMETHODCALLTYPE on_env_created(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *self,
    HRESULT result, ICoreWebView2Environment *env) {
    (void)self;
    if (FAILED(result) || !env) {
        swprintf(g_note, 512, L"WebView2 环境创建失败（0x%08lX）", (unsigned long)result);
        wv_log(L"[错误] %ls", g_note);
        return S_OK;
    }
    wv_log(L"[阶段] WebView2 环境就绪");
    g_env = env;
    env->lpVtbl->AddRef(env);
    env->lpVtbl->CreateCoreWebView2Controller(env, g_hwnd, &g_ctrlHandler);
    return S_OK;
}

static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl g_envVtbl = {
    (void *)qi_noop, (void *)addref1, (void *)release1,
    (void *)on_env_created
};
static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler g_envHandler = {
    &g_envVtbl
};

/* ---- 资源请求拦截 ---- */

static HRESULT STDMETHODCALLTYPE on_web_resource_requested(
    ICoreWebView2WebResourceRequestedEventHandler *self,
    ICoreWebView2 *sender, ICoreWebView2WebResourceRequestedEventArgs *args) {
    (void)self; (void)sender;
    serve(args);
    return S_OK;
}

static ICoreWebView2WebResourceRequestedEventHandlerVtbl g_wrrVtbl = {
    (void *)qi_noop, (void *)addref1, (void *)release1,
    (void *)on_web_resource_requested
};
static ICoreWebView2WebResourceRequestedEventHandler g_wrrHandler = { &g_wrrVtbl };

/* ---- 新窗口：交给系统默认浏览器打开外链 ---- */

static HRESULT STDMETHODCALLTYPE on_new_window(
    ICoreWebView2NewWindowRequestedEventHandler *self,
    ICoreWebView2 *sender, ICoreWebView2NewWindowRequestedEventArgs *args) {
    (void)self; (void)sender;
    LPWSTR uri = NULL;
    if (SUCCEEDED(args->lpVtbl->get_Uri(args, &uri)) && uri) {
        /* 界面内部的跳转不该走到这儿；真出现外链就用系统浏览器打开 */
        ShellExecuteW(NULL, L"open", uri, NULL, NULL, SW_SHOWNORMAL);
        CoTaskMemFree(uri);
    }
    args->lpVtbl->put_Handled(args, TRUE);
    return S_OK;
}

static ICoreWebView2NewWindowRequestedEventHandlerVtbl g_nwVtbl = {
    (void *)qi_noop, (void *)addref1, (void *)release1,
    (void *)on_new_window
};
static ICoreWebView2NewWindowRequestedEventHandler g_nwHandler = { &g_nwVtbl };

/* ---- 控制器就绪 ---- */

static HRESULT STDMETHODCALLTYPE on_controller_created(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *self,
    HRESULT result, ICoreWebView2Controller *ctrl) {
    (void)self;
    if (FAILED(result) || !ctrl) {
        swprintf(g_note, 512, L"WebView2 控制器创建失败（0x%08lX）", (unsigned long)result);
        wv_log(L"[错误] %ls", g_note);
        return S_OK;
    }
    wv_log(L"[阶段] 控制器创建成功，准备导航");
    g_ctrl = ctrl;
    ctrl->lpVtbl->AddRef(ctrl);

    RECT rc; GetClientRect(g_hwnd, &rc);
    HRESULT hr;
    hr = ctrl->lpVtbl->put_Bounds(ctrl, rc);
    wv_log(L"[步骤] put_Bounds( %ldx%ld ) -> 0x%08lX", (long)rc.right, (long)rc.bottom, (unsigned long)hr);
    hr = ctrl->lpVtbl->put_IsVisible(ctrl, TRUE);
    wv_log(L"[步骤] put_IsVisible -> 0x%08lX", (unsigned long)hr);

    /*
     * 取 ICoreWebView2。
     * 先走 get_CoreWebView2（按理是槽位 23）；万一 vtable 顺序对不上导致
     * 拿到 NULL，就退回用槽位 0 的 QueryInterface —— 那个槽位是 IUnknown
     * 规定的，绝对不会有歧义。
     */

    g_web = NULL;
    hr = ctrl->lpVtbl->get_CoreWebView2(ctrl, &g_web);
    wv_log(L"[步骤] get_CoreWebView2 -> 0x%08lX, ptr=%p", (unsigned long)hr, (void *)g_web);

    if (!g_web) {
        static const GUID IID_ICoreWebView2 =
            { 0x76ECEACB, 0x0462, 0x4D94,
              { 0xAC, 0x83, 0x42, 0x3A, 0x67, 0x93, 0x77, 0x5E } };
        hr = ctrl->lpVtbl->QueryInterface(ctrl, &IID_ICoreWebView2, (void **)&g_web);
        wv_log(L"[步骤] QueryInterface(ICoreWebView2) -> 0x%08lX, ptr=%p",
               (unsigned long)hr, (void *)g_web);
    }

    if (!g_web) {
        swprintf(g_note, 512,
                 L"无法取得 WebView2 核心对象（get_CoreWebView2 返回 NULL）");
        wv_log(L"[错误] %ls", g_note);
        return S_OK;
    }

    /* 界面设置：关掉用不上的东西，减少干扰 */
    ICoreWebView2Settings *st = NULL;
    hr = g_web->lpVtbl->get_Settings(g_web, &st);
    wv_log(L"[步骤] get_Settings -> 0x%08lX, ptr=%p", (unsigned long)hr, (void *)st);
    if (SUCCEEDED(hr) && st) {
        st->lpVtbl->put_IsStatusBarEnabled(st, FALSE);
        st->lpVtbl->put_AreDevToolsEnabled(st, FALSE);
        st->lpVtbl->put_AreDefaultContextMenusEnabled(st, FALSE);
        st->lpVtbl->put_IsBuiltInErrorPageEnabled(st, TRUE);
        st->lpVtbl->Release(st);
    }

    /* 拦截本虚拟域的全部请求 */
    hr = g_web->lpVtbl->AddWebResourceRequestedFilter(
        g_web, L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    wv_log(L"[步骤] AddWebResourceRequestedFilter -> 0x%08lX", (unsigned long)hr);
    EventRegistrationToken tok;
    hr = g_web->lpVtbl->add_WebResourceRequested(g_web, &g_wrrHandler, &tok);
    wv_log(L"[步骤] add_WebResourceRequested -> 0x%08lX", (unsigned long)hr);
    hr = g_web->lpVtbl->add_NewWindowRequested(g_web, &g_nwHandler, &tok);
    wv_log(L"[步骤] add_NewWindowRequested -> 0x%08lX", (unsigned long)hr);

    /* 导航到内嵌界面 */
    wchar_t url[512];
    swprintf(url, 512, L"%lsindex.html?token=%ls", LCWV_HOST, g_token);
    wv_log(L"[阶段] 导航到 %ls", url);
    g_web->lpVtbl->Navigate(g_web, url);
    return S_OK;
}

/* ==================================================== 窗口过程 ========== */

static LRESULT CALLBACK wv_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE:
        if (g_ctrl) {
            RECT rc;
            GetClientRect(h, &rc);
            g_ctrl->lpVtbl->put_Bounds(g_ctrl, rc);
        }
        return 0;

    case WM_SETFOCUS:
        if (g_ctrl) g_ctrl->lpVtbl->MoveFocus(g_ctrl, COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;

    case WM_CLOSE:
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        InterlockedExchange(&g_closed, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ==================================================== 入口 ============== */

int lc_webview_run(ApiState *api, const wchar_t *token) {
    g_api = api;
    wcsncpy(g_token, token ? token : L"", 63);
    g_token[63] = 0;
    g_note[0] = 0;

    HMODULE ld = load_loader();
    if (!ld) {
        swprintf(g_note, 512,
                 L"找不到 WebView2Loader.dll。\n"
                 L"请把它和 LocalCraft.exe 放在同一个目录里。");
        return -1;
    }
    pCreateEnv = (PFN_CreateCoreWebView2EnvironmentWithOptions)
        (void *)GetProcAddress(ld, "CreateCoreWebView2EnvironmentWithOptions");
    pGetVer = (PFN_GetAvailableCoreWebView2BrowserVersionString)
        (void *)GetProcAddress(ld, "GetAvailableCoreWebView2BrowserVersionString");
    if (!pCreateEnv) {
        swprintf(g_note, 512, L"WebView2Loader.dll 里没有需要的导出函数。");
        return -1;
    }

    /* 运行库版本写进诊断信息，出问题时能一眼看出装没装 */
    if (pGetVer) {
        LPWSTR ver = NULL;
        if (SUCCEEDED(pGetVer(NULL, &ver)) && ver) {
            swprintf(g_note, 512, L"WebView2 运行库 %ls", ver);
            CoTaskMemFree(ver);
        }
    }
    if (!g_note[0]) swprintf(g_note, 512, L"WebView2 运行库未检测到版本号");

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    HINSTANCE hi = GetModuleHandleW(NULL);
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wv_proc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = LCWV_CLASS;
    wc.hIcon = lc_app_icon(0);
    wc.hIconSm = lc_app_icon(1);
    RegisterClassExW(&wc);

    int W = 1280, H = 820;
    {
        HDC hdc = GetDC(NULL);
        if (hdc) {
            int d = GetDeviceCaps(hdc, LOGPIXELSY);
            ReleaseDC(NULL, hdc);
            if (d >= 72 && d <= 480) { W = MulDiv(W, d, 96); H = MulDiv(H, d, 96); }
        }
    }
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    g_hwnd = CreateWindowExW(0, LCWV_CLASS, L"LocalCraft · 本地文件处理工具",
                             WS_OVERLAPPEDWINDOW,
                             (sw - W) / 2, (sh - H) / 2, W, H,
                             NULL, NULL, hi, NULL);
    if (!g_hwnd) { CoUninitialize(); return -1; }

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    SetForegroundWindow(g_hwnd);

    /* 用户数据目录放在应用数据根下，不污染系统别处 */
    wchar_t *root = lc_data_root();
    WStr ud; wstr_init(&ud);
    wstr_puts(&ud, root);
    wstr_puts(&ud, L"\\webview");
    lc_mkdir_p(ud.data);
    free(root);

    HRESULT hr = pCreateEnv(NULL, ud.data, NULL, &g_envHandler);
    wstr_free(&ud);
    if (FAILED(hr)) {
        swprintf(g_note, 512, L"创建 WebView2 环境失败（0x%08lX）", (unsigned long)hr);
        DestroyWindow(g_hwnd);
        CoUninitialize();
        return -1;
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_web)  { g_web->lpVtbl->Release(g_web);  g_web = NULL; }
    if (g_ctrl) { g_ctrl->lpVtbl->Release(g_ctrl); g_ctrl = NULL; }
    if (g_env)  { g_env->lpVtbl->Release(g_env);  g_env = NULL; }
    CoUninitialize();
    return g_note[0] && wcsstr(g_note, L"失败") ? -1 : 0;
}

const wchar_t *lc_webview_note(void) { return g_note; }
