/* ==========================================================================
 * api.c —— JSON API：界面层与核心逻辑之间的唯一通道
 *
 * 所有方法都是 `fn(state, scratch_arena, params) -> JSON`。
 * 重活全部落在已通过测试的核心层（planner / executor / rollback）。
 *
 * 并发模型：
 *   · HTTP 连接线程直接调用 api_dispatch（预览、切勾选等，很快）
 *   · 真正执行（exec.run / rollback_run）丢到后台线程，界面轮询 job.status
 *   · 会话表与任务表用一把临界区锁保护
 *
 * 内存：每次调用建一个 scratch arena，返回前整体释放 —— 不必逐个 free。
 *       只有会话（Plan + 规则）用自己的长期 arena。
 * ========================================================================== */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lc.h"

/* ==================================================== 会话 ============ */

/* 批量摘要任务的负载：job.status 也要读它，所以定义必须放在最前面 */
struct BatchSum {
    wchar_t **paths;
    size_t    n;
    int       sentences;
    int       algorithm;
    char     *json;        /* 任务拼好的结果 JSON */
    size_t    json_len;
    int       ok_count;
    int       fail_count;
};

typedef struct PlanSession {
    char       *plan_id;
    wchar_t    *title;
    Plan       *plan;
    Arena      *arena;        /* == plan->arena */
    int         kind;         /* 0=回退 1=重命名 2=分类 3=去重 */
    wchar_t    *root;
    RenameRule   rrule;
    OrganizeRule orule;
    DedupRule    drule;
    DedupResult *dres;        /* 去重扫描结果（kind==3 时用于重建计划） */
    Arena      *cfg_arena;
    wchar_t    *journal_path;
    int64_t     created;
    struct PlanSession *next;
} PlanSession;

/* ==================================================== 任务 ============ */

typedef struct Job {
    char            id[32];
    int             kind;       /* 0=执行 1=回退 */
    volatile LONG   running;
    volatile LONG   finished;
    volatile LONG   cancelled;
    volatile LONG   done;
    volatile LONG   total;
    wchar_t         label[256];
    CRITICAL_SECTION label_lock;

    Plan           *plan;       /* 由 session 的 arena 拥有 */
    PlanSession    *session;

    /* 非文件系统任务（例如批量摘要）：设了 task 就走它，否则按 kind 走执行/回退 */
    int           (*task)(void *payload, struct Job *j);
    void           *payload;
    void           (*payload_free)(void *payload);

    RunResult       result;
    char           *error;

    struct Job     *next;
} Job;

struct ApiState {
    CRITICAL_SECTION lock;
    PlanSession     *sessions;
    Job             *jobs;
    volatile LONG    quit_requested;
    volatile LONG    last_beat;
    volatile LONG    ui_seen;      /* 界面是否至少调过一次接口（=页面真的加载出来了） */
    volatile LONG    api_calls;    /* 累计接口调用次数，用于诊断 */
    int64_t          started;
    wchar_t          web_dir[MAX_PATH * 2];
};

/* ==================================================== 小工具 ========== */

/* 从 JSON 取宽字符串，内存来自 scratch arena */
static wchar_t *jw(Arena *a, const JsonValue *v, const char *key) {
    const char *s = json_string(json_get(v, key), NULL);
    if (!s) return arena_wcsdup(a, L"");
    wchar_t *w = lc_utf8_to_wcs(s);
    if (!w) return arena_wcsdup(a, L"");
    wchar_t *r = arena_wcsdup(a, w);
    free(w);
    return r;
}

static int jint(const JsonValue *v, const char *key, int fallback) {
    return json_int(json_get(v, key), fallback);
}
static long long jll(const JsonValue *v, const char *key, long long fallback) {
    return json_llong(json_get(v, key), fallback);
}
static int jbool(const JsonValue *v, const char *key, int fallback) {
    return json_bool(json_get(v, key), fallback);
}

static void put_key_w(Str *s, const char *key, const wchar_t *w) {
    str_putc(s, '"'); str_puts(s, key); str_puts(s, "\":");
    json_escape_to(s, w ? w : L"");
}
static void put_key_u8(Str *s, const char *key, const char *v) {
    str_putc(s, '"'); str_puts(s, key); str_puts(s, "\":");
    json_escape_utf8_to(s, v ? v : "");
}

static const char *level_name(Level l) {
    switch (l) {
        case LEVEL_OK:    return "ok";
        case LEVEL_INFO:  return "info";
        case LEVEL_WARN:  return "warn";
        case LEVEL_ERROR: return "error";
    }
    return "ok";
}
static const wchar_t *level_text(Level l) {
    switch (l) {
        case LEVEL_OK:    return L"正常";
        case LEVEL_INFO:  return L"提示";
        case LEVEL_WARN:  return L"警告";
        case LEVEL_ERROR: return L"冲突";
    }
    return L"正常";
}

/* ============================================== 计划 -> JSON ========== */

static void plan_to_json(Plan *p, const wchar_t *title, Str *out) {
    plan_compute_stats(p);

    str_puts(out, "{\"plan_id\":");
    json_escape_utf8_to(out, p->plan_id ? p->plan_id : "");
    str_puts(out, ",\"title\":");
    json_escape_to(out, title ? title : L"预览对照表");
    str_puts(out, ",\"root\":");
    json_escape_to(out, p->root);
    str_puts(out, ",\"category\":");
    json_escape_to(out, p->category);
    str_puts(out, ",\"note\":");
    json_escape_to(out, p->note);

    str_printf(out, ",\"stats\":{\"total\":%llu,\"selected\":%llu,\"executable\":%llu,"
                    "\"blocked\":%llu,\"warned\":%llu,\"overwrite\":%llu},",
               (unsigned long long)p->st_total, (unsigned long long)p->st_selected,
               (unsigned long long)p->st_executable, (unsigned long long)p->st_blocked,
               (unsigned long long)p->st_warned, (unsigned long long)p->st_overwrite);

    str_puts(out, "\"rows\":[");
    for (size_t i = 0; i < p->n_actions; ++i) {
        Action *a = &p->actions[i];
        if (i) str_putc(out, ',');
        str_printf(out, "{\"index\":%llu,\"action_id\":",
                   (unsigned long long)(i + 1));
        json_escape_utf8_to(out, a->action_id ? a->action_id : "");
        str_puts(out, ",\"selected\":");
        json_escape_to(out, a->selected ? L"✓" : L"✗");
        str_putc(out, ',');
        put_key_u8(out, "level", level_name(action_level(a)));
        str_putc(out, ',');
        put_key_u8(out, "status", a->status == ST_DONE ? "done"
                                 : a->status == ST_FAILED ? "failed" : "pending");
        str_putc(out, ',');
        put_key_w(out, "category", a->category);
        str_putc(out, ',');
        put_key_w(out, "old", a->src);
        str_putc(out, ',');
        put_key_w(out, "new", a->dst);
        str_putc(out, ',');
        put_key_w(out, "note", a->note);

        str_puts(out, ",\"conflicts\":");
        {
            WStr conf;
            wstr_init(&conf);
            for (size_t k = 0; k < a->n_conflicts; ++k) {
                if (k) wstr_puts(&conf, L"；");
                wstr_puts(&conf, a->conflicts[k].message);
            }
            json_escape_to(out, conf.data ? conf.data : L"");
            wstr_free(&conf);
        }
        str_printf(out, ",\"executable\":%s", action_executable(a) ? "true" : "false");
        str_putc(out, ',');
        put_key_w(out, "level_text", level_text(action_level(a)));
        str_putc(out, '}');
    }
    str_puts(out, "]}");
}

/* ================================================ 会话管理 ============ */


static void session_free(PlanSession *s) {
    if (!s) return;
    rename_rule_free(&s->rrule);
    organize_rule_free(&s->orule);
    vec_free(&s->drule.include_ext);
    if (s->dres) { dedup_result_free(s->dres); free(s->dres); s->dres = NULL; }
    arena_free(s->cfg_arena);
    arena_free(s->arena);
    free(s->plan_id);
    free(s->title);
    free(s->root);
    free(s->journal_path);
    free(s);
}

static PlanSession *session_find(ApiState *st, const char *plan_id) {
    if (!plan_id) return NULL;
    for (PlanSession *s = st->sessions; s; s = s->next)
        if (s->plan_id && strcmp(s->plan_id, plan_id) == 0) return s;
    return NULL;
}

static int session_in_use(ApiState *st, PlanSession *s) {
    for (Job *j = st->jobs; j; j = j->next)
        if (j->session == s && j->running) return 1;
    return 0;
}

static void sessions_expire(ApiState *st) {
    int64_t now = lc_now_unix();
    PlanSession **pp = &st->sessions;
    while (*pp) {
        PlanSession *s = *pp;
        if (now - s->created > 3600 && !session_in_use(st, s)) {
            *pp = s->next;
            session_free(s);
        } else {
            pp = &s->next;
        }
    }
}

static PlanSession *session_new(ApiState *st, Plan *plan, const wchar_t *title) {
    PlanSession *s = (PlanSession *)calloc(1, sizeof(PlanSession));
    if (!s) return NULL;
    s->plan = plan;
    s->arena = plan->arena;
    s->title = _wcsdup(title ? title : L"预览对照表");
    s->plan_id = _strdup(plan->plan_id ? plan->plan_id : "");
    s->created = lc_now_unix();
    s->next = st->sessions;
    st->sessions = s;
    return s;
}

/* ==================================================== 任务 ============ */

static void job_free(Job *j) {
    if (!j) return;
    DeleteCriticalSection(&j->label_lock);
    run_result_free(&j->result);
    if (j->payload_free && j->payload) j->payload_free(j->payload);
    free(j->error);
    free(j);
}

/* 只保留最近的若干个已结束任务，避免长时间运行后无限堆积。
   任务表是头插的，所以"最老"的在链表末尾。 */
static void jobs_gc(ApiState *st) {
    size_t total = 0;
    for (Job *j = st->jobs; j; j = j->next) total++;
    if (total <= 40) return;

    size_t kill_n = total - 40;
    for (size_t k = 0; k < kill_n; ++k) {
        Job *oldest = NULL;
        for (Job *j = st->jobs; j; j = j->next)
            if (j->finished && !j->running) oldest = j;
        if (!oldest) break;
        Job **pp = &st->jobs;
        while (*pp && *pp != oldest) pp = &(*pp)->next;
        if (*pp) { *pp = oldest->next; job_free(oldest); }
    }
}

static Job *job_find(ApiState *st, const char *id) {
    if (!id) return NULL;
    for (Job *j = st->jobs; j; j = j->next)
        if (strcmp(j->id, id) == 0) return j;
    return NULL;
}

static void job_progress(void *ud, int done, int total, const wchar_t *label) {
    Job *j = (Job *)ud;
    InterlockedExchange(&j->done, done);
    InterlockedExchange(&j->total, total);
    if (label && label[0]) {
        EnterCriticalSection(&j->label_lock);
        wcsncpy(j->label, label, 255);
        j->label[255] = 0;
        LeaveCriticalSection(&j->label_lock);
    }
}

static int job_cancel_check(void *ud) {
    return ((Job *)ud)->cancelled != 0;
}

static DWORD WINAPI job_thread(LPVOID param) {
    Job *j = (Job *)param;

    /* 自定义任务（批量摘要之类）优先 */
    if (j->task) {
        j->task(j->payload, j);
        InterlockedExchange(&j->running, 0);
        InterlockedExchange(&j->finished, 1);
        return 0;
    }

    ExecOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.progress = job_progress;
    opt.progress_ud = j;
    opt.cancel_check = job_cancel_check;
    opt.cancel_ud = j;
    opt.is_rollback = (j->kind == 1);

    if (j->kind == 1) {
        const wchar_t *jp = j->session ? j->session->journal_path : NULL;
        if (jp && jp[0]) {
            rollback_apply(jp, &opt, &j->result);
        } else {
            run_result_init(&j->result);
            j->result.failed = 1;
            j->error = _strdup("缺少要回退的日志路径");
        }
    } else {
        executor_run(j->plan, &opt, &j->result);
    }

    InterlockedExchange(&j->running, 0);
    InterlockedExchange(&j->finished, 1);
    return 0;
}

static int job_start(ApiState *st, PlanSession *s, Plan *plan, int kind,
                     char *out_id, size_t idcch) {
    Job *j = (Job *)calloc(1, sizeof(Job));
    if (!j) return -1;
    snprintf(j->id, sizeof(j->id), "job%08x%04x",
             (unsigned)GetTickCount(), (unsigned)(rand() & 0xffff));
    j->kind = kind;
    j->plan = plan;
    j->session = s;
    InitializeCriticalSection(&j->label_lock);
    run_result_init(&j->result);

    int total = 0;
    for (size_t i = 0; i < plan->n_actions; ++i)
        if (action_executable(&plan->actions[i])) total++;
    j->total = total;

    EnterCriticalSection(&st->lock);
    j->next = st->jobs;
    st->jobs = j;
    jobs_gc(st);
    LeaveCriticalSection(&st->lock);

    snprintf(out_id, idcch, "%s", j->id);

    InterlockedExchange(&j->running, 1);
    HANDLE h = CreateThread(NULL, 0, job_thread, j, 0, NULL);
    if (!h) {
        InterlockedExchange(&j->running, 0);
        InterlockedExchange(&j->finished, 1);
        j->error = _strdup("无法创建执行线程");
        return -1;
    }
    CloseHandle(h);
    return 0;
}

/* ================================================== 错误宏 ============ */

#define FAIL(msg) do { if (err) str_puts(err, (msg)); return -1; } while (0)

static int fail_w(Str *err, const wchar_t *fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 1023, fmt, ap);
    buf[1023] = 0;
    va_end(ap);
    char *u = lc_wcs_to_utf8(buf);
    if (err) str_puts(err, u ? u : "error");
    free(u);
    return -1;
}

/* ================================================== 各接口 ============ */

typedef int (*MethodFn)(ApiState *, Arena *, const JsonValue *, Str *, Str *);

/* ---------------- app.info ---------------- */
static int m_app_info(ApiState *st, Arena *a, const JsonValue *p, Str *out, Str *err) {
    (void)st; (void)a; (void)p; (void)err;
    wchar_t *data_root = lc_data_root();
    wchar_t *jdir = lc_journal_dir();
    wchar_t *tr = lc_trash_root();

    long long trash = 0;
    if (lc_is_dir(tr)) {
        Arena *ta = arena_new();
        FileList fl;
        filelist_init(&fl, ta);
        if (lc_scan_dir(tr, 1, 0, 1, &fl) == 0)
            for (size_t i = 0; i < fl.n; ++i) trash += lc_size_of(fl.paths[i]);
        free(fl.paths);
        arena_free(ta);
    }
    wchar_t trash_buf[64];
    lc_human_size(trash, trash_buf, 64);

    int jcount = 0;
    {
        WStr pat;
        wstr_init(&pat);
        wstr_puts(&pat, jdir);
        wstr_puts(&pat, L"\\*.jsonl");
        wchar_t *lp = lc_long_path(pat.data);
        wstr_free(&pat);
        if (lp) {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW(lp, &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do { jcount++; } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            free(lp);
        }
    }

    str_puts(out, "{\"app\":\"LocalCraft\",\"version\":\"" LC_APP_VERSION_A "\"");
    str_printf(out, ",\"portable\":%s", lc_is_portable() ? "true" : "false");
    str_putc(out, ',');
    put_key_w(out, "data_root", data_root);
    str_putc(out, ',');
    put_key_w(out, "journal_dir", jdir);
    str_putc(out, ',');
    put_key_w(out, "trash", trash_buf);
    str_printf(out, ",\"journal_count\":%d,\"platform\":\"win32\"}", jcount);

    free(data_root); free(jdir); free(tr);
    return 0;
}

/* ---------------- app.quit / app.beat ---------------- */
static int m_app_quit(ApiState *st, Arena *a, const JsonValue *p, Str *out, Str *err) {
    (void)a; (void)p; (void)err;
    InterlockedExchange(&st->quit_requested, 1);
    str_puts(out, "{\"quitting\":true}");
    return 0;
}

static int m_app_beat(ApiState *st, Arena *a, const JsonValue *p, Str *out, Str *err) {
    (void)a; (void)p; (void)err;
    InterlockedExchange(&st->last_beat, (LONG)lc_now_unix());
    int busy = 0;
    EnterCriticalSection(&st->lock);
    for (Job *j = st->jobs; j; j = j->next) if (j->running) { busy = 1; break; }
    LeaveCriticalSection(&st->lock);
    str_printf(out, "{\"ok\":true,\"busy\":%s}", busy ? "true" : "false");
    return 0;
}

/* ---------------- app.engines ---------------- */
static int m_app_engines(ApiState *st, Arena *a, const JsonValue *p, Str *out, Str *err) {
    (void)st; (void)p; (void)err;
    wchar_t *tools = lc_tools_dir();

    static const struct { const char *key, *name, *prov, *hint; } E[] = {
        {"pandoc", "Pandoc", "Markdown / Word / HTML / EPUB 高质量互转",
         "未接入。把 pandoc.exe 放进程序目录的 tools\\ 即可启用（约 30 MB）"},
        {"soffice", "LibreOffice", "Word / PPT / Excel / PDF 互转，版式保真度最高",
         "未接入。把 LibreOffice 的 program\\ 目录放进 tools\\soffice\\ 即可启用"},
        {"ffmpeg", "FFmpeg", "音视频格式转换与提取",
         "未接入。把 ffmpeg.exe 放进 tools\\ 即可启用（约 80 MB）"},
        {"libraw", "LibRaw / dcraw", "相机 RAW（CR2/CR3/NEF/ARW/RAF/RW2…）解码",
         "未接入。建议后续版本集成 dcraw.exe 或 rawpy"},
    };
    str_puts(out, "{\"engines\":[");
    for (size_t i = 0; i < sizeof(E)/sizeof(E[0]); ++i) {
        if (i) str_putc(out, ',');
        str_puts(out, "{\"key\":"); json_escape_utf8_to(out, E[i].key);
        str_puts(out, ",\"name\":"); json_escape_utf8_to(out, E[i].name);
        str_puts(out, ",\"provides\":"); json_escape_utf8_to(out, E[i].prov);
        str_puts(out, ",\"hint\":"); json_escape_utf8_to(out, E[i].hint);
        str_puts(out, ",\"found\":false,\"path\":\"\",\"version\":\"\"}");
    }
    str_puts(out, "],\"packages\":[],\"tools_dir\":");
    json_escape_to(out, tools);
    str_puts(out, "}");
    free(tools);
    (void)a;
    return 0;
}

/* ---------------- fs.* ---------------- */
static int m_pick_dir(ApiState *st, Arena *a, const JsonValue *p, Str *out, Str *err) {
    (void)st; (void)err;
    wchar_t *initial = jw(a, p, "initial");
    wchar_t picked[MAX_PATH * 2];
    lc_pick_folder(initial[0] ? initial : NULL, picked, MAX_PATH * 2);
    str_puts(out, "{\"path\":"); json_escape_to(out, picked); str_puts(out, "}");
    return 0;
}

static int m_pick_file(ApiState *st, Arena *a, const JsonValue *p, Str *out, Str *err) {
    (void)st; (void)err;
    wchar_t *initial = jw(a, p, "initial");
    wchar_t picked[MAX_PATH * 2];
    lc_pick_open_file(initial[0] ? initial : NULL, picked, MAX_PATH * 2);
    str_puts(out, "{\"path\":"); json_escape_to(out, picked); str_puts(out, "}");
    return 0;
}

static int m_pick_save(ApiState *st, Arena *a, const JsonValue *p, Str *out, Str *err) {
    (void)st; (void)err;
    wchar_t *initial = jw(a, p, "initial");
    wchar_t *fname = jw(a, p, "filename");
    wchar_t picked[MAX_PATH * 2];
    lc_pick_save_file(initial[0] ? initial : NULL,
                      fname[0] ? fname : L"localcraft-preview.csv",
                      picked, MAX_PATH * 2);
    str_puts(out, "{\"path\":"); json_escape_to(out, picked); str_puts(out, "}");
    return 0;
}

static int m_reveal(ApiState *st, Arena *a, const JsonValue *p, Str *out, Str *err) {
    (void)st;
    wchar_t *path = jw(a, p, "path");
    if (!path[0]) FAIL("缺少路径");
    wchar_t *target = lc_is_dir(path) ? _wcsdup(path) : lc_dirname(path);
    if (!target || !lc_is_dir(target)) {
        free(target);
        FAIL("目录不存在");
    }
    if (lc_shell_open(target) != 0) {
        free(target);
        FAIL("无法打开该目录");
    }
    str_puts(out, "{\"opened\":"); json_escape_to(out, target); str_puts(out, "}");
    free(target);
    return 0;
}

/* ---------------- 规则解析 ---------------- */

static void parse_wvec(Arena *a, const JsonValue *v, Vec *out) {
    if (!v || !json_is_array(v)) return;
    size_t n = json_array_len(v);
    for (size_t i = 0; i < n; ++i) {
        const char *s = json_string(json_at(v, i), NULL);
        if (!s || !s[0]) continue;
        wchar_t *w = lc_utf8_to_wcs(s);
        if (!w) continue;
        vec_push(out, arena_wcsdup(a, w));
        free(w);
    }
}

static void parse_rename_rule(Arena *a, const JsonValue *p, RenameRule *r) {
    rename_rule_init(r, a);
    r->find = jw(a, p, "find");
    r->replace = jw(a, p, "replace");
    r->use_regex = jbool(p, "regex", 0);
    r->ignore_case = jbool(p, "ignore_case", 0);
    r->target = jw(a, p, "target");
    if (!r->target[0]) r->target = arena_wcsdup(a, L"stem");
    r->template_ = jw(a, p, "template");
    r->prefix = jw(a, p, "prefix");
    r->suffix = jw(a, p, "suffix");
    r->number = jbool(p, "number", 0);
    r->number_pos = jw(a, p, "number_pos");
    if (!r->number_pos[0]) r->number_pos = arena_wcsdup(a, L"suffix");
    r->number_start = jint(p, "number_start", 1);
    r->number_step = jint(p, "number_step", 1);
    r->number_width = jint(p, "number_width", 3);
    r->number_sep = jw(a, p, "number_sep");
    r->case_mode = jw(a, p, "case");
    if (!r->case_mode[0]) r->case_mode = arena_wcsdup(a, L"keep");
    r->ext_case = jw(a, p, "ext_case");
    if (!r->ext_case[0]) r->ext_case = arena_wcsdup(a, L"keep");
    parse_wvec(a, json_get(p, "include_ext"), &r->include_ext);
    parse_wvec(a, json_get(p, "exclude_ext"), &r->exclude_ext);
    r->recursive = jbool(p, "recursive", 0);
    r->skip_hidden = 1;
    r->sort = jw(a, p, "sort");
    if (!r->sort[0]) r->sort = arena_wcsdup(a, L"name");
    r->reverse = jbool(p, "reverse", 0);
    r->limit = jint(p, "limit", 0);
    r->files_only = 1;
}

static void parse_organize_rule(Arena *a, const JsonValue *p, OrganizeRule *r) {
    organize_rule_init(r, a);
    wchar_t *tpl = jw(a, p, "template");
    r->template_ = tpl[0] ? tpl : arena_wcsdup(a, L"{category}");
    wchar_t *mode = jw(a, p, "mode");
    r->mode = (mode[0] && lc_wcs_ieq(mode, L"copy")) ? arena_wcsdup(a, L"copy")
                                                     : arena_wcsdup(a, L"move");
    r->recursive = jbool(p, "recursive", 1);
    r->skip_hidden = 1;
    parse_wvec(a, json_get(p, "include_ext"), &r->include_ext);
    parse_wvec(a, json_get(p, "exclude_ext"), &r->exclude_ext);
    r->min_size = (long long)jll(p, "min_size", 0);
    r->max_size = (long long)jll(p, "max_size", 0);
    r->older_than_days = jint(p, "older_than", 0);
    r->newer_than_days = jint(p, "newer_than", 0);
    r->skip_already_sorted = 1;
    r->limit = jint(p, "limit", 0);
    r->files_only = 1;
}

static Policy policy_of(const JsonValue *p, Policy fallback) {
    const char *s = json_string(json_get(p, "policy"), NULL);
    if (!s) return fallback;
    if (strcmp(s, "ask") == 0) return POLICY_ASK;
    if (strcmp(s, "skip") == 0) return POLICY_SKIP;
    if (strcmp(s, "overwrite") == 0) return POLICY_OVERWRITE;
    if (strcmp(s, "rename") == 0) return POLICY_RENAME;
    return fallback;
}

/* ---------------- rename.preview ---------------- */
static int m_rename_preview(ApiState *st, Arena *a, const JsonValue *p, Str *out, Str *err) {
    wchar_t *dir = jw(a, p, "dir");
    if (!dir[0]) FAIL("请先选择文件夹。");
    if (!lc_is_dir(dir)) return fail_w(err, L"文件夹不存在：%ls", dir);

    Arena *ca = arena_new();
    RenameRule rr;
    parse_rename_rule(ca, p, &rr);

    if (!(rr.template_ && rr.template_[0]) &&
        !(rr.find && rr.find[0]) &&
        !(rr.prefix && rr.prefix[0]) &&
        !(rr.suffix && rr.suffix[0]) && !rr.number &&
        wcscmp(rr.case_mode, L"keep") == 0 && wcscmp(rr.ext_case, L"keep") == 0) {
        rename_rule_free(&rr);
        arena_free(ca);
        FAIL("请至少填写模板、查找替换、前后缀或编号中的一项。");
    }

    Arena *pa = arena_new();
    Plan *plan = rename_build_plan(pa, dir, &rr, policy_of(p, POLICY_RENAME), NULL);
    if (!plan) {
        rename_rule_free(&rr); arena_free(ca); arena_free(pa);
        FAIL("无法读取目录");
    }

    WStr title;
    wstr_init(&title);
    wstr_puts(&title, L"重命名预览 — ");
    rename_rule_describe(&rr, &title, pa);

    EnterCriticalSection(&st->lock);
    sessions_expire(st);
    PlanSession *s = session_new(st, plan, title.data);
    if (s) {
        s->kind = 1;
        s->root = _wcsdup(dir);
        s->cfg_arena = ca;
        s->rrule = rr;
    } else {
        rename_rule_free(&rr);
        arena_free(ca);
    }
    LeaveCriticalSection(&st->lock);
    wstr_free(&title);

    if (!s) FAIL("内存不足");
    plan_to_json(plan, s->title, out);
    (void)a;
    return 0;
}

/* ---------------- organize.preview ---------------- */
static int m_organize_preview(ApiState *st, Arena *a, const JsonValue *p, Str *out, Str *err) {
    wchar_t *dir = jw(a, p, "dir");
    if (!dir[0]) FAIL("请先选择文件夹。");
    if (!lc_is_dir(dir)) return fail_w(err, L"文件夹不存在：%ls", dir);

    Arena *ca = arena_new();
    OrganizeRule orule;
    parse_organize_rule(ca, p, &orule);

    Arena *pa = arena_new();
    Plan *plan = organize_build_plan(pa, dir, &orule, policy_of(p, POLICY_RENAME), NULL);
    if (!plan) {
        organize_rule_free(&orule); arena_free(ca); arena_free(pa);
        FAIL("无法读取目录");
    }

    WStr title;
    wstr_init(&title);
    wstr_printf(&title, L"整理分类预览 — %ls", orule.template_);

    EnterCriticalSection(&st->lock);
    sessions_expire(st);
    PlanSession *s = session_new(st, plan, title.data);
    if (s) {
        s->kind = 2;
        s->root = _wcsdup(dir);
        s->cfg_arena = ca;
        s->orule = orule;
    } else {
        organize_rule_free(&orule);
        arena_free(ca);
    }
    LeaveCriticalSection(&st->lock);
    wstr_free(&title);

    if (!s) FAIL("内存不足");
    plan_to_json(plan, s->title, out);
    (void)a;
    return 0;
}

/* ---------------- organize.scan ---------------- */
static int m_organize_scan(ApiState *st, Arena *a, const JsonValue *p, Str *out, Str *err) {
    (void)st;
    wchar_t *dir = jw(a, p, "dir");
    if (!dir[0]) FAIL("请先选择文件夹。");
    if (!lc_is_dir(dir)) return fail_w(err, L"文件夹不存在：%ls", dir);

    int recursive = jbool(p, "recursive", 1);
    CategoryStat *stats = NULL;
    size_t n = 0;
    int total_n = 0;
    long long total_s = 0;

    if (organize_category_stats(dir, recursive, a, &stats, &n, &total_n, &total_s) != 0)
        FAIL("扫描失败");

    str_puts(out, "{\"rows\":[");
    for (size_t i = 0; i < n; ++i) {
        if (i) str_putc(out, ',');
        str_putc(out, '{');
        put_key_w(out, "category", stats[i].category);
        str_printf(out, ",\"count\":%d,\"size\":%lld,", stats[i].count,
                   (long long)stats[i].size);
        {
            wchar_t buf[64];
            lc_human_size(stats[i].size, buf, 64);
            put_key_w(out, "size_text", buf);
        }
        str_printf(out, ",\"ratio\":%.4f}",
                   total_n ? (double)stats[i].count / (double)total_n : 0.0);
    }
    str_printf(out, "],\"total_count\":%d,\"total_size\":%lld,\"total_size_text\":",
               total_n, total_s);
    {
        wchar_t buf[64];
        lc_human_size(total_s, buf, 64);
        json_escape_to(out, buf);
    }
    str_putc(out, '}');

    for (size_t i = 0; i < n; ++i) free(stats[i].category);
    free(stats);
    return 0;
}

/* ---------------- plan.* ---------------- */

static PlanSession *require_session(ApiState *st, const JsonValue *p, Str *err) {
    const char *pid = json_string(json_get(p, "plan_id"), NULL);
    PlanSession *s = session_find(st, pid);
    if (!s && err) str_puts(err, "预览已过期，请重新生成预览。");
    return s;
}

#define REQUIRE_SESSION()                                                    \
    PlanSession *s;                                                          \
    do {                                                                     \
        EnterCriticalSection(&st->lock);                                     \
        s = require_session(st, p, err);                                     \
        LeaveCriticalSection(&st->lock);                                     \
        if (!s) return -1;                                                    \
    } while (0)

static int m_plan_get(ApiState *st, Arena *a, const JsonValue *p, Str *out, Str *err) {
    (void)a;
    REQUIRE_SESSION();
    plan_to_json(s->plan, s->title, out);
    return 0;
}

static int m_plan_toggle(ApiState *st, Arena *a, const JsonValue *p, Str *out, Str *err) {
    (void)a;
    REQUIRE_SESSION();
    int idx = jint(p, "index", -1);
    if (idx < 0 || (size_t)idx >= s->plan->n_actions) FAIL("找不到该行。");
    Action *act = &s->plan->actions[idx];
    if (action_blocked(act))
        FAIL("该行存在必须解决的冲突，无法勾选。");
    act->selected = !act->selected;
    plan_to_json(s->plan, s->title, out);
    return 0;
}

static int m_plan_set_all(ApiState *st, Arena *a, const JsonValue *p, Str *out, Str *err) {
    (void)a;
    REQUIRE_SESSION();
    int sel = jbool(p, "selected", 1);
    for (size_t i = 0; i < s->plan->n_actions; ++i)
        if (!action_blocked(&s->plan->actions[i])) s->plan->actions[i].selected = sel;
    plan_to_json(s->plan, s->title, out);
    return 0;
}

static int m_plan_set_policy(ApiState *st, Arena *a, const JsonValue *p, Str *out, Str *err) {
    (void)a;
    REQUIRE_SESSION();
    Policy pol = policy_of(p, s->plan->policy);

    if (s->kind == 0) {          /* 回退计划不参与策略重算 */
        plan_to_json(s->plan, s->title, out);
        return 0;
    }

    Arena *pa = arena_new();
    Plan *np = NULL;
    if (s->kind == 1)
        np = rename_build_plan(pa, s->root, &s->rrule, pol, NULL);
    else if (s->kind == 3)
        np = s->dres ? dedup_build_plan(pa, s->root, s->dres, &s->drule, pol,
                                        NULL)
                     : NULL;
    else
        np = organize_build_plan(pa, s->root, &s->orule, pol, NULL);
    if (!np) { arena_free(pa); FAIL("重新生成预览失败"); }

    EnterCriticalSection(&st->lock);
    sessions_expire(st);
    PlanSession *ns = session_new(st, np, s->title);
    if (!ns) {
        LeaveCriticalSection(&st->lock);
        arena_free(pa);
        FAIL("内存不足");
    }
    ns->kind = s->kind;
    ns->root = s->root ? _wcsdup(s->root) : NULL;
    ns->cfg_arena = s->cfg_arena;
    s->cfg_arena = NULL;         /* 所有权转移 */
    ns->rrule = s->rrule;
    ns->orule = s->orule;
    ns->drule = s->drule;
    ns->dres  = s->dres;          /* 所有权转移 */
    s->dres   = NULL;
    s->rrule.include_ext.len = s->rrule.include_ext.cap = 0;
    s->rrule.exclude_ext.len = s->rrule.exclude_ext.cap = 0;
    s->orule.include_ext.len = s->orule.include_ext.cap = 0;
    s->orule.exclude_ext.len = s->orule.exclude_ext.cap = 0;
    s->drule.include_ext.len = s->drule.include_ext.cap = 0;
    LeaveCriticalSection(&st->lock);

    plan_to_json(np, ns->title, out);
    return 0;
}

static int m_plan_keep_selected(ApiState *st, Arena *a, const JsonValue *p, Str *out, Str *err) {
    (void)a;
    REQUIRE_SESSION();
    size_t w = 0;
    for (size_t i = 0; i < s->plan->n_actions; ++i)
        if (s->plan->actions[i].selected)
            s->plan->actions[w++] = s->plan->actions[i];
    s->plan->n_actions = w;
    plan_to_json(s->plan, s->title, out);
    return 0;
}

static int m_plan_close(ApiState *st, Arena *a, const JsonValue *p, Str *out, Str *err) {
    (void)a; (void)err;
    const char *pid = json_string(json_get(p, "plan_id"), NULL);
    EnterCriticalSection(&st->lock);
    PlanSession **pp = &st->sessions;
    while (*pp) {
        if ((*pp)->plan_id && pid && strcmp((*pp)->plan_id, pid) == 0) {
            PlanSession *dead = *pp;
            if (!session_in_use(st, dead)) {
                *pp = dead->next;
                session_free(dead);
            }
            break;
        }
        pp = &(*pp)->next;
    }
    LeaveCriticalSection(&st->lock);
    str_puts(out, "{\"closed\":true}");
    return 0;
}

static int m_plan_export_csv(ApiState *st, Arena *a, const JsonValue *p, Str *out, Str *err) {
    REQUIRE_SESSION();
    wchar_t *path = jw(a, p, "path");
    if (!path[0]) {
        wchar_t fname[128];
        swprintf(fname, 128, L"localcraft-%hs.csv", s->plan->plan_id);
        wchar_t picked[MAX_PATH * 2];
        lc_pick_save_file(NULL, fname, picked, MAX_PATH * 2);
        if (!picked[0]) { str_puts(out, "{\"cancelled\":true}"); return 0; }
        path = arena_wcsdup(a, picked);
    }

    wchar_t *lp = lc_long_path(path);
    HANDLE h = CreateFileW(lp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) FAIL("无法写入该文件");

    Str csv;
    str_init(&csv);
    str_puts(&csv, "\xEF\xBB\xBF");      /* UTF-8 BOM，Excel 打开不乱码 */
    str_puts(&csv, "序号,勾选,级别,类型,旧状态,新状态,备注,冲突说明\r\n");
    for (size_t i = 0; i < s->plan->n_actions; ++i) {
        Action *act = &s->plan->actions[i];
        char buf[64];
        snprintf(buf, sizeof(buf), "%llu", (unsigned long long)(i + 1));
        str_puts(&csv, buf);
        str_printf(&csv, ",%s,%s,", act->selected ? "是" : "否",
                   level_name(action_level(act)));
        {
            char *u = lc_wcs_to_utf8(act->category);
            str_puts(&csv, u ? u : ""); free(u);
        }
        str_putc(&csv, ',');
        { char *u = lc_wcs_to_utf8(act->src); str_puts(&csv, u ? u : ""); free(u); }
        str_putc(&csv, ',');
        { char *u = lc_wcs_to_utf8(act->dst); str_puts(&csv, u ? u : ""); free(u); }
        str_putc(&csv, ',');
        { char *u = lc_wcs_to_utf8(act->note); str_puts(&csv, u ? u : ""); free(u); }
        str_putc(&csv, ',');
        {
            WStr conf;
            wstr_init(&conf);
            for (size_t k = 0; k < act->n_conflicts; ++k) {
                if (k) wstr_puts(&conf, L"；");
                wstr_puts(&conf, act->conflicts[k].message);
            }
            char *u = lc_wcs_to_utf8(conf.data ? conf.data : L"");
            str_puts(&csv, u ? u : ""); free(u);
            wstr_free(&conf);
        }
        str_puts(&csv, "\r\n");
    }
    DWORD written = 0;
    WriteFile(h, csv.data, (DWORD)csv.len, &written, NULL);
    CloseHandle(h);
    str_free(&csv);

    str_puts(out, "{\"path\":"); json_escape_to(out, path); str_puts(out, "}");
    return 0;
}

/* ---------------- exec.* ---------------- */

static int m_exec_dry(ApiState *st, Arena *a, const JsonValue *p, Str *out, Str *err) {
    (void)a;
    REQUIRE_SESSION();
    RunResult r;
    ExecOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.dry_run = 1;
    executor_run(s->plan, &opt, &r);

    str_printf(out, "{\"ok\":%d,\"failed\":%d,\"skipped\":%d,\"total\":%d,\"problems\":[",
               r.ok, r.failed, r.skipped, r.total);
    size_t lim = r.n_errors > 50 ? 50 : r.n_errors;
    for (size_t i = 0; i < lim; ++i) {
        if (i) str_putc(out, ',');
        json_escape_to(out, r.errors[i]);
    }
    str_printf(out, "],\"message\":\"成功 %d，跳过 %d（共 %d）\"}",
               r.ok, r.skipped, r.total);
    run_result_free(&r);
    return 0;
}

static int m_exec_run(ApiState *st, Arena *a, const JsonValue *p, Str *out, Str *err) {
    (void)a;
    REQUIRE_SESSION();
    if (s->plan->st_executable == 0) {
        /* 统计可能是旧的，重算一次保证准确 */
        plan_compute_stats(s->plan);
    }
    if (s->plan->st_executable == 0)
        FAIL("没有可执行的项：所有行要么未勾选，要么存在冲突。");

    char id[32];
    if (job_start(st, s, s->plan, 0, id, sizeof(id)) != 0) FAIL("无法开始执行");
    str_printf(out, "{\"job_id\":\"%s\"}", id);
    return 0;
}

/* ---------------- history.* ---------------- */

typedef struct { wchar_t *path; int64_t mtime; } JEntry;

static int cmp_jentry(const void *a, const void *b) {
    const JEntry *x = (const JEntry *)a, *y = (const JEntry *)b;
    if (x->mtime < y->mtime) return 1;
    if (x->mtime > y->mtime) return -1;
    return 0;
}

static int list_journals(Arena *a, JEntry **out, size_t *n_out) {
    wchar_t *dir = lc_journal_dir();
    WStr pat;
    wstr_init(&pat);
    wstr_puts(&pat, dir);
    wstr_puts(&pat, L"\\*.jsonl");
    free(dir);

    wchar_t *lp = lc_long_path(pat.data);
    wstr_free(&pat);
    if (!lp) return -1;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(lp, &fd);
    if (h == INVALID_HANDLE_VALUE) { free(lp); *out = NULL; *n_out = 0; return 0; }

    JEntry *arr = NULL;
    size_t n = 0, cap = 0;
    wchar_t *base = lc_dirname(lp);
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (n + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 32;
            JEntry *na = (JEntry *)realloc(arr, ncap * sizeof(JEntry));
            if (!na) break;
            arr = na;
            cap = ncap;
        }
        WStr full;
        wstr_init(&full);
        wstr_puts(&full, base);
        wstr_putc(&full, L'\\');
        wstr_puts(&full, fd.cFileName);
        arr[n].path = arena_wcsdup(a, full.data);
        wstr_free(&full);
        ULARGE_INTEGER u;
        u.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        u.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        arr[n].mtime = (int64_t)(u.QuadPart / 10000000ULL);
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    free(base);
    free(lp);

    if (n > 1) qsort(arr, n, sizeof(JEntry), cmp_jentry);
    *out = arr;
    *n_out = n;
    return 0;
}

static int m_history_list(ApiState *st, Arena *a, const JsonValue *p, Str *out, Str *err) {
    (void)st; (void)err;
    int limit = jint(p, "limit", 200);

    JEntry *arr = NULL;
    size_t n = 0;
    if (list_journals(a, &arr, &n) != 0) FAIL("无法读取日志目录");

    str_puts(out, "{\"rows\":[");
    int emitted = 0;
    for (size_t i = 0; i < n && emitted < limit; ++i) {
        JournalDoc doc;
        if (journal_load_doc(arr[i].path, &doc, a) != 0) continue;

        const wchar_t *fname = lc_basename(arr[i].path);
        int is_rollback = lc_wcs_starts_ci(fname, L"rollback-");

        if (emitted) str_putc(out, ',');
        emitted++;
        str_putc(out, '{');

        char *pathu = lc_wcs_to_utf8(arr[i].path);
        put_key_u8(out, "path", pathu ? pathu : "");
        free(pathu);

        str_putc(out, ',');
        put_key_w(out, "name", fname);
        str_putc(out, ',');
        put_key_u8(out, "plan_id", doc.plan_id ? doc.plan_id : "");
        str_putc(out, ',');
        put_key_u8(out, "kind", is_rollback ? "rollback" : "execute");
        str_putc(out, ',');
        put_key_w(out, "category", doc.category ? doc.category : L"");
        str_putc(out, ',');
        put_key_w(out, "root", doc.root ? doc.root : L"");

        wchar_t whenbuf[64];
        lc_format_time(doc.created, whenbuf, 64);
        str_putc(out, ',');
        put_key_w(out, "when", whenbuf);

        str_putc(out, ',');
        put_key_w(out, "status", doc.complete
                    ? (doc.footer_status ? doc.footer_status : L"unknown")
                    : L"incomplete");
        str_printf(out, ",\"steps\":%llu", (unsigned long long)doc.n_steps);

        {
            wchar_t szbuf[64];
            WIN32_FILE_ATTRIBUTE_DATA fad;
            long long sz = 0;
            wchar_t *lp = lc_long_path(arr[i].path);
            if (lp && GetFileAttributesExW(lp, GetFileExInfoStandard, &fad))
                sz = ((long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
            free(lp);
            lc_human_size(sz, szbuf, 64);
            str_putc(out, ',');
            put_key_w(out, "size_text", szbuf);
        }

        int undone = 0;
        if (!is_rollback && doc.plan_id) {
            for (size_t k = 0; k < n; ++k) {
                const wchar_t *bn = lc_basename(arr[k].path);
                if (!lc_wcs_starts_ci(bn, L"rollback-")) continue;
                char *ne = lc_wcs_to_utf8(bn);
                if (ne && doc.plan_id && strstr(ne, doc.plan_id)) undone = 1;
                free(ne);
                if (undone) break;
            }
        }
        str_printf(out, ",\"undone\":%s}", undone ? "true" : "false");
    }
    str_puts(out, "],\"storage\":\"\"}");
    free(arr);
    return 0;
}

static int m_history_detail(ApiState *st, Arena *a, const JsonValue *p, Str *out, Str *err) {
    (void)st;
    wchar_t *path = jw(a, p, "path");
    if (!path[0]) FAIL("缺少日志路径。");
    if (!lc_is_file(path)) return fail_w(err, L"文件不存在：%ls", path);

    JournalSteps steps;
    if (journal_load_steps(path, &steps) != 0) FAIL("无法读取日志");

    str_puts(out, "{\"steps\":[");
    size_t lim = steps.n > 400 ? 400 : steps.n;
    for (size_t i = 0; i < lim; ++i) {
        JournalStep *js = &steps.items[i];
        if (i) str_putc(out, ',');
        str_putc(out, '{');
        const char *k = js->kind == STEP_MOVE ? "move"
                       : js->kind == STEP_COPY ? "copy"
                       : js->kind == STEP_MKDIR ? "mkdir"
                       : js->kind == STEP_RMDIR ? "rmdir" : "create";
        put_key_u8(out, "kind", k);
        str_putc(out, ',');
        put_key_w(out, "src", js->src ? js->src : L"");
        str_putc(out, ',');
        put_key_w(out, "dst", js->dst ? js->dst : L"");
        str_printf(out, ",\"size\":%lld}", js->size);
    }
    str_printf(out, "],\"total\":%llu,\"header\":{\"plan_id\":",
               (unsigned long long)steps.n);
    json_escape_utf8_to(out, steps.doc.plan_id ? steps.doc.plan_id : "");
    str_puts(out, ",\"root\":");
    json_escape_to(out, steps.doc.root ? steps.doc.root : L"");
    str_puts(out, "},\"undone_of\":\"\"}");

    arena_free(steps.arena);
    return 0;
}

static int m_history_rollback_preview(ApiState *st, Arena *a,
                                      const JsonValue *p, Str *out, Str *err) {
    wchar_t *path = jw(a, p, "path");
    if (!path[0]) FAIL("缺少日志路径。");
    if (!lc_is_file(path)) return fail_w(err, L"文件不存在：%ls", path);

    Arena *pa = arena_new();
    RollbackPreview rp;
    if (rollback_preview(path, pa, &rp) != 0) {
        arena_free(pa);
        FAIL("无法读取日志");
    }

    WStr title;
    wstr_init(&title);
    wstr_printf(&title, L"回退预览 — %ls", lc_basename(path));

    EnterCriticalSection(&st->lock);
    sessions_expire(st);
    PlanSession *s = session_new(st, rp.plan, title.data);
    if (s) {
        s->kind = 0;
        s->journal_path = _wcsdup(path);
    }
    LeaveCriticalSection(&st->lock);
    wstr_free(&title);

    if (!s) { arena_free(pa); FAIL("内存不足"); }
    plan_to_json(rp.plan, s->title, out);
    (void)a;
    return 0;
}

static int m_history_rollback_run(ApiState *st, Arena *a,
                                  const JsonValue *p, Str *out, Str *err) {
    (void)a;
    REQUIRE_SESSION();
    if (!s->journal_path || !s->journal_path[0]) FAIL("缺少要回退的日志。");
    if (!lc_is_file(s->journal_path)) FAIL("日志文件不存在。");

    Arena *pa = arena_new();
    RollbackPreview rp;
    if (rollback_preview(s->journal_path, pa, &rp) != 0) {
        arena_free(pa);
        FAIL("无法读取日志");
    }
    int exec_n = 0;
    for (size_t i = 0; i < rp.plan->n_actions; ++i)
        if (action_executable(&rp.plan->actions[i])) exec_n++;
    arena_free(pa);

    if (exec_n == 0)
        FAIL("没有可回退的步骤（文件可能已被移动，或原位置已被占用）。");

    char id[32];
    if (job_start(st, s, s->plan, 1, id, sizeof(id)) != 0) FAIL("无法开始回退");
    str_printf(out, "{\"job_id\":\"%s\"}", id);
    return 0;
}

static int m_purge_journals(ApiState *st, Arena *a, const JsonValue *p,
                            Str *out, Str *err) {
    (void)st; (void)err;
    int days = jint(p, "days", 90);
    int64_t cutoff = lc_now_unix() - (int64_t)days * 86400;

    JEntry *arr = NULL;
    size_t n = 0;
    if (list_journals(a, &arr, &n) != 0) FAIL("无法读取日志目录");

    int removed = 0;
    for (size_t i = 0; i < n; ++i) {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        wchar_t *lp = lc_long_path(arr[i].path);
        int64_t mt = 0;
        if (lp && GetFileAttributesExW(lp, GetFileExInfoStandard, &fad)) {
            ULARGE_INTEGER u;
            u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
            u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
            mt = (int64_t)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
        }
        free(lp);
        if (mt && mt < cutoff) {
            wchar_t *lp2 = lc_long_path(arr[i].path);
            if (lp2) { if (DeleteFileW(lp2)) removed++; free(lp2); }
        }
    }
    free(arr);
    str_printf(out, "{\"removed\":%d,\"days\":%d}", removed, days);
    return 0;
}

static int remove_tree(const wchar_t *dir) {
    int count = 0;
    WStr pat;
    wstr_init(&pat);
    wstr_puts(&pat, dir);
    if (pat.len && !lc_is_sep(pat.data[pat.len - 1])) wstr_putc(&pat, L'\\');
    wstr_puts(&pat, L"*");
    wchar_t *lp = lc_long_path(pat.data);
    wstr_free(&pat);
    if (!lp) return 0;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(lp, &fd);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        WStr full;
        wstr_init(&full);
        wstr_puts(&full, dir);
        if (!lc_is_sep(full.data[full.len - 1])) wstr_putc(&full, L'\\');
        wstr_puts(&full, fd.cFileName);
        wchar_t *lp2 = lc_long_path(full.data);
        if (lp2) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                count += remove_tree(full.data);
                RemoveDirectoryW(lp2);
                count++;
            } else {
                if (DeleteFileW(lp2)) count++;
            }
            free(lp2);
        }
        wstr_free(&full);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return count;
}

static int m_purge_trash(ApiState *st, Arena *a, const JsonValue *p,
                         Str *out, Str *err) {
    (void)st; (void)a; (void)p; (void)err;
    wchar_t *tr = lc_trash_root();
    long long before = 0;
    if (lc_is_dir(tr)) {
        Arena *ta = arena_new();
        FileList fl;
        filelist_init(&fl, ta);
        if (lc_scan_dir(tr, 1, 0, 1, &fl) == 0)
            for (size_t i = 0; i < fl.n; ++i) before += lc_size_of(fl.paths[i]);
        free(fl.paths);
        arena_free(ta);
    }
    int removed = lc_is_dir(tr) ? remove_tree(tr) : 0;
    wchar_t freed[64];
    lc_human_size(before, freed, 64);

    str_printf(out, "{\"removed\":%d,\"freed\":", removed);
    json_escape_to(out, freed);
    str_putc(out, '}');
    free(tr);
    return 0;
}

/* ---------------- job.* ---------------- */

static int m_job_status(ApiState *st, Arena *a, const JsonValue *p, Str *out, Str *err) {
    (void)a;
    const char *id = json_string(json_get(p, "job_id"), NULL);
    EnterCriticalSection(&st->lock);
    Job *j = job_find(st, id);
    LeaveCriticalSection(&st->lock);
    if (!j) FAIL("任务不存在或已结束。");

    int done = j->done, total = j->total;
    int running = j->running, finished = j->finished;
    wchar_t label[256];
    EnterCriticalSection(&j->label_lock);
    wcsncpy(label, j->label, 255); label[255] = 0;
    LeaveCriticalSection(&j->label_lock);

    str_puts(out, "{\"job_id\":");
    json_escape_utf8_to(out, j->id);
    str_printf(out, ",\"kind\":\"%s\",\"running\":%s,\"finished\":%s,\"cancelled\":%s",
               j->kind == 1 ? "rollback" : "execute",
               running ? "true" : "false",
               finished ? "true" : "false",
               j->cancelled ? "true" : "false");
    str_printf(out, ",\"done\":%d,\"total\":%d,\"label\":", done, total);
    json_escape_to(out, label);

    if (j->error) {
        str_puts(out, ",\"error\":");
        json_escape_utf8_to(out, j->error);
    } else {
        str_puts(out, ",\"error\":null");
    }

    if (finished && j->task) {
        /* 自定义任务：结果就是它自己拼好的 JSON */
        struct BatchSum *b = (struct BatchSum *)j->payload;
        str_puts(out, ",\"result\":");
        if (b && b->json) str_puts(out, b->json);
        else str_puts(out, "null");
        str_putc(out, '}');
        return 0;
    }

    if (finished) {
        RunResult *r = &j->result;
        str_puts(out, ",\"result\":{");
        str_printf(out, "\"ok\":%d,\"failed\":%d,\"skipped\":%d,\"total\":%d,"
                        "\"cancelled\":%s,\"seconds\":%.2f",
                   r->ok, r->failed, r->skipped, r->total,
                   r->cancelled ? "true" : "false", r->seconds);
        str_puts(out, ",\"journal\":");
        json_escape_to(out, r->journal_path ? r->journal_path : L"");
        str_puts(out, ",\"errors\":[");
        size_t lim = r->n_errors > 30 ? 30 : r->n_errors;
        for (size_t i = 0; i < lim; ++i) {
            if (i) str_putc(out, ',');
            json_escape_to(out, r->errors[i]);
        }
        str_puts(out, "],\"message\":");
        {
            wchar_t msg[256];
            swprintf(msg, 256, L"成功 %d，失败 %d，跳过 %d（共 %d，耗时 %.1f 秒）",
                     r->ok, r->failed, r->skipped, r->total, r->seconds);
            if (r->cancelled) wcscat(msg, L"，已中断");
            json_escape_to(out, msg);
        }
        str_putc(out, '}');
    } else {
        str_puts(out, ",\"result\":null");
    }
    str_putc(out, '}');
    return 0;
}

static int m_job_cancel(ApiState *st, Arena *a, const JsonValue *p, Str *out, Str *err) {
    (void)a;
    const char *id = json_string(json_get(p, "job_id"), NULL);
    EnterCriticalSection(&st->lock);
    Job *j = job_find(st, id);
    LeaveCriticalSection(&st->lock);
    if (!j) FAIL("任务不存在。");
    InterlockedExchange(&j->cancelled, 1);
    str_puts(out, "{\"cancelling\":true}");
    return 0;
}

/* ================================================== 模块 3：内容去重 ==== */

static void parse_dedup_rule(Arena *a, const JsonValue *p, DedupRule *r) {
    dedup_rule_init(r, a);
    double th = json_double(json_get(p, "threshold"), 0.92);
    if (th < 0.5) th = 0.5;
    if (th > 1.0) th = 1.0;
    r->threshold = th;
    r->recursive = jbool(p, "recursive", 1);
    r->min_chars = jint(p, "min_chars", 40);
    r->action = jint(p, "action", 0);
    wchar_t *td = jw(a, p, "target_dir");
    if (td[0]) r->target_dir = td;
}

static void dedup_result_to_json(const DedupResult *res, const DedupRule *r,
                                 Str *out) {
    /* 统计 */
    size_t dup_items = 0;
    for (size_t i = 0; i < res->n; ++i)
        if (res->items[i].group > 0) dup_items++;

    str_printf(out, "{\"stats\":{\"scanned\":%llu,\"groups\":%llu,"
                    "\"duplicates\":%llu,\"skipped\":%d,\"max_distance\":%d,"
                    "\"threshold\":%.4f}",
               (unsigned long long)res->n, (unsigned long long)res->n_groups,
               (unsigned long long)dup_items, res->skipped,
               res->max_distance, r->threshold);

    str_puts(out, ",\"groups\":[");
    int emitted = 0;
    for (int g = 1; g <= (int)res->n_groups; ++g) {
        size_t keeper = (size_t)-1;
        for (size_t i = 0; i < res->n; ++i)
            if (res->items[i].group == g) { keeper = i; break; }
        if (keeper == (size_t)-1) continue;

        if (emitted) str_putc(out, ',');
        emitted++;
        str_printf(out, "{\"group\":%d,\"items\":[", g);

        int first_item = 1;
        for (size_t i = 0; i < res->n; ++i) {
            if (res->items[i].group != g) continue;
            if (!first_item) str_putc(out, ',');
            first_item = 0;
            str_putc(out, '{');

            wchar_t szbuf[64];
            lc_human_size(res->items[i].size, szbuf, 64);
            double sim = simhash_similarity(res->items[i].hash,
                                            res->items[keeper].hash);

            put_key_w(out, "path", res->items[i].path);
            str_putc(out, ',');
            put_key_w(out, "name", lc_basename(res->items[i].path));
            str_printf(out, ",\"size\":%lld,\"size_text\":",
                       (long long)res->items[i].size);
            json_escape_to(out, szbuf);
            str_printf(out, ",\"tokens\":%d,\"similarity\":%.4f,\"keeper\":%s}",
                       res->items[i].tokens, sim, (i == keeper) ? "true" : "false");
        }
        str_puts(out, "]}");
    }
    str_puts(out, "],\"error\":");
    json_escape_to(out, res->first_error);
    str_putc(out, '}');
}

static int m_dedup_scan(ApiState *st, Arena *a, const JsonValue *p,
                        Str *out, Str *err) {
    (void)st;
    wchar_t *dir = jw(a, p, "dir");
    if (!dir[0]) FAIL("请先选择文件夹。");
    if (!lc_is_dir(dir)) return fail_w(err, L"文件夹不存在：%ls", dir);

    Arena *ca = arena_new();
    DedupRule rule;
    parse_dedup_rule(ca, p, &rule);

    DedupResult res;
    wchar_t e[512];
    if (dedup_scan(dir, &rule, &res, e, 512) != 0) {
        dedup_result_free(&res);
        arena_free(ca);
        if (err) {
            char *u8 = lc_wcs_to_utf8(e);
            str_puts(err, u8 ? u8 : "扫描失败");
            free(u8);
        }
        return -1;
    }

    dedup_result_to_json(&res, &rule, out);
    dedup_result_free(&res);
    arena_free(ca);
    return 0;
}

static int m_dedup_preview(ApiState *st, Arena *a, const JsonValue *p,
                           Str *out, Str *err) {
    wchar_t *dir = jw(a, p, "dir");
    if (!dir[0]) FAIL("请先选择文件夹。");
    if (!lc_is_dir(dir)) return fail_w(err, L"文件夹不存在：%ls", dir);

    Arena *ca = arena_new();
    DedupRule rule;
    parse_dedup_rule(ca, p, &rule);

    DedupResult *res = (DedupResult *)calloc(1, sizeof(DedupResult));
    wchar_t e[512];
    if (!res || dedup_scan(dir, &rule, res, e, 512) != 0) {
        if (res) dedup_result_free(res);
        free(res);
        arena_free(ca);
        if (err) {
            char *u8 = lc_wcs_to_utf8(e);
            str_puts(err, u8 ? u8 : "扫描失败");
            free(u8);
        }
        return -1;
    }

    Arena *pa = arena_new();
    Plan *plan = dedup_build_plan(pa, dir, res, &rule, policy_of(p, POLICY_RENAME),
                                  NULL);
    if (!plan) {
        dedup_result_free(res); free(res); arena_free(ca); arena_free(pa);
        FAIL("生成去重计划失败");
    }

    WStr title;
    wstr_init(&title);
    wstr_printf(&title, L"内容去重 — 相似度 ≥ %.0f%%，%llu 组",
                rule.threshold * 100.0, (unsigned long long)res->n_groups);

    EnterCriticalSection(&st->lock);
    sessions_expire(st);
    PlanSession *s = session_new(st, plan, title.data);
    if (s) {
        s->kind = 3;
        s->root = _wcsdup(dir);
        s->cfg_arena = ca;
        s->drule = rule;
        s->dres = res;
    } else {
        dedup_result_free(res); free(res); arena_free(ca);
    }
    LeaveCriticalSection(&st->lock);
    wstr_free(&title);

    if (!s) { arena_free(pa); FAIL("内存不足"); }
    plan_to_json(plan, s->title, out);
    (void)a;
    return 0;
}

/* ================================================== 模块 4：摘要 ======== */

static void summary_to_json(const Summary *s, const wchar_t *text,
                            size_t text_len, Str *out) {
    /* WARNING: this function is embedded inside another object; do not emit a leading brace */
    str_puts(out, "\"sentences\":[");
    for (size_t i = 0; i < s->n_sentences; ++i) {
        if (i) str_putc(out, ',');
        size_t st = s->sentences[i].start, en = s->sentences[i].end;
        if (en > text_len) en = text_len;
        wchar_t *seg = (en > st) ? arena_wcsndup(s->arena, text + st, en - st)
                                 : arena_wcsdup(s->arena, L"");
        str_printf(out, "{\"start\":%llu,\"end\":%llu,\"score\":%.4f,\"text\":",
                   (unsigned long long)st, (unsigned long long)en,
                   s->sentences[i].score);
        json_escape_to(out, seg ? seg : L"");
        str_putc(out, '}');
    }
    str_puts(out, "],\"keywords\":[");
    for (size_t i = 0; i < s->n_keywords; ++i) {
        if (i) str_putc(out, ',');
        str_puts(out, "{\"word\":");
        json_escape_to(out, s->keywords[i]);
        str_printf(out, ",\"score\":%.4f}", s->keyword_scores[i]);
    }
    /* 不能自带收尾的 } —— 本函数是被内嵌的片段，括号由调用方负责 */
    str_printf(out, "],\"text_length\":%llu",
               (unsigned long long)text_len);
}

static int m_summarize_file(ApiState *st, Arena *a, const JsonValue *p,
                            Str *out, Str *err) {
    (void)st;
    wchar_t *path = jw(a, p, "path");
    if (!path[0]) FAIL("缺少文件路径。");
    if (!lc_is_file(path)) return fail_w(err, L"文件不存在：%ls", path);

    wchar_t e[512];
    e[0] = 0;
    wchar_t *text = extract_text(path, 4u << 20, e, 512);
    if (!text) {
        if (err) {
            char *u8 = lc_wcs_to_utf8(e[0] ? e : L"无法抽取文本");
            str_puts(err, u8 ? u8 : "无法抽取文本");
            free(u8);
        }
        return -1;
    }

    int n_sent = jint(p, "sentences", 0);
    int algo = jint(p, "algorithm", 1);      /* 1 = TextRank, 0 = TF-IDF */

    Summary s;
    memset(&s, 0, sizeof(s));
    if (summarize_text(text, n_sent, algo, &s, a) != 0) {
        free(text);
        FAIL("无法生成摘要（文档可能太短或没有可识别的句子）");
    }

    str_puts(out, "{\"path\":");
    json_escape_to(out, path);
    str_puts(out, ",\"name\":");
    json_escape_to(out, lc_basename(path));
    str_putc(out, ',');
    summary_to_json(&s, text, wcslen(text), out);
    str_putc(out, '}');
    free(text);
    return 0;
}

/* ---- 批量摘要：放到后台任务里跑 ---- */

static void batch_sum_free(void *payload) {
    struct BatchSum *b = (struct BatchSum *)payload;
    if (!b) return;
    for (size_t i = 0; i < b->n; ++i) free(b->paths[i]);
    free(b->paths);
    free(b->json);
    free(b);
}

static int batch_sum_task(void *payload, struct Job *j) {
    struct BatchSum *b = (struct BatchSum *)payload;
    Str out;
    str_init(&out);
    str_puts(&out, "{\"results\":[");

    for (size_t i = 0; i < b->n; ++i) {
        if (j->cancelled) break;
        InterlockedExchange(&j->total, (LONG)b->n);
        InterlockedExchange(&j->done, (LONG)i);
        EnterCriticalSection(&j->label_lock);
        wcsncpy(j->label, lc_basename(b->paths[i]), 255);
        j->label[255] = 0;
        LeaveCriticalSection(&j->label_lock);

        if (i) str_putc(&out, ',');

        wchar_t e[512];
        e[0] = 0;
        wchar_t *text = extract_text(b->paths[i], 4u << 20, e, 512);
        if (!text) {
            str_puts(&out, "{\"ok\":false,\"path\":");
            json_escape_to(&out, b->paths[i]);
            str_puts(&out, ",\"name\":");
            json_escape_to(&out, lc_basename(b->paths[i]));
            str_puts(&out, ",\"error\":");
            json_escape_to(&out, e[0] ? e : L"无法抽取文本");
            str_putc(&out, '}');
            b->fail_count++;
            continue;
        }

        Arena *tmp = arena_new();
        Summary s;
        memset(&s, 0, sizeof(s));
        if (summarize_text(text, b->sentences, b->algorithm, &s, tmp) != 0) {
            str_puts(&out, "{\"ok\":false,\"path\":");
            json_escape_to(&out, b->paths[i]);
            str_puts(&out, ",\"name\":");
            json_escape_to(&out, lc_basename(b->paths[i]));
            str_puts(&out, ",\"error\":");
            json_escape_to(&out, L"文档太短或没有可识别的句子");
            str_putc(&out, '}');
            b->fail_count++;
        } else {
            str_puts(&out, "{\"ok\":true,\"path\":");
            json_escape_to(&out, b->paths[i]);
            str_puts(&out, ",\"name\":");
            json_escape_to(&out, lc_basename(b->paths[i]));
            str_putc(&out, ',');
            summary_to_json(&s, text, wcslen(text), &out);
            str_putc(&out, '}');
            b->ok_count++;
        }
        arena_free(tmp);
        free(text);
    }

    str_printf(&out, "],\"ok\":%d,\"failed\":%d,\"total\":%llu}",
               b->ok_count, b->fail_count, (unsigned long long)b->n);
    b->json = str_steal(&out);
    b->json_len = b->json ? strlen(b->json) : 0;
    InterlockedExchange(&j->done, (LONG)b->n);
    return 0;
}

static int m_summarize_batch(ApiState *st, Arena *a, const JsonValue *p,
                             Str *out, Str *err) {
    wchar_t *dir = jw(a, p, "dir");
    if (!dir[0]) FAIL("请先选择文件夹。");
    if (!lc_is_dir(dir)) return fail_w(err, L"文件夹不存在：%ls", dir);

    int recursive = jbool(p, "recursive", 1);
    int limit = jint(p, "limit", 100);
    if (limit <= 0 || limit > 500) limit = 100;

    FileList fl;
    filelist_init(&fl, a);
    if (lc_scan_dir(dir, recursive, 1, 1, &fl) != 0) FAIL("无法读取目录");

    struct BatchSum *b = (struct BatchSum *)calloc(1, sizeof(struct BatchSum));
    if (!b) FAIL("内存不足");
    b->sentences = jint(p, "sentences", 0);
    b->algorithm = jint(p, "algorithm", 1);
    b->paths = (wchar_t **)calloc(limit, sizeof(wchar_t *));

    for (size_t i = 0; i < fl.n && b->n < (size_t)limit; ++i) {
        if (!extract_supported(fl.paths[i])) continue;
        b->paths[b->n++] = _wcsdup(fl.paths[i]);
    }
    if (b->n == 0) {
        batch_sum_free(b);
        FAIL("该目录下没有可抽取文本的文档（支持 docx/xlsx/pptx/pdf/txt/md/html 等）");
    }

    Job *j = (Job *)calloc(1, sizeof(Job));
    if (!j) { batch_sum_free(b); FAIL("内存不足"); }
    snprintf(j->id, sizeof(j->id), "job%08x%04x",
             (unsigned)GetTickCount(), (unsigned)(rand() & 0xffff));
    j->kind = 9;
    InitializeCriticalSection(&j->label_lock);
    run_result_init(&j->result);
    j->task = batch_sum_task;
    j->payload = b;
    j->payload_free = batch_sum_free;
    j->total = (LONG)b->n;

    EnterCriticalSection(&st->lock);
    j->next = st->jobs;
    st->jobs = j;
    jobs_gc(st);
    LeaveCriticalSection(&st->lock);

    InterlockedExchange(&j->running, 1);
    HANDLE h = CreateThread(NULL, 0, job_thread, j, 0, NULL);
    if (!h) {
        InterlockedExchange(&j->running, 0);
        InterlockedExchange(&j->finished, 1);
        j->error = _strdup("无法创建任务线程");
        FAIL("无法创建任务线程");
    }
    CloseHandle(h);

    str_printf(out, "{\"job_id\":\"%s\",\"total\":%llu}", j->id,
               (unsigned long long)b->n);
    return 0;
}

/* ================================================== 模块 5：导出 ======== */

/*
 * 造一个"产物型"计划（导出 / 格式转换）。
 *
 * after_finalize 是在 plan_finalize **之后**才调的，可以为 NULL。
 *
 * 为什么需要这个钩子
 * ------------------
 * plan_finalize 第一件事就是 action_clear_conflicts()，把每一行的冲突全清掉
 * 再按标准流程重算（静态校验 → 链式移动 → 重名策略 → 重复目标 → 拆步骤）。
 * 所以任何在 add_actions 里加的冲突都会**当场被冲走** ——
 * 我第一版就是这么写的，结果"txt 转 png 不支持"这个标记根本没出现在界面上。
 * 想在标准分析之外补一条自己的判断，只能等 finalize 跑完再说。
 */
static int make_artifact_plan(ApiState *st, const wchar_t *root,
                              const wchar_t *title, int kind,
                              void (*add_actions)(Plan *, void *),
                              void (*after_finalize)(Plan *, void *),
                              void *ud, Str *out, Str *err) {
    Arena *pa = arena_new();
    Plan *plan = (Plan *)arena_zalloc(pa, sizeof(Plan));
    if (!plan) { arena_free(pa); FAIL("内存不足"); }
    plan_init(plan, pa, root, POLICY_RENAME, L"convert", title);

    add_actions(plan, ud);
    plan_finalize(plan, 1, 1);
    if (after_finalize) after_finalize(plan, ud);

    EnterCriticalSection(&st->lock);
    sessions_expire(st);
    PlanSession *s = session_new(st, plan, title);
    LeaveCriticalSection(&st->lock);
    if (!s) { arena_free(pa); FAIL("内存不足"); }

    plan_to_json(plan, s->title, out);
    return 0;
}

typedef struct {
    wchar_t  *src;          /* 源文件（Markdown / 文本） */
    wchar_t  *dst;
    wchar_t  *generator;
} ExportSpec;

static void add_export_action(Plan *plan, void *ud) {
    ExportSpec *sp = (ExportSpec *)ud;
    Action *a = plan_add(plan, L"export", sp->src, sp->dst,
                         L"导出为文档");
    if (!a) return;
    a->produce_only = 1;
    a->generator = arena_wcsdup(plan->arena, sp->generator);
    a->producer = arena_wcsdup(plan->arena, sp->generator);
    a->no_source_check = 0;
}

static int m_export_run(ApiState *st, Arena *a, const JsonValue *p,
                        Str *out, Str *err) {
    wchar_t *src = jw(a, p, "path");
    wchar_t *fmt = jw(a, p, "format");
    wchar_t *outdir = jw(a, p, "out_dir");
    wchar_t *outname = jw(a, p, "out_name");

    if (!src[0]) FAIL("缺少源文件。");
    if (!lc_is_file(src)) return fail_w(err, L"文件不存在：%ls", src);

    if (!fmt[0]) {
        /* 没指定就按源扩展名推断 */
        fmt = lc_wcs_ieq(lc_extname(src), L".md") ? L"docx" : L"md";
    }
    if (!lc_wcs_ieq(fmt, L"docx") && !lc_wcs_ieq(fmt, L"pdf") &&
        !lc_wcs_ieq(fmt, L"md")) {
        return fail_w(err, L"不支持的导出格式：%ls（可用 docx / pdf / md）", fmt);
    }

    if (!outdir[0]) {
        outdir = lc_dirname(src);
    } else if (!lc_is_dir(outdir)) {
        return fail_w(err, L"输出目录不存在：%ls", outdir);
    }

    wchar_t *stem = lc_stem(src);
    WStr dst;
    wstr_init(&dst);
    wstr_puts(&dst, outdir);
    if (dst.len && !lc_is_sep(dst.data[dst.len - 1])) wstr_putc(&dst, L'\\');
    wstr_puts(&dst, outname[0] ? outname : stem);
    wstr_puts(&dst, L".");
    wstr_puts(&dst, fmt);

    ExportSpec sp;
    sp.src = src;
    sp.dst = dst.data;
    WStr gen;
    wstr_init(&gen);
    wstr_puts(&gen, L"export:");
    wstr_puts(&gen, fmt);
    sp.generator = gen.data;

    WStr title;
    wstr_init(&title);
    wstr_printf(&title, L"导出预览 — %ls → %ls", lc_basename(src),
                lc_basename(dst.data));

    int rc = make_artifact_plan(st, outdir, title.data, 5, add_export_action, NULL, &sp,
                                out, err);

    wstr_free(&title);
    wstr_free(&gen);
    wstr_free(&dst);
    free(stem);
    (void)a;
    return rc;
}

/* ================================================== 模块 2：格式转换 ==== */

typedef struct {
    Vec      sources;      /* wchar_t* */
    Vec      targets;      /* wchar_t* */
    Vec      gens;         /* wchar_t* */
    Vec      feas;         /* int*：convert_feasibility 的结果 */
} ConvertSpec;

static void add_convert_action(Plan *plan, void *ud) {
    ConvertSpec *sp = (ConvertSpec *)ud;
    for (size_t i = 0; i < sp->sources.len; ++i) {
        Action *a = plan_add(plan, L"convert",
                             (const wchar_t *)sp->sources.items[i],
                             (const wchar_t *)sp->targets.items[i],
                             L"格式转换");
        if (!a) continue;
        a->produce_only = 1;
        a->generator = arena_wcsdup(plan->arena,
                                    (const wchar_t *)sp->gens.items[i]);
        a->producer = arena_wcsdup(plan->arena,
                                   (const wchar_t *)sp->gens.items[i]);
    }
}

/*
 * 把"这次转换到底做不做得到"如实标在预览表上。
 *
 * 必须在 plan_finalize **之后**调用。原因：
 * finalize 开头的 action_clear_conflicts() 会把此前加的任何冲突全部清掉，
 * 再按标准流程重算。第一版把标记写在 add_convert_action 里，
 * 结果全被冲走 —— "txt 转 png"照样显示成"正常可执行"。
 *
 * 行序与 sp 的下标一一对应：add_convert_action 是顺序 plan_add 的。
 */
static void apply_convert_feasibility(Plan *plan, void *ud) {
    ConvertSpec *sp = (ConvertSpec *)ud;

    for (size_t i = 0; i < plan->n_actions && i < sp->sources.len; ++i) {
        Action *a = &plan->actions[i];
        int feas = sp->feas.items ? *(int *)sp->feas.items[i] : 2;
        if (feas == 2) continue;                 /* 内置就能做，不必多说 */

        const wchar_t *se = lc_extname(a->src);
        const wchar_t *de = lc_extname(a->dst);

        if (feas == 0) {
            action_add_conflict(a, C_CONVERT_UNSUPPORTED, LEVEL_ERROR,
                L"这种转换做不到：%ls 无法变成 %ls。"
                L"把文件内容画成图片属于识别类工作，本项目不做，因此这一行不会执行。",
                se, de);
        } else {
            action_add_conflict(a, C_CONVERT_NEEDS_ENGINE, LEVEL_WARN,
                L"内置转换不支持 %ls → %ls，需要外部引擎"
                L"（Pandoc / LibreOffice / FFmpeg）。没装的话这一步会失败。",
                se, de);
        }
    }
}

static int m_convert_preview(ApiState *st, Arena *a, const JsonValue *p,
                             Str *out, Str *err) {
    wchar_t *dir = jw(a, p, "dir");
    if (!dir[0]) FAIL("请先选择文件夹。");
    if (!lc_is_dir(dir)) return fail_w(err, L"文件夹不存在：%ls", dir);

    wchar_t *target = jw(a, p, "target");     /* 目标扩展名，如 png / md / docx */
    if (!target[0]) FAIL("请选择目标格式。");
    /* 统一成带点的形式 */
    WStr tgt;
    wstr_init(&tgt);
    if (target[0] != L'.') wstr_putc(&tgt, L'.');
    wstr_puts(&tgt, target);

    int recursive = jbool(p, "recursive", 0);
    int limit = jint(p, "limit", 500);
    if (limit <= 0 || limit > 5000) limit = 500;

    FileList fl;
    filelist_init(&fl, a);
    if (lc_scan_dir(dir, recursive, 1, 1, &fl) != 0) {
        wstr_free(&tgt);
        FAIL("无法读取目录");
    }

    ConvertSpec *sp = (ConvertSpec *)calloc(1, sizeof(ConvertSpec));
    if (!sp) { wstr_free(&tgt); FAIL("内存不足"); }
    vec_init(&sp->sources); vec_init(&sp->targets); vec_init(&sp->gens);
    vec_init(&sp->feas);

    for (size_t i = 0; i < fl.n; ++i) {
        const wchar_t *srcp = fl.paths[i];
        if (lc_wcs_ieq(lc_extname(srcp), tgt.data)) continue;   /* 已经是目标格式 */

        /*
         * 先判断这次转换做不做得到，**结果要留下来**。
         * 之前这里是 if (!supported) { } —— 判断算了却丢掉，
         * 于是"转成 PNG"时把 .txt 也列了进去还标成正常。
         */
        int feas = convert_feasibility(srcp, tgt.data);

        wchar_t *stem = lc_stem(srcp);
        WStr dst;
        wstr_init(&dst);
        wstr_puts(&dst, dir);
        if (dst.len && !lc_is_sep(dst.data[dst.len - 1])) wstr_putc(&dst, L'\\');
        wstr_puts(&dst, stem);
        wstr_puts(&dst, tgt.data);
        free(stem);

        WStr gen;
        wstr_init(&gen);
        wstr_puts(&gen, L"convert:");
        wstr_puts(&gen, tgt.data + 1);

        vec_push(&sp->sources, arena_wcsdup(a, srcp));
        vec_push(&sp->targets, arena_wcsdup(a, dst.data));
        vec_push(&sp->gens,    arena_wcsdup(a, gen.data));
        {
            int *fv = (int *)calloc(1, sizeof(int));
            if (fv) { *fv = feas; vec_push(&sp->feas, fv); }
        }

        wstr_free(&dst);
        wstr_free(&gen);
        if (sp->sources.len >= (size_t)limit) break;
    }

    if (sp->sources.len == 0) {
        vec_free(&sp->sources); vec_free(&sp->targets); vec_free(&sp->gens);
        for (size_t i = 0; i < sp->feas.len; ++i) free(sp->feas.items[i]);
        vec_free(&sp->feas);
        free(sp); wstr_free(&tgt);
        FAIL("该目录下没有需要转换的文件（可能都已经是指定的格式）");
    }

    WStr title;
    wstr_init(&title);
    wstr_printf(&title, L"格式转换预览 — 转为 %ls（%llu 个文件）",
                tgt.data, (unsigned long long)sp->sources.len);

    int rc = make_artifact_plan(st, dir, title.data, 4, add_convert_action, apply_convert_feasibility, sp,
                                out, err);

    wstr_free(&title);
    vec_free(&sp->sources); vec_free(&sp->targets); vec_free(&sp->gens);
    for (size_t i = 0; i < sp->feas.len; ++i) free(sp->feas.items[i]);
    vec_free(&sp->feas);
    free(sp);
    wstr_free(&tgt);
    return rc;
}

static int m_convert_engines(ApiState *st, Arena *a, const JsonValue *p,
                             Str *out, Str *err) {
    (void)st; (void)p; (void)err;
    ConvertEngine eng[4];
    memset(eng, 0, sizeof(eng));
    convert_probe_engines(eng, 4);

    str_puts(out, "{\"engines\":[");
    for (int i = 0; i < 4; ++i) {
        if (i) str_putc(out, ',');
        str_putc(out, '{');
        put_key_w(out, "key", eng[i].key);
        str_printf(out, ",\"found\":%s,", eng[i].found ? "true" : "false");
        put_key_w(out, "path", eng[i].path);
        str_putc(out, '}');
        convert_engine_free(&eng[i]);
    }
    wchar_t *tools = lc_tools_dir();
    str_puts(out, "],\"tools_dir\":");
    json_escape_to(out, tools);
    str_putc(out, '}');
    free(tools);
    (void)a;
    return 0;
}

/* ================================================== 分发表 ============ */

typedef struct { const char *name; MethodFn fn; } MethodEntry;

static const MethodEntry METHODS[] = {
    {"app.info", m_app_info},
    {"app.quit", m_app_quit},
    {"app.beat", m_app_beat},
    {"app.engines", m_app_engines},
    {"fs.pick_dir", m_pick_dir},
    {"fs.pick_file", m_pick_file},
    {"fs.pick_save", m_pick_save},
    {"fs.reveal", m_reveal},
    {"rename.preview", m_rename_preview},
    {"organize.preview", m_organize_preview},
    {"organize.scan", m_organize_scan},
    {"dedup.scan", m_dedup_scan},
    {"dedup.preview", m_dedup_preview},
    {"summarize.file", m_summarize_file},
    {"summarize.batch", m_summarize_batch},
    {"export.run", m_export_run},
    {"convert.preview", m_convert_preview},
    {"convert.engines", m_convert_engines},
    {"plan.get", m_plan_get},
    {"plan.toggle", m_plan_toggle},
    {"plan.set_all", m_plan_set_all},
    {"plan.set_policy", m_plan_set_policy},
    {"plan.keep_selected", m_plan_keep_selected},
    {"plan.export_csv", m_plan_export_csv},
    {"plan.close", m_plan_close},
    {"exec.run", m_exec_run},
    {"exec.dry", m_exec_dry},
    {"history.list", m_history_list},
    {"history.detail", m_history_detail},
    {"history.rollback_preview", m_history_rollback_preview},
    {"history.rollback_run", m_history_rollback_run},
    {"history.purge_journals", m_purge_journals},
    {"history.purge_trash", m_purge_trash},
    {"job.status", m_job_status},
    {"job.cancel", m_job_cancel},
    {NULL, NULL}
};

/* ================================================== 生命周期 ========== */

ApiState *api_create(const wchar_t *web_dir) {
    ApiState *st = (ApiState *)calloc(1, sizeof(ApiState));
    if (!st) return NULL;
    InitializeCriticalSection(&st->lock);
    st->started = lc_now_unix();
    st->last_beat = (LONG)st->started;
    st->ui_seen = 0;
    st->api_calls = 0;
    if (web_dir) {
        wcsncpy(st->web_dir, web_dir, MAX_PATH * 2 - 1);
        st->web_dir[MAX_PATH * 2 - 1] = 0;
    }
    return st;
}

void api_destroy(ApiState *st) {
    if (!st) return;
    PlanSession *s = st->sessions;
    while (s) { PlanSession *n = s->next; session_free(s); s = n; }
    Job *j = st->jobs;
    while (j) { Job *n = j->next; job_free(j); j = n; }
    DeleteCriticalSection(&st->lock);
    free(st);
}

void api_cancel_all(ApiState *st) {
    if (!st) return;
    for (Job *j = st->jobs; j; j = j->next) {
        if (j->running) InterlockedExchange(&j->cancelled, 1);
    }
}

int api_watchdog_tick(ApiState *st) {
    if (!st) return 0;
    if (st->quit_requested) return 1;
    /* 有任务在跑时绝不因为心跳超时退出 */
    EnterCriticalSection(&st->lock);
    int busy = 0;
    for (Job *j = st->jobs; j; j = j->next) if (j->running) { busy = 1; break; }
    LeaveCriticalSection(&st->lock);
    if (busy) { InterlockedExchange(&st->last_beat, (LONG)lc_now_unix()); return 0; }

    if (lc_now_unix() - st->last_beat > 600) return 1;
    return 0;
}

/* 界面是否真的连上来过 —— 用来判断"窗口开了但页面没出来" */
 int api_ui_seen(void *ud) {
    ApiState *st = (ApiState *)ud;
    return st && st->ui_seen != 0;
}
int api_call_count(void *ud) {
    ApiState *st = (ApiState *)ud;
    return st ? (int)st->api_calls : 0;
}

int api_dispatch(void *ud, const char *method, const JsonValue *params,
                 Str *out_json, Str *err_msg) {
    ApiState *st = (ApiState *)ud;
    if (!st || !method) {
        if (err_msg) str_puts(err_msg, "非法请求");
        return -1;
    }
    /* 任何一次接口调用都说明页面脚本真的站起来了 */
    InterlockedExchange(&st->ui_seen, 1);
    InterlockedIncrement(&st->api_calls);

    for (size_t i = 0; METHODS[i].name; ++i) {
        if (strcmp(METHODS[i].name, method) != 0) continue;
        Arena *scratch = arena_new();
        if (!scratch) {
            if (err_msg) str_puts(err_msg, "内存不足");
            return -1;
        }
        /* 注意：这里**不**持有全局锁 —— 有些方法会阻塞在文件对话框上，
           持锁会让心跳与任务轮询一起卡死。各方法自己在需要时加锁。 */
        int rc = METHODS[i].fn(st, scratch, params, out_json, err_msg);
        arena_free(scratch);
        return rc;
    }

    if (err_msg) {
        Str tmp;
        str_init(&tmp);
        str_printf(&tmp, "未知接口：%s", method);
        str_puts(err_msg, tmp.data ? tmp.data : "未知接口");
        str_free(&tmp);
    }
    return -1;
}

