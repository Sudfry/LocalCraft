/* ==========================================================================
 * http.c —— 本机回环 HTTP 服务（Winsock，纯标准库 + Win32）
 *
 *  · 只绑 127.0.0.1，不接受来自局域网的连接
 *  · 每次启动随机生成令牌，根路径与所有 API 调用都必须携带
 *  · 绝不发起任何出站连接
 *  · 界面资源优先从内嵌数据读取（打包后单文件即可运行），
 *    开发时可用 web_dir 直接读磁盘，改完刷新即可见效
 * ========================================================================== */

/* winsock2.h 必须先于 windows.h */
#include <winsock2.h>
#include <ws2tcpip.h>

#include "lc.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_REQ (4 * 1024 * 1024)   /* 请求体上限；每个连接都要 malloc 一次，
                                       64MB 纯属浪费，接口请求实际都很小 */

struct HttpServer {
    SOCKET       listen_sock;
    int          port;
    char         token[64];
    HttpConfig   cfg;
    volatile LONG stopping;
    HANDLE       thread;
};

typedef struct { HttpServer *srv; SOCKET sock; } ConnCtx;

/* ==================================================== 小工具 ========== */

static const char *mime_for(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return "application/octet-stream";
    if (_stricmp(dot, ".html") == 0) return "text/html; charset=utf-8";
    if (_stricmp(dot, ".css") == 0)  return "text/css; charset=utf-8";
    if (_stricmp(dot, ".js") == 0)   return "application/javascript; charset=utf-8";
    if (_stricmp(dot, ".json") == 0) return "application/json; charset=utf-8";
    if (_stricmp(dot, ".svg") == 0)  return "image/svg+xml";
    if (_stricmp(dot, ".png") == 0)  return "image/png";
    if (_stricmp(dot, ".ico") == 0)  return "image/x-icon";
    return "application/octet-stream";
}

static int send_all(SOCKET s, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = send(s, buf + sent, (int)((len - sent) > 65536 ? 65536 : (len - sent)), 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static void send_response(SOCKET s, int code, const char *ctype,
                          const char *body, size_t body_len) {
    char head[512];
    const char *reason = code == 200 ? "OK"
                       : code == 403 ? "Forbidden"
                       : code == 404 ? "Not Found"
                       : code == 400 ? "Bad Request"
                       : code == 413 ? "Payload Too Large"
                       : "Error";
    int n = snprintf(head, sizeof(head),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %llu\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Connection: close\r\n\r\n",
        code, reason, ctype, (unsigned long long)body_len);
    if (n > 0) send_all(s, head, (size_t)n);
    if (body_len) send_all(s, body, body_len);
}

static void send_text(SOCKET s, int code, const char *ctype, const char *body) {
    send_response(s, code, ctype, body, body ? strlen(body) : 0);
}

/* ================================================= 请求解析 =========== */

typedef struct {
    char method[16];
    char path[2048];
    char *query;
    char *body;
    size_t body_len;
} Request;

static char *find_header_end(char *buf, size_t len) {
    for (size_t i = 0; i + 3 < len; ++i)
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n')
            return buf + i + 4;
    return NULL;
}

static long long header_value(const char *headers, const char *key) {
    size_t klen = strlen(key);
    const char *p = headers;
    while (p && *p) {
        const char *eol = strstr(p, "\r\n");
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);
        if (llen > klen && _strnicmp(p, key, klen) == 0 && p[klen] == ':') {
            return _strtoi64(p + klen + 1, NULL, 10);
        }
        if (!eol) break;
        p = eol + 2;
    }
    return 0;
}

static const char *header_str(const char *headers, const char *key,
                              char *out, size_t cch) {
    size_t klen = strlen(key);
    const char *p = headers;
    while (p && *p) {
        const char *eol = strstr(p, "\r\n");
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);
        if (llen > klen && _strnicmp(p, key, klen) == 0 && p[klen] == ':') {
            const char *v = p + klen + 1;
            while (*v == ' ') ++v;
            size_t vlen = (size_t)((p + llen) - v);
            if (vlen >= cch) vlen = cch - 1;
            memcpy(out, v, vlen);
            out[vlen] = 0;
            return out;
        }
        if (!eol) break;
        p = eol + 2;
    }
    out[0] = 0;
    return out;
}

/* ================================================= 令牌校验 =========== */

static int token_ok(HttpServer *srv, const char *query) {
    const char *want = srv->token;
    size_t wlen = strlen(want);
    if (query) {
        const char *p = query;
        while (*p) {
            if (_strnicmp(p, "token=", 6) == 0) {
                const char *v = p + 6;
                const char *amp = strchr(v, '&');
                size_t vlen = amp ? (size_t)(amp - v) : strlen(v);
                if (vlen == wlen && strncmp(v, want, wlen) == 0) return 1;
                return 0;
            }
            const char *amp = strchr(p, '&');
            if (!amp) break;
            p = amp + 1;
        }
    }
    return 0;
}

/* ================================================== 资源读取 ========== */

static int serve_asset(HttpServer *srv, SOCKET s, const char *name) {
    /* 只取文件名，杜绝 ../ 穿越 */
    const char *base = name;
    for (const char *p = name; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    if (!*base) { send_text(s, 404, "text/plain; charset=utf-8", "not found"); return -1; }

    if (srv->cfg.web_dir) {
        wchar_t *dir = (wchar_t *)srv->cfg.web_dir;
        wchar_t wn[512];
        MultiByteToWideChar(CP_UTF8, 0, base, -1, wn, 512);
        WStr full;
        wstr_init(&full);
        wstr_puts(&full, dir);
        wstr_puts(&full, L"\\");
        wstr_puts(&full, wn);
        char *a = lc_wcs_to_utf8(full.data);
        wstr_free(&full);
        if (a) {
            FILE *f = fopen(a, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long n = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (n > 0) {
                    char *buf = (char *)malloc((size_t)n);
                    size_t got = buf ? fread(buf, 1, (size_t)n, f) : 0;
                    fclose(f);
                    if (buf) {
                        send_response(s, 200, mime_for(base), buf, got);
                        free(buf);
                        free(a);
                        return 0;
                    }
                } else {
                    fclose(f);
                }
            }
            free(a);
        }
    }

    size_t len = 0;
    const char *data = webui_asset(base, &len);
    if (!data) { send_text(s, 404, "text/plain; charset=utf-8", "not found"); return -1; }
    send_response(s, 200, mime_for(base), data, len);
    return 0;
}

/* ================================================== 连接处理 ========== */

static DWORD WINAPI conn_thread(LPVOID param) {
    ConnCtx *ctx = (ConnCtx *)param;
    SOCKET s = ctx->sock;
    HttpServer *srv = ctx->srv;
    free(ctx);

    char *buf = (char *)malloc(MAX_REQ);
    if (!buf) { closesocket(s); return 0; }

    size_t total = 0;
    char *hdr_end = NULL;
    /* 先读到头部结束 */
    while (total < MAX_REQ) {
        int n = recv(s, buf + total, (int)(MAX_REQ - total - 1), 0);
        if (n <= 0) break;
        total += (size_t)n;
        buf[total] = 0;
        hdr_end = find_header_end(buf, total);
        if (hdr_end) break;
        if (total > 128 * 1024) break;   /* 头部不该这么大 */
    }

    if (!hdr_end) {
        send_text(s, 400, "text/plain; charset=utf-8", "bad request");
        free(buf);
        closesocket(s);
        return 0;
    }

    /* 解析请求行 */
    Request req;
    memset(&req, 0, sizeof(req));
    char *line_end = strstr(buf, "\r\n");
    if (!line_end) line_end = strchr(buf, '\n');
    if (!line_end) {
        send_text(s, 400, "text/plain; charset=utf-8", "bad request");
        free(buf);
        closesocket(s);
        return 0;
    }
    *line_end = 0;
    {
        char *sp1 = strchr(buf, ' ');
        if (!sp1) {
            send_text(s, 400, "text/plain; charset=utf-8", "bad request");
            free(buf);
            closesocket(s);
            return 0;
        }
        *sp1 = 0;
        snprintf(req.method, sizeof(req.method), "%s", buf);
        char *target = sp1 + 1;
        char *sp2 = strchr(target, ' ');
        if (sp2) *sp2 = 0;

        char *q = strchr(target, '?');
        if (q) { *q = 0; req.query = q + 1; }
        snprintf(req.path, sizeof(req.path), "%s", target);
    }

    const char *headers = line_end + 2;

    /* 读取请求体 */
    long long clen = header_value(headers, "Content-Length");
    if (clen < 0) clen = 0;
    if (clen > MAX_REQ) {
        send_text(s, 413, "text/plain; charset=utf-8", "too large");
        free(buf);
        closesocket(s);
        return 0;
    }
    size_t have = total - (size_t)(hdr_end - buf);
    while ((long long)have < clen) {
        if (total >= MAX_REQ) break;
        int n = recv(s, buf + total, (int)(MAX_REQ - total - 1), 0);
        if (n <= 0) break;
        total += (size_t)n;
        have += (size_t)n;
    }
    req.body = hdr_end;
    req.body_len = have < (size_t)clen ? have : (size_t)clen;
    if (req.body_len < MAX_REQ) req.body[req.body_len] = 0;

    /* ---------------- 路由 ---------------- */
    int is_get = (strcmp(req.method, "GET") == 0);
    int is_post = (strcmp(req.method, "POST") == 0);

    if (is_get && strncmp(req.path, "/static/", 8) == 0) {
        /* 静态资源不含用户数据，允许无令牌加载；
           真正的门禁在根路径和 API 调用上 */
        serve_asset(srv, s, req.path + 8);
        free(buf);
        closesocket(s);
        return 0;
    }

    if (!token_ok(srv, req.query)) {
        send_text(s, 403, "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"\\u4ee4\\u724c\\u65e0\\u6548\\u3002"
                  "\\u8bf7\\u901a\\u8fc7 LocalCraft \\u542f\\u52a8\\u7684\\u7a97\\u53e3\\u8bbf\\u95ee\\u3002\"}");
        free(buf);
        closesocket(s);
        return 0;
    }

    if (is_get && (strcmp(req.path, "/") == 0 || strcmp(req.path, "/index.html") == 0)) {
        serve_asset(srv, s, "index.html");
        free(buf);
        closesocket(s);
        return 0;
    }

    if (is_get && strcmp(req.path, "/api/ping") == 0) {
        send_text(s, 200, "application/json; charset=utf-8",
                  "{\"ok\":true,\"version\":\"" LC_APP_VERSION_A "\"}");
        free(buf);
        closesocket(s);
        return 0;
    }

    if (is_post && strcmp(req.path, "/api/call") == 0) {
        /* 本机回环服务，仍检查来源，避免被其它网页跨站调用 */
        char origin[256];
        header_str(headers, "Origin", origin, sizeof(origin));
        if (origin[0] &&
            strncmp(origin, "http://127.0.0.1", 16) != 0 &&
            strncmp(origin, "http://localhost", 16) != 0) {
            send_text(s, 403, "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"cross-origin denied\"}");
            free(buf);
            closesocket(s);
            return 0;
        }

        Arena *a = arena_new();
        JsonValue *payload = json_parse(req.body ? req.body : "{}", a);
        if (!payload) {
            send_text(s, 400, "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"\\u8bf7\\u6c42\\u683c\\u5f0f\\u9519\\u8bef\"}");
            arena_free(a);
            free(buf);
            closesocket(s);
            return 0;
        }
        const char *method = json_string(json_get(payload, "method"), "");
        const JsonValue *params = json_get(payload, "params");

        Str out, err;
        str_init(&out);
        str_init(&err);
        int rc = srv->cfg.handler(srv->cfg.ud, method, params, &out, &err);

        Str body;
        str_init(&body);
        if (rc == 0) {
            str_puts(&body, "{\"ok\":true,\"result\":");
            str_puts(&body, out.data ? out.data : "null");
            str_puts(&body, "}");
        } else {
            str_puts(&body, "{\"ok\":false,\"error\":");
            {
                Str esc;
                str_init(&esc);
                json_escape_utf8_to(&esc, err.data ? err.data : "unknown error");
                str_puts(&body, esc.data);
                str_free(&esc);
            }
            str_puts(&body, "}");
        }
        send_response(s, 200, "application/json; charset=utf-8",
                      body.data ? body.data : "{}", body.len);

        str_free(&body);
        str_free(&out);
        str_free(&err);
        arena_free(a);
        free(buf);
        closesocket(s);
        return 0;
    }

    send_text(s, 404, "application/json; charset=utf-8",
              "{\"ok\":false,\"error\":\"not found\"}");
    free(buf);
    closesocket(s);
    return 0;
}

/* ================================================== 接受循环 ========== */

static DWORD WINAPI accept_thread(LPVOID param) {
    HttpServer *srv = (HttpServer *)param;
    while (!srv->stopping) {
        SOCKET c = accept(srv->listen_sock, NULL, NULL);
        if (c == INVALID_SOCKET) {
            if (srv->stopping) break;
            Sleep(20);
            continue;
        }
        ConnCtx *ctx = (ConnCtx *)malloc(sizeof(ConnCtx));
        if (!ctx) { closesocket(c); continue; }
        ctx->srv = srv;
        ctx->sock = c;
        HANDLE h = CreateThread(NULL, 0, conn_thread, ctx, 0, NULL);
        if (h) CloseHandle(h);
        else { free(ctx); closesocket(c); }
    }
    return 0;
}

/* ================================================== 对外接口 ========== */

static int g_wsa_ready = 0;

HttpServer *http_start(const HttpConfig *cfg, int port, wchar_t *err, size_t errcch) {
    if (!g_wsa_ready) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            swprintf(err, errcch, L"无法初始化网络库（WSAStartup 失败）");
            return NULL;
        }
        g_wsa_ready = 1;
    }

    HttpServer *srv = (HttpServer *)calloc(1, sizeof(HttpServer));
    if (!srv) {
        swprintf(err, errcch, L"内存不足");
        return NULL;
    }
    srv->cfg = *cfg;
    srv->listen_sock = INVALID_SOCKET;
    srv->stopping = 0;

    /* 随机令牌 */
    {
        unsigned char raw[24];
        for (int i = 0; i < 24; ++i) raw[i] = (unsigned char)(rand() & 0xff);
        HCRYPTPROV prov = 0;
        if (CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
            CryptGenRandom(prov, 24, raw);
            CryptReleaseContext(prov, 0);
        }
        static const char *alpha =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        for (int i = 0; i < 32; ++i)
            srv->token[i] = alpha[raw[i % 24] % 64];
        srv->token[32] = 0;
    }

    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) {
        swprintf(err, errcch, L"无法创建套接字");
        free(srv);
        return NULL;
    }

    BOOL yes = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    /* 只绑回环地址 —— 局域网无法访问 */
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        swprintf(err, errcch, L"无法绑定 127.0.0.1:%d（端口可能被占用）", port);
        closesocket(ls);
        free(srv);
        return NULL;
    }
    if (listen(ls, 32) != 0) {
        swprintf(err, errcch, L"监听失败");
        closesocket(ls);
        free(srv);
        return NULL;
    }

    struct sockaddr_in bound;
    int blen = sizeof(bound);
    if (getsockname(ls, (struct sockaddr *)&bound, &blen) == 0)
        srv->port = ntohs(bound.sin_port);
    else
        srv->port = port;

    srv->listen_sock = ls;
    srv->thread = CreateThread(NULL, 0, accept_thread, srv, 0, NULL);
    if (!srv->thread) {
        swprintf(err, errcch, L"无法启动服务线程");
        closesocket(ls);
        free(srv);
        return NULL;
    }
    return srv;
}

void http_stop(HttpServer *srv) {
    if (!srv) return;
    InterlockedExchange(&srv->stopping, 1);
    if (srv->listen_sock != INVALID_SOCKET) {
        closesocket(srv->listen_sock);
        srv->listen_sock = INVALID_SOCKET;
    }
    if (srv->thread) {
        WaitForSingleObject(srv->thread, 2000);
        CloseHandle(srv->thread);
        srv->thread = NULL;
    }
    free(srv);
}

int http_port(HttpServer *srv) { return srv ? srv->port : 0; }
const char *http_token(HttpServer *srv) { return srv ? srv->token : ""; }
/* ================================================== 回环自检 ========== */

/*
 * 用自己的 socket 连一次自己的服务，发一个最小请求看有没有 HTTP 响应。
 *
 * 为什么要这个：界面上报"服务端收到接口调用 0 次"时，有两种完全不同的可能 ——
 *   1) 服务根本没起来 / 没在监听
 *   2) 服务好好地在监听，但浏览器那头因为代理、防火墙、安全软件连不进来
 * 这两者要查的方向完全相反。自己连一次就能把它们分开。
 */
int lc_probe_loopback(int port, wchar_t *why, size_t whycch) {
    if (why && whycch) why[0] = 0;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        if (why) swprintf(why, whycch, L"无法创建套接字（%d）", WSAGetLastError());
        return 0;
    }

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        if (why) swprintf(why, whycch, L"连接 127.0.0.1:%d 失败（错误 %d）",
                          port, WSAGetLastError());
        closesocket(s);
        return 0;
    }

    const char *req = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    if (send(s, req, (int)strlen(req), 0) <= 0) {
        if (why) swprintf(why, whycch, L"请求发送失败");
        closesocket(s);
        return 0;
    }

    char buf[64];
    int n = recv(s, buf, sizeof(buf) - 1, 0);
    closesocket(s);
    if (n <= 0) {
        if (why) swprintf(why, whycch, L"已连接但没收到任何响应");
        return 0;
    }
    buf[n] = 0;
    if (strncmp(buf, "HTTP/", 5) != 0) {
        if (why) {
            MultiByteToWideChar(CP_UTF8, 0, buf, -1, why, (int)whycch);
        }
        return 0;
    }
    return 1;
}