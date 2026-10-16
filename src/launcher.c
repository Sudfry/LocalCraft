/* ==========================================================================
 * launcher.c —— 打开界面窗口 + 本地文件对话框
 *
 * 窗口怎么来的
 * ------------
 * Windows 10/11 自带 WebView2 / Edge。用 `msedge.exe --app=<url>` 打开，
 * 得到的是**没有地址栏、没有标签页、没有浏览器工具栏**的独立窗口 ——
 * 观感与原生桌面应用一致，而界面完全由 HTML/CSS/JS 驱动。
 *
 * 好处：
 *   1. 零第三方依赖 —— 不需要 WebView2 SDK、pythonnet、Qt
 *   2. 观感上限高 —— 满血 CSS，阴影、圆角、动效、明暗主题都可用
 *   3. 依然完全离线 —— 页面只访问 127.0.0.1 上的一次性令牌地址
 *
 * 降级链：Edge 应用模式 → Chrome 应用模式 → 系统默认浏览器 → 打印地址
 * ========================================================================== */

#include <winsock2.h>
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lc.h"

/* ================================================== 应用模式窗口 ====== */

static BOOL CALLBACK find_console_window(HWND hwnd, LPARAM param) {
    HWND *out = (HWND *)param;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    /* 隐藏控制台窗口（双模式 exe 在 GUI 启动时会带一个黑框） */
    wchar_t cls[64];
    if (GetClassNameW(hwnd, cls, 64) && _wcsicmp(cls, L"ConsoleWindowClass") == 0) {
        ShowWindow(hwnd, SW_HIDE);
        *out = hwnd;
    }
    return TRUE;
}

void lc_hide_console(void) {
    HWND found = NULL;
    EnumWindows(find_console_window, (LPARAM)&found);
    if (found) ShowWindow(found, SW_HIDE);
}

static int profile_dir(wchar_t *out, size_t cch) {
    wchar_t *root = lc_data_root();
    WStr s;
    wstr_init(&s);
    wstr_puts(&s, root);
    wstr_puts(&s, L"\\ui-profile");
    free(root);
    /* 建不出来就返回失败：调用方会改成不带 --user-data-dir 启动，
       而不是把一个不存在的目录丢给 Edge —— 后者会让 Edge 直接退出 */
    int ok = (lc_mkdir_p(s.data) == 0 || lc_is_dir(s.data)) ? 0 : -1;
    if (ok == 0) {
        wcsncpy(out, s.data, cch - 1);
        out[cch - 1] = 0;
    } else {
        out[0] = 0;
    }
    wstr_free(&s);
    return ok;
}

/*
 * 用「资源管理器所在的那个普通权限用户」的令牌来启动子进程。
 *
 * 为什么需要：
 *   如果 LocalCraft 是以管理员身份运行的，直接 CreateProcess 出来的浏览器
 *   也会带上管理员权限。而用户桌面上通常已经开着一个普通权限的浏览器，
 *   两者不允许共存 —— Edge 会弹
 *     "因为现有实例正在以提升的权限运行，是否要用普通权限重启现有实例？"
 *   无论点"是"还是"否"，页面都出不来。
 *
 *   explorer.exe 始终以交互用户的普通权限运行，复制它的令牌来启动浏览器，
 *   就等于"以普通用户身份打开"，和用户自己双击浏览器完全一致。
 */
static int spawn_as_shell_user(const wchar_t *cmdline, DWORD *exit_code_out,
                               DWORD *pid_out) {
    HWND shell = GetShellWindow();
    if (!shell) return -1;
    DWORD pid = 0;
    GetWindowThreadProcessId(shell, &pid);
    if (!pid) return -1;

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return -1;

    HANDLE tok = NULL;
    if (!OpenProcessToken(proc, TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY,
                          &tok)) {
        CloseHandle(proc);
        return -1;
    }

    int rc = -1;
    HANDLE dup = NULL;
    if (DuplicateTokenEx(tok, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation,
                         TokenPrimary, &dup)) {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        si.lpDesktop = L"winsta0\\default";
        memset(&pi, 0, sizeof(pi));

        wchar_t *cmd = _wcsdup(cmdline);
        if (cmd) {
            if (CreateProcessWithTokenW(dup, 0, NULL, cmd, CREATE_NO_WINDOW,
                                        NULL, NULL, &si, &pi)) {
                if (WaitForSingleObject(pi.hProcess, 1500) == WAIT_TIMEOUT) {
                    rc = 0;
                    if (pid_out) *pid_out = pi.dwProcessId;
                } else if (exit_code_out) {
                    GetExitCodeProcess(pi.hProcess, exit_code_out);
                }
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }
            free(cmd);
        }
        CloseHandle(dup);
    }
    CloseHandle(tok);
    CloseHandle(proc);
    return rc;
}

/* 在普通权限下打开一个 URL（走资源管理器，避免继承管理员权限） */
static int open_url_unelevated(const wchar_t *url, wchar_t *how, size_t howcch) {
    if (lc_is_elevated()) {
        WStr cmd;
        wstr_init(&cmd);
        wstr_printf(&cmd, L"explorer.exe \"%ls\"", url);
        int rc = spawn_as_shell_user(cmd.data, NULL, NULL);
        wstr_free(&cmd);
        if (rc == 0) return 0;
    }
    HINSTANCE r = ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)r > 32) return 0;
    swprintf(how, howcch, L"打开浏览器失败（ShellExecute 返回 %lld）",
             (long long)(INT_PTR)r);
    return -1;
}

/* 带上该带的参数启动一次 Edge/Chrome 应用模式窗口。
   use_profile=0 时不传 --user-data-dir（用浏览器自己的默认配置）。
   返回 0 表示进程起来了并且**没有立刻退出**。 */
static int spawn_app_window(const wchar_t *exe, const wchar_t *url,
                            const wchar_t *prof, int use_profile,
                            DWORD *exit_code_out, DWORD *pid_out) {
    WStr cmd;
    wstr_init(&cmd);
    wstr_printf(&cmd, L"\"%ls\" --app=\"%ls\" ", exe, url);
    if (use_profile && prof && prof[0])
        wstr_printf(&cmd, L"--user-data-dir=\"%ls\" ", prof);
    wstr_puts(&cmd,
        /*
         * --no-first-run / --no-default-browser-check 是**必须**的：
         * 用全新配置目录启动时，缺了 --no-first-run，Edge 会优先显示它的
         * 首次运行欢迎页，我们给的 --app 地址根本不加载 ——
         * 表现就是窗口开了、标题停在 "127.0.0.1_/"，页面永远空白。
         *
         * --no-proxy-server：让这个实例完全不使用代理，回环请求不受代理影响。
         * 但**绝不能**同时加 --proxy-bypass-list=<-loopback> ——
         * 那个写法在 Chromium 里表示"把 loopback 从绕过列表里剔除"，
         * 也就是强制 127.0.0.1 走代理，正好是反的（实测会导致页面一直空白）。
         */
        L"--window-size=1320,880 --no-first-run --no-default-browser-check "
        L"--disable-features=Translate,MediaRouter "
        L"--disable-background-networking --disable-sync "
        L"--disable-component-update --no-service-autorun "
        L"--password-store=basic "
        L"--no-proxy-server");

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    int rc = -1;
    /*
     * 管理员启动时，必须用普通权限把浏览器拉起来。
     * 否则浏览器同样带管理员权限，和桌面上已有的普通权限实例冲突，
     * Edge 会弹"现有实例正在以提升的权限运行"。
     */
    if (lc_is_elevated()) {
        rc = spawn_as_shell_user(cmd.data, exit_code_out, pid_out);
        if (rc == 0) { wstr_free(&cmd); return 0; }
        /* 降权失败就退回普通启动，至少还有机会 */
    }

    if (CreateProcessW(NULL, cmd.data, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        /* CreateProcess 成功只代表"起来了"，不代表"没退出"。
           组策略、坏的配置目录都可能让它立刻死掉 —— 等一小会儿确认。 */
        if (WaitForSingleObject(pi.hProcess, 1500) == WAIT_TIMEOUT) {
            rc = 0;
            if (pid_out) *pid_out = pi.dwProcessId;
        } else {
            DWORD ec = 0;
            GetExitCodeProcess(pi.hProcess, &ec);
            if (exit_code_out) *exit_code_out = ec;
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    wstr_free(&cmd);
    return rc;
}

/*
 * 打开界面窗口。strategy 决定用哪种方式：
 *   0 = Edge/Chrome 应用模式 + 独立配置目录（首选，不干扰用户日常窗口）
 *   1 = Edge/Chrome 应用模式 + 浏览器默认配置（独立配置不可用时的退路）
 *   2 = 系统默认浏览器（最后退路）
 * *pid_out 返回我们启动的浏览器进程号，方便调用方在"页面没出来"时把它关掉。
 */
int launcher_open_window(const wchar_t *url, const wchar_t *browser,
                         int strategy, wchar_t *how, size_t howcch,
                         DWORD *pid_out) {
    wchar_t exe[MAX_PATH * 2];
    exe[0] = 0;
    how[0] = 0;
    if (pid_out) *pid_out = 0;
    int have = 0;

    if (browser && browser[0]) {
        wcsncpy(exe, browser, MAX_PATH * 2 - 1);
        exe[MAX_PATH * 2 - 1] = 0;
        have = lc_is_file(exe);
        if (!have)
            swprintf(how, howcch, L"指定的浏览器不存在（%ls）", exe);
    }
    if (!have && lc_find_browser(exe, MAX_PATH * 2) == 0) have = 1;

    if (strategy <= 1) {
        if (!have) {
            swprintf(how, howcch,
                     L"未在常见位置找到 Edge / Chrome，改试系统默认浏览器");
        } else {
            wchar_t prof[MAX_PATH * 2];
            prof[0] = 0;
            int use_prof = 0;
            if (strategy == 0) {
                if (profile_dir(prof, MAX_PATH * 2) == 0) use_prof = 1;
                /* 独立配置目录建不出来时，直接按默认配置来 */
                else strategy = 1;
            }

            DWORD ec = 0;
            DWORD pid = 0;
            if (spawn_app_window(exe, url, prof, use_prof, &ec, &pid) == 0) {
                if (pid_out) *pid_out = pid;
                swprintf(how, howcch, L"%ls 应用模式%ls", lc_basename(exe),
                         use_prof ? L"（独立配置）" : L"（默认配置）");
                return 0;
            }
            swprintf(how, howcch, L"%ls 应用模式启动后立即退出（代码 %lu）",
                     lc_basename(exe), (unsigned long)ec);
        }
    } else {
        swprintf(how, howcch, L"系统默认浏览器");
    }

    /* 退回默认浏览器（同样要避免继承管理员权限） */
    if (strategy >= 2 || !have) {
        wchar_t why[256];
        why[0] = 0;
        if (open_url_unelevated(url, why, 256) == 0) {
            if (!how[0]) swprintf(how, howcch, L"系统默认浏览器");
            else {
                wchar_t tail[160];
                wcsncpy(tail, how, 159); tail[159] = 0;
                swprintf(how, howcch, L"%ls；已改试系统默认浏览器", tail);
            }
            return 0;
        }
        if (why[0]) swprintf(how, howcch, L"%ls", why);
    }

    return -1;
}

/* ================================================== 文件夹对话框 ====== */

int lc_pick_folder(const wchar_t *initial, wchar_t *out, size_t cch) {
    out[0] = 0;

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int did_init = SUCCEEDED(hr);

    IFileDialog *dlg = NULL;
    hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IFileDialog, (void **)&dlg);
    if (SUCCEEDED(hr) && dlg) {
        DWORD opts = 0;
        dlg->lpVtbl->GetOptions(dlg, &opts);
        dlg->lpVtbl->SetOptions(dlg, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dlg->lpVtbl->SetTitle(dlg, L"选择文件夹");

        if (initial && initial[0]) {
            IShellItem *item = NULL;
            if (SUCCEEDED(SHCreateItemFromParsingName(initial, NULL, &IID_IShellItem,
                                                      (void **)&item)) && item) {
                dlg->lpVtbl->SetFolder(dlg, item);
                item->lpVtbl->Release(item);
            }
        }

        hr = dlg->lpVtbl->Show(dlg, NULL);
        if (SUCCEEDED(hr)) {
            IShellItem *item = NULL;
            if (SUCCEEDED(dlg->lpVtbl->GetResult(dlg, &item)) && item) {
                PWSTR psz = NULL;
                if (SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &psz)) && psz) {
                    wcsncpy(out, psz, cch - 1);
                    out[cch - 1] = 0;
                    CoTaskMemFree(psz);
                }
                item->lpVtbl->Release(item);
            }
        }
        dlg->lpVtbl->Release(dlg);
        if (did_init) CoUninitialize();
        return out[0] ? 0 : -1;
    }

    /* 老式回退 */
    {
        BROWSEINFOW bi;
        memset(&bi, 0, sizeof(bi));
        bi.lpszTitle = L"选择文件夹";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
        int rc = -1;
        if (pidl) {
            if (SHGetPathFromIDListW(pidl, out)) rc = 0;
            CoTaskMemFree(pidl);
        }
        if (did_init) CoUninitialize();
        return rc;
    }
}

int lc_pick_open_file(const wchar_t *initial, wchar_t *out, size_t cch) {
    out[0] = 0;

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int did_init = SUCCEEDED(hr);

    IFileOpenDialog *dlg = NULL;
    hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IFileOpenDialog, (void **)&dlg);
    if (FAILED(hr) || !dlg) {
        if (did_init) CoUninitialize();
        return -1;
    }

    COMDLG_FILTERSPEC filter[] = {
        { L"可处理的文档", L"*.docx;*.docm;*.xlsx;*.pptx;*.pdf;*.txt;*.md;*.html;*.htm;*.csv;*.json;*.xml" },
        { L"Word 文档", L"*.docx;*.docm" },
        { L"PDF", L"*.pdf" },
        { L"文本 / Markdown", L"*.txt;*.md;*.markdown" },
        { L"所有文件", L"*.*" }
    };
    dlg->lpVtbl->SetFileTypes(dlg, 5, filter);
    dlg->lpVtbl->SetTitle(dlg, L"选择文件");

    if (initial && initial[0]) {
        IShellItem *item = NULL;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial, NULL, &IID_IShellItem,
                                                  (void **)&item)) && item) {
            dlg->lpVtbl->SetFolder(dlg, item);
            item->lpVtbl->Release(item);
        }
    }

    hr = dlg->lpVtbl->Show(dlg, NULL);
    if (SUCCEEDED(hr)) {
        IShellItem *item = NULL;
        if (SUCCEEDED(dlg->lpVtbl->GetResult(dlg, &item)) && item) {
            PWSTR psz = NULL;
            if (SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &psz)) && psz) {
                wcsncpy(out, psz, cch - 1);
                out[cch - 1] = 0;
                CoTaskMemFree(psz);
            }
            item->lpVtbl->Release(item);
        }
    }
    dlg->lpVtbl->Release(dlg);
    if (did_init) CoUninitialize();
    return out[0] ? 0 : -1;
}

int lc_pick_save_file(const wchar_t *initial, const wchar_t *filename,
                      wchar_t *out, size_t cch) {
    out[0] = 0;

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int did_init = SUCCEEDED(hr);

    IFileSaveDialog *dlg = NULL;
    hr = CoCreateInstance(&CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IFileSaveDialog, (void **)&dlg);
    if (FAILED(hr) || !dlg) {
        if (did_init) CoUninitialize();
        return -1;
    }

    COMDLG_FILTERSPEC filter[] = { { L"CSV 文件", L"*.csv" }, { L"所有文件", L"*.*" } };
    dlg->lpVtbl->SetFileTypes(dlg, 2, filter);
    dlg->lpVtbl->SetTitle(dlg, L"保存为");
    if (filename && filename[0]) dlg->lpVtbl->SetFileName(dlg, filename);
    if (initial && initial[0]) {
        IShellItem *item = NULL;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial, NULL, &IID_IShellItem,
                                                  (void **)&item)) && item) {
            dlg->lpVtbl->SetFolder(dlg, item);
            item->lpVtbl->Release(item);
        }
    }

    hr = dlg->lpVtbl->Show(dlg, NULL);
    if (SUCCEEDED(hr)) {
        IShellItem *item = NULL;
        if (SUCCEEDED(dlg->lpVtbl->GetResult(dlg, &item)) && item) {
            PWSTR psz = NULL;
            if (SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &psz)) && psz) {
                wcsncpy(out, psz, cch - 1);
                out[cch - 1] = 0;
                CoTaskMemFree(psz);
            }
            item->lpVtbl->Release(item);
        }
    }
    dlg->lpVtbl->Release(dlg);
    if (did_init) CoUninitialize();
    return out[0] ? 0 : -1;
}
