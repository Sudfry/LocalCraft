/* ==========================================================================
 * appui.c —— 原生 Win32 界面
 *
 * 定位
 * ----
 * 这是把原来的 HTML 界面整个换掉的原生版本。**不启动浏览器、不启动 HTTP 服务、
 * 不监听任何端口**，只用 user32 / comctl32 / gdi32。
 *
 * 架构
 * ----
 *   原生控件（本文件）
 *        ↓  直接调用
 *   api_dispatch(方法名, JSON 参数)      ← 和原来浏览器走的是同一个入口
 *        ↓
 *   已测试的后端（计划/日志/执行/回退/去重/摘要/导出/转换）
 *
 * 好处是后端一行都不用改：862 条断言覆盖的仍然是同一条代码路径。
 * 界面只是换了张皮，从"浏览器渲染 HTML"变成"Win32 控件"。
 *
 * 界面结构
 * --------
 *   主窗口
 *     ├─ 左侧导航（自绘 ListBox，带选中高亮）
 *     ├─ 右侧内容区（每页一个子窗口，切换时显示/隐藏）
 *     └─ 底部状态栏
 * ========================================================================== */
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lc.h"

/* ==================================================== 全局 ============== */

#define APP_CLASS   L"LocalCraftMain"
#define NAV_W       176          /* 左侧导航宽度（逻辑像素） */
#define STATUS_H    26

#define IDC_NAV     3001
#define IDC_STATUS  3002

static int        g_dpi = 96;
static HFONT      g_font = NULL;        /* 正文 */
static HFONT      g_font_ui = NULL;     /* 导航 / 按钮 */
static HFONT      g_font_title = NULL;  /* 标题 */
static HFONT      g_font_mono = NULL;   /* 路径 */
static ApiState  *g_api = NULL;
static HINSTANCE  g_inst = NULL;

static HWND g_main = NULL, g_nav = NULL, g_content = NULL, g_status = NULL;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

/* ==================================================== 页面表 ============ */

enum {
    PG_RENAME = 0, PG_ORGANIZE, PG_CONVERT, PG_DEDUP,
    PG_SUMMARY, PG_HISTORY, PG_ENV, PG_COUNT
};

static const wchar_t *PG_TITLE[PG_COUNT] = {
    L"批量重命名", L"整理分类", L"格式转换",
    L"内容去重",   L"文档摘要", L"回退历史", L"离线自检"
};
static const wchar_t *PG_SUB[PG_COUNT] = {
    L"按规则批量改名，执行前先看对照表，随时可回退",
    L"按类型 / 时间 / 名称归类，扫描阶段完全只读",
    L"文档与图片批量互转，产物同样可回退",
    L"按内容指纹找出文件名不同但正文雷同的文档",
    L"TextRank / TF-IDF 抽取关键句与关键词，不依赖 AI",
    L"查看历史操作，逐项或整体回退",
    L"如实列出哪些能力已接入、哪些没有"
};

static HWND g_page[PG_COUNT];
static int  g_cur = -1;

/* 每页的窗口类名 */
static const wchar_t *PG_CLASS[PG_COUNT] = {
    L"LcPageRename", L"LcPageOrganize", L"LcPageConvert",
    L"LcPageDedup",  L"LcPageSummary",  L"LcPageHistory", L"LcPageEnv"
};

/* 由 pages.c 提供 */
extern LRESULT CALLBACK lc_page_rename(HWND, UINT, WPARAM, LPARAM);
extern LRESULT CALLBACK lc_page_organize(HWND, UINT, WPARAM, LPARAM);
extern LRESULT CALLBACK lc_page_convert(HWND, UINT, WPARAM, LPARAM);
extern LRESULT CALLBACK lc_page_dedup(HWND, UINT, WPARAM, LPARAM);
extern LRESULT CALLBACK lc_page_summary(HWND, UINT, WPARAM, LPARAM);
extern LRESULT CALLBACK lc_page_history(HWND, UINT, WPARAM, LPARAM);
extern LRESULT CALLBACK lc_page_env(HWND, UINT, WPARAM, LPARAM);

static WNDPROC PG_PROC[PG_COUNT] = {
    lc_page_rename, lc_page_organize, lc_page_convert,
    lc_page_dedup,  lc_page_summary,  lc_page_history, lc_page_env
};

/* ==================================================== 公共工具 ========== */

void ui_status(const wchar_t *fmt, ...) {
    if (!g_status) return;
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 1023, fmt, ap);
    va_end(ap);
    buf[1023] = 0;
    SetWindowTextW(g_status, buf);
}

void ui_get_text(HWND parent, int id, wchar_t *buf, size_t cch) {
    buf[0] = 0;
    HWND h = GetDlgItem(parent, id);
    if (h) GetWindowTextW(h, buf, (int)cch);
}

void ui_set_text(HWND parent, int id, const wchar_t *s) {
    HWND h = GetDlgItem(parent, id);
    if (h) SetWindowTextW(h, s ? s : L"");
}

int ui_checked(HWND parent, int id) {
    HWND h = GetDlgItem(parent, id);
    return h && SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

int ui_combo_sel(HWND parent, int id) {
    HWND h = GetDlgItem(parent, id);
    return h ? (int)SendMessageW(h, CB_GETCURSEL, 0, 0) : 0;
}

/* 提示框 */
void ui_info(HWND owner, const wchar_t *title, const wchar_t *fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 2047, fmt, ap);
    va_end(ap);
    buf[2047] = 0;
    MessageBoxW(owner ? owner : g_main, buf, title ? title : L"LocalCraft",
                MB_ICONINFORMATION | MB_OK);
}

void ui_warn(HWND owner, const wchar_t *fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 2047, fmt, ap);
    va_end(ap);
    buf[2047] = 0;
    MessageBoxW(owner ? owner : g_main, buf, L"LocalCraft",
                MB_ICONWARNING | MB_OK);
}

/* ---- JSON 参数构造 ---- */

typedef struct { Str s; int n; } JB;

void jb_init(JB *b) { str_init(&b->s); str_putc(&b->s, '{'); b->n = 0; }
static void jb_sep(JB *b) { if (b->n++) str_putc(&b->s, ','); }
void jb_str(JB *b, const char *k, const wchar_t *v) {
    jb_sep(b);
    str_printf(&b->s, "\"%s\":", k);
    json_escape_to(&b->s, v ? v : L"");
}
void jb_int(JB *b, const char *k, int v) {
    jb_sep(b); str_printf(&b->s, "\"%s\":%d", k, v);
}
void jb_bool(JB *b, const char *k, int v) {
    jb_sep(b); str_printf(&b->s, "\"%s\":%s", k, v ? "true" : "false");
}
void jb_dbl(JB *b, const char *k, double v) {
    jb_sep(b); str_printf(&b->s, "\"%s\":%.4f", k, v);
}
const char *jb_end(JB *b) { str_putc(&b->s, '}'); return b->s.data; }
void jb_free(JB *b) { str_free(&b->s); }

/*
 * 调用后端。
 * 参数是先拼成 JSON 字符串再解析回 JsonValue —— 看起来绕，但好处是
 * 界面层完全不用碰后端的数据结构，后端一行都不用改。
 * 返回的结果也直接解析成 JsonValue，用完 arena 一起释放。
 */
JsonValue *ui_call(Arena *a, const char *method, const char *params,
                   Str *err_out) {
    Str out, err;
    str_init(&out);
    str_init(&err);

    JsonValue *pv = json_parse(params ? params : "{}", a);
    if (!pv) {
        if (err_out) str_puts(err_out, "界面参数构造失败");
        str_free(&out); str_free(&err);
        return NULL;
    }

    int rc = api_dispatch(g_api, method, pv, &out, &err);
    if (rc != 0) {
        if (err_out) str_puts(err_out, err.data ? err.data : "调用失败");
        str_free(&out); str_free(&err);
        return NULL;
    }

    JsonValue *r = json_parse(out.data ? out.data : "{}", a);
    if (!r && err_out) str_puts(err_out, "后端返回的内容无法解析");
    str_free(&out); str_free(&err);
    return r;
}

/* 路径 / 字符串便捷读取（宽字符版） */
const wchar_t *jws(Arena *a, const JsonValue *obj, const char *key) {
    const char *s = json_string(json_get(obj, key), "");
    wchar_t *w = lc_utf8_to_wcs(s);
    if (!w) return arena_wcsdup(a, L"");
    return (const wchar_t *)arena_wcsdup(a, w);   /* 让 arena 管生命周期 */
}

/* ---- ListView 小工具 ---- */

void lv_init(HWND lv) {
    ListView_SetExtendedListViewStyle(lv,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
    SendMessageW(lv, WM_SETFONT, (WPARAM)g_font, TRUE);
}

void lv_clear(HWND lv) { ListView_DeleteAllItems(lv); }

void lv_col(HWND lv, int i, const wchar_t *t, int w) {
    LVCOLUMNW c;
    memset(&c, 0, sizeof(c));
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    c.pszText = (LPWSTR)t;
    c.cx = S(w);
    c.iSubItem = i;
    ListView_InsertColumn(lv, i, &c);
}

int lv_row(HWND lv, const wchar_t *const *cells, int n) {
    LVITEMW it;
    memset(&it, 0, sizeof(it));
    it.mask = LVIF_TEXT;
    it.iItem = ListView_GetItemCount(lv);
    it.pszText = (LPWSTR)(cells[0] ? cells[0] : L"");
    int idx = ListView_InsertItem(lv, &it);
    for (int i = 1; i < n; ++i) {
        ListView_SetItemText(lv, idx, i, (LPWSTR)(cells[i] ? cells[i] : L""));
    }
    return idx;
}

/* ==================================================== 控件创建 ========== */

HWND ui_make(HWND parent, const wchar_t *cls, const wchar_t *text,
             DWORD style, int x, int y, int w, int h, int id, HFONT f) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             S(x), S(y), S(w), S(h), parent,
                             (HMENU)(INT_PTR)id, g_inst, NULL);
    if (c && f) SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
    return c;
}

HWND ui_label(HWND p, const wchar_t *t, int x, int y, int w, int h) {
    return ui_make(p, L"STATIC", t, SS_LEFT, x, y, w, h, -1, g_font);
}

HWND ui_label_dim(HWND p, const wchar_t *t, int x, int y, int w, int h) {
    return ui_make(p, L"STATIC", t, SS_LEFT, x, y, w, h, -1, g_font_ui);
}

HWND ui_edit(HWND p, int id, int x, int y, int w, int h) {
    return ui_make(p, L"EDIT", L"",
                   WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                   x, y, w, h, id, g_font);
}

HWND ui_edit_path(HWND p, int id, int x, int y, int w, int h) {
    return ui_make(p, L"EDIT", L"",
                   WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                   x, y, w, h, id, g_font_mono);
}

HWND ui_btn(HWND p, int id, const wchar_t *t, int x, int y, int w, int h) {
    return ui_make(p, L"BUTTON", t,
                   BS_PUSHBUTTON | WS_TABSTOP, x, y, w, h, id, g_font_ui);
}

HWND ui_check(HWND p, int id, const wchar_t *t, int x, int y, int w, int h) {
    return ui_make(p, L"BUTTON", t,
                   BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, h, id, g_font_ui);
}

HWND ui_combo(HWND p, int id, int x, int y, int w, int h, int drop_h) {
    HWND c = ui_make(p, L"COMBOBOX", L"",
                     CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                     x, y, w, h, id, g_font_ui);
    if (c) SendMessageW(c, CB_SETMINVISIBLE, 12, 0);
    (void)drop_h;
    return c;
}

void ui_combo_add(HWND p, int id, const wchar_t *t) {
    HWND c = GetDlgItem(p, id);
    if (c) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)t);
}

/* ---- 重名策略下拉：各页共用 ---- */

void ui_policy_fill(HWND p, int id) {
    ui_combo_add(p, id, L"自动改名（推荐）");
    ui_combo_add(p, id, L"跳过已存在的");
    ui_combo_add(p, id, L"覆盖（旧文件进回收站）");
    HWND c = GetDlgItem(p, id);
    if (c) SendMessageW(c, CB_SETCURSEL, 0, 0);
}

const wchar_t *ui_policy_str(HWND p, int id) {
    int i = ui_combo_sel(p, id);
    switch (i) {
        case 1: return L"skip";
        case 2: return L"overwrite";
        default: return L"rename";
    }
}

/* 在预览表上双击 = 切换这一行的勾选状态（plan.toggle） */
void ui_toggle_row(HWND page, HWND lv, const wchar_t *plan_id) {
    if (!plan_id || !plan_id[0]) return;
    int sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    if (sel < 0) return;

    char params[192];
    snprintf(params, sizeof(params),
             "{\"plan_id\":\"%ls\",\"index\":%d}", plan_id, sel + 1);

    Arena *a = arena_new(); Str err; str_init(&err);
    JsonValue *r = ui_call(a, "plan.toggle", params, &err);
    if (r) {
        /* 后端返回整张表，按行把「状态」列刷回去 */
        JsonValue *rows = json_get(r, "rows");
        Arena *t = arena_new();
        for (size_t i = 0; i < json_array_len(rows); ++i) {
            JsonValue *row = json_at(rows, i);
            const wchar_t *s = jws(t, row, "selected");
            const wchar_t *lvl = jws(t, row, "level_text");
            wchar_t buf[128];
            swprintf(buf, 128, L"%ls %ls", s ? s : L"", lvl ? lvl : L"");
            ListView_SetItemText(lv, (int)i, 0, buf);
        }
        arena_free(t);
        ui_status(L"已切换第 %d 项（双击行可切换勾选）", sel + 1);
    } else {
        ui_warn(page, L"切换失败：%hs", err.data ? err.data : "未知原因");
    }
    str_free(&err); arena_free(a);
}

/* 把当前预览表导出成 CSV */
void ui_export_csv(HWND page, const wchar_t *plan_id) {
    if (!plan_id || !plan_id[0]) { ui_warn(page, L"请先生成预览。"); return; }
    char params[192];
    snprintf(params, sizeof(params), "{\"plan_id\":\"%ls\"}", plan_id);

    Arena *a = arena_new(); Str err; str_init(&err);
    JsonValue *r = ui_call(a, "plan.export_csv", params, &err);
    if (!r) {
        ui_warn(page, L"导出失败：%hs", err.data ? err.data : "未知原因");
    } else if (json_bool(json_get(r, "cancelled"), 0)) {
        ui_status(L"已取消导出");
    } else {
        ui_info(page, L"导出完成", L"预览表已保存为 CSV：\n%ls",
                jws(a, r, "path"));
        ui_status(L"预览表已导出");
    }
    str_free(&err); arena_free(a);
}

/* ---- 选择文件夹：调后端原生对话框 ---- */
int ui_pick_dir(HWND owner, int edit_id) {
    wchar_t cur[1024];
    ui_get_text(owner, edit_id, cur, 1024);

    JB b; jb_init(&b);
    jb_str(&b, "initial", cur);
    const char *params = jb_end(&b);

    Arena *a = arena_new();
    Str err; str_init(&err);
    JsonValue *r = ui_call(a, "fs.pick_dir", params, &err);
    const wchar_t *picked = r ? jws(a, r, "path") : L"";
    int ok = 0;
    if (picked && picked[0]) {
        ui_set_text(owner, edit_id, picked);
        ok = 1;
    }
    str_free(&err);
    arena_free(a);
    jb_free(&b);
    return ok;
}

int ui_pick_file(HWND owner, int edit_id) {
    wchar_t cur[1024];
    ui_get_text(owner, edit_id, cur, 1024);

    JB b; jb_init(&b);
    jb_str(&b, "initial", cur);
    const char *params = jb_end(&b);

    Arena *a = arena_new();
    Str err; str_init(&err);
    JsonValue *r = ui_call(a, "fs.pick_file", params, &err);
    const wchar_t *picked = r ? jws(a, r, "path") : L"";
    int ok = 0;
    if (picked && picked[0]) {
        ui_set_text(owner, edit_id, picked);
        ok = 1;
    }
    str_free(&err);
    arena_free(a);
    jb_free(&b);
    return ok;
}

/* ==================================================== 导航自绘 ========== */

static void nav_measure(MEASUREITEMSTRUCT *m) {
    m->itemHeight = (UINT)S(38);
}

static void nav_draw(DRAWITEMSTRUCT *d) {
    if (d->itemID == (UINT)-1) return;

    int sel = (d->itemState & ODS_SELECTED) != 0;
    HBRUSH bg = CreateSolidBrush(sel ? RGB(0xE8, 0xEF, 0xFF) : RGB(0xF7, 0xF8, 0xFA));
    FillRect(d->hDC, &d->rcItem, bg);
    DeleteObject(bg);

    /* 选中项左侧一条竖线，作为更明显的"当前位置"标记 */
    if (sel) {
        RECT bar = d->rcItem;
        bar.right = bar.left + S(3);
        HBRUSH b2 = CreateSolidBrush(RGB(0x3B, 0x6E, 0xF5));
        FillRect(d->hDC, &bar, b2);
        DeleteObject(b2);
    }

    wchar_t text[128];
    SendMessageW(d->hwndItem, LB_GETTEXT, d->itemID, (LPARAM)text);

    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, sel ? RGB(0x1B, 0x3A, 0x8A) : RGB(0x33, 0x38, 0x40));
    HFONT old = (HFONT)SelectObject(d->hDC, g_font_ui);

    RECT r = d->rcItem;
    r.left += S(18);
    DrawTextW(d->hDC, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(d->hDC, old);
    if (d->itemState & ODS_FOCUS) DrawFocusRect(d->hDC, &d->rcItem);
}

/* ==================================================== 页面切换 ========== */

static void page_show(int idx) {
    if (idx < 0 || idx >= PG_COUNT) return;
    if (g_cur == idx) return;

    for (int i = 0; i < PG_COUNT; ++i) {
        if (g_page[i]) ShowWindow(g_page[i], i == idx ? SW_SHOW : SW_HIDE);
    }

    if (!g_page[idx]) {
        RECT rc;
        GetClientRect(g_content, &rc);
        g_page[idx] = CreateWindowExW(
            0, PG_CLASS[idx], L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
            0, 0, rc.right, rc.bottom,
            g_content, NULL, g_inst, NULL);
        if (g_page[idx]) SendMessageW(g_page[idx], WM_SETFONT, 0, 0);
    } else {
        ShowWindow(g_page[idx], SW_SHOW);
    }

    g_cur = idx;
    SendMessageW(g_nav, LB_SETCURSEL, idx, 0);
    ui_status(L"%ls — %ls", PG_TITLE[idx], PG_SUB[idx]);
}

void ui_goto(int idx) { page_show(idx); }

/* ==================================================== 主窗口 ============ */

static void layout_main(HWND h) {
    RECT rc;
    GetClientRect(h, &rc);
    int W = rc.right, H = rc.bottom;
    int navw = S(NAV_W), sth = S(STATUS_H);

    MoveWindow(g_nav, 0, 0, navw, H - sth, TRUE);
    MoveWindow(g_content, navw, 0, W - navw, H - sth, TRUE);
    MoveWindow(g_status, 0, H - sth, W, sth, TRUE);

    for (int i = 0; i < PG_COUNT; ++i) {
        if (g_page[i])
            MoveWindow(g_page[i], 0, 0, W - navw, H - sth, TRUE);
    }

    /* 让当前页重新排版 */
    if (g_cur >= 0 && g_page[g_cur]) {
        SendMessageW(g_page[g_cur], WM_SIZE, 0, 0);
    }
}

static LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        g_nav = CreateWindowExW(
            0, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY |
            LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
            0, 0, 100, 100, h, (HMENU)IDC_NAV, g_inst, NULL);
        SendMessageW(g_nav, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
        for (int i = 0; i < PG_COUNT; ++i)
            SendMessageW(g_nav, LB_ADDSTRING, 0, (LPARAM)PG_TITLE[i]);

        g_content = CreateWindowExW(
            0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
            0, 0, 100, 100, h, NULL, g_inst, NULL);

        g_status = CreateWindowExW(
            0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 100, 100, h, (HMENU)IDC_STATUS, g_inst, NULL);
        SendMessageW(g_status, WM_SETFONT, (WPARAM)g_font_ui, TRUE);

        return 0;
    }

    case WM_SIZE:
        layout_main(h);
        return 0;

    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)l;
        if (mi->CtlID == IDC_NAV) nav_measure(mi);
        return TRUE;
    }

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)l;
        if (di->CtlID == IDC_NAV) { nav_draw(di); return TRUE; }
        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(w);
        int code = HIWORD(w);
        if (id == IDC_NAV && code == LBN_SELCHANGE) {
            int sel = (int)SendMessageW(g_nav, LB_GETCURSEL, 0, 0);
            if (sel >= 0) page_show(sel);
            return 0;
        }
        /* 页面下拉框等控件的通知转到当前页 */
        if (g_cur >= 0 && g_page[g_cur]) {
            SendMessageW(g_page[g_cur], WM_COMMAND, w, l);
        }
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* 自绘导航需要把 WM_DRAWITEM 转发给主窗口 —— ListBox 的 OWNERDRAW
   通知本来就发给父窗口，这里不用额外处理 */

static int make_fonts(void) {
    HDC hdc = GetDC(NULL);
    if (hdc) {
        int d = GetDeviceCaps(hdc, LOGPIXELSY);
        ReleaseDC(NULL, hdc);
        if (d >= 72 && d <= 480) g_dpi = d;
    }

    g_font       = CreateFontW(-S(15), 0,0,0, FW_NORMAL, 0,0,0, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    g_font_ui    = CreateFontW(-S(14), 0,0,0, FW_NORMAL, 0,0,0, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    g_font_title = CreateFontW(-S(20), 0,0,0, FW_SEMIBOLD, 0,0,0, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    g_font_mono  = CreateFontW(-S(14), 0,0,0, FW_NORMAL, 0,0,0, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    if (!g_font) g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (!g_font_ui) g_font_ui = g_font;
    if (!g_font_title) g_font_title = g_font;
    if (!g_font_mono) g_font_mono = g_font;
    return 0;
}

/* 页面注册自己的窗口类时用这个比例获取字体 */
HFONT ui_font(void)      { return g_font; }
HFONT ui_font_ui(void)   { return g_font_ui; }
HFONT ui_font_title(void){ return g_font_title; }
HFONT ui_font_mono(void) { return g_font_mono; }
int   ui_sc(int v)       { return S(v); }

/* ==================================================== 入口 ============== */

int lc_app_run(ApiState *api) {
    g_api = api;
    g_inst = GetModuleHandleW(NULL);
    make_fonts();

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = main_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = APP_CLASS;
    wc.hIcon = lc_app_icon(0);
    wc.hIconSm = lc_app_icon(1);
    if (!RegisterClassExW(&wc)) return -1;

    /* 页面类 */
    for (int i = 0; i < PG_COUNT; ++i) {
        WNDCLASSEXW pc;
        memset(&pc, 0, sizeof(pc));
        pc.cbSize = sizeof(pc);
        pc.style = CS_DBLCLKS;
        pc.lpfnWndProc = PG_PROC[i];
        pc.hInstance = g_inst;
        pc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        pc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        pc.lpszClassName = PG_CLASS[i];
        RegisterClassExW(&pc);
    }

    int W = S(1180), H = S(760);
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    W = W > 0 ? W : 1180;
    H = H > 0 ? H : 760;

    g_main = CreateWindowExW(
        0, APP_CLASS, L"LocalCraft · 本地文件处理工具",
        WS_OVERLAPPEDWINDOW,
        (sw - W) / 2, (sh - H) / 2, W, H,
        NULL, NULL, g_inst, NULL);
    if (!g_main) return -1;

    ShowWindow(g_main, SW_SHOW);
    UpdateWindow(g_main);
    SetForegroundWindow(g_main);

    page_show(PG_RENAME);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_main, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (g_font) DeleteObject(g_font);
    if (g_font_ui) DeleteObject(g_font_ui);
    if (g_font_title) DeleteObject(g_font_title);
    if (g_font_mono) DeleteObject(g_font_mono);
    return 0;
}
