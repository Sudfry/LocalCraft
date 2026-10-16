/* ==========================================================================
 * pages2.c —— 原生界面：整理分类 / 格式转换 / 内容去重 / 文档摘要 / 回退历史 / 离线自检
 *
 * 每页是一个独立子窗口。控件在 WM_CREATE 里按固定位置建好，
 * WM_SIZE 时只把主要区域拉满，布局逻辑简单、不易出错。
 * ========================================================================== */
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lc.h"

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
extern void  lv_init(HWND);
extern void  lv_clear(HWND);
extern void  lv_col(HWND, int, const wchar_t *, int);
extern int   lv_row(HWND, const wchar_t *const *, int);

/* 由 pages.c 提供 */
extern int   lc_render_plan(HWND, JsonValue *, wchar_t *, size_t);
extern int   lc_wait_job(HWND, const char *, int *, int *);

/* 通用的「预览 → 执行」流程 */
static void generic_exec(HWND h, const wchar_t *plan_id, const wchar_t *what) {
    if (!plan_id || !plan_id[0]) { ui_warn(h, L"请先生成预览。"); return; }

    wchar_t q[400];
    swprintf(q, 400,
        L"即将执行%ls。\n\n"
        L"· 过程会逐步写入回退日志\n"
        L"· 之后可在「回退历史」里一键还原\n\n确定吗？", what);
    if (MessageBoxW(h, q, L"确认执行", MB_ICONQUESTION | MB_YESNO) != IDYES) return;

    char params[160];
    snprintf(params, sizeof(params), "{\"plan_id\":\"%ls\"}", plan_id);

    Arena *a = arena_new(); Str err; str_init(&err);
    JsonValue *r = ui_call(a, "exec.run", params, &err);
    if (!r) {
        ui_warn(h, L"执行失败：%hs", err.data ? err.data : "未知原因");
        str_free(&err); arena_free(a); return;
    }
    char jid[64];
    snprintf(jid, sizeof(jid), "%s", json_string(json_get(r, "job_id"), ""));
    str_free(&err); arena_free(a);
    if (!jid[0]) { ui_warn(h, L"没能启动任务。"); return; }

    int ok = 0, fail = 0;
    if (lc_wait_job(h, jid, &ok, &fail)) {
        ui_status(L"%ls完成：成功 %d 项，失败 %d 项", what, ok, fail);
        ui_info(h, L"完成", L"%ls完成。\n\n成功 %d 项，失败 %d 项。\n\n可在「回退历史」里还原。",
                what, ok, fail);
    }
}

/* ==================================================== 页 2：整理分类 == */

#define O_ID_DIR   4101
#define O_ID_PICK  4102
#define O_ID_MODE  4103
#define O_ID_RECUR 4104
#define O_ID_TPL   4105
#define O_ID_OLD   4106
#define O_ID_AGE   4107
#define O_ID_PREV  4108
#define O_ID_EXEC  4109
#define O_ID_LV    4110
#define O_ID_INFO  4111
#define O_ID_POLICY 4112

static wchar_t g_org_plan[64];

LRESULT CALLBACK lc_page_organize(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        ui_label(h, L"文件夹", 12, 20, 60, 20);
        ui_edit_path(h, O_ID_DIR, 80, 14, 500, 26);
        ui_btn(h, O_ID_PICK, L"选择文件夹", 590, 14, 120, 26);

        ui_label(h, L"分类方式", 12, 58, 66, 20);
        ui_combo(h, O_ID_MODE, 80, 52, 250, 200, 0);
        ui_combo_add(h, O_ID_MODE, L"按类型（文档 / 图片 / 音视频…)");
        ui_combo_add(h, O_ID_MODE, L"按年份");
        ui_combo_add(h, O_ID_MODE, L"按扩展名");
        ui_combo_add(h, O_ID_MODE, L"按文件首字母");
        ui_combo_add(h, O_ID_MODE, L"按体积分层");
        SendMessageW(GetDlgItem(h, O_ID_MODE), CB_SETCURSEL, 0, 0);
        ui_check(h, O_ID_RECUR, L"包含子文件夹", 346, 56, 140, 22);

        ui_label(h, L"目标模板", 12, 96, 66, 20);
        ui_edit(h, O_ID_TPL, 80, 90, 250, 26);
        ui_set_text(h, O_ID_TPL, L"{category}");
        ui_label(h, L"可用：{category} {year} {ext} {letter} {size}",
                 342, 96, 420, 20);

        ui_check(h, O_ID_OLD, L"只处理", 80, 128, 66, 22);
        ui_edit(h, O_ID_AGE, 150, 122, 70, 26);
        ui_set_text(h, O_ID_AGE, L"30");
        ui_label(h, L"天未修改过的文件", 228, 128, 160, 20);

        ui_label(h, L"重名时", 406, 168, 56, 20);
        ui_combo(h, O_ID_POLICY, 466, 162, 170, 200, 0);
        ui_policy_fill(h, O_ID_POLICY);
        ui_btn(h, O_ID_PREV, L"生成预览", 12, 162, 120, 32);
        ui_btn(h, O_ID_EXEC, L"执行",     142, 162, 110, 32);
        ui_label(h, L"", 268, 168, 500, 22);

        {
            HWND lv = ui_make(h, WC_LISTVIEWW, L"",
                              LVS_REPORT | LVS_SINGLESEL | WS_BORDER,
                              12, 204, 900, 300, O_ID_LV, ui_font());
            lv_init(lv);
            lv_col(lv, 0, L"状态", 110);
            lv_col(lv, 1, L"原位置", 340);
            lv_col(lv, 2, L"将移动到", 340);
            lv_col(lv, 3, L"说明", 200);
        }
        return 0;

    case WM_SIZE: {
        RECT rc; GetClientRect(h, &rc);
        int y = ui_sc(204);
        int hh = rc.bottom - y - ui_sc(12);
        if (hh > ui_sc(60))
            MoveWindow(GetDlgItem(h, O_ID_LV), ui_sc(12), y,
                       rc.right - ui_sc(24), hh, TRUE);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == O_ID_PICK) { ui_pick_dir(h, O_ID_DIR); return 0; }

        if (id == O_ID_PREV) {
            wchar_t dir[1024], tpl[512], age[32];
            ui_get_text(h, O_ID_DIR, dir, 1024);
            if (!dir[0]) { ui_warn(h, L"请先选择文件夹。"); return 0; }
            ui_get_text(h, O_ID_TPL, tpl, 512);
            ui_get_text(h, O_ID_AGE, age, 32);

            static const wchar_t *MODES[] = { L"type", L"year", L"ext", L"letter", L"size" };
            int mi = ui_combo_sel(h, O_ID_MODE); if (mi < 0) mi = 0;

            JB b; jb_init(&b);
            jb_str(&b, "dir", dir);
            jb_str(&b, "template", tpl);
            jb_str(&b, "mode", MODES[mi]);
            jb_bool(&b, "recursive", ui_checked(h, O_ID_RECUR));
            if (ui_checked(h, O_ID_OLD)) jb_int(&b, "older_than", _wtoi(age));
            jb_str(&b, "policy", ui_policy_str(h, O_ID_POLICY));

            Arena *a = arena_new(); Str err; str_init(&err);
            ui_status(L"正在扫描并归类 …");
            JsonValue *r = ui_call(a, "organize.preview", jb_end(&b), &err);
            jb_free(&b);
            if (!r) {
                ui_warn(h, L"生成预览失败：%hs", err.data ? err.data : "未知原因");
                ui_status(L"预览失败"); str_free(&err); arena_free(a); return 0;
            }
            const wchar_t *pid = jws(a, r, "plan_id");
            wcsncpy(g_org_plan, pid ? pid : L"", 63); g_org_plan[63] = 0;

            int n = lc_render_plan(GetDlgItem(h, O_ID_LV), r, g_org_plan, 64);
            JsonValue *st = json_get(r, "stats");
            wchar_t info[256];
            swprintf(info, 256, L"共 %d 项 · 可执行 %d",
                     n, json_int(json_get(st, "executable"), 0));
            ui_set_text(h, O_ID_INFO, info);
            ui_status(L"预览完成：%ls", info);
            str_free(&err); arena_free(a);
            return 0;
        }

        if (id == O_ID_EXEC) { generic_exec(h, g_org_plan, L"整理移动"); return 0; }
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

/* ==================================================== 页 3：格式转换 == */

#define C_ID_DIR   4201
#define C_ID_PICK  4202
#define C_ID_TGT   4203
#define C_ID_RECUR 4204
#define C_ID_PREV  4205
#define C_ID_EXEC  4206
#define C_ID_LV    4207
#define C_ID_INFO  4208
#define C_ID_ENG   4209
#define C_ID_POLICY 4210

static wchar_t g_cvt_plan[64];

static void convert_show_engines(HWND h) {
    Arena *a = arena_new(); Str err; str_init(&err);
    JsonValue *e = ui_call(a, "convert.engines", "{}", &err);
    if (e) {
        JsonValue *arr = json_get(e, "engines");
        WStr s; wstr_init(&s);
        for (size_t i = 0; i < json_array_len(arr); ++i) {
            JsonValue *o = json_at(arr, i);
            if (i) wstr_puts(&s, L"    ");
            wstr_printf(&s, L"%ls%ls",
                        json_bool(json_get(o, "found"), 0) ? L"✓ " : L"✗ ",
                        jws(a, o, "key"));
        }
        ui_set_text(h, C_ID_ENG, s.data);
        wstr_free(&s);
    }
    str_free(&err); arena_free(a);
}

LRESULT CALLBACK lc_page_convert(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        ui_label(h, L"文件夹", 12, 20, 60, 20);
        ui_edit_path(h, C_ID_DIR, 80, 14, 500, 26);
        ui_btn(h, C_ID_PICK, L"选择文件夹", 590, 14, 120, 26);

        ui_label(h, L"转换为", 12, 58, 60, 20);
        ui_combo(h, C_ID_TGT, 80, 52, 260, 240, 0);
        ui_combo_add(h, C_ID_TGT, L"Markdown (.md)   【内置】");
        ui_combo_add(h, C_ID_TGT, L"纯文本 (.txt)    【内置】");
        ui_combo_add(h, C_ID_TGT, L"网页 (.html)     【内置】");
        ui_combo_add(h, C_ID_TGT, L"表格 (.csv)      【内置】");
        ui_combo_add(h, C_ID_TGT, L"PNG 图片         【内置】");
        ui_combo_add(h, C_ID_TGT, L"JPEG 图片        【内置】");
        ui_combo_add(h, C_ID_TGT, L"BMP 图片         【内置】");
        ui_combo_add(h, C_ID_TGT, L"Word 文档        【需 Pandoc】");
        ui_combo_add(h, C_ID_TGT, L"PDF              【需 LibreOffice】");
        SendMessageW(GetDlgItem(h, C_ID_TGT), CB_SETCURSEL, 0, 0);
        ui_check(h, C_ID_RECUR, L"包含子文件夹", 356, 56, 140, 22);

        ui_btn(h, C_ID_PREV, L"生成预览", 12, 92, 120, 32);
        ui_label(h, L"重名时", 268, 98, 56, 20);
        ui_combo(h, C_ID_POLICY, 328, 92, 170, 200, 0);
        ui_policy_fill(h, C_ID_POLICY);
        ui_btn(h, C_ID_EXEC, L"执行",     142, 92, 110, 32);
        ui_label(h, L"", 268, 98, 600, 22);

        ui_make(h, L"STATIC", L"外部引擎：", SS_LEFT, 12, 136, 70, 20,
                -1, ui_font_ui());
        ui_make(h, L"STATIC", L"", SS_LEFT, 86, 134, 800, 44,
                C_ID_ENG, ui_font_ui());

        {
            HWND lv = ui_make(h, WC_LISTVIEWW, L"",
                              LVS_REPORT | LVS_SINGLESEL | WS_BORDER,
                              12, 188, 900, 300, C_ID_LV, ui_font());
            lv_init(lv);
            lv_col(lv, 0, L"源文件", 420);
            lv_col(lv, 1, L"将生成", 420);
            lv_col(lv, 2, L"状态", 160);
        }
        convert_show_engines(h);
        return 0;

    case WM_SIZE: {
        RECT rc; GetClientRect(h, &rc);
        int y = ui_sc(188);
        int hh = rc.bottom - y - ui_sc(12);
        if (hh > ui_sc(60))
            MoveWindow(GetDlgItem(h, C_ID_LV), ui_sc(12), y,
                       rc.right - ui_sc(24), hh, TRUE);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == C_ID_PICK) { ui_pick_dir(h, C_ID_DIR); return 0; }

        if (id == C_ID_PREV) {
            wchar_t dir[1024];
            ui_get_text(h, C_ID_DIR, dir, 1024);
            if (!dir[0]) { ui_warn(h, L"请先选择文件夹。"); return 0; }

            static const wchar_t *TGTS[] = {
                L"md", L"txt", L"html", L"csv", L"png", L"jpg", L"bmp", L"docx", L"pdf"
            };
            int ti = ui_combo_sel(h, C_ID_TGT); if (ti < 0) ti = 0;

            JB b; jb_init(&b);
            jb_str(&b, "dir", dir);
            jb_str(&b, "target", TGTS[ti]);
            jb_bool(&b, "recursive", ui_checked(h, C_ID_RECUR));
            jb_str(&b, "policy", ui_policy_str(h, C_ID_POLICY));

            Arena *a = arena_new(); Str err; str_init(&err);
            ui_status(L"正在扫描 …");
            JsonValue *r = ui_call(a, "convert.preview", jb_end(&b), &err);
            jb_free(&b);
            if (!r) {
                ui_warn(h, L"生成预览失败：%hs", err.data ? err.data : "未知原因");
                ui_status(L"预览失败"); str_free(&err); arena_free(a); return 0;
            }
            const wchar_t *pid = jws(a, r, "plan_id");
            wcsncpy(g_cvt_plan, pid ? pid : L"", 63); g_cvt_plan[63] = 0;

            HWND lv = GetDlgItem(h, C_ID_LV);
            lv_clear(lv);
            Arena *t = arena_new();
            JsonValue *rows = json_get(r, "rows");
            for (size_t i = 0; i < json_array_len(rows); ++i) {
                JsonValue *row = json_at(rows, i);
                const wchar_t *cells[3];
                cells[0] = jws(t, row, "old");
                cells[1] = jws(t, row, "new");
                cells[2] = json_bool(json_get(row, "executable"), 0)
                           ? L"可执行" : L"有冲突";
                lv_row(lv, cells, 3);
            }
            arena_free(t);

            JsonValue *st = json_get(r, "stats");
            wchar_t info[256];
            swprintf(info, 256, L"待转换 %d 个 · 可执行 %d",
                     json_int(json_get(st, "total"), 0),
                     json_int(json_get(st, "executable"), 0));
            ui_set_text(h, C_ID_INFO, info);
            ui_status(L"预览完成：%ls", info);
            str_free(&err); arena_free(a);
            return 0;
        }

        if (id == C_ID_EXEC) { generic_exec(h, g_cvt_plan, L"格式转换"); return 0; }
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

/* ==================================================== 页 4：内容去重 == */

#define D_ID_DIR   4301
#define D_ID_PICK  4302
#define D_ID_THR   4303
#define D_ID_THRV  4304
#define D_ID_ACT   4305
#define D_ID_RECUR 4306
#define D_ID_SCAN  4307
#define D_ID_PREV  4308
#define D_ID_LV    4309
#define D_ID_INFO  4310
#define D_ID_MIN   4311
#define D_ID_EXEC  4312
#define D_ID_POLICY 4313

static wchar_t g_ded_plan[64];

LRESULT CALLBACK lc_page_dedup(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        ui_label(h, L"文件夹", 12, 20, 60, 20);
        ui_edit_path(h, D_ID_DIR, 80, 14, 500, 26);
        ui_btn(h, D_ID_PICK, L"选择文件夹", 590, 14, 120, 26);

        ui_label(h, L"相似度阈值", 12, 60, 76, 20);
        ui_make(h, TRACKBAR_CLASSW, L"",
                TBS_HORZ | TBS_AUTOTICKS | TBS_NOTICKS,
                92, 54, 240, 28, D_ID_THR, ui_font_ui());
        {
            HWND tb = GetDlgItem(h, D_ID_THR);
            SendMessageW(tb, TBM_SETRANGE, TRUE, MAKELONG(50, 99));
            SendMessageW(tb, TBM_SETPOS, TRUE, 92);
        }
        ui_make(h, L"STATIC", L"92%", SS_LEFT, 338, 60, 50, 20,
                D_ID_THRV, ui_font_ui());
        ui_check(h, D_ID_RECUR, L"包含子文件夹", 396, 58, 140, 22);
        ui_label(h, L"忽略正文少于", 548, 60, 90, 20);
        ui_edit(h, D_ID_MIN, 642, 54, 50, 26);
        ui_set_text(h, D_ID_MIN, L"40");
        ui_label(h, L"字的文件", 698, 60, 70, 20);

        ui_label(h, L"处理方式", 12, 98, 66, 20);
        ui_combo(h, D_ID_ACT, 80, 92, 320, 200, 0);
        ui_combo_add(h, D_ID_ACT, L"移到同目录的 _重复文件 文件夹（可回退）");
        ui_combo_add(h, D_ID_ACT, L"移到回收站（可回退）");
        SendMessageW(GetDlgItem(h, D_ID_ACT), CB_SETCURSEL, 0, 0);

        ui_btn(h, D_ID_SCAN, L"只读扫描", 12, 130, 120, 32);
        ui_btn(h, D_ID_PREV, L"生成预览", 142, 130, 120, 32);
        ui_btn(h, D_ID_EXEC, L"执行",     272, 130, 110, 32);
        ui_label(h, L"重名时", 396, 136, 56, 20);
        ui_combo(h, D_ID_POLICY, 456, 130, 170, 200, 0);
        ui_policy_fill(h, D_ID_POLICY);
        ui_label(h, L"", 274, 136, 700, 22);

        {
            HWND lv = ui_make(h, WC_LISTVIEWW, L"",
                              LVS_REPORT | LVS_SINGLESEL | WS_BORDER,
                              12, 172, 900, 300, D_ID_LV, ui_font());
            lv_init(lv);
            lv_col(lv, 0, L"组", 56);
            lv_col(lv, 1, L"相似度", 80);
            lv_col(lv, 2, L"文件", 470);
            lv_col(lv, 3, L"大小", 100);
            lv_col(lv, 4, L"词数", 80);
        }
        return 0;

    case WM_HSCROLL: {
        HWND tb = GetDlgItem(h, D_ID_THR);
        if (tb && (HWND)l == tb) {
            int v = (int)SendMessageW(tb, TBM_GETPOS, 0, 0);
            wchar_t s[16]; swprintf(s, 16, L"%d%%", v);
            ui_set_text(h, D_ID_THRV, s);
        }
        return 0;
    }

    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)l;
        if (nh->idFrom == D_ID_LV && nh->code == NM_DBLCLK) {
            ui_toggle_row(h, GetDlgItem(h, D_ID_LV), g_ded_plan);
        }
        return 0;
    }

    case WM_SIZE: {
        RECT rc; GetClientRect(h, &rc);
        int y = ui_sc(172);
        int hh = rc.bottom - y - ui_sc(12);
        if (hh > ui_sc(60))
            MoveWindow(GetDlgItem(h, D_ID_LV), ui_sc(12), y,
                       rc.right - ui_sc(24), hh, TRUE);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == D_ID_PICK) { ui_pick_dir(h, D_ID_DIR); return 0; }
        if (id == D_ID_EXEC) { generic_exec(h, g_ded_plan, L"内容去重"); return 0; }
        if (id != D_ID_SCAN && id != D_ID_PREV) return 0;

        wchar_t dir[1024], mn[32];
        ui_get_text(h, D_ID_DIR, dir, 1024);
        ui_get_text(h, D_ID_MIN, mn, 32);
        if (!dir[0]) { ui_warn(h, L"请先选择文件夹。"); return 0; }

        int thr = (int)SendMessageW(GetDlgItem(h, D_ID_THR), TBM_GETPOS, 0, 0);
        int act = ui_combo_sel(h, D_ID_ACT); if (act < 0) act = 0;

        JB b; jb_init(&b);
        jb_str(&b, "dir", dir);
        jb_dbl(&b, "threshold", thr / 100.0);
        jb_bool(&b, "recursive", ui_checked(h, D_ID_RECUR));
        jb_int(&b, "min_chars", _wtoi(mn) ? _wtoi(mn) : 40);
        jb_int(&b, "action", act);
        if (id == D_ID_PREV) jb_str(&b, "policy", ui_policy_str(h, D_ID_POLICY));

        Arena *a = arena_new(); Str err; str_init(&err);
        ui_status(L"正在抽取文本并计算指纹 …");
        JsonValue *r = ui_call(a, id == D_ID_SCAN ? "dedup.scan" : "dedup.preview",
                               jb_end(&b), &err);
        jb_free(&b);
        if (!r) {
            ui_warn(h, L"扫描失败：%hs", err.data ? err.data : "未知原因");
            ui_status(L"扫描失败"); str_free(&err); arena_free(a); return 0;
        }

        HWND lv = GetDlgItem(h, D_ID_LV);
        lv_clear(lv);
        Arena *t = arena_new();

        if (id == D_ID_PREV) {
            const wchar_t *pid = jws(a, r, "plan_id");
            wcsncpy(g_ded_plan, pid ? pid : L"", 63); g_ded_plan[63] = 0;
            JsonValue *rows = json_get(r, "rows");
            for (size_t i = 0; i < json_array_len(rows); ++i) {
                JsonValue *row = json_at(rows, i);
                const wchar_t *cells[5];
                cells[0] = L"";
                cells[1] = L"重复";
                cells[2] = jws(t, row, "old");
                cells[3] = L"";
                cells[4] = L"";
                lv_row(lv, cells, 5);
            }
        } else {
            JsonValue *gs = json_get(r, "groups");
            for (size_t gi = 0; gi < json_array_len(gs); ++gi) {
                JsonValue *g = json_at(gs, gi);
                JsonValue *items = json_get(g, "items");
                wchar_t gno[16];
                swprintf(gno, 16, L"%llu", (unsigned long long)(gi + 1));
                for (size_t k = 0; k < json_array_len(items); ++k) {
                    JsonValue *it = json_at(items, k);
                    const wchar_t *cells[5];
                    wchar_t sim[32], toc[32];
                    swprintf(sim, 32, L"%.0f%%",
                             json_double(json_get(it, "similarity"), 0) * 100);
                    swprintf(toc, 32, L"%d", json_int(json_get(it, "tokens"), 0));
                    cells[0] = gno;
                    cells[1] = json_bool(json_get(it, "keeper"), 0) ? L"保留" : sim;
                    cells[2] = jws(t, it, "path");
                    cells[3] = jws(t, it, "size_text");
                    cells[4] = toc;
                    lv_row(lv, cells, 5);
                }
            }
        }
        arena_free(t);

        JsonValue *st = json_get(r, "stats");
        wchar_t info[320];
        if (id == D_ID_PREV)
            swprintf(info, 320, L"将移走 %d 份重复文件",
                     json_int(json_get(st, "executable"), 0));
        else
            swprintf(info, 320, L"扫描 %d 个文档 · 发现 %d 组重复 · 可移走 %d 份",
                     json_int(json_get(st, "scanned"), 0),
                     json_int(json_get(st, "groups"), 0),
                     json_int(json_get(st, "duplicates"), 0));
        ui_set_text(h, D_ID_INFO, info);
        ui_status(L"%ls", info);
        str_free(&err); arena_free(a);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

/* ==================================================== 页 5：文档摘要 == */

#define S_ID_PATH  4401
#define S_ID_PICK  4402
#define S_ID_ALGO  4403
#define S_ID_N     4404
#define S_ID_GO    4405
#define S_ID_OUT   4406
#define S_ID_EXP_MD   4407
#define S_ID_EXP_DOCX 4408
#define S_ID_EXP_PDF  4409
#define S_ID_INFO  4410
#define S_ID_BDIR  4411
#define S_ID_BPICK 4412
#define S_ID_BGO   4413

static wchar_t g_sum_path[1024];

static void summary_export(HWND h, const wchar_t *fmt) {
    if (!g_sum_path[0]) { ui_warn(h, L"请先生成摘要。"); return; }

    wchar_t stem[512];
    wcsncpy(stem, lc_basename(g_sum_path), 511);
    stem[511] = 0;
    wchar_t *dot = wcsrchr(stem, L'.');
    if (dot) *dot = 0;
    wchar_t outname[600];
    swprintf(outname, 600, L"%ls_摘要", stem);

    JB b; jb_init(&b);
    jb_str(&b, "path", g_sum_path);
    jb_str(&b, "format", fmt);
    jb_str(&b, "out_name", outname);

    Arena *a = arena_new(); Str err; str_init(&err);
    JsonValue *r = ui_call(a, "export.run", jb_end(&b), &err);
    jb_free(&b);
    if (!r) {
        ui_warn(h, L"无法准备导出：%hs", err.data ? err.data : "未知原因");
        str_free(&err); arena_free(a); return;
    }
    const wchar_t *pid = jws(a, r, "plan_id");
    wchar_t plan[64];
    wcsncpy(plan, pid ? pid : L"", 63); plan[63] = 0;
    str_free(&err); arena_free(a);

    generic_exec(h, plan, L"导出");
}

LRESULT CALLBACK lc_page_summary(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        ui_label(h, L"文档", 12, 20, 60, 20);
        ui_edit_path(h, S_ID_PATH, 80, 14, 500, 26);
        ui_btn(h, S_ID_PICK, L"选择文件", 590, 14, 120, 26);

        ui_label(h, L"算法", 12, 58, 60, 20);
        ui_combo(h, S_ID_ALGO, 80, 52, 220, 120, 0);
        ui_combo_add(h, S_ID_ALGO, L"TextRank（推荐）");
        ui_combo_add(h, S_ID_ALGO, L"TF-IDF");
        SendMessageW(GetDlgItem(h, S_ID_ALGO), CB_SETCURSEL, 0, 0);

        ui_label(h, L"摘要句数", 316, 58, 70, 20);
        ui_edit(h, S_ID_N, 392, 52, 56, 26);
        ui_set_text(h, S_ID_N, L"0");
        ui_label(h, L"0 = 自动 3~8 句", 456, 58, 160, 20);

        ui_btn(h, S_ID_GO, L"生成摘要", 12, 88, 120, 32);
        ui_label(h, L"导出：", 144, 94, 46, 20);
        ui_btn(h, S_ID_EXP_MD,   L"Markdown", 194, 88, 110, 32);
        ui_btn(h, S_ID_EXP_DOCX, L"Word",     310, 88, 90, 32);
        ui_btn(h, S_ID_EXP_PDF,  L"PDF",      406, 88, 90, 32);
        ui_label(h, L"", 510, 94, 500, 22);

        /* 批量摘要：给一个文件夹里的每份文档各出一份摘要 */
        ui_label(h, L"或批量处理文件夹", 12, 136, 122, 20);
        ui_edit_path(h, S_ID_BDIR, 140, 130, 380, 26);
        ui_btn(h, S_ID_BPICK, L"选择文件夹", 528, 130, 110, 26);
        ui_btn(h, S_ID_BGO, L"批量摘要", 646, 130, 110, 26);

        ui_make(h, L"EDIT", L"",
                WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
                12, 168, 900, 300, S_ID_OUT, ui_font());
        return 0;

    case WM_SIZE: {
        RECT rc; GetClientRect(h, &rc);
        int y = ui_sc(168);
        int hh = rc.bottom - y - ui_sc(12);
        if (hh > ui_sc(60))
            MoveWindow(GetDlgItem(h, S_ID_OUT), ui_sc(12), y,
                       rc.right - ui_sc(24), hh, TRUE);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == S_ID_PICK) { ui_pick_file(h, S_ID_PATH); return 0; }
        if (id == S_ID_BPICK) { ui_pick_dir(h, S_ID_BDIR); return 0; }

        if (id == S_ID_BGO) {
            wchar_t dir[1024], nn[32];
            ui_get_text(h, S_ID_BDIR, dir, 1024);
            if (!dir[0]) { ui_warn(h, L"请先选择要批量处理的文件夹。"); return 0; }
            ui_get_text(h, S_ID_N, nn, 32);
            int algo = ui_combo_sel(h, S_ID_ALGO); if (algo < 0) algo = 0;

            JB b; jb_init(&b);
            jb_str(&b, "dir", dir);
            jb_bool(&b, "recursive", 1);
            jb_int(&b, "limit", 100);
            jb_int(&b, "sentences", _wtoi(nn));
            jb_int(&b, "algorithm", algo == 0 ? 1 : 0);

            Arena *a = arena_new(); Str err; str_init(&err);
            ui_status(L"正在提取文档 …");
            JsonValue *r = ui_call(a, "summarize.batch", jb_end(&b), &err);
            jb_free(&b);
            if (!r) {
                ui_warn(h, L"无法开始批量摘要：%hs", err.data ? err.data : "未知原因");
                str_free(&err); arena_free(a); return 0;
            }
            char jid[64];
            snprintf(jid, sizeof(jid), "%s", json_string(json_get(r, "job_id"), ""));
            str_free(&err); arena_free(a);
            if (!jid[0]) { ui_warn(h, L"没能启动任务。"); return 0; }

            int ok = 0, fail = 0;
            if (!lc_wait_job(h, jid, &ok, &fail)) return 0;

            char jp[128];
            snprintf(jp, sizeof(jp), "{\"job_id\":\"%s\"}", jid);
            Arena *a2 = arena_new(); Str e2; str_init(&e2);
            JsonValue *jr = ui_call(a2, "job.status", jp, &e2);

            WStr txt; wstr_init(&txt);
            wstr_printf(&txt, L"批量摘要完成：成功 %d 篇，失败 %d 篇\r\n"
                              L"================================================\r\n\r\n",
                        ok, fail);
            if (jr) {
                JsonValue *res = json_get(jr, "result");
                JsonValue *rows = res ? json_get(res, "results") : NULL;
                for (size_t i = 0; rows && i < json_array_len(rows); ++i) {
                    JsonValue *it = json_at(rows, i);
                    wstr_printf(&txt, L"■ %ls\r\n", jws(a2, it, "name"));
                    if (!json_bool(json_get(it, "ok"), 0)) {
                        wstr_printf(&txt, L"    （跳过：%ls）\r\n\r\n", jws(a2, it, "error"));
                        continue;
                    }
                    JsonValue *ss = json_get(it, "sentences");
                    for (size_t k = 0; k < json_array_len(ss); ++k)
                        wstr_printf(&txt, L"    · %ls\r\n", jws(a2, json_at(ss, k), "text"));
                    wstr_puts(&txt, L"\r\n");
                }
            }
            SetWindowTextW(GetDlgItem(h, S_ID_OUT), txt.data);
            wstr_free(&txt);
            str_free(&e2); arena_free(a2);

            ui_status(L"批量摘要完成：成功 %d 篇，失败 %d 篇", ok, fail);
            return 0;
        }

        if (id == S_ID_EXP_MD || id == S_ID_EXP_DOCX || id == S_ID_EXP_PDF) {
            summary_export(h, id == S_ID_EXP_MD ? L"md" :
                              id == S_ID_EXP_DOCX ? L"docx" : L"pdf");
            return 0;
        }

        if (id == S_ID_GO) {
            wchar_t path[1024], nn[32];
            ui_get_text(h, S_ID_PATH, path, 1024);
            if (!path[0]) { ui_warn(h, L"请先选择文档。"); return 0; }
            ui_get_text(h, S_ID_N, nn, 32);
            int algo = ui_combo_sel(h, S_ID_ALGO); if (algo < 0) algo = 0;

            JB b; jb_init(&b);
            jb_str(&b, "path", path);
            jb_int(&b, "sentences", _wtoi(nn));
            jb_int(&b, "algorithm", algo == 0 ? 1 : 0);

            Arena *a = arena_new(); Str err; str_init(&err);
            ui_status(L"正在分析文档 …");
            JsonValue *r = ui_call(a, "summarize.file", jb_end(&b), &err);
            jb_free(&b);
            if (!r) {
                ui_warn(h, L"无法生成摘要：%hs", err.data ? err.data : "未知原因");
                ui_status(L"分析失败"); str_free(&err); arena_free(a); return 0;
            }
            wcsncpy(g_sum_path, path, 1023); g_sum_path[1023] = 0;

            WStr txt; wstr_init(&txt);
            wstr_printf(&txt, L"文件名：%ls\r\n摘要句数：%d    原文长度：%d 字\r\n"
                              L"================================================\r\n\r\n",
                        jws(a, r, "name"),
                        (int)json_array_len(json_get(r, "sentences")),
                        json_int(json_get(r, "text_length"), 0));

            JsonValue *sents = json_get(r, "sentences");
            for (size_t i = 0; i < json_array_len(sents); ++i) {
                JsonValue *s = json_at(sents, i);
                wstr_printf(&txt, L"%d. %ls\r\n"
                                  L"     [原文位置 %d-%d  权重 %.3f]\r\n\r\n",
                            (int)(i + 1), jws(a, s, "text"),
                            json_int(json_get(s, "start"), 0),
                            json_int(json_get(s, "end"), 0),
                            json_double(json_get(s, "score"), 0));
            }
            wstr_puts(&txt, L"关键词：\r\n  ");
            JsonValue *kw = json_get(r, "keywords");
            for (size_t i = 0; i < json_array_len(kw); ++i) {
                if (i) wstr_puts(&txt, L" · ");
                wstr_puts(&txt, jws(a, json_at(kw, i), "word"));
            }
            wstr_puts(&txt, L"\r\n");

            SetWindowTextW(GetDlgItem(h, S_ID_OUT), txt.data);
            wstr_free(&txt);

            wchar_t info[256];
            swprintf(info, 256, L"抽出 %d 个关键句 · %d 个关键词",
                     (int)json_array_len(sents),
                     (int)json_array_len(kw));
            ui_set_text(h, S_ID_INFO, info);
            ui_status(L"摘要已生成，可导出为 Markdown / Word / PDF");
            str_free(&err); arena_free(a);
            return 0;
        }
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

/* ==================================================== 页 6：回退历史 == */

#define H_ID_LV     4501
#define H_ID_GO     4502
#define H_ID_RB     4503
#define H_ID_INFO   4504
#define H_ID_DETAIL 4505
#define H_ID_PURGE_J 4506
#define H_ID_PURGE_T 4507

static wchar_t g_hist[512][1024];
static int     g_hist_n = 0;

static void history_refresh(HWND h) {
    Arena *a = arena_new(); Str err; str_init(&err);
    JsonValue *r = ui_call(a, "history.list", "{\"limit\":500}", &err);
    HWND lv = GetDlgItem(h, H_ID_LV);
    lv_clear(lv);
    g_hist_n = 0;
    if (!r) {
        ui_status(L"读取历史失败");
        str_free(&err); arena_free(a);
        return;
    }

    Arena *t = arena_new();
    JsonValue *rows = json_get(r, "rows");
    size_t n = json_array_len(rows);
    for (size_t i = 0; i < n && g_hist_n < 512; ++i) {
        JsonValue *row = json_at(rows, i);
        const wchar_t *p = jws(t, row, "path");
        wcsncpy(g_hist[g_hist_n], p ? p : L"", 1023);
        g_hist[g_hist_n][1023] = 0;
        g_hist_n++;

        const wchar_t *cells[4];
        cells[0] = jws(t, row, "mtime_text");
        cells[1] = json_string(json_get(row, "kind"), "")[0] == 'r' ? L"回退" : L"执行";
        wchar_t res[64];
        swprintf(res, 64, L"成功 %d · 失败 %d",
                 json_int(json_get(row, "ok"), 0),
                 json_int(json_get(row, "failed"), 0));
        cells[2] = res;
        cells[3] = jws(t, row, "category");
        lv_row(lv, cells, 4);
    }
    arena_free(t);
    str_free(&err); arena_free(a);

    wchar_t info[128];
    swprintf(info, 128, L"共 %d 条记录", g_hist_n);
    ui_set_text(h, H_ID_INFO, info);
    ui_status(L"%ls · 选中一条后可查看详情或回退", info);
}

static void history_detail(HWND h, int sel) {
    if (sel < 0 || sel >= g_hist_n) return;
    JB b; jb_init(&b);
    jb_str(&b, "path", g_hist[sel]);

    Arena *a = arena_new(); Str err; str_init(&err);
    JsonValue *r = ui_call(a, "history.detail", jb_end(&b), &err);
    jb_free(&b);

    WStr txt; wstr_init(&txt);
    if (r) {
        wstr_printf(&txt, L"%ls\r\n\r\n", jws(a, r, "name"));
        wstr_puts(&txt, jws(a, r, "summary"));
        wstr_puts(&txt, L"\r\n\r\n");

        JsonValue *rows = json_get(r, "rows");
        for (size_t i = 0; i < json_array_len(rows); ++i) {
            JsonValue *row = json_at(rows, i);
            wstr_printf(&txt, L"%ls → %ls\r\n",
                        jws(a, row, "old"), jws(a, row, "new"));
        }
    } else {
        wstr_puts(&txt, L"（无法读取这条日志的详情）");
    }
    SetWindowTextW(GetDlgItem(h, H_ID_DETAIL), txt.data);
    wstr_free(&txt);
    str_free(&err); arena_free(a);
}

LRESULT CALLBACK lc_page_history(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        ui_btn(h, H_ID_GO, L"刷新列表", 12, 12, 110, 32);
        ui_btn(h, H_ID_RB, L"回退选中这条", 132, 12, 140, 32);
        ui_btn(h, H_ID_PURGE_J, L"清理旧日志", 282, 12, 120, 32);
        ui_btn(h, H_ID_PURGE_T, L"清空回收站", 412, 12, 120, 32);
        ui_make(h, L"STATIC", L"", SS_LEFT, 544, 18, 600, 22,
                H_ID_INFO, ui_font_ui());

        {
            HWND lv = ui_make(h, WC_LISTVIEWW, L"",
                              LVS_REPORT | LVS_SINGLESEL | WS_BORDER,
                              12, 56, 500, 400, H_ID_LV, ui_font());
            lv_init(lv);
            lv_col(lv, 0, L"时间", 150);
            lv_col(lv, 1, L"类型", 60);
            lv_col(lv, 2, L"结果", 150);
            lv_col(lv, 3, L"分类", 100);
        }
        ui_make(h, L"EDIT", L"",
                WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                524, 56, 400, 400, H_ID_DETAIL, ui_font_mono());
        history_refresh(h);
        return 0;

    case WM_SIZE: {
        RECT rc; GetClientRect(h, &rc);
        int y = ui_sc(56);
        int hh = rc.bottom - y - ui_sc(12);
        if (hh < ui_sc(80)) hh = ui_sc(80);
        int left = (rc.right - ui_sc(36)) * 55 / 100;
        MoveWindow(GetDlgItem(h, H_ID_LV), ui_sc(12), y, left, hh, TRUE);
        MoveWindow(GetDlgItem(h, H_ID_DETAIL), ui_sc(24) + left, y,
                   rc.right - left - ui_sc(36), hh, TRUE);
        return 0;
    }

    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)l;
        if (nh->idFrom == H_ID_LV && nh->code == LVN_ITEMCHANGED) {
            int sel = ListView_GetNextItem(GetDlgItem(h, H_ID_LV), -1, LVNI_SELECTED);
            history_detail(h, sel);
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == H_ID_GO) { history_refresh(h); return 0; }

        if (id == H_ID_PURGE_J || id == H_ID_PURGE_T) {
            int is_trash = (id == H_ID_PURGE_T);
            const wchar_t *q = is_trash
                ? L"要清空回收站吗？\n\n回收站里是执行时被覆盖的旧文件。\n清空后这些旧文件无法再通过「回退」取回。"
                : L"要清理旧日志吗？\n\n只删除超过 90 天的回退日志，近期的会保留。";
            if (MessageBoxW(h, q, L"确认清理", MB_ICONQUESTION | MB_YESNO) != IDYES) return 0;

            JB b; jb_init(&b);
            jb_int(&b, "days", 90);
            Arena *a = arena_new(); Str err; str_init(&err);
            JsonValue *r = ui_call(a, is_trash ? "history.purge_trash"
                                              : "history.purge_journals",
                                   jb_end(&b), &err);
            jb_free(&b);
            if (r) {
                ui_info(h, L"清理完成", L"已删除 %d 个文件。",
                        json_int(json_get(r, "removed"), 0));
                history_refresh(h);
            } else {
                ui_warn(h, L"清理失败：%hs", err.data ? err.data : "未知原因");
            }
            str_free(&err); arena_free(a);
            return 0;
        }

        if (id == H_ID_RB) {
            int sel = ListView_GetNextItem(GetDlgItem(h, H_ID_LV), -1, LVNI_SELECTED);
            if (sel < 0 || sel >= g_hist_n) {
                ui_warn(h, L"请先在左边选一条记录。"); return 0;
            }
            if (MessageBoxW(h,
                    L"要用这条日志把操作整体还原吗？\n\n"
                    L"· 按逆序逐项还原\n"
                    L"· 原位置已被别的文件占用的项会被跳过，不会覆盖任何文件\n"
                    L"· 恢复出来的文件如果和新文件重名，会自动改名，不会覆盖",
                    L"确认回退", MB_ICONQUESTION | MB_YESNO) != IDYES) return 0;

            JB b; jb_init(&b);
            jb_str(&b, "path", g_hist[sel]);
            Arena *a = arena_new(); Str err; str_init(&err);
            JsonValue *r = ui_call(a, "history.rollback_run", jb_end(&b), &err);
            jb_free(&b);
            if (r) {
                ui_info(h, L"回退完成", L"已按日志逆序还原。");
                ui_status(L"回退完成");
            } else {
                ui_warn(h, L"回退失败：%hs", err.data ? err.data : "未知原因");
                ui_status(L"回退失败");
            }
            str_free(&err); arena_free(a);
            history_refresh(h);
            return 0;
        }
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

/* ==================================================== 页 7：离线自检 == */

#define E_ID_OUT 4601

LRESULT CALLBACK lc_page_env(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        ui_make(h, L"EDIT", L"",
                WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                12, 12, 900, 500, E_ID_OUT, ui_font_mono());

        WStr t; wstr_init(&t);
        wstr_puts(&t, L"LocalCraft 离线自检\r\n");
        wstr_puts(&t, L"================================================\r\n\r\n");

        Arena *a = arena_new(); Str err; str_init(&err);
        JsonValue *info = ui_call(a, "app.info", "{}", &err);
        if (info) {
            wstr_printf(&t, L"版本：%ls\r\n", jws(a, info, "version"));
            wstr_printf(&t, L"数据目录：%ls\r\n", jws(a, info, "data_root"));
            wstr_printf(&t, L"界面：原生 Win32 控件（不启动浏览器、不监听任何端口）\r\n\r\n");
        }

        wstr_puts(&t, L"【内置能力】不依赖任何外部程序，断网可用\r\n");
        wstr_puts(&t, L"  ✓ 批量重命名 / 移动 / 归类\r\n");
        wstr_puts(&t, L"  ✓ 预览 → 执行 → 日志 → 一键回退\r\n");
        wstr_puts(&t, L"  ✓ 文档抽文本：docx / xlsx / pptx / pdf / txt / md / html\r\n");
        wstr_puts(&t, L"  ✓ 内容级去重（SimHash）—— 按正文，不看文件名\r\n");
        wstr_puts(&t, L"  ✓ 抽取式摘要（TextRank / TF-IDF）—— 不使用任何 AI 模型\r\n");
        wstr_puts(&t, L"  ✓ 导出 Markdown / Word(.docx) / PDF\r\n");
        wstr_puts(&t, L"  ✓ 文本互转、图片互转（png / jpg / bmp / gif / tiff）\r\n\r\n");

        wstr_puts(&t, L"【外部引擎】找到就用，找不到就如实说没有\r\n");
        JsonValue *eng = ui_call(a, "convert.engines", "{}", &err);
        if (eng) {
            JsonValue *arr = json_get(eng, "engines");
            for (size_t i = 0; i < json_array_len(arr); ++i) {
                JsonValue *o = json_at(arr, i);
                int found = json_bool(json_get(o, "found"), 0);
                wstr_printf(&t, L"  %ls %-10ls ", found ? L"✓" : L"✗",
                            jws(a, o, "key"));
                if (found) {
                    wstr_puts(&t, L"已找到： ");
                    wstr_puts(&t, jws(a, o, "path"));
                } else {
                    wstr_puts(&t, L"未找到");
                }
                wstr_puts(&t, L"\r\n");
            }
            wstr_printf(&t, L"\r\n把可执行文件放进这个目录即可启用：\r\n  %ls\r\n\r\n",
                        jws(a, eng, "tools_dir"));
        }

        wstr_puts(&t, L"【本项目不做的事】\r\n");
        wstr_puts(&t, L"  ✗ 扫描件 OCR —— 需要 AI 模型，与「不使用任何 AI 模型」冲突\r\n");
        wstr_puts(&t, L"  ✗ 联网 / 上传 / 遥测 —— 程序不发起任何出站连接\r\n");
        wstr_puts(&t, L"  ✗ Identity-H CID 字体的 PDF 文字抽取（明确报不支持，不返回乱码）\r\n");

        SetWindowTextW(GetDlgItem(h, E_ID_OUT), t.data);
        wstr_free(&t);
        str_free(&err); arena_free(a);
        return 0;
    }

    case WM_SIZE: {
        RECT rc; GetClientRect(h, &rc);
        MoveWindow(GetDlgItem(h, E_ID_OUT), ui_sc(12), ui_sc(12),
                   rc.right - ui_sc(24), rc.bottom - ui_sc(24), TRUE);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}
