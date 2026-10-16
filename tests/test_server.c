/* ==========================================================================
 * test_server.c —— 端到端集成测试：本地服务 + JSON API + 执行 + 回退
 *
 * 起一个真实的服务，用真实 HTTP 请求（环回）走一遍完整流程，
 * 这样令牌校验、JSON 编解码、后台任务线程、回退链路全都验到了。
 * ========================================================================== */
#include <winsock2.h>
#include <ws2tcpip.h>
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
static wchar_t g_home[MAX_PATH * 2];
static int     g_port = 0;
static char    g_token[64];
static SOCKET  g_sock = INVALID_SOCKET;
static size_t  g_last_body_len = 0;

/* --------------------------------------------------------- 文件工具 --- */

static void write_file(const wchar_t *path, const char *content) {
    wchar_t *lp = lc_long_path(path);
    HANDLE h = CreateFileW(lp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, content, (DWORD)strlen(content), &w, NULL);
    CloseHandle(h);
}

static void read_file(const wchar_t *path, char *buf, size_t cch) {
    buf[0] = 0;
    wchar_t *lp = lc_long_path(path);
    HANDLE h = CreateFileW(lp, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD got = 0;
    if (ReadFile(h, buf, (DWORD)(cch - 1), &got, NULL)) buf[got] = 0;
    CloseHandle(h);
}

/* Path helper for an explicit base directory */
static wchar_t *wp2(const wchar_t *base, const wchar_t *name) {
    static wchar_t buf[4][MAX_PATH * 2];
    static int slot = 0;
    slot = (slot + 1) % 4;
    swprintf(buf[slot], MAX_PATH * 2, L"%ls\\%ls", base, name);
    return buf[slot];
}

static wchar_t *wp(const wchar_t *name) {
    static wchar_t buf[8][MAX_PATH * 2];
    static int slot = 0;
    slot = (slot + 1) % 8;
    swprintf(buf[slot], MAX_PATH * 2, L"%ls\\%ls", g_root, name);
    return buf[slot];
}

/* ------------------------------------------------------------ HTTP --- */

static int tcp_connect(void) {
    if (g_sock != INVALID_SOCKET) { closesocket(g_sock); g_sock = INVALID_SOCKET; }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)g_port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    for (int attempt = 0; attempt < 50; ++attempt) {
        if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            g_sock = s;
            return 0;
        }
        Sleep(50);
    }
    closesocket(s);
    return -1;
}

/* 发一个 POST /api/call，把响应体写到 resp。返回 0 成功。 */
static int api_call_raw(const char *method, const char *params_json,
                        char *resp, size_t cch, const char *token_override) {
    if (tcp_connect() != 0) return -1;

    char body[8192];
    snprintf(body, sizeof(body), "{\"method\":\"%s\",\"params\":%s}",
             method, params_json && params_json[0] ? params_json : "{}");

    const char *tok = token_override ? token_override : g_token;
    char req[16384];
    int n = snprintf(req, sizeof(req),
        "POST /api/call?token=%s HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n%s",
        tok, (int)strlen(body), body);
    if (n <= 0 || (size_t)n >= sizeof(req)) return -1;

    if (send(g_sock, req, n, 0) != n) return -1;

    size_t total = 0;
    for (;;) {
        if (total + 1 >= cch) break;
        int got = recv(g_sock, resp + total, (int)(cch - total - 1), 0);
        if (got <= 0) break;
        total += (size_t)got;
    }
    resp[total] = 0;
    closesocket(g_sock);
    g_sock = INVALID_SOCKET;
    if (total == 0) return -1;

    /* 跳过响应头 */
    char *split = strstr(resp, "\r\n\r\n");
    if (split) {
        size_t hlen = (size_t)(split + 4 - resp);
        memmove(resp, split + 4, total - hlen + 1);
        g_last_body_len = total - hlen;
    }
    return 0;
}

/* 调用接口并把 result 解析成 JsonValue（arena 分配） */
static JsonValue *api_call(const char *method, const char *params,
                           char *errbuf, size_t errcch, Arena *a) {
    static char resp[1 << 16];
    if (api_call_raw(method, params, resp, sizeof(resp), NULL) != 0) {
        if (errbuf) snprintf(errbuf, errcch, "connection failed");
        return NULL;
    }
    JsonValue *v = json_parse(resp, a);
    if (!v) {
        /* 解析失败时把原始响应落盘，方便定位到底是哪个字节有问题 */
        {
            wchar_t dump[MAX_PATH * 2];
            swprintf(dump, MAX_PATH * 2, L"%ls\\bad-json.txt", g_root);
            FILE *fp = _wfopen(dump, L"wb");
            if (fp) {
                char head[256];
                snprintf(head, sizeof(head), "method=%s len=%llu strlen=%llu\n", method, (unsigned long long)g_last_body_len, (unsigned long long)strlen(resp));
                fwrite(head, 1, strlen(head), fp);
                fwrite(resp, 1, g_last_body_len, fp);
                fclose(fp);
            }
        }
        if (errbuf) snprintf(errbuf, errcch, "bad json: %.200s", resp);
        return NULL;
    }
    if (!json_bool(json_get(v, "ok"), 0)) {
        const char *e = json_string(json_get(v, "error"), "unknown");
        if (errbuf) snprintf(errbuf, errcch, "%s", e);
        return NULL;
    }
    return json_get(v, "result");
}


/* ====================================================== 用例 =========== */

static void test_security(void) {
    wprintf(L"[1] 令牌与安全边界\n");
    static char resp[4096];

    /* 错令牌必须被拒 */
    JsonValue *v;
    Arena *a = arena_new();
    char err[256];
    a = arena_new();
    CHECK(api_call_raw("app.info", "{}", resp, sizeof(resp), "wrong-token") == 0,
          L"请求应能发出");
    v = json_parse(resp, a);
    CHECK(v && !json_bool(json_get(v, "ok"), 0), L"错误令牌必须被拒绝");
    arena_free(a);

    /* 正确令牌能拿到版本 */
    a = arena_new();
    JsonValue *r = api_call("app.info", "{}", err, sizeof(err), a);
    CHECK(r != NULL, L"正确令牌应成功：%s", err);
    if (r) {
        const char *ver = json_string(json_get(r, "version"), "");
        CHECK(ver[0] != 0, L"应返回版本号");
        CHECK(json_bool(json_get(r, "portable"), 1) == 0, L"默认不是便携模式");
    }
    arena_free(a);

    /* 未知接口 */
    a = arena_new();
    CHECK(api_call("nope.nope", "{}", err, sizeof(err), a) == NULL,
          L"未知接口应失败");
    CHECK(strstr(err, "\xe6\x9c\xaa\xe7\x9f\xa5") != NULL,
          L"应报“未知接口”，实际：%s", err);
    arena_free(a);
}

static void test_preview_and_toggle(void) {
    wprintf(L"[2] 预览 / 勾选 / 策略切换\n");
    Arena *a = arena_new();
    char params[2048], err[256];

    snprintf(params, sizeof(params),
             "{\"dir\":\"%ls\",\"template\":\"R_{n:03d}{ext}\",\"policy\":\"rename\"}",
             g_root);
    /* 路径里有反斜杠，JSON 里要转义 */
    {
        char esc[2048]; size_t j = 0;
        for (size_t i = 0; params[i] && j + 2 < sizeof(esc); ++i) {
            if (params[i] == '\\') { esc[j++] = '\\'; esc[j++] = '\\'; }
            else esc[j++] = params[i];
        }
        esc[j] = 0;
        memcpy(params, esc, j + 1);
    }

    JsonValue *r = api_call("rename.preview", params, err, sizeof(err), a);
    CHECK(r != NULL, L"预览应成功：%s", err);
    if (!r) { arena_free(a); return; }

    JsonValue *stats = json_get(r, "stats");
    CHECK(json_int(json_get(stats, "total"), -1) == 3,
          L"应识别 3 个文件，实际 %d", json_int(json_get(stats, "total"), -1));
    CHECK(json_int(json_get(stats, "executable"), -1) == 3, L"应可执行 3 项");

    const char *pid = json_string(json_get(r, "plan_id"), "");
    CHECK(pid[0] != 0, L"应返回 plan_id");

    JsonValue *rows = json_get(r, "rows");
    CHECK(json_array_len(rows) == 3, L"应有 3 行");
    if (json_array_len(rows) == 3) {
        const char *nm = json_string(json_get(json_at(rows, 0), "new"), "");
        CHECK(nm[0] != 0, L"新路径不应为空");
        /* 文件名应为 R_001.txt 这种（按名字排序） */
        const char *base = strrchr(nm, '\\');
        base = base ? base + 1 : nm;
        CHECK(strcmp(base, "R_001.txt") == 0, L"首行新名应为 R_001.txt，实际 %s", base);
    }

    /* 取消勾选第一行 */
    char p2[512];
    snprintf(p2, sizeof(p2), "{\"plan_id\":\"%s\",\"index\":0}", pid);
    JsonValue *r2 = api_call("plan.toggle", p2, err, sizeof(err), a);
    CHECK(r2 != NULL, L"toggle 应成功：%s", err);
    if (r2)
        CHECK(json_int(json_get(json_get(r2, "stats"), "executable"), -1) == 2,
              L"取消勾选后可执行应变成 2");

    /* 再勾回来 */
    r2 = api_call("plan.toggle", p2, err, sizeof(err), a);
    if (r2)
        CHECK(json_int(json_get(json_get(r2, "stats"), "executable"), -1) == 3,
              L"勾回来应恢复 3");

    /* 切换策略 */
    snprintf(p2, sizeof(p2), "{\"plan_id\":\"%s\",\"policy\":\"ask\"}", pid);
    r2 = api_call("plan.set_policy", p2, err, sizeof(err), a);
    CHECK(r2 != NULL, L"切策略应成功：%s", err);
    if (r2) CHECK(json_string(json_get(r2, "plan_id"), "")[0] != 0, L"应返回新计划");

    /* 过期的 plan_id */
    r2 = api_call("plan.get", "{\"plan_id\":\"nope\"}", err, sizeof(err), a);
    CHECK(r2 == NULL, L"未知 plan_id 应失败");
    CHECK(strstr(err, "expired") || err[0], L"应有错误说明");

    arena_free(a);
}

static int wait_job(const char *job_id, Arena *a, char *out_json, size_t cch,
                    int timeout_ms) {
    char p[256];
    snprintf(p, sizeof(p), "{\"job_id\":\"%s\"}", job_id);
    DWORD t0 = GetTickCount();
    for (;;) {
        Arena *tmp = arena_new();
        char err[256];
        JsonValue *j = api_call("job.status", p, err, sizeof(err), tmp);
        int finished = 0;
        if (j && json_bool(json_get(j, "finished"), 0)) finished = 1;
        if (finished) {
            /* 用 snprintf 重新序列化关键字段太麻烦，直接把原始 result 拷出来 */
            extern int api_call_raw(const char *, const char *, char *, size_t, const char *);
            char raw[1 << 16];
            api_call_raw("job.status", p, raw, sizeof(raw), NULL);
            snprintf(out_json, cch, "%s", raw);
            arena_free(tmp);
            return 0;
        }
        arena_free(tmp);
        if (GetTickCount() - t0 > (DWORD)timeout_ms) return -1;
        Sleep(60);
    }
}

static void test_execute_and_rollback(void) {
    wprintf(L"[3] 执行 → 日志 → 回退（全链路）\n");
    Arena *a = arena_new();
    char params[2048], err[256];
    snprintf(params, sizeof(params),
             "{\"dir\":\"%ls\",\"template\":\"R_{n:03d}{ext}\",\"policy\":\"rename\"}",
             g_root);
    {
        char esc[2048]; size_t j = 0;
        for (size_t i = 0; params[i] && j + 2 < sizeof(esc); ++i) {
            if (params[i] == '\\') { esc[j++] = '\\'; esc[j++] = '\\'; }
            else esc[j++] = params[i];
        }
        esc[j] = 0;
        memcpy(params, esc, j + 1);
    }
    JsonValue *r = api_call("rename.preview", params, err, sizeof(err), a);
    if (!r) { CHECK(0, L"预览失败：%s", err); arena_free(a); return; }
    const char *pid = json_string(json_get(r, "plan_id"), "");

    /* 只读预演不应改动文件 */
    char p2[512];
    snprintf(p2, sizeof(p2), "{\"plan_id\":\"%s\"}", pid);
    JsonValue *dry = api_call("exec.dry", p2, err, sizeof(err), a);
    CHECK(dry != NULL, L"预演应成功：%s", err);
    if (dry) CHECK(json_int(json_get(dry, "ok"), -1) == 3, L"预演应报告 3 项可执行");

    /* 真正执行 */
    JsonValue *start = api_call("exec.run", p2, err, sizeof(err), a);
    CHECK(start != NULL, L"启动执行应成功：%s", err);
    if (!start) { arena_free(a); return; }
    const char *job = json_string(json_get(start, "job_id"), "");
    CHECK(job[0] != 0, L"应返回 job_id");

    char raw[1 << 16];
    CHECK(wait_job(job, a, raw, sizeof(raw), 30000) == 0, L"任务应完成");

    Arena *ja = arena_new();
    /* 响应是 {"ok":true,"result":{ ...job status... }}，
       job 状态里再有一层 "result" 才是执行结果 —— 要剥两层。 */
    JsonValue *jr = json_parse(raw, ja);
    JsonValue *status = jr ? json_get(jr, "result") : NULL;
    JsonValue *res = status ? json_get(status, "result") : NULL;
    CHECK(status != NULL, L"应返回任务状态");
    CHECK(res != NULL, L"应返回执行结果");
    if (!res) wprintf(L"  RAW=[%hs]\n", raw);
    if (res) {
        CHECK(json_int(json_get(res, "ok"), -1) == 3,
              L"应成功 3 项，实际 %d", json_int(json_get(res, "ok"), -1));
        CHECK(json_int(json_get(res, "failed"), -1) == 0, L"不应有失败");
        const char *jn = json_string(json_get(res, "journal"), "");
        CHECK(jn[0] != 0, L"应返回日志路径");
        wchar_t *jpath = lc_utf8_to_wcs(jn);
        CHECK(jpath && lc_is_file(jpath), L"日志文件应存在：%ls", jpath ? jpath : L"");
        free(jpath);
    }

    /* 文件确实改了名 */
    CHECK(lc_is_file(wp(L"R_001.txt")), L"应存在 R_001.txt");
    {
        char buf[64];
        read_file(wp(L"R_001.txt"), buf, sizeof(buf));
        CHECK(strcmp(buf, "A") == 0, L"内容应保持 A，实际 %s", buf);
    }

    /* 历史里应能查到 */
    JsonValue *hist = api_call("history.list", "{\"limit\":20}", err, sizeof(err), a);
    CHECK(hist != NULL, L"历史应可读：%s", err);
    const char *journal_path = "";
    if (hist) {
        JsonValue *rows = json_get(hist, "rows");
        CHECK(json_array_len(rows) >= 1, L"至少应有一条历史");
        if (json_array_len(rows) >= 1)
            journal_path = json_string(json_get(json_at(rows, 0), "path"), "");
    }
    CHECK(journal_path[0] != 0, L"应拿到日志路径");

    /* 回退预览 */
    char p3[2048];
    snprintf(p3, sizeof(p3), "{\"path\":\"%s\"}", journal_path);
    {
        char esc[2048]; size_t j = 0;
        for (size_t i = 0; p3[i] && j + 2 < sizeof(esc); ++i) {
            if (p3[i] == '\\') { esc[j++] = '\\'; esc[j++] = '\\'; }
            else esc[j++] = p3[i];
        }
        esc[j] = 0;
        memcpy(p3, esc, j + 1);
    }
    JsonValue *rp = api_call("history.rollback_preview", p3, err, sizeof(err), a);
    CHECK(rp != NULL, L"回退预览应成功：%s", err);
    if (!rp) { arena_free(ja); arena_free(a); return; }

    CHECK(json_int(json_get(json_get(rp, "stats"), "executable"), -1) == 3,
          L"应报告 3 步可回退");
    /* 预览不能动文件 */
    CHECK(lc_is_file(wp(L"R_001.txt")), L"回退预览不该改动文件");

    const char *rb_pid = json_string(json_get(rp, "plan_id"), "");
    char p4[512];
    snprintf(p4, sizeof(p4), "{\"plan_id\":\"%s\"}", rb_pid);
    JsonValue *rb = api_call("history.rollback_run", p4, err, sizeof(err), a);
    CHECK(rb != NULL, L"启动回退应成功：%s", err);
    if (rb) {
        const char *rjob = json_string(json_get(rb, "job_id"), "");
        CHECK(wait_job(rjob, a, raw, sizeof(raw), 30000) == 0, L"回退任务应完成");

        Arena *ra = arena_new();
        JsonValue *rj = json_parse(raw, ra);
        JsonValue *rstatus = rj ? json_get(rj, "result") : NULL;
        JsonValue *rres = rstatus ? json_get(rstatus, "result") : NULL;
        CHECK(rres != NULL, L"回退应返回结果");
        if (rres) {
            CHECK(json_int(json_get(rres, "failed"), -1) == 0,
                  L"回退不应有失败");
            CHECK(json_int(json_get(rres, "ok"), -1) == 3, L"应回退 3 步");
        }
        arena_free(ra);
    }

    /* 文件回到原位 */
    CHECK(lc_is_file(wp(L"a.txt")), L"回退后应恢复 a.txt");
    CHECK(lc_is_file(wp(L"b.txt")), L"回退后应恢复 b.txt");
    CHECK(lc_is_file(wp(L"c.txt")), L"回退后应恢复 c.txt");
    CHECK(!lc_is_file(wp(L"R_001.txt")), L"回退后 R_001.txt 应消失");
    {
        char buf[64];
        read_file(wp(L"a.txt"), buf, sizeof(buf));
        CHECK(strcmp(buf, "A") == 0, L"回退后内容应完好，实际 %s", buf);
    }

    arena_free(ja);
    arena_free(a);
}

static void test_missing_dir(void) {
    wprintf(L"[4] 错误处理\n");
    Arena *a = arena_new();
    char err[512];
    CHECK(api_call("rename.preview", "{\"dir\":\"C:\\\\__no_such__dir__\"}",
                   err, sizeof(err), a) == NULL,
          L"不存在的目录应报错");
    CHECK(err[0] != 0, L"应给出错误说明");
    arena_free(a);

    a = arena_new();
    CHECK(api_call("organize.scan", "{}", err, sizeof(err), a) == NULL,
          L"缺参数应报错");
    arena_free(a);
}

static void test_engine_report(void) {
    wprintf(L"[5] 环境自检\n");
    Arena *a = arena_new();
    char err[256];
    JsonValue *r = api_call("app.engines", "{}", err, sizeof(err), a);
    CHECK(r != NULL, L"引擎探测应成功：%s", err);
    if (r) {
        CHECK(json_array_len(json_get(r, "engines")) == 4, L"应报告 4 个引擎项");
        CHECK(json_string(json_get(r, "tools_dir"), "")[0] != 0, L"应返回 tools 目录");
    }
    arena_free(a);

    a = arena_new();
    r = api_call("app.beat", "{}", err, sizeof(err), a);
    CHECK(r != NULL && json_bool(json_get(r, "ok"), 0), L"心跳应成功");
    arena_free(a);
}

/* 把宽字符路径转成 JSON 字符串里可用的 UTF-8（反斜杠转义） */
static void json_path(char *out, size_t cch, const wchar_t *path) {
    char *u = lc_wcs_to_utf8(path);
    size_t j = 0;
    if (!u) { out[0] = 0; return; }
    for (size_t i = 0; u[i] && j + 2 < cch; ++i) {
        if (u[i] == '\\') { out[j++] = '\\'; out[j++] = '\\'; }
        else out[j++] = u[i];
    }
    out[j] = 0;
    free(u);
}

/* 模块 2/3/4/5 的接口回归：重点是 JSON 结构必须能被解析回对象 */
static void test_new_modules(void) {
    wprintf(L"[6] 新模块接口（去重 / 摘要 / 导出 / 转换）\n");
    char esc_dir[4096], esc[4096], params[8192], err[512];

    /* Separate subdirectory: the older cases count files in g_root, so this
       test must not pollute it. */
    wchar_t mod[MAX_PATH * 2];
    swprintf(mod, MAX_PATH * 2, L"%ls\\mod", g_root);
    lc_mkdir_p(mod);
    json_path(esc_dir, sizeof(esc_dir), mod);

    /* 造内容雷同但文件名毫不相干的三份文档 */
    {
        const char *body =
            "\xE5\xAD\xA3\xE5\xBA\xA6\xE7\xBB\x8F\xE8\x90\xA5\xE5\x88\x86\xE6\x9E\x90\xE6\x8A\xA5\xE5\x91\x8A\n"
            "\xE6\x9C\xAC\xE5\xAD\xA3\xE5\xBA\xA6\xE8\x90\xA5\xE4\xB8\x9A\xE6\x94\xB6\xE5\x85\xA5\xE8\xBE\x83\xE4\xB8\x8A\xE5\xAD\xA3\xE5\xBA\xA6\xE5\xA2\x9E\xE9\x95\xBF\xE7\x99\xBE\xE5\x88\x86\xE4\xB9\x8B\xE5\x8D\x81\xE4\xBA\x8C\xEF\xBC\x8C\xE4\xB8\xBB\xE8\xA6\x81\xE6\x9D\xA5\xE8\x87\xAA\xE5\x8D\x8E\xE4\xB8\x9C\xE5\x8C\xBA\xE5\x9F\x9F\xE7\x9A\x84\xE6\xB8\xA0\xE9\x81\x93\xE6\x89\xA9\xE5\xBC\xA0\xE3\x80\x82\n"
            "\xE6\xAF\x9B\xE5\x88\xA9\xE7\x8E\x87\xE7\xBB\xB4\xE6\x8C\x81\xE5\x9C\xA8\xE7\x99\xBE\xE5\x88\x86\xE4\xB9\x8B\xE4\xB8\x89\xE5\x8D\x81\xE5\x85\xAB\xE5\xB7\xA6\xE5\x8F\xB3\xEF\xBC\x8C\xE4\xB8\x8E\xE5\x8E\xBB\xE5\xB9\xB4\xE5\x90\x8C\xE6\x9C\x9F\xE5\x9F\xBA\xE6\x9C\xAC\xE6\x8C\x81\xE5\xB9\xB3\xE3\x80\x82\n"
            "\xE4\xB8\x8B\xE5\xAD\xA3\xE5\xBA\xA6\xE8\xAE\xA1\xE5\x88\x92\xE7\xBB\xA7\xE7\xBB\xAD\xE5\x8A\xA0\xE5\xA4\xA7\xE5\x9C\xA8\xE5\x8D\x8E\xE5\x8D\x97\xE5\x92\x8C\xE8\xA5\xBF\xE5\x8D\x97\xE7\x9A\x84\xE6\x8A\x95\xE5\x85\xA5\xEF\xBC\x8C\xE5\x90\x8C\xE6\x97\xB6\xE6\x8E\xA7\xE5\x88\xB6\xE4\xBA\xBA\xE5\x91\x98\xE8\xA7\x84\xE6\xA8\xA1\xE3\x80\x82\n";
        write_file(wp2(mod, L"report_A.txt"), body);
        write_file(wp2(mod, L"会议纪要_最终版.txt"), body);
        write_file(wp2(mod, L"z_copy.txt"), body);
        write_file(wp2(mod, L"unrelated.txt"),
            "\xE4\xBB\x8A\xE5\xA4\xA9\xE5\xA4\xA9\xE6\xB0\x94\xE6\x99\xB4\xE6\x9C\x97\xEF\xBC\x8C\xE6\xB0\x94\xE6\xB8\xA9\xE4\xBA\x8C\xE5\x8D\x81\xE5\xBA\xA6\xE5\xB7\xA6\xE5\x8F\xB3\xEF\xBC\x8C\xE9\x80\x82\xE5\x90\x88\xE5\xA4\x96\xE5\x87\xBA\xE6\xB4\xBB\xE5\x8A\xA8\xE3\x80\x82\n"
            "\xE5\x82\x8D\xE6\x99\x9A\xE6\x9C\x89\xE8\xBD\xBB\xE5\xBE\xAE\xE5\x8C\x97\xE9\xA3\x8E\xEF\xBC\x8C\xE7\xA9\xBA\xE6\xB0\x94\xE8\xB4\xA8\xE9\x87\x8F\xE8\x89\xAF\xE5\xA5\xBD\xE3\x80\x82\n");
    }

    /* ---- 内容去重：结构必须完整可解析 ---- */
    {
        snprintf(params, sizeof(params),
                 "{\"dir\":\"%s\",\"threshold\":0.85,\"recursive\":true,\"min_chars\":20}",
                 esc_dir);
        Arena *a = arena_new();
        JsonValue *r = api_call("dedup.scan", params, err, sizeof(err), a);
        CHECK(r != NULL, L"dedup.scan 应成功：%hs", err);
        if (r) {
            JsonValue *st = json_get(r, "stats");
            CHECK(st != NULL, L"应有 stats");
            CHECK(json_int(json_get(st, "scanned"), -1) >= 4,
                  L"应扫描到至少 4 个文档");
            CHECK(json_int(json_get(st, "groups"), -1) >= 1,
                  L"应至少发现 1 组重复");

            JsonValue *gs = json_get(r, "groups");
            CHECK(json_is_array(gs) && json_array_len(gs) >= 1, L"groups 应是数组");
            if (json_is_array(gs) && json_array_len(gs) >= 1) {
                JsonValue *items = json_get(json_at(gs, 0), "items");
                CHECK(json_is_array(items) && json_array_len(items) >= 2,
                      L"第一组应至少 2 项");
                if (json_is_array(items) && json_array_len(items) >= 1) {
                    JsonValue *it0 = json_at(items, 0);
                    /* 曾经漏写开头的 '{'，这几条会直接全部失败 */
                    CHECK(json_string(json_get(it0, "name"), NULL) != NULL,
                          L"重复项应有 name");
                    CHECK(json_int(json_get(it0, "tokens"), -1) > 0,
                          L"重复项应有 tokens");
                    CHECK(json_bool(json_get(it0, "keeper"), -1) >= 0,
                          L"重复项应有 keeper");
                }
            }
        }
        arena_free(a);
    }

    /* ---- 去重预览 ---- */
    {
        snprintf(params, sizeof(params),
                 "{\"dir\":\"%s\",\"threshold\":0.85,\"min_chars\":20,"
                 "\"action\":0,\"policy\":\"rename\"}", esc_dir);
        Arena *a = arena_new();
        JsonValue *r = api_call("dedup.preview", params, err, sizeof(err), a);
        CHECK(r != NULL, L"dedup.preview 应成功：%hs", err);
        if (r)
            CHECK(json_int(json_get(json_get(r, "stats"), "executable"), -1) >= 1,
                  L"去重计划应至少 1 项可执行");
        CHECK(lc_is_file(wp2(mod, L"z_copy.txt")), L"预览阶段不该移动文件");
        arena_free(a);
    }

    /* ---- 文档摘要 ---- */
    {
        wchar_t doc[MAX_PATH * 2];
        swprintf(doc, MAX_PATH * 2, L"%ls\\sum.txt", mod);
        const wchar_t *body =
            L"LocalCraft 是一款完全离线的本地文件处理工具。\n"
            L"它不会发起任何网络请求，也不会把文件上传到任何地方。\n"
            L"所有批量操作都遵循先预览、再执行、可回退的流程。\n"
            L"今天午饭吃的是番茄炒蛋和米饭，味道还不错。\n"
            L"预览阶段是只读的，会把每个文件的旧状态和新状态列成对照表。\n"
            L"执行过程会逐步写入回退日志，之后任何时候都能一键还原。\n"
            L"程序使用纯 C 语言实现，静态链接后只有一个可执行文件。\n";
        char *u8 = lc_wcs_to_utf8(body);
        if (u8) { write_file(doc, u8); free(u8); }

        json_path(esc, sizeof(esc), doc);
        snprintf(params, sizeof(params),
                 "{\"path\":\"%s\",\"sentences\":3,\"algorithm\":1}", esc);
        Arena *a = arena_new();
        JsonValue *r = api_call("summarize.file", params, err, sizeof(err), a);
        CHECK(r != NULL, L"summarize.file 应成功：%hs", err);
        if (r) {
            JsonValue *sents = json_get(r, "sentences");
            CHECK(json_is_array(sents) && json_array_len(sents) == 3,
                  L"应抽出 3 个句子，实际 %llu",
                  (unsigned long long)json_array_len(sents));
            if (json_is_array(sents) && json_array_len(sents) >= 1) {
                JsonValue *s0 = json_at(sents, 0);
                const char *t = json_string(json_get(s0, "text"), NULL);
                CHECK(t != NULL && t[0] != 0, L"句子应有 text");
                CHECK(json_int(json_get(s0, "end"), 0) >
                      json_int(json_get(s0, "start"), 0),
                      L"句子应有可定位的原文区间");
            }
            JsonValue *kw = json_get(r, "keywords");
            CHECK(json_is_array(kw) && json_array_len(kw) > 0, L"应有关键词");
            if (json_is_array(kw) && json_array_len(kw) >= 1)
                CHECK(json_string(json_get(json_at(kw, 0), "word"), NULL) != NULL,
                      L"关键词应有 word 字段");
        }
        arena_free(a);
    }

    /* ---- 导出 ---- */
    {
        wchar_t md[MAX_PATH * 2];
        swprintf(md, MAX_PATH * 2, L"%ls\\doc.md", mod);
        write_file(md, "# Title\n\n## Section\n\nBody text here.\n");

        json_path(esc, sizeof(esc), md);
        snprintf(params, sizeof(params), "{\"path\":\"%s\",\"format\":\"docx\"}", esc);
        Arena *a = arena_new();
        JsonValue *r = api_call("export.run", params, err, sizeof(err), a);
        CHECK(r != NULL, L"export.run 应成功：%hs", err);
        if (r) {
            CHECK(json_int(json_get(json_get(r, "stats"), "executable"), -1) == 1,
                  L"导出计划应有 1 项可执行");
            JsonValue *rows = json_get(r, "rows");
            if (json_is_array(rows) && json_array_len(rows) >= 1) {
                const char *np = json_string(json_get(json_at(rows, 0), "new"), "");
                CHECK(strstr(np, ".docx") != NULL, L"目标应是 .docx，实际 %s", np);
            }
        }
        arena_free(a);
    }

    /* ---- 转换 ---- */
    {
        snprintf(params, sizeof(params),
                 "{\"dir\":\"%s\",\"target\":\"md\",\"recursive\":false}", esc_dir);
        Arena *a = arena_new();
        JsonValue *r = api_call("convert.preview", params, err, sizeof(err), a);
        CHECK(r != NULL, L"convert.preview 应成功：%hs", err);
        if (r)
            CHECK(json_int(json_get(json_get(r, "stats"), "total"), -1) >= 1,
                  L"应至少有 1 个待转换文件");
        arena_free(a);

        a = arena_new();
        r = api_call("convert.engines", "{}", err, sizeof(err), a);
        CHECK(r != NULL, L"convert.engines 应成功：%hs", err);
        if (r) CHECK(json_array_len(json_get(r, "engines")) == 4,
                     L"应报告 4 个外部引擎");
        arena_free(a);
    }
}

/* ====================================================== main =========== */

int main(void) {
    SetConsoleOutputCP(CP_UTF8);

    /* 工作目录内的临时目录（受限环境不允许写 %TEMP%） */
    wchar_t exe[MAX_PATH * 2], *slash;
    GetModuleFileNameW(NULL, exe, MAX_PATH * 2);
    slash = wcsrchr(exe, L'\\');
    if (slash) *slash = 0;
    slash = wcsrchr(exe, L'\\');
    if (slash) *slash = 0;

    swprintf(g_home, MAX_PATH * 2, L"%ls\\testtmp\\server", exe);
    lc_mkdir_p(g_home);
    SetEnvironmentVariableW(L"LOCALCRAFT_HOME", g_home);

    /* 每次跑用全新的工作目录，避免上一轮的残留文件干扰断言 */
    swprintf(g_root, MAX_PATH * 2, L"%ls\\work-%u", g_home, (unsigned)GetTickCount());
    lc_mkdir_p(g_root);

    wprintf(L"=== LocalCraft 服务端到端自检 ===\n");
    wprintf(L"工作目录：%ls\n\n", g_root);

    /* 起服务 */
    ApiState *api = api_create(NULL);
    CHECK(api != NULL, L"api_create 失败");
    if (!api) return 1;

    HttpConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.handler = api_dispatch;
    cfg.ud = api;

    wchar_t werr[512];
    HttpServer *srv = http_start(&cfg, 0, werr, 512);
    CHECK(srv != NULL, L"服务启动失败：%ls", werr);
    if (!srv) { api_destroy(api); return 1; }

    g_port = http_port(srv);
    snprintf(g_token, sizeof(g_token), "%s", http_token(srv));
    CHECK(g_port > 0, L"端口应为有效值");
    wprintf(L"服务已启动：127.0.0.1:%d\n\n", g_port);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    /* 准备测试文件 */
    write_file(wp(L"a.txt"), "A");
    write_file(wp(L"b.txt"), "B");
    write_file(wp(L"c.txt"), "C");

    test_security();
    test_engine_report();
    test_missing_dir();
    test_new_modules();
    test_preview_and_toggle();
    test_execute_and_rollback();

    if (g_sock != INVALID_SOCKET) closesocket(g_sock);
    http_stop(srv);
    api_destroy(api);
    WSACleanup();

    wprintf(L"\n=== 通过 %d / 失败 %d ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
