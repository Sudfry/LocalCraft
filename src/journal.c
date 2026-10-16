/* ==========================================================================
 * journal.c —— 回退日志：JSONL 追加写 + 每步 fsync + 读取
 *
 * 日志是整个"可回退"承诺的物理载体：
 *   · 追加写、永不修改 —— 已写入的行就是事实，回退只读不写
 *   · 逐步落盘 —— 第 7 步断电，前 6 步依然能完整回退
 *   · 自描述 —— header 带齐 plan_id / 策略 / 根目录，单个文件即可独立回退
 *   · 回退本身也写日志 —— 于是回退可以再被回退（重做）
 * ========================================================================== */
#include "lc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>

/* ------------------------------------------------------- 枚举 -> 文本 --- */

static const char *step_kind_name(StepKind k) {
    switch (k) {
        case STEP_MOVE:   return "move";
        case STEP_COPY:   return "copy";
        case STEP_MKDIR:  return "mkdir";
        case STEP_RMDIR:  return "rmdir";
        case STEP_CREATE: return "create";
    }
    return "unknown";
}

static int step_kind_from(const char *s) {
    if (!s) return -1;
    if (strcmp(s, "move") == 0)   return STEP_MOVE;
    if (strcmp(s, "copy") == 0)   return STEP_COPY;
    if (strcmp(s, "mkdir") == 0)  return STEP_MKDIR;
    if (strcmp(s, "rmdir") == 0)  return STEP_RMDIR;
    if (strcmp(s, "create") == 0) return STEP_CREATE;
    return -1;
}

static const char *policy_name(Policy p) {
    switch (p) {
        case POLICY_ASK:       return "ask";
        case POLICY_SKIP:      return "skip";
        case POLICY_OVERWRITE: return "overwrite";
        case POLICY_RENAME:    return "rename";
    }
    return "rename";
}

static const char *status_name(ActionStatus s) {
    switch (s) {
        case ST_PENDING: return "pending";
        case ST_DONE:    return "done";
        case ST_FAILED:  return "failed";
        case ST_SKIPPED: return "skipped";
        case ST_PARTIAL: return "partial";
    }
    return "pending";
}

/* ---------------------------------------------------------- 打开日志 --- */

static wchar_t *journal_path_for_impl(const wchar_t *plan_id, int is_rollback) {
    wchar_t *dir = lc_journal_dir();
    WStr s;
    wstr_init(&s);
    wstr_puts(&s, dir);
    wstr_putc(&s, L'\\');
    wstr_puts(&s, is_rollback ? L"rollback-" : L"journal-");
    wstr_puts(&s, plan_id);
    wstr_puts(&s, L".jsonl");
    free(dir);
    return wstr_steal(&s);
}

wchar_t *journal_path_for(const wchar_t *plan_id, int is_rollback) {
    return journal_path_for_impl(plan_id, is_rollback);
}

static HANDLE journal_open_append(const wchar_t *path) {
    wchar_t *dir = lc_dirname(path);
    if (dir) { lc_ensure_dir(dir); free(dir); }
    wchar_t *lp = lc_long_path(path);
    if (!lp) return INVALID_HANDLE_VALUE;
    HANDLE h = CreateFileW(lp, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    return h;
}

/* --------------------------------------------------------- 写记录 ------ */

static int jw_emit(JournalWriter *jw, Str *line) {
    if (!jw || jw->closed) return -1;
    if (lc_append_line_fsync(jw->file, line->data ? line->data : "") != 0)
        return -1;
    jw->records++;
    return 0;
}

/* 打开日志追加写句柄（供 executor / rollback 使用） */
HANDLE journal_open(const wchar_t *path) {
    return journal_open_append(path);
}

int journal_write_header(JournalWriter *jw, Plan *plan) {
    Str line;
    str_init(&line);
    str_puts(&line, "{\"t\":\"header\",\"v\":1,\"app\":\"LocalCraft\",\"app_version\":\""
                    LC_APP_VERSION_A "\"");
    str_printf(&line, ",\"kind\":\"%s\"",
               jw->is_rollback_hint ? "rollback" : "execute");
    str_puts(&line, ",\"plan_id\":");
    json_escape_utf8_to(&line, plan->plan_id ? plan->plan_id : "");
    str_printf(&line, ",\"created\":%lld,\"started\":%lld",
               (long long)plan->created, (long long)time(NULL));
    str_puts(&line, ",\"root\":");
    json_escape_to(&line, plan->root);
    str_puts(&line, ",\"policy\":");
    json_escape_utf8_to(&line, policy_name(plan->policy));
    str_puts(&line, ",\"category\":");
    json_escape_to(&line, plan->category);
    str_puts(&line, ",\"note\":");
    json_escape_to(&line, plan->note);
    str_printf(&line, ",\"action_count\":%llu",
               (unsigned long long)plan->n_actions);

    str_puts(&line, ",\"actions\":[");
    for (size_t i = 0; i < plan->n_actions; ++i) {
        Action *a = &plan->actions[i];
        if (i) str_putc(&line, ',');
        str_puts(&line, "{\"action_id\":");
        json_escape_utf8_to(&line, a->action_id ? a->action_id : "");
        str_puts(&line, ",\"category\":");
        json_escape_to(&line, a->category);
        str_puts(&line, ",\"src\":");
        json_escape_to(&line, a->src);
        str_puts(&line, ",\"dst\":");
        json_escape_to(&line, a->dst);
        str_puts(&line, ",\"note\":");
        json_escape_to(&line, a->note);
        str_printf(&line, ",\"selected\":%s}", a->selected ? "true" : "false");
    }
    str_puts(&line, "]}");

    int rc = jw_emit(jw, &line);
    str_free(&line);
    return rc;
}

int journal_action_begin(JournalWriter *jw, Action *a) {
    Str line;
    str_init(&line);
    str_puts(&line, "{\"t\":\"action_begin\",\"action_id\":");
    json_escape_utf8_to(&line, a->action_id ? a->action_id : "");
    str_puts(&line, ",\"category\":");
    json_escape_to(&line, a->category);
    str_puts(&line, ",\"src\":");
    json_escape_to(&line, a->src);
    str_puts(&line, ",\"dst\":");
    json_escape_to(&line, a->dst);
    str_printf(&line, ",\"ts\":%lld}", (long long)time(NULL));
    int rc = jw_emit(jw, &line);
    str_free(&line);
    return rc;
}

int journal_step(JournalWriter *jw, Action *a, int seq, Step *s) {
    Str line;
    str_init(&line);
    str_puts(&line, "{\"t\":\"step\",\"action_id\":");
    json_escape_utf8_to(&line, a->action_id ? a->action_id : "");
    str_printf(&line, ",\"seq\":%d,\"kind\":\"%s\"", seq, step_kind_name(s->kind));
    if (s->src) {
        str_puts(&line, ",\"src\":");
        json_escape_to(&line, s->src);
    }
    if (s->dst) {
        str_puts(&line, ",\"dst\":");
        json_escape_to(&line, s->dst);
    }
    str_printf(&line, ",\"ok\":%s,", s->ok == 1 ? "true" : "false");
    str_puts(&line, "\"error\":");
    json_escape_to(&line, s->error ? s->error : L"");
    str_printf(&line, ",\"size\":%lld,", s->size);
    str_puts(&line, "\"note\":");
    json_escape_to(&line, s->note ? s->note : L"");
    str_printf(&line, ",\"ts\":%lld}", (long long)time(NULL));
    int rc = jw_emit(jw, &line);
    str_free(&line);
    return rc;
}

int journal_action_end(JournalWriter *jw, Action *a) {
    Str line;
    str_init(&line);
    str_puts(&line, "{\"t\":\"action_end\",\"action_id\":");
    json_escape_utf8_to(&line, a->action_id ? a->action_id : "");
    str_printf(&line, ",\"status\":\"%s\",\"error\":", status_name(a->status));
    json_escape_to(&line, a->error ? a->error : L"");
    str_printf(&line, ",\"ts\":%lld}", (long long)time(NULL));
    int rc = jw_emit(jw, &line);
    str_free(&line);
    return rc;
}

int journal_footer(JournalWriter *jw, const wchar_t *status, int ok, int failed,
                   int skipped, const wchar_t *message) {
    Str line;
    str_init(&line);
    str_puts(&line, "{\"t\":\"footer\",\"plan_id\":");
    json_escape_utf8_to(&line, jw->plan_id ? jw->plan_id : "");
    str_puts(&line, ",\"status\":");
    json_escape_to(&line, status);
    str_printf(&line, ",\"ok\":%d,\"failed\":%d,\"skipped\":%d,\"message\":",
               ok, failed, skipped);
    json_escape_to(&line, message ? message : L"");
    str_printf(&line, ",\"ts\":%lld}", (long long)time(NULL));
    int rc = jw_emit(jw, &line);
    str_free(&line);
    return rc;
}

void journal_close(JournalWriter *jw) {
    if (!jw || jw->closed) return;
    if (jw->file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(jw->file);
        CloseHandle(jw->file);
        jw->file = INVALID_HANDLE_VALUE;
    }
    jw->closed = 1;
}

/* ============================================================ 读取 ====== */

static char *read_whole_file(const wchar_t *path, size_t *len_out) {
    wchar_t *lp = lc_long_path(path);
    if (!lp) return NULL;
    HANDLE h = CreateFileW(lp, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > (LONGLONG)(256ll << 20)) {
        CloseHandle(h);
        return NULL;
    }
    size_t n = (size_t)sz.QuadPart;
    char *buf = (char *)malloc(n + 1);
    if (!buf) { CloseHandle(h); return NULL; }

    size_t got = 0;
    while (got < n) {
        DWORD chunk = 0;
        DWORD want = (DWORD)((n - got) > (1u << 20) ? (1u << 20) : (n - got));
        if (!ReadFile(h, buf + got, want, &chunk, NULL) || chunk == 0) break;
        got += chunk;
    }
    CloseHandle(h);
    buf[got] = 0;
    if (len_out) *len_out = got;
    return buf;
}

static wchar_t *json_wstr(const JsonValue *obj, const char *key) {
    const char *u = json_string(json_get(obj, key), NULL);
    if (!u) return NULL;
    return lc_utf8_to_wcs(u);
}

static wchar_t *json_wstr_direct(const JsonValue *v) {
    const char *u = json_string(v, NULL);
    return u ? lc_utf8_to_wcs(u) : NULL;
}

int journal_load_doc(const wchar_t *path, JournalDoc *doc, Arena *a) {
    memset(doc, 0, sizeof(*doc));
    doc->path = arena_wcsdup(a, path);

    size_t len = 0;
    char *data = read_whole_file(path, &len);
    if (!data) return -1;

    Arena *tmp = arena_new();
    char *p = data;
    char *end = data + len;

    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        char *line = p;
        p = nl ? nl + 1 : end;

        while (line_len > 0 && (line[line_len - 1] == '\r' || line[line_len - 1] == ' '))
            line_len--;
        if (line_len == 0) continue;

        char saved = line[line_len];
        line[line_len] = 0;
        JsonValue *v = json_parse(line, tmp);
        line[line_len] = saved;
        if (!v) continue;

        doc->n_records++;
        const char *t = json_string(json_get(v, "t"), NULL);
        if (!t) continue;

        if (strcmp(t, "header") == 0) {
            const char *pid = json_string(json_get(v, "plan_id"), NULL);
            if (pid) doc->plan_id = arena_strdup(a, pid);
            doc->root = json_wstr(v, "root");
            doc->category = json_wstr(v, "category");
            const char *pol = json_string(json_get(v, "policy"), "rename");
            if (strcmp(pol, "ask") == 0) doc->policy = POLICY_ASK;
            else if (strcmp(pol, "skip") == 0) doc->policy = POLICY_SKIP;
            else if (strcmp(pol, "overwrite") == 0) doc->policy = POLICY_OVERWRITE;
            else doc->policy = POLICY_RENAME;
            doc->created = (int64_t)json_llong(json_get(v, "created"), 0);
        } else if (strcmp(t, "step") == 0) {
            if (json_bool(json_get(v, "ok"), 0)) doc->n_steps++;
        } else if (strcmp(t, "footer") == 0) {
            doc->complete = 1;
            doc->footer_status = json_wstr(v, "status");
            const wchar_t *st = doc->footer_status;
            doc->status_ok = st && (wcscmp(st, L"completed") == 0);
        }
    }

    arena_free(tmp);
    free(data);
    return 0;
}

void journal_doc_free(JournalDoc *doc) {
    if (!doc) return;
    /* 字段都从 Arena 分配，这里只清指针 */
    memset(doc, 0, sizeof(*doc));
}

int journal_load_steps(const wchar_t *path, JournalSteps *out) {
    memset(out, 0, sizeof(*out));
    out->arena = arena_new();

    size_t len = 0;
    char *data = read_whole_file(path, &len);
    if (!data) return -1;

    Arena *tmp = arena_new();
    char *p = data;
    char *end = data + len;
    size_t cap = 0;

    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        char *line = p;
        p = nl ? nl + 1 : end;

        while (line_len > 0 && (line[line_len - 1] == '\r' || line[line_len - 1] == ' '))
            line_len--;
        if (line_len == 0) continue;

        char saved = line[line_len];
        line[line_len] = 0;
        JsonValue *v = json_parse(line, tmp);
        line[line_len] = saved;
        if (!v) continue;

        const char *t = json_string(json_get(v, "t"), NULL);
        if (!t) continue;

        if (strcmp(t, "header") == 0) {
            const char *pid = json_string(json_get(v, "plan_id"), NULL);
            if (pid) out->doc.plan_id = arena_strdup(out->arena, pid);
            out->doc.root = json_wstr_direct(json_get(v, "root"));
            out->doc.created = (int64_t)json_llong(json_get(v, "created"), 0);
            continue;
        }
        if (strcmp(t, "footer") == 0) {
            out->doc.complete = 1;
            out->doc.footer_status = json_wstr_direct(json_get(v, "status"));
            continue;
        }
        if (strcmp(t, "step") != 0) continue;
        if (!json_bool(json_get(v, "ok"), 0)) continue;
        if (out->n >= 500000) break;      /* 安全上限 */

        int kind = step_kind_from(json_string(json_get(v, "kind"), NULL));
        if (kind < 0) continue;

        if (out->n + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 256;
            JournalStep *ni = (JournalStep *)realloc(out->items,
                                                     ncap * sizeof(JournalStep));
            if (!ni) break;
            out->items = ni;
            cap = ncap;
        }
        JournalStep *js = &out->items[out->n++];
        memset(js, 0, sizeof(*js));
        js->kind = (StepKind)kind;
        js->src = json_wstr_direct(json_get(v, "src"));
        js->dst = json_wstr_direct(json_get(v, "dst"));
        js->size = (long long)json_llong(json_get(v, "size"), 0);
        const char *aid = json_string(json_get(v, "action_id"), NULL);
        if (aid) js->action_id = arena_strdup(out->arena, aid);

        /* 把字符串挪到 arena 里，便于统一释放 */
        if (js->src) {
            wchar_t *a2 = arena_wcsdup(out->arena, js->src);
            free(js->src);
            js->src = a2;
        }
        if (js->dst) {
            wchar_t *a2 = arena_wcsdup(out->arena, js->dst);
            free(js->dst);
            js->dst = a2;
        }
    }

    arena_free(tmp);
    free(data);
    return 0;
}

wchar_t *journal_find_rollback_of(const wchar_t *journal_path) {
    /* 从日志里取 plan_id */
    Arena *a = arena_new();
    JournalDoc doc;
    if (journal_load_doc(journal_path, &doc, a) != 0 || !doc.plan_id) {
        arena_free(a);
        return NULL;
    }
    char *pid = doc.plan_id;

    wchar_t *dir = lc_journal_dir();
    WStr pat;
    wstr_init(&pat);
    wstr_puts(&pat, dir);
    wstr_puts(&pat, L"\\rollback-*undo_");
    wchar_t *pidw = lc_utf8_to_wcs(pid);
    wstr_puts(&pat, pidw ? pidw : L"");
    wstr_puts(&pat, L"*.jsonl");

    wchar_t *lp = lc_long_path(pat.data);
    wstr_free(&pat);
    free(pidw);

    wchar_t *found = NULL;
    if (lp) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(lp, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            WStr full;
            wstr_init(&full);
            wstr_puts(&full, dir);
            wstr_putc(&full, L'\\');
            wstr_puts(&full, fd.cFileName);
            found = wstr_steal(&full);
            FindClose(h);
        }
    }
    free(lp);
    free(dir);
    arena_free(a);
    return found;
}
