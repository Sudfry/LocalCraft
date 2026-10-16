/* ==========================================================================
 * LocalCraft — 完全离线的本地文件处理工具
 * lc.h —— 公共类型与接口
 *
 * 设计约定
 * --------
 *  1. 程序内部一律用 **宽字符 (UTF-16, wchar_t)** 表示路径与文本 ——
 *     Windows 的原生 API 就是宽字符，中文路径不会出乱码。
 *  2. 只在 **进程边界**（JSON / HTTP）转换成 UTF-8。
 *  3. 所有动态内存都从 **Arena** 分配。一个 Plan 一个 Arena，
 *     Plan 用完一次 arena_free 全部回收 —— 避免上千处 free 漏掉。
 *  4. 不依赖任何第三方库；MinGW 没有 regex.h，正则自己实现。
 * ========================================================================== */
#ifndef LC_H
#define LC_H

#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#define LC_APP_NAME     L"LocalCraft"
#define LC_APP_VERSION  L"1.0.0"
#define LC_APP_VERSION_A "1.0.0"

/* 前置声明：这些结构在后面完整定义，但很多接口先用到它们。
   C11 允许同名的冗余 typedef，所以这里和后面的定义不冲突。 */
typedef struct Plan Plan;
typedef struct Action Action;
typedef struct Step Step;
typedef struct JsonValue JsonValue;

/* ------------------------------------------------------------------ 等级 */
typedef enum {
    LEVEL_OK = 0,
    LEVEL_INFO = 1,
    LEVEL_WARN = 2,
    LEVEL_ERROR = 3
} Level;

/* 冲突码 */
typedef enum {
    C_NONE = 0,
    C_MISSING_SOURCE,
    C_NO_CHANGE,
    C_TARGET_EXISTS,
    C_TARGET_EXISTS_RENAMED,
    C_TARGET_EXISTS_SKIPPED,
    C_OVERWRITE,
    C_DUPLICATE_TARGET,
    C_INTO_SELF,
    C_INVALID_NAME,
    C_BAD_TARGET,
    C_TARGET_IS_DIR,
    C_CHAIN_MOVE,
    C_TARGET_INSIDE_MOVED_DIR,
    C_LONG_PATH,
    C_ROLLBACK_SOURCE_MISSING,
    C_ROLLBACK_TARGET_OCCUPIED,
    C_ROLLBACK_DIR_NOT_EMPTY,
    C_ROLLBACK_TARGET_EXISTS,
    C_ROLLBACK_SIZE_MISMATCH,
    C_UNKNOWN_STEP,
    C_CONVERT_UNSUPPORTED,      /* 这种格式组合根本不成立（如 txt → png） */
    C_CONVERT_NEEDS_ENGINE      /* 内置做不了，要靠外部引擎 */
} ConflictCode;

/* 冲突策略 */
typedef enum {
    POLICY_ASK = 0,
    POLICY_SKIP,
    POLICY_OVERWRITE,
    POLICY_RENAME
} Policy;

/* 原子步骤类型 */
typedef enum {
    STEP_MOVE = 0,
    STEP_COPY,
    STEP_MKDIR,
    STEP_RMDIR,
    STEP_CREATE
} StepKind;

/* 动作状态 */
typedef enum {
    ST_PENDING = 0,
    ST_DONE,
    ST_FAILED,
    ST_SKIPPED,
    ST_PARTIAL
} ActionStatus;

/* ================================================================= Arena */
typedef struct ArenaBlock ArenaBlock;

typedef struct {
    ArenaBlock *head;
    size_t total;
} Arena;

Arena *arena_new(void);
void   arena_free(Arena *a);
void  *arena_alloc(Arena *a, size_t n);
void  *arena_zalloc(Arena *a, size_t n);
wchar_t *arena_wcsdup(Arena *a, const wchar_t *s);
wchar_t *arena_wcsndup(Arena *a, const wchar_t *s, size_t n);
char    *arena_strdup(Arena *a, const char *s);

/* ================================================================= 动态串 */
/* 可增长的宽字符串（构建路径、拼接文本用） */
typedef struct {
    wchar_t *data;
    size_t   len;
    size_t   cap;
} WStr;

/* 可增长的 UTF-8 字符串（构建 JSON 用） */
typedef struct {
    char   *data;
    size_t  len;
    size_t  cap;
} Str;

void  wstr_init(WStr *s);
void  wstr_free(WStr *s);
void  wstr_clear(WStr *s);
int   wstr_reserve(WStr *s, size_t need);
int   wstr_putc(WStr *s, wchar_t c);
int   wstr_puts(WStr *s, const wchar_t *p);
int   wstr_putn(WStr *s, const wchar_t *p, size_t n);
int   wstr_printf(WStr *s, const wchar_t *fmt, ...);
wchar_t *wstr_steal(WStr *s);

void  str_init(Str *s);
void  str_free(Str *s);
void  str_clear(Str *s);
int   str_reserve(Str *s, size_t need);
int   str_putc(Str *s, char c);
int   str_puts(Str *s, const char *p);
int   str_putn(Str *s, const char *p, size_t n);
int   str_printf(Str *s, const char *fmt, ...);
char *str_steal(Str *s);

/* 通用指针数组 */
typedef struct {
    void **items;
    size_t len;
    size_t cap;
} Vec;

void  vec_init(Vec *v);
void  vec_free(Vec *v);
int   vec_push(Vec *v, void *item);
void *vec_get(Vec *v, size_t i);

/* ============================================================ 编码转换 */
/* 全部返回新分配的内存（用 lc_free 释放），失败返回 NULL */
char    *lc_wcs_to_utf8(const wchar_t *w);
wchar_t *lc_utf8_to_wcs(const char *u);
char    *lc_wcs_to_utf8_n(const wchar_t *w, size_t n);

void lc_free(void *p);
void *lc_alloc(size_t n);
void *lc_realloc(void *p, size_t n);

/* ============================================================== 路径工具 */
int   lc_is_sep(wchar_t c);
int   lc_is_windows(void);

/* 返回绝对路径（新分配）。失败返回 NULL。 */
wchar_t *lc_abs_path(const wchar_t *path);
/* 取父目录（新分配）。 */
wchar_t *lc_dirname(const wchar_t *path);
/* 取文件名（指向原串内部，不分配）。 */
const wchar_t *lc_basename(const wchar_t *path);
/* 取扩展名（含点，指向原串内部）。无扩展名返回末尾空串。 */
const wchar_t *lc_extname(const wchar_t *path);
/* 取主名（不含目录与扩展名，新分配）。 */
wchar_t *lc_stem(const wchar_t *path);

int lc_exists(const wchar_t *path);
int lc_is_dir(const wchar_t *path);
int lc_is_file(const wchar_t *path);
int lc_mkdir_p(const wchar_t *path);
int lc_rmdir_empty(const wchar_t *path);
int lc_dir_is_empty(const wchar_t *path);
long long lc_size_of(const wchar_t *path);

/* 用于"这两个路径是不是同一个"的比较键（大小写不敏感、统一分隔符，新分配） */
wchar_t *lc_norm_key(const wchar_t *path);
int      lc_same_path(const wchar_t *a, const wchar_t *b);
/* 严格比较（区分大小写）：判断"是否真的需要改名" */
int      lc_same_path_exact(const wchar_t *a, const wchar_t *b);
/* child 是否位于 parent 内部（严格包含） */
int      lc_is_within(const wchar_t *child, const wchar_t *parent);

/* 把任意字符串规整成合法文件名分量（新分配） */
wchar_t *lc_sanitize_component(const wchar_t *name);
int      lc_is_valid_component(const wchar_t *name);

/* 生成不冲突的目标路径：a.txt -> "a (1).txt"（新分配） */
wchar_t *lc_unique_path(const wchar_t *path, Vec *taken_keys);

/* 给 os / CRT 用的长路径形式（\\?\ 前缀，新分配） */
wchar_t *lc_long_path(const wchar_t *path);
/* 去掉 \\?\ 前缀（新分配） */
wchar_t *lc_display_path(const wchar_t *path);

int lc_path_too_long(const wchar_t *path);

/* 人类可读体积，如 "1.18 MB" */
void lc_human_size(long long bytes, wchar_t *buf, size_t cch);
/* 人类可读时间 */
void lc_format_time(int64_t unix_seconds, wchar_t *buf, size_t cch);
int64_t lc_now_unix(void);

/* ============================================================== 字符串工具 */
int   lc_wcs_ieq(const wchar_t *a, const wchar_t *b);
int   lc_wcs_icmp(const wchar_t *a, const wchar_t *b);
int   lc_wcs_starts_ci(const wchar_t *s, const wchar_t *prefix);
int   lc_wcs_ends_ci(const wchar_t *s, const wchar_t *suffix);
int   lc_wcs_ends(const wchar_t *s, const wchar_t *suffix);
wchar_t *lc_wcs_replace_all(const wchar_t *s, const wchar_t *find,
                            const wchar_t *repl, int ignore_case);
wchar_t *lc_wcs_trim(const wchar_t *s);
void  lc_wcs_lower(wchar_t *s);
void  lc_wcs_upper(wchar_t *s);
void  lc_wcs_title(wchar_t *s);
int   lc_wcs_to_int(const wchar_t *s, int fallback);

/* =============================================================== 文件系统 */
typedef struct {
    wchar_t **paths;
    size_t    n, cap;
    Arena    *arena;
} FileList;

void     filelist_init(FileList *fl, Arena *a);
int      filelist_push(FileList *fl, const wchar_t *path);

/* 递归/非递归列目录。返回 0 成功。 */
int lc_scan_dir(const wchar_t *root, int recursive, int skip_hidden,
                int files_only, FileList *out);

/* 回收站/数据目录定位 */
wchar_t *lc_app_dir(void);
int      lc_is_portable(void);
wchar_t *lc_data_root(void);
wchar_t *lc_journal_dir(void);
wchar_t *lc_trash_root(void);
wchar_t *lc_cache_dir(void);
wchar_t *lc_tools_dir(void);
int      lc_ensure_dir(const wchar_t *path);

/* 打开外部程序/文件夹 */
int lc_shell_open(const wchar_t *path);
int lc_find_browser(wchar_t *out, size_t cch);

/* 原子写文件 + fsync */
int lc_append_line_fsync(HANDLE h, const char *utf8_line);

/* ================================================================ 日志 */
typedef struct {
    HANDLE   file;          /* 追加写句柄 */
    wchar_t *path;
    int      closed;
    int64_t  records;
    Arena   *arena;
    int      is_rollback_hint;
    char    *plan_id;
} JournalWriter;

typedef struct {
    wchar_t *path;
    /* header */
    char    *plan_id;
    wchar_t *root;
    wchar_t *category;
    int      policy;
    int64_t  created;
    /* 计数 */
    size_t   n_steps;
    size_t   n_records;
    int      complete;
    int      status_ok;
    wchar_t *footer_status;
} JournalDoc;

/* 读取日志中的成功步骤记录 */
typedef struct {
    StepKind kind;
    wchar_t *src;
    wchar_t *dst;
    long long size;
    char    *action_id;
} JournalStep;

typedef struct {
    JournalStep *items;
    size_t       n, cap;
    Arena       *arena;
    JournalDoc   doc;
} JournalSteps;

int journal_write_header(JournalWriter *jw, Plan *plan);
int journal_action_begin(JournalWriter *jw, Action *a);
int journal_step(JournalWriter *jw, Action *a, int seq, Step *s);
int journal_action_end(JournalWriter *jw, Action *a);
int journal_footer(JournalWriter *jw, const wchar_t *status, int ok, int failed,
                   int skipped, const wchar_t *message);
void journal_close(JournalWriter *jw);
HANDLE journal_open(const wchar_t *path);
wchar_t *journal_path_for(const wchar_t *plan_id, int is_rollback);

int  journal_load_steps(const wchar_t *path, JournalSteps *out);
/* 读 header 概要（列表用） */
int  journal_load_doc(const wchar_t *path, JournalDoc *doc, Arena *a);
void journal_doc_free(JournalDoc *doc);
wchar_t *journal_find_rollback_of(const wchar_t *journal_path);

/* ================================================================ 计划 */
typedef struct Step {
    StepKind kind;
    wchar_t *src;       /* 可为 NULL */
    wchar_t *dst;
    wchar_t *note;
    /*: 只对 STEP_CREATE 有意义：执行到这一步时按此规格现生成产物。
        形如 "export:docx" / "convert:png"。为空则只校验文件已存在。 */
    wchar_t *generator;
    int      lenient;   /* 该步"不适用"不算失败 */
    int      ok;        /* -1 未知 / 1 成功 */
    int      skipped;
    wchar_t *error;
    long long size;
} Step;

typedef struct Conflict {
    ConflictCode code;
    Level        level;
    wchar_t     *message;
} Conflict;

typedef struct Action {
    char    *action_id;
    wchar_t *category;
    wchar_t *src;
    wchar_t *dst;
    wchar_t *orig_src;
    wchar_t *orig_dst;
    wchar_t *note;
    int      selected;

    /* meta */
    int      is_copy;
    int      produce_only;

    /*: 原地改文件内容（文本批量查找替换用）。
        置 1 时 planner 会把步骤拆成两条：
            MOVE(原文件 → 回收站)  +  CREATE(原路径)
        这样逆序回退天然正确 —— 先删掉改过的版本，再把原件搬回来。
        如果只写一条 CREATE，回退就只会删掉新文件，原内容永远找不回来。
        另外它还要配 force_exec=1：因为 src 和 dst 是同一个路径，
        action_changed() 会认为"没有变化"，不加这个就不会执行。 */
    int      inplace_edit;

    wchar_t *producer;
    /*: 产物生成规格，交给 executor 在 CREATE 步骤里执行。
        这样"导出 / 格式转换"产出的新文件同样进日志、同样可回退。 */
    wchar_t *generator;
    wchar_t *rule_desc;
    int      trash_seq;
    int      has_trash_seq;
    int      force_exec;
    int      no_source_check;
    int      target_is_dir_ok;

    Conflict *conflicts;
    size_t    n_conflicts, cap_conflicts;

    Step  *steps;
    size_t n_steps, cap_steps;

    ActionStatus status;
    wchar_t     *error;
} Action;

typedef struct Plan {
    char    *plan_id;
    wchar_t *root;
    Policy   policy;
    wchar_t *category;
    wchar_t *note;
    int64_t  created;

    Action *actions;
    size_t  n_actions, cap_actions;

    Arena  *arena;
    /* 统计缓存 */
    size_t  st_total, st_selected, st_executable, st_blocked, st_warned, st_overwrite;
} Plan;

typedef struct {
    wchar_t *trash_root;
    Plan    *plan;
} PlanBuilder;

Arena *plan_arena(Plan *p);
void   plan_init(Plan *p, Arena *a, const wchar_t *root, Policy policy,
                 const wchar_t *category, const wchar_t *note);
Action *plan_add(Plan *p, const wchar_t *category, const wchar_t *src,
                 const wchar_t *dst, const wchar_t *note);
void   plan_finalize(Plan *p, int check_source, int recheck_disk);
void   plan_compute_stats(Plan *p);
int    action_blocked(const Action *a);
int    action_changed(const Action *a);
int    action_executable(const Action *a);
Level  action_level(const Action *a);
void   action_add_conflict(Action *a, ConflictCode code, Level level,
                           const wchar_t *fmt, ...);
Step  *action_push_step(Action *a, StepKind kind, const wchar_t *src,
                        const wchar_t *dst, const wchar_t *note);
wchar_t *plan_build_trash_path(Plan *p, Action *a, const wchar_t *name);

/* ================================================================ 执行器 */
typedef struct {
    wchar_t   *journal_path;
    int        stop_on_error;
    int        is_rollback;
    int        dry_run;
    int        cancelled;
    wchar_t   *extra_json;   /* 附加到 header 的 JSON 片段，可为 NULL */

    /* 进度回调 */
    void (*progress)(void *ud, int done, int total, const wchar_t *label);
    void  *progress_ud;
    int  (*cancel_check)(void *ud);
    void  *cancel_ud;
} ExecOptions;

typedef struct {
    int      ok;
    int      failed;
    int      skipped;
    int      total;
    int      cancelled;
    double   seconds;
    wchar_t *journal_path;
    wchar_t **errors;
    size_t    n_errors;
} RunResult;

void run_result_init(RunResult *r);
void run_result_free(RunResult *r);
int  executor_run(Plan *plan, const ExecOptions *opt, RunResult *result);

/* ================================================================ 回退 */
typedef struct {
    Plan    *plan;
    Arena   *arena;
    JournalDoc doc;
} RollbackPreview;

int  rollback_preview(const wchar_t *journal_path, Arena *a, RollbackPreview *out);
int  rollback_apply(const wchar_t *journal_path, const ExecOptions *opt,
                    RunResult *result);

/* ============================================================ 重命名规则 */
typedef struct {
    wchar_t *find;
    wchar_t *replace;
    int      use_regex;
    int      ignore_case;
    wchar_t *target;      /* stem / name / ext */
    wchar_t *template_;
    wchar_t *prefix;
    wchar_t *suffix;
    int      number;
    wchar_t *number_pos;  /* prefix / suffix / only */
    int      number_start;
    int      number_step;
    int      number_width;
    wchar_t *number_sep;
    wchar_t *case_mode;   /* keep / lower / upper / title */
    wchar_t *ext_case;
    Vec      include_ext;
    Vec      exclude_ext;
    int      recursive;
    int      skip_hidden;
    wchar_t *sort;        /* name / mtime / size / path */
    int      reverse;
    int      limit;
    int      files_only;
} RenameRule;

void rename_rule_init(RenameRule *r, Arena *a);
void rename_rule_free(RenameRule *r);
int  rename_rule_describe(const RenameRule *r, WStr *out, Arena *a);
int  rename_scan(const wchar_t *root, const RenameRule *r, FileList *out);
/* 计算新名字（不含目录），成功返回 0 */
int  rename_new_name(const wchar_t *path, int index, const RenameRule *r,
                     WStr *out, wchar_t *errbuf, size_t errcch);
Plan *rename_build_plan(Arena *a, const wchar_t *root, const RenameRule *r,
                        Policy policy, const wchar_t *trash_root);

/* ============================================================ 分类规则 */
typedef struct {
    wchar_t *template_;
    wchar_t *mode;         /* move / copy */
    int      recursive;
    int      skip_hidden;
    Vec      include_ext;
    Vec      exclude_ext;
    long long min_size;
    long long max_size;
    int      older_than_days;
    int      newer_than_days;
    int      skip_already_sorted;
    int      limit;
    int      files_only;
} OrganizeRule;

/* ------------------------------------------------------------------ 文件夹监控
 *
 * 监控**只负责发现**，把变化的文件放进队列；要不要处理由用户点一下再说。
 *
 * 这么设计是因为"实时监控"的常见形态（监控到变化就自动处理）和本项目的
 * 核心承诺"先预览、再执行、可回退"是直接冲突的：自动处理意味着用户
 * 还没看过预览，文件就已经被动了。
 *
 * 也就是说：**监控永远不会自己动你的文件。** 队列满、程序关了、
 * 用户没看，都只是"这些还没处理"，不会造成任何意外改动。
 */
typedef struct Watcher Watcher;

/* 事件类型 */
#define WK_ADDED    0
#define WK_MODIFIED 1
#define WK_REMOVED  2
#define WK_RENAMED  3

Watcher *watch_start(const wchar_t *dir, int recursive, wchar_t *err, size_t errcch);
void     watch_stop(Watcher *w);

size_t    watch_count(Watcher *w);
long long watch_dropped(Watcher *w);
const wchar_t *watch_root(Watcher *w);

int  watch_item_at(Watcher *w, size_t i, const wchar_t **rel, int *kind,
                   long long *when, long long *count);

/* 把队列拷出来并清空（只含新增/修改的项）。调用方负责释放每个字符串和数组本身。 */
int  watch_take_all(Watcher *w, wchar_t ***paths, size_t *n_out,
                    long long *dropped_out);
void watch_clear(Watcher *w);

/* 登记"这个路径是我们自己动的"，在 ttl_sec 秒内不再记进队列。
   用按路径抑制而不是"静默 N 秒"：静默期间用户真改了别的文件会漏掉，
   按路径抑制只挡住我们动过的那些，没有盲区。 */
void watch_suppress(Watcher *w, const wchar_t *abs_path, int ttl_sec);
/* ------------------------------------------------------------------ 表格处理
 *
 * xlsx / csv 的读取、筛选、排序、合并、导出。
 *
 * 现有的 xlsx 读取（extract.c）是给摘要和去重用的 —— 它把整张表摊平成
 * 一段文本。表格处理要的是**单元格结构**，所以另写一套。
 *
 * 日期是最容易踩的坑：xlsx 里日期根本不存在，它是"一个数字 + 一个格式"。
 * 单元格存的是 45000，靠样式告诉 Excel 显示成 2023-03-15。只读数字的话，
 * 用户拿到的表格里所有日期都变成天书。所以这里会去读 xl/styles.xml。
 *
 * 输出只管 CSV，不写 xlsx。理由：CSV 到处都能打开；写 xlsx 要自己拼一套
 * OOXML（共享字符串、样式、列宽），出错方式多而收益只是"双击能直接打开"。
 * 与其做个半吊子的 xlsx 写出器，不如把 CSV 做扎实 —— 尤其是编码：
 * 中文 Excel 默认按 GBK 打开 CSV，写成不带 BOM 的 UTF-8 会满屏乱码。
 */
typedef struct {
    wchar_t **cells;     /* 行优先，每行 ncols 个 */
    size_t    nrows, ncols;
} Table;

void table_init(Table *t);
void table_free(Table *t);
void table_set(Table *t, size_t r, size_t c, const wchar_t *val);
const wchar_t *table_get(const Table *t, size_t r, size_t c);

int  table_read_xlsx(const wchar_t *path, Table *t, wchar_t *err, size_t errcch);
int  table_read_csv(const wchar_t *path, Table *t, wchar_t *err, size_t errcch);
/* 按扩展名自动选读取器（xlsx / csv） */
int  load_table_pub(const wchar_t *path, Table *t, wchar_t *err, size_t errcch);

/* utf8_bom=1 写带 BOM 的 UTF-8（Excel 认得出）；0 写 GBK（老工具用） */
int  table_write_csv(const Table *t, const wchar_t *path, int utf8_bom,
                     wchar_t *err, size_t errcch);

int  table_sort(Table *t, int col, int numeric, int desc, int has_header);
int  table_select_cols(Table *t, const int *cols, size_t ncols);
int  table_filter(Table *t, int col, const wchar_t *op, const wchar_t *val2,
                  int has_header);
int  table_append(Table *dst, const Table *src, int skip_header, int align_header);

/* 生成器：tbl:<hex>，里面 hex 编码一段 key=value 参数文本。
   整体编码而不是逐段拼 —— 参数里有路径和用户填的筛选词，
   逐段拼就得给每个字段定一套转义规则，太容易出错。 */
int  table_make_spec(const wchar_t *params, wchar_t *out, size_t cch);
int  table_generate(const wchar_t *spec, const wchar_t *src, const wchar_t *dst,
                    wchar_t *err, size_t errcch);
/* ------------------------------------------------------------------ 备份 / 同步
 *
 * 这个模块**不直接搬文件**，只产出"从哪复制到哪"的清单，
 * 再由上层变成普通的 COPY 动作送进计划 —— 于是预览、冲突检测、
 * 一键回退全是现成的，不用另写一套。
 *
 * 名字叫"同步（只增不删）"而不是"镜像"：真正的镜像工具会删掉目标里
 * 多余的文件，而这个程序承诺过"没有任何不可逆的删除"。
 * 一次误操作把目标设成 D:\ 就会清掉整个盘，这种破坏不该藏在一个复选框里。
 */
typedef struct {
    wchar_t  *src;
    wchar_t  *dst;
    wchar_t  *rel;        /* 相对路径，给界面显示 */
    long long size;
    int       state;      /* 0=新增 1=更新 2=已相同 */
} BackupItem;

typedef struct {
    BackupItem *items;
    size_t      n, cap;
    long long   total_bytes;   /* 需要复制的总字节数 */
    long long   n_new;
    long long   n_diff;
    long long   n_same;
    long long   scanned;
    long long   skipped_ext;
    long long   skipped_size;
} BackupScan;

/* 只读分析源和目标，算出哪些文件需要复制。
   strict=0 时按"体积 + 修改时间"判断（快，和 robocopy 一致）；
   strict=1 时算内容哈希（慢，但能抓住体积相同内容不同的情况）。 */
int  backup_scan(const wchar_t *src_root, const wchar_t *dst_root,
                 const wchar_t *include, const wchar_t *exclude,
                 int recursive, int skip_hidden, int strict, long long max_bytes,
                 BackupScan *out, wchar_t *err, size_t errcch);
void backup_scan_free(BackupScan *sc);
int  backup_item_at(const BackupScan *sc, size_t i, const wchar_t **src,
                    const wchar_t **dst, const wchar_t **rel,
                    long long *size, int *state);

/* 复制后校验：体积一致且内容哈希一致。
   返回 0 = 一致，1 = 不一致，-1 = 读不了。
   复制出错不一定报错 —— 磁盘满、网盘掉线都可能"成功"写出一个长度不对的文件。
   备份最怕的不是抄错，是抄错了还不知道。 */
int  backup_verify(const wchar_t *src, const wchar_t *dst);
/* ------------------------------------------------------------------ 媒体元数据
 *
 * 从图片里读 EXIF、从音频里读 ID3，用于按拍摄时间 / 艺术家重命名。
 *
 * 所有字段都是宽字符串，读不到就是空串 —— 调用方只要判断 out[0]，
 * 不必再去区分"读失败"和"这个字段本来就没有"。
 *
 * 为什么手写解析器：项目定的是零第三方库、目标机不装任何东西。
 * 这两个格式只用得到一小部分字段，自己解反而可控：
 * 读到哪算哪，遇到畸形文件当没有，绝不会崩。
 */
typedef struct {
    int      has_exif;
    wchar_t  exif_date[64];      /* 原始形式 "2026:01:15 14:30:22"；格式化交给 meta_lookup */
    wchar_t  exif_make[64];
    wchar_t  exif_model[64];
    wchar_t  exif_lens[64];
    wchar_t  exif_iso[16];
    wchar_t  exif_fnum[16];      /* 2.8（不含 F） */
    wchar_t  exif_exposure[24];  /* 1/250 或 2.5 */
    wchar_t  exif_focal[16];     /* 50（不含 mm） */
    wchar_t  exif_w[16];
    wchar_t  exif_h[16];
    wchar_t  exif_orient[16];

    int      has_id3;
    wchar_t  id3_title[128];
    wchar_t  id3_artist[128];
    wchar_t  id3_album[128];
    wchar_t  id3_year[16];
    wchar_t  id3_track[32];
    wchar_t  id3_genre[64];
} MediaMeta;

/* 读一个文件的元数据。
   返回 0 = 读到了元数据；1 = 读到了文件但里面没有元数据；-1 = 打不开。 */
int  meta_read(const wchar_t *path, MediaMeta *out);

/* 按命名空间取值。ns 是 "exif" 或 "id3"，key 是字段名（大小写不敏感）。
   fmt 只对日期类字段有意义（如 "%Y%m%d"）。
   命中返回可用的字符串指针，没有这个字段返回 NULL。 */
const wchar_t *meta_lookup(const MediaMeta *m, const wchar_t *ns,
                           const wchar_t *key, const wchar_t *fmt,
                           wchar_t *buf, size_t cch);

/* 按扩展名粗略判断这个文件有没有可能带元数据（给界面提示用） */
int  meta_ext_maybe(const wchar_t *path);

void organize_rule_init(OrganizeRule *r, Arena *a);
void organize_rule_free(OrganizeRule *r);
const wchar_t *organize_category_of(const wchar_t *path);
const wchar_t *organize_size_bucket(long long bytes);
int  organize_scan(const wchar_t *root, const OrganizeRule *r, FileList *out);
int  organize_target_rel(const wchar_t *path, const OrganizeRule *r,
                         WStr *out, wchar_t *errbuf, size_t errcch);
/* preset 非空时只处理这些文件，不扫描目录（给文件夹监控用） */
Plan *organize_build_plan_ex(Arena *a, const wchar_t *root, const OrganizeRule *r,
                             Policy policy, const wchar_t *trash_root,
                             FileList *preset);
Plan *organize_build_plan(Arena *a, const wchar_t *root, const OrganizeRule *r,
                          Policy policy, const wchar_t *trash_root);

typedef struct {
    wchar_t *category;
    int      count;
    long long size;
} CategoryStat;

/* ------------------------------------------------------------------ 自定义分类
 *
 * 配置放在数据目录的「分类规则.ini」，用户可以自己加 [分类名] 段落，
 * 用 ext（扩展名）/ name（文件名关键词）/ name_regex（正则）来定义。
 * 一个文件只归一类：按 order 从小到大试，第一个命中的生效；
 * 都没命中就退回内置规则。
 *
 * 选 INI 而不是 JSON，是因为这个文件是给**人**手改的：
 * 一行一条、可以写注释、格式错了一眼能看出是哪一行。
 */
typedef struct CategorySet CategorySet;

/* 取全局配置（首次调用时自动载入；配置文件不存在会自动生成带注释的模板） */
CategorySet *categories_get(void);

/* 重新载入。err 收致命错误，warn 收"某一行有问题但已跳过"的提示。 */
int  categories_reload(wchar_t *err, size_t errcch,
                       wchar_t *warn, size_t warncch, int *created);

/* 配置文件路径（给界面上的"打开配置文件"用） */
const wchar_t *categories_config_path(void);

/* 按自定义规则匹配。命中返回分类名（静态生命周期）；没命中返回 NULL。 */
const wchar_t *categories_match(const wchar_t *path);

/* 给界面看的快照 */
size_t categories_count(void);

/* 取第 idx 条的完整信息。exts / kws 会用「、」拼成一句话，方便界面直接显示。
   任何一项传 NULL 就跳过。返回 -1 表示 idx 越界。 */
int    categories_detail(size_t idx, wchar_t *name, size_t ncch,
                         wchar_t *exts, size_t ecch,
                         wchar_t *kws, size_t kcch,
                         wchar_t *regex_src, size_t rcch, int *order);

int organize_category_stats(const wchar_t *root, int recursive, Arena *a,                            CategoryStat **out, size_t *n_out,
                            int *total_count, long long *total_size);

/* ================================================================== 正则 */
typedef struct LcRegex LcRegex;

LcRegex *regex_compile(const wchar_t *pattern, int ignore_case, wchar_t *err,
                       size_t errcch);
void     regex_free(LcRegex *re);
/* 替换所有匹配。repl 支持 \0..\9 反向引用。返回 0 成功 */
int      regex_replace_all(LcRegex *re, const wchar_t *input,
                           const wchar_t *repl, WStr *out);
int      regex_matches(LcRegex *re, const wchar_t *input);

/* 统计匹配次数（预览阶段用）。出错返回 -1。
   与 regex_replace_all 用同一套推进规则，保证"预览说几处、执行就改几处"。 */
int      regex_count_matches(LcRegex *re, const wchar_t *input);

/* ==================================================== 文本批量查找替换 ==
 *
 * 这是第一个"改文件内容"的模块（前几个改的都是文件名和位置）。
 *
 * 内容改动的回退：拆成两条原子步骤，让逆序回退天然正确 ——
 *     正向：  ① MOVE(原文件 → 回收站)   ② CREATE(原文件)
 *     回退：  ②' 删掉改过的版本        ①' 把原件搬回来
 * 所以回退引擎一行都不用改。
 * ========================================================================== */

/* 一条命中的样例，用来在界面上展示"改前 / 改后" */
typedef struct {
    wchar_t *file;
    int      line;        /* 行号，从 1 开始 */
    wchar_t *before;      /* 原文那一行（去掉换行、过长截断） */
    wchar_t *after;       /* 替换后那一行 */
} FindRepHit;

typedef struct {
    wchar_t  *path;
    int       encoding;      /* 见 findrep.c 里的 ENC_* */
    long long bytes;
    int       matches;
    int       selected;
} FindRepFile;

typedef struct {
    FindRepFile *files;  size_t n_files, cap_files;
    FindRepHit  *hits;   size_t n_hits,  cap_hits;
    long long    total_matches;
    long long    scanned;
    long long    skipped_binary;   /* 判定为二进制，跳过 */
    long long    skipped_big;      /* 超过体积上限 */
    long long    skipped_ext;      /* 扩展名不在文本名单里 */
    long long    unreadable;
} FindRepResult;

typedef struct {
    const wchar_t *find;
    const wchar_t *replace;
    int            use_regex;
    int            ignore_case;
    int            recursive;
    const wchar_t *include;     /* 逗号分隔的扩展名白名单；空 = 用内置文本名单 */
    const wchar_t *exclude;     /* 逗号分隔的扩展名黑名单 */
    int            any_ext;     /* 1 = 不限扩展名（靠二进制检测兜底） */
    long long      max_bytes;   /* 0 = 默认 64MB */
    int            limit;
    int            sample_per_file;
} FindRepOpts;

/* 只读扫描：找出所有有匹配的文件。不改任何东西。 */
int  findrep_scan(const wchar_t *dir, const FindRepOpts *o,
                  FindRepResult *res, wchar_t *err, size_t errcch);
void findrep_free(FindRepResult *r);

/* 构造生成器规格：edit:<flags>:<hexfind>:<hexreplace> */
int  findrep_make_spec(const FindRepOpts *o, wchar_t *out, size_t cch);

/* 执行一次替换。src = 回收站里的原始副本，dst = 原路径。
   executor 在 STEP_CREATE 里调它。 */
int  findrep_apply(const wchar_t *spec, const wchar_t *src,
                   const wchar_t *dst, wchar_t *err, size_t errcch);

/* ================================================================== JSON */
/* --- 写 --- */
void json_escape_to(Str *out, const wchar_t *w);
void json_escape_utf8_to(Str *out, const char *u);
void json_escape_char_to(Str *out, char c);
void json_escape_wchar_to(Str *out, wchar_t c);

/* --- 读 --- */
JsonValue *json_parse(const char *utf8, Arena *a);
void       json_free(JsonValue *v);

int         json_is_null(const JsonValue *v);
int         json_is_object(const JsonValue *v);
int         json_is_array(const JsonValue *v);
const char *json_string(const JsonValue *v, const char *fallback);
int         json_bool(const JsonValue *v, int fallback);
int         json_int(const JsonValue *v, int fallback);
long long   json_llong(const JsonValue *v, long long fallback);
double      json_double(const JsonValue *v, double fallback);
JsonValue  *json_get(const JsonValue *obj, const char *key);
size_t      json_array_len(const JsonValue *v);
JsonValue  *json_at(const JsonValue *v, size_t i);

/* ================================================================== HTTP */
typedef struct HttpServer HttpServer;

/* API 回调：返回 JSON 结果（写入 out），或填 err 表示失败。
   返回 0 表示成功。 */
typedef int (*ApiHandler)(void *ud, const char *method,
                          const JsonValue *params,
                          Str *out_json, Str *err_msg);

typedef struct {
    ApiHandler handler;
    void      *ud;
    /* 静态资源目录（可为 NULL，则用内嵌资源） */
    const wchar_t *web_dir;
} HttpConfig;

HttpServer *http_start(const HttpConfig *cfg, int port, wchar_t *err, size_t errcch);
void        http_stop(HttpServer *s);
int         http_port(HttpServer *s);
const char *http_token(HttpServer *s);

/* 内嵌界面资源 */
const char *webui_asset(const char *name, size_t *len_out);

/* ================================================================== 启动 */
int  launcher_open_window(const wchar_t *url, const wchar_t *browser,
                         int strategy, wchar_t *how, size_t howcch,
                         DWORD *pid_out);
/* 界面是否真的连上来过（页面加载成功就会调 app.info） */
int  api_ui_seen(void *st);
int  api_call_count(void *st);   /* 诊断用：收到了多少次接口调用 */

/* 自检：程序自己用 socket 连一次自己的回环服务。
   用来区分「服务端没起来」和「浏览器/代理连不上」。返回 1 = 通。 */
int  lc_probe_loopback(int port, wchar_t *why, size_t whycch);

/* 把一段文本放进剪贴板（自动打开浏览器失败时，用户一个 Ctrl+V 就能用） */
int  lc_copy_to_clipboard(const wchar_t *text);
/* ================================================================== 原生启动窗 */

/* 原生 Win32 启动窗口：显示地址、后台尝试打开浏览器、汇报是否连上。
   只用 user32/gdi32，不依赖任何浏览器，创建出来就一定可见。
   opener      : 尝试打开浏览器（后台线程里调用），返回 0 表示已发起
   is_connected: 界面是否已经连上来（用来把状态改成"已打开"）
   stop        : 置位后窗口自行关闭
   返回 0 正常退出。 */
int  lc_launcher_window(const wchar_t *url,
                        int (*opener)(const wchar_t *url, wchar_t *how,
                                      size_t cch, void *ud),
                        void *ud,
                        int (*is_connected)(void *ud),
                        volatile LONG *stop);
void lc_hide_console(void);
int  lc_pick_folder(const wchar_t *initial, wchar_t *out, size_t cch);
int  lc_pick_open_file(const wchar_t *initial, wchar_t *out, size_t cch);
/* 写一份启动信息到数据目录：窗口没能打开时，用户可以从这里拿到地址 */
void lc_write_startup_note(const wchar_t *url, int ok, const wchar_t *how);
/* 当前进程是否以提升的权限（管理员）运行 */
int  lc_is_elevated(void);
int  lc_pick_save_file(const wchar_t *initial, const wchar_t *filename,
                       wchar_t *out, size_t cch);

/* ================================================================== 服务 */
typedef struct ApiState ApiState;
/* 原生界面：创建主窗口并跑消息循环。不监听端口、不启动浏览器。 */
int       lc_app_run(ApiState *api);
/* WebView2 宿主：在原生窗口里用 WebView2 渲染内嵌的 HTML 界面。
   不启动外部浏览器、不监听端口。返回 0 正常退出；-1 表示 WebView2 不可用
   （此时 lc_webview_note() 里有中文原因）。 */
int         lc_webview_run(ApiState *api, const wchar_t *token);
const wchar_t *lc_webview_note(void);

/* 取嵌在 exe 里的应用图标（见 src/app.rc）。
   small = 1 取小图标（任务栏、标题栏），0 取大图标（Alt+Tab）。 */
HICON       lc_app_icon(int small);

/* 界面资源（由 tools/embed.c 生成） */
const char *webui_asset(const char *name, size_t *len_out);
ApiState *api_create(const wchar_t *web_dir);
void      api_destroy(ApiState *st);
int       api_dispatch(void *ud, const char *method, const JsonValue *params,
                       Str *out_json, Str *err_msg);
int       api_watchdog_tick(ApiState *st);
void      api_cancel_all(ApiState *st);

/* ================================================================== ZIP
 * 读：docx / xlsx / pptx 都是 zip 包；写：我们只用 STORED（无需压缩）
 * ================================================================== */
uint32_t lc_crc32(const void *data, size_t len);
int      lc_inflate(const unsigned char *src, size_t srclen,
                    unsigned char *dst, size_t dstcap, size_t *dstlen);

typedef struct LcZip LcZip;
LcZip *zip_open(const wchar_t *path);
void   zip_close(LcZip *z);
size_t zip_count(const LcZip *z);
const char *zip_name_at(const LcZip *z, size_t i);          /* UTF-8 */
int    zip_read_at(const LcZip *z, size_t i, unsigned char **out, size_t *outlen);
int    zip_read_by_name(const LcZip *z, const char *name_utf8,
                        unsigned char **out, size_t *outlen);

typedef struct LcZipWriter LcZipWriter;
LcZipWriter *zip_writer_create(void);
int  zip_writer_add(LcZipWriter *w, const char *name_utf8,
                    const void *data, size_t len);
int  zip_writer_save(LcZipWriter *w, const wchar_t *path);
int  zip_writer_data(LcZipWriter *w, unsigned char **out, size_t *outlen);
void zip_writer_free(LcZipWriter *w);

/* ================================================================== 文本
 * 分词与分句：中文按相邻二元组（免词典），拉丁文与数字按词。
 * ================================================================== */
typedef struct {
    wchar_t **items;
    size_t    n, cap;
    Arena    *arena;
} TokenList;

typedef struct {
    size_t *starts;
    size_t *ends;
    size_t  n, cap;
    Arena  *arena;
} SpanList;

void tokens_init(TokenList *t, Arena *a);
void tokens_free(TokenList *t);
/* 分词。返回 0 成功。 */
int  tokenize(const wchar_t *text, TokenList *out);
void spanlist_init(SpanList *s, Arena *a);
void spanlist_free(SpanList *s);
/* 分句：按 。！？；… 及 .!? 换行切分，span 是 [start, end) 下标区间 */
void split_sentences(const wchar_t *text, SpanList *out);

/* 原始字节 -> 宽字符串：自动识别 BOM / UTF-8 / GBK */
wchar_t *decode_text_bytes(const unsigned char *data, size_t len);

/* ============================================================ 文档抽取 */
/* 把文档抽成纯文本（malloc，调用方 free）。失败返回 NULL 并填 err。 */
wchar_t *extract_text(const wchar_t *path, size_t max_chars,
                      wchar_t *err, size_t errcch);
/* 该扩展名是否支持抽取 */
int extract_supported(const wchar_t *path);
/* 分类标签："文档" / "表格" / "演示" / "文本" / "网页" / ""（不支持） */
const wchar_t *extract_kind(const wchar_t *path);

/* ============================================================ SimHash */
typedef struct {
    uint64_t bits;
    int      tokens;
} SimHash;

SimHash simhash_text(const wchar_t *text);
SimHash simhash_tokens(const TokenList *t);
/* 汉明距离 0..64 */
int     simhash_distance(SimHash a, SimHash b);
/* 相似度 0.0..1.0 */
double  simhash_similarity(SimHash a, SimHash b);

/* ---- 去重 ---- */

typedef struct {
    wchar_t  *path;
    SimHash   hash;
    int       tokens;
    long long size;
    int       group;      /* 1..n_groups；-1 表示不重复 */
} DedupItem;

typedef struct {
    DedupItem *items;
    size_t     n;
    size_t     n_groups;
    int        skipped;        /* 无法抽取文本 / 太短而跳过的数量 */
    int        max_distance;   /* 由阈值换算出的汉明距离上限 */
    Arena     *arena;
    wchar_t    first_error[512];
} DedupResult;

typedef struct {
    double    threshold;    /* 0..1，相似度阈值，默认 0.92 */
    int       recursive;
    int       min_chars;    /* 文本短于这个长度不参与比较 */
    int       action;       /* 0 = 移入重复文件夹；1 = 移入回收站 */
    wchar_t  *target_dir;   /* action 0 时用的子目录名 */
    Vec       include_ext;
} DedupRule;

void dedup_rule_init(DedupRule *r, Arena *a);
const wchar_t *dedup_action_name(int action);
int  dedup_scan(const wchar_t *root, const DedupRule *r, DedupResult *out,
                wchar_t *err, size_t errcch);
void dedup_result_free(DedupResult *res);
Plan *dedup_build_plan(Arena *a, const wchar_t *root, const DedupResult *res,
                       const DedupRule *r, Policy policy, const wchar_t *trash_root);

/* ============================================================ 摘要 */
typedef struct {
    size_t start, end;   /* 原文字符区间，用于"可定位到原文" */
    double score;
} KeySentence;

typedef struct {
    KeySentence *sentences;   size_t n_sentences;
    wchar_t    **keywords;    size_t n_keywords;
    double      *keyword_scores;
    Arena       *arena;
} Summary;

/* 抽取式摘要。use_textrank!=0 用 TextRank，否则用 TF-IDF。
   max_sentences 为 0 时按正文长度自动决定（3~8 句）。 */
int summarize_text(const wchar_t *text, int max_sentences, int use_textrank,
                   Summary *out, Arena *arena);

/* ============================================================ 导出 */
/* Markdown 源文本 -> 目标格式。返回 0 成功。 */
int export_markdown_file(const wchar_t *md_text, const wchar_t *out_path,
                         wchar_t *err, size_t errcch);
int export_docx_file(const wchar_t *md_text, const wchar_t *out_path,
                     wchar_t *err, size_t errcch);
int export_pdf_file(const wchar_t *md_text, const wchar_t *out_path,
                    wchar_t *err, size_t errcch);
/* 由扩展名选择导出器 */
int export_auto(const wchar_t *md_text, const wchar_t *out_path,
                wchar_t *err, size_t errcch);

/* ============================================================ 格式转换 */
typedef struct {
    wchar_t *key;         /* pandoc / soffice / ffmpeg */
    wchar_t *path;
    int      found;
} ConvertEngine;

/* 探测外部引擎（只查本地，不联网）。n_out 个槽位。 */
void convert_probe_engines(ConvertEngine *out, size_t n_out);
void convert_engine_free(ConvertEngine *e);

/* 该 src -> dst 能否用**内置**方式完成（不依赖任何外部程序） */
int convert_builtin_supported(const wchar_t *src_path, const wchar_t *dst_path);

/* 转换可行性：2 = 内置可完成，1 = 需要外部引擎，0 = 这种组合不成立。
   详见 convert.c 里的实现说明 —— 这个判断曾经被算出来又丢掉，
   导致 .txt 也被列进"转成 PNG"的预览表。 */
int convert_feasibility(const wchar_t *src_path, const wchar_t *dst_path);

/* 内置/外部产物生成。generator 形如 "convert:png"、"export:docx"。
   返回 0 成功。这是 executor 在 CREATE 步骤里调用的唯一入口。 */
int lc_generate_artifact(const wchar_t *generator, const wchar_t *src,
                         const wchar_t *dst, wchar_t *err, size_t errcch);

#endif /* LC_H */
