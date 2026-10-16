/* ==========================================================================
 * pages.c —— 原生界面的各个功能页
 *
 * 每页是一个独立的子窗口（自己的窗口类 + 窗口过程），控件命令发给它自己，
 * 互不干扰。页面按需创建：切到哪一页才建哪一页。
 *
 * 所有数据都通过 ui_call() 走 api_dispatch，和原来浏览器走的是同一条后端路径。
 * ========================================================================== */
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lc.h"

/* ---- 来自 appui.c 的工具 ---- */
typedef struct { Str s; int n; } JB;
extern void jb_init(JB *);
extern void jb_str(JB *, const char *, const wchar_t *);
extern void jb_int(JB *, const char *, int);
extern void jb_bool(JB *, const char *, int);
extern void jb_dbl(JB *, const char *, double);
extern const char *jb_end(JB *);
extern void jb_free(JB *);

extern JsonValue *ui_call(Arena *, const char *, const char *, Str *);
extern const wchar_t *jws(Arena *, const JsonValue *, const char *);

extern HFONT ui_font(void), ui_font_ui(void), ui_font_title(void), ui_font_mono(void);
extern int   ui_sc(int);
extern HWND  ui_make(HWND, const wchar_t *, const wchar_t *, DWORD, int,int,int,int,int,HFONT);
extern HWND  ui_label(HWND, const wchar_t *, int,int,int,int);
extern HWND  ui_edit(HWND, int, int,int,int,int);
extern HWND  ui_edit_path(HWND, int, int,int,int,int);
extern HWND  ui_btn(HWND, int, const wchar_t *, int,int,int,int);
extern HWND  ui_check(HWND, int, const wchar_t *, int,int,int,int);
extern HWND  ui_combo(HWND, int, int,int,int,int,int);
extern void  ui_combo_add(HWND, int, const wchar_t *);
extern void  ui_get_text(HWND, int, wchar_t *, size_t);
extern void  ui_set_text(HWND, int, const wchar_t *);
extern int   ui_checked(HWND, int);
extern int   ui_combo_sel(HWND, int);
extern int   ui_pick_dir(HWND, int);
extern int   ui_pick_file(HWND, int);
extern void  ui_policy_fill(HWND, int);
extern const wchar_t *ui_policy_str(HWND, int);
extern void  ui_toggle_row(HWND, HWND, const wchar_t *);
extern void  ui_export_csv(HWND, const wchar_t *);
extern void  ui_info(HWND, const wchar_t *, const wchar_t *, ...);
extern void  ui_warn(HWND, const wchar_t *, ...);
extern void  ui_status(const wchar_t *, ...);
extern void  ui_goto(int);
extern void  lv_init(HWND);
extern void  lv_clear(HWND);
extern void  lv_col(HWND, int, const wchar_t *, int);
extern int   lv_row(HWND, const wchar_t *const *, int);

#define CLS_BG  (HBRUSH)(COLOR_BTNFACE + 1)

/* ==================================================== 共享：计划预览 ===== */

/*
 * 把 preview 返回的 plan 渲染进 ListView。
 * 返回行数；plan_id 写进 out_id。
 */
int lc_render_plan(HWND lv, JsonValue *r, wchar_t *out_id, size_t idcch) {
    lv_clear(lv);
    out_id[0] = 0;
    if (!r) return 0;


    JsonValue *rows = json_get(r, "rows");
    size_t n = json_array_len(rows);
    Arena *tmp = arena_new();

    for (size_t i = 0; i < n; ++i) {
        JsonValue *row = json_at(rows, i);
        const wchar_t *sel   = jws(tmp, row, "selected");
        const wchar_t *lvl   = jws(tmp, row, "level_text");
        const wchar_t *oldp  = jws(tmp, row, "old");
        const wchar_t *newp  = jws(tmp, row, "new");
        const wchar_t *note  = jws(tmp, row, "note");
        const wchar_t *exc   = json_bool(json_get(row, "executable"), 0)
                               ? L"" : L"（不会执行）";
        const wchar_t *cells[4];
        wchar_t lvlx[128];
        swprintf(lvlx, 128, L"%ls %ls", sel ? sel : L"", lvl ? lvl : L"");
        cells[0] = lvlx;
        cells[1] = oldp;
        cells[2] = newp;
        cells[3] = note;
        cells[3] = exc;
        lv_row(lv, cells, 4);
    }
    arena_free(tmp);
    return (int)n;
}

/* 轮询后台任务直到结束。返回 1 表示成功。 */
int lc_wait_job(HWND owner, const char *job_id, int *ok_out, int *fail_out) {
    for (int i = 0; i < 1800; ++i) {     /* 最多约 6 分钟 */
        JB b; jb_init(&b);
        jb_str(&b, "job_id", L"");
        /* job_id 是 ASCII，这里手工拼 */
        char params[128];
        snprintf(params, sizeof(params), "{\"job_id\":\"%s\"}", job_id);

        Arena *a = arena_new();
        Str err; str_init(&err);
        JsonValue *r = ui_call(a, "job.status", params, &err);

        int finished = 0, ok = 0, failed = 0, done = 0, total = 0;
        if (r) {
            finished = json_bool(json_get(r, "finished"), 0);
            done  = json_int(json_get(r, "done"), 0);
            total = json_int(json_get(r, "total"), 0);
            JsonValue *res = json_get(r, "result");
            if (res) {
                ok     = json_int(json_get(res, "ok"), 0);
                failed = json_int(json_get(res, "failed"), 0);
            }
        }
        str_free(&err);
        arena_free(a);
        jb_free(&b);

        if (finished) {
            if (ok_out) *ok_out = ok;
            if (fail_out) *fail_out = failed;
            return 1;
        }

        ui_status(L"正在执行 … %d / %d", done, total);
        MSG m;
        /* 边等边抽消息，界面不假死 */
        DWORD start = GetTickCount();
        while (GetTickCount() - start < 250) {
            while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            Sleep(10);
        }
    }
    ui_warn(owner, L"任务执行超时。");
    return 0;
}

/* ==================================================== 页 1：批量重命名 == */

#define R_ID_DIR     4001
#define R_ID_PICK    4002
#define R_ID_TPL     4003
#define R_ID_FIND    4004
#define R_ID_REPL    4005
#define R_ID_REGEX   4006
#define R_ID_ICASE   4007
#define R_ID_PREFIX  4008
#define R_ID_SUFFIX  4009
#define R_ID_TARGET  4010
#define R_ID_RECUR   4011
#define R_ID_NUM     4012
#define R_ID_NPOS    4013
#define R_ID_NSTART  4014
#define R_ID_NWIDTH  4015
#define R_ID_NSTEP   4016
#define R_ID_PREVIEW 4017
#define R_ID_EXEC    4018
#define R_ID_ALL     4019
#define R_ID_NONE    4020
#define R_ID_LV      4021
#define R_ID_INFO    4022
#define R_ID_ROLLBACK 4023
#define R_ID_POLICY 4024
#define R_ID_CSV    4025

static wchar_t g_rename_plan[64];

/*
 * 只有两个东西需要跟着窗口大小走：文件夹输入框和预览表。
 * 其余控件（含所有标签）在 WM_CREATE 里就摆在最终位置 ——
 * 标签是用 ui_label 建的（id = -1），GetDlgItem 找不到它们，
 * 在 WM_SIZE 里 MoveWindow 会静默失败，导致标签和输入框错位。
 */
static void rename_layout(HWND h) {
    RECT rc; GetClientRect(h, &rc);
    int W = rc.right - ui_sc(24);
    if (W < ui_sc(400)) W = ui_sc(400);

    /* 文件夹那一行：输入框跟着变宽，按钮贴在右边 */
    MoveWindow(GetDlgItem(h, R_ID_DIR), ui_sc(92), ui_sc(14),
               W - ui_sc(200), ui_sc(26), TRUE);
    MoveWindow(GetDlgItem(h, R_ID_PICK), ui_sc(92) + W - ui_sc(196), ui_sc(14),
               ui_sc(120), ui_sc(26), TRUE);
    MoveWindow(GetDlgItem(h, R_ID_TPL), ui_sc(92), ui_sc(50),
               W - ui_sc(96), ui_sc(26), TRUE);

    int y = ui_sc(284);
    int lvh = rc.bottom - y - ui_sc(12);
    if (lvh < ui_sc(80)) lvh = ui_sc(80);
    MoveWindow(GetDlgItem(h, R_ID_LV), ui_sc(12), y, W, lvh, TRUE);
}
static void rename_preview(HWND h) {
    wchar_t dir[1024], tpl[512], find[256], repl[256], pre[128], suf[128];
    wchar_t ns[32], nw[32], nst[32];
    ui_get_text(h, R_ID_DIR, dir, 1024);
    if (!dir[0]) { ui_warn(h, L"请先选择文件夹。"); return; }
    ui_get_text(h, R_ID_TPL, tpl, 512);
    ui_get_text(h, R_ID_FIND, find, 256);
    ui_get_text(h, R_ID_REPL, repl, 256);
    ui_get_text(h, R_ID_PREFIX, pre, 128);
    ui_get_text(h, R_ID_SUFFIX, suf, 128);
    ui_get_text(h, R_ID_NSTART, ns, 32);
    ui_get_text(h, R_ID_NWIDTH, nw, 32);
    ui_get_text(h, R_ID_NSTEP, nst, 32);

    int ti = ui_combo_sel(h, R_ID_TARGET); if (ti < 0) ti = 0;
    int np = ui_combo_sel(h, R_ID_NPOS);   if (np < 0) np = 0;

    JB b; jb_init(&b);
    jb_str(&b, "dir", dir);
    jb_str(&b, "template", tpl);
    jb_str(&b, "find", find);
    jb_str(&b, "replace", repl);
    jb_bool(&b, "regex", ui_checked(h, R_ID_REGEX));
    jb_bool(&b, "ignore_case", ui_checked(h, R_ID_ICASE));
    jb_str(&b, "prefix", pre);
    jb_str(&b, "suffix", suf);
    jb_bool(&b, "number", ui_checked(h, R_ID_NUM));
    jb_int(&b, "number_pos", np);
    jb_int(&b, "number_start", _wtoi(ns));
    jb_int(&b, "number_width", _wtoi(nw));
    jb_int(&b, "number_step", _wtoi(nst) ? _wtoi(nst) : 1);
    jb_int(&b, "target", ti);
    jb_bool(&b, "recursive", ui_checked(h, R_ID_RECUR));
    jb_str(&b, "policy", ui_policy_str(h, R_ID_POLICY));
    const char *params = jb_end(&b);

    Arena *a = arena_new();
    Str err; str_init(&err);
    ui_status(L"正在扫描 …");
    JsonValue *r = ui_call(a, "rename.preview", params, &err);

    if (!r) {
        ui_warn(h, L"生成预览失败：%hs", err.data ? err.data : "未知原因");
        ui_status(L"预览失败");
        str_free(&err); arena_free(a); jb_free(&b);
        return;
    }

    const wchar_t *pid = jws(a, r, "plan_id");
    wcsncpy(g_rename_plan, pid ? pid : L"", 63);
    g_rename_plan[63] = 0;

    HWND lv = GetDlgItem(h, R_ID_LV);
    int n = lc_render_plan(lv, r, g_rename_plan, 64);

    JsonValue *st = json_get(r, "stats");
    int ex  = json_int(json_get(st, "executable"), 0);
    int bl  = json_int(json_get(st, "blocked"), 0);
    int wn  = json_int(json_get(st, "warned"), 0);

    wchar_t info[256];
    swprintf(info, 256, L"共 %d 项 · 可执行 %d · 冲突未解决 %d · 需确认 %d", n, ex, bl, wn);
    ui_set_text(h, R_ID_INFO, info);
    ui_status(L"预览完成：%ls", info);

    str_free(&err); arena_free(a); jb_free(&b);
}

static void rename_exec(HWND h) {
    if (!g_rename_plan[0]) { ui_warn(h, L"请先生成预览。"); return; }

    wchar_t q[300];
    swprintf(q, 300,
        L"即将按预览表执行重命名。\n\n"
        L"· 执行过程会逐步写入回退日志\n"
        L"· 之后可以在「回退历史」里一键还原\n\n"
        L"确定执行吗？");
    if (MessageBoxW(h, q, L"确认执行", MB_ICONQUESTION | MB_YESNO) != IDYES) return;

    char params[128];
    snprintf(params, sizeof(params), "{\"plan_id\":\"%ls\"}", g_rename_plan);

    Arena *a = arena_new();
    Str err; str_init(&err);
    JsonValue *r = ui_call(a, "exec.run", params, &err);
    if (!r) {
        ui_warn(h, L"执行失败：%hs", err.data ? err.data : "未知原因");
        str_free(&err); arena_free(a);
        return;
    }
    const char *jid = json_string(json_get(r, "job_id"), "");
    str_free(&err); arena_free(a);

    if (!jid[0]) { ui_warn(h, L"没能启动任务。"); return; }

    int ok = 0, fail = 0;
    if (lc_wait_job(h, jid, &ok, &fail)) {
        ui_status(L"执行完成：成功 %d 项，失败 %d 项", ok, fail);
        ui_info(h, L"执行完成",
                L"成功 %d 项，失败 %d 项。\n\n可以在「回退历史」里一键还原。", ok, fail);
        g_rename_plan[0] = 0;
    }
}

static void rename_rollback(HWND h) {
    /* 取最近一条日志，确认后整体回退 */
    Arena *a = arena_new();
    Str err; str_init(&err);
    JsonValue *r = ui_call(a, "history.list", "{\"limit\":1}", &err);
    if (!r) { ui_warn(h, L"读取历史失败。"); str_free(&err); arena_free(a); return; }

    JsonValue *rows = json_get(r, "rows");
    if (json_array_len(rows) == 0) {
        ui_warn(h, L"没有可回退的历史记录。");
        str_free(&err); arena_free(a); return;
    }
    JsonValue *row0 = json_at(rows, 0);
    const wchar_t *path = jws(a, row0, "path");
    const wchar_t *name = jws(a, row0, "name");

    wchar_t q[900];
    swprintf(q, 900,
        L"要回退最近一次操作吗？\n\n日志：%ls\n\n"
        L"回退会按逆序把每一步还原。原位置若已被别的文件占用，"
        L"那一项会被跳过，不会覆盖任何东西。", name);
    if (MessageBoxW(h, q, L"确认回退", MB_ICONQUESTION | MB_YESNO) != IDYES) {
        str_free(&err); arena_free(a); return;
    }

    JB b; jb_init(&b);
    jb_str(&b, "path", path);
    JsonValue *rr = ui_call(a, "history.rollback_run", jb_end(&b), &err);
    jb_free(&b);
    str_free(&err); arena_free(a);

    if (rr) ui_info(h, L"回退完成", L"已按日志还原。");
    else    ui_warn(h, L"回退失败。");
}

LRESULT CALLBACK lc_page_rename(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        ui_label(h, L"文件夹", 12, 20, 76, 20);
        ui_edit_path(h, R_ID_DIR, 92, 14, 400, 26);
        ui_btn(h, R_ID_PICK, L"选择文件夹", 500, 14, 120, 26);

        ui_label(h, L"命名模板", 12, 56, 76, 20);
        ui_edit(h, R_ID_TPL, 92, 50, 600, 26);
        ui_label(h, L"可用：{name} 原名 · {n:03d} 序号 · {ext} 扩展名 · {date} {year} {month} {day}",
                 92, 80, 700, 20);

        ui_label(h, L"查找", 12, 114, 46, 20);
        ui_edit(h, R_ID_FIND, 62, 108, 200, 26);
        ui_label(h, L"替换为", 280, 114, 56, 20);
        ui_edit(h, R_ID_REPL, 340, 108, 200, 26);

        ui_check(h, R_ID_REGEX, L"正则表达式", 92, 142, 110, 22);
        ui_check(h, R_ID_ICASE, L"忽略大小写",   210, 142, 130, 22);
        ui_check(h, R_ID_RECUR, L"包含子文件夹", 350, 142, 140, 22);

        ui_label(h, L"前缀", 12, 176, 46, 20);
        ui_edit(h, R_ID_PREFIX, 62, 170, 150, 26);
        ui_label(h, L"后缀", 224, 176, 46, 20);
        ui_edit(h, R_ID_SUFFIX, 274, 170, 150, 26);
        ui_label(h, L"作用对象", 436, 176, 56, 20);
        ui_combo(h, R_ID_TARGET, 496, 170, 150, 200, 0);
        ui_combo_add(h, R_ID_TARGET, L"主名（不含扩展名）");
        ui_combo_add(h, R_ID_TARGET, L"完整文件名");
        ui_combo_add(h, R_ID_TARGET, L"扩展名");
        SendMessageW(GetDlgItem(h, R_ID_TARGET), CB_SETCURSEL, 0, 0);

        ui_check(h, R_ID_NUM, L"追加序号", 92, 208, 110, 22);
        ui_label(h, L"位置", 210, 210, 34, 20);
        ui_combo(h, R_ID_NPOS, 246, 204, 96, 200, 0);
        ui_combo_add(h, R_ID_NPOS, L"后缀");
        ui_combo_add(h, R_ID_NPOS, L"前缀");
        SendMessageW(GetDlgItem(h, R_ID_NPOS), CB_SETCURSEL, 0, 0);
        ui_label(h, L"起始", 350, 210, 34, 20);
        ui_edit(h, R_ID_NSTART, 386, 204, 52, 26);
        ui_set_text(h, R_ID_NSTART, L"1");
        ui_label(h, L"位数", 446, 210, 34, 20);
        ui_edit(h, R_ID_NWIDTH, 482, 204, 52, 26);
        ui_set_text(h, R_ID_NWIDTH, L"3");
        ui_label(h, L"步长", 542, 210, 34, 20);
        ui_edit(h, R_ID_NSTEP, 578, 204, 52, 26);
        ui_set_text(h, R_ID_NSTEP, L"1");

        ui_btn(h, R_ID_PREVIEW, L"生成预览", 12, 248, 120, 32);
        ui_btn(h, R_ID_ALL,  L"全选",   142, 248, 80, 32);
        ui_btn(h, R_ID_NONE, L"全不选", 228, 248, 80, 32);
        ui_btn(h, R_ID_EXEC, L"执行",   320, 248, 110, 32);
        ui_btn(h, R_ID_ROLLBACK, L"回退上次执行", 440, 248, 150, 32);
        ui_btn(h, R_ID_CSV, L"导出 CSV", 600, 248, 110, 32);
        ui_label(h, L"重名时", 726, 254, 56, 20);
        ui_make(h, L"STATIC", L"", SS_LEFT, 12, 288, 900, 20, R_ID_INFO, ui_font_ui());
        ui_combo(h, R_ID_POLICY, 786, 248, 170, 200, 0);
        ui_policy_fill(h, R_ID_POLICY);

        {
            HWND lv = ui_make(h, WC_LISTVIEWW, L"",
                              LVS_REPORT | LVS_SINGLESEL | WS_BORDER | WS_TABSTOP,
                              12, 284, 900, 300, R_ID_LV, ui_font());
            lv_init(lv);
            lv_col(lv, 0, L"状态", 110);
            lv_col(lv, 1, L"原名", 300);
            lv_col(lv, 2, L"新名", 300);
            lv_col(lv, 3, L"说明", 330);
        }
        ui_status(L"选择文件夹后点「生成预览」，这个阶段完全只读。");
        return 0;

    case WM_SIZE:
        rename_layout(h);
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(w);
        switch (id) {
        case R_ID_PICK:    ui_pick_dir(h, R_ID_DIR); break;
        case R_ID_PREVIEW: rename_preview(h); break;
        case R_ID_EXEC:    rename_exec(h); break;
        case R_ID_ROLLBACK: rename_rollback(h); break;
        case R_ID_CSV: ui_export_csv(h, g_rename_plan); break;
        case R_ID_ALL:
        case R_ID_NONE: {
            if (!g_rename_plan[0]) break;
            char params[160];
            snprintf(params, sizeof(params),
                     "{\"plan_id\":\"%ls\",\"selected\":%s}",
                     g_rename_plan, id == R_ID_ALL ? "true" : "false");
            Arena *a = arena_new(); Str err; str_init(&err);
            JsonValue *r = ui_call(a, "plan.set_all", params, &err);
            if (r) {
                HWND lv = GetDlgItem(h, R_ID_LV);
                JsonValue *rows = json_get(r, "rows");
                size_t n = json_array_len(rows);
                Arena *t = arena_new();
                for (size_t i = 0; i < n; ++i) {
                    const wchar_t *s = jws(t, json_at(rows, i), "selected");
                    ListView_SetItemText(lv, (int)i, 0, (LPWSTR)(s ? s : L""));
                }
                arena_free(t);
            }
            str_free(&err); arena_free(a);
            break;
        }
        default: break;
        }
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}
