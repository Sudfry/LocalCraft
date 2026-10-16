/* ==========================================================================
 * watch.c —— 文件夹实时监控
 *
 * 和这个程序的核心承诺的关系
 * --------------------------
 * 本项目从第一天就承诺"先预览、再执行、可回退"。而"实时监控"的常见形态是
 * **监控到变化就自动处理** —— 这两件事是直接冲突的：
 * 自动处理意味着用户没看过预览，文件就已经被动了。
 *
 * 所以这里的分工是：
 *   监控只负责**发现**，把变化的文件放进一个队列；
 *   要不要处理、怎么处理，由用户点一下"整理这些文件"再说。
 *
 * 也就是说：**监控永远不会自己动你的文件。**
 * 队列满、程序关了、用户没看，都只是"这些东西还没处理"，不会造成任何意外改动。
 *
 * 这个取舍是刻意的。自动化的价值在于省事，但省下的那点事，
 * 补不回"发现文件已经被挪走、还不知道挪去哪了"的代价。
 *
 * 实现要点
 * --------
 * · ReadDirectoryChangesW 会为同一个文件的多次修改重复上报。
 *   队列按路径去重 —— 一个文件存了十次，队列里只有一条。
 *   否则用户看到的是一屏重复项，根本没法用。
 * · 队列有上限（默认 5000）。超了就丢最旧并记一个计数，
 *   免得长时间开着把内存吃光。
 * · 临时文件、隐藏文件默认过滤掉。编辑器写盘时会先在旁边建
 *   .tmp / ~$xxx 之类的中间文件，把它们也报出来纯属噪音。
 * ========================================================================== */
#include "lc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define WATCH_BUF_SIZE (64 * 1024)
#define WATCH_QUEUE_MAX 5000

typedef struct {
    wchar_t  *path;
    int       kind;          /* 见 WK_* */
    long long when;          /* unix 秒 */
    long long count;         /* 这个路径被上报了多少次（去重后合并计数） */
} WatchEntry;

struct Watcher {
    HANDLE            dir;        /* 目录句柄 */
    HANDLE            thread;
    HANDLE            stop_ev;    /* 用于唤醒并结束 */
    CRITICAL_SECTION  lock;
    WatchEntry       *items;
    size_t            n, cap;
    wchar_t          *root;
    int               recursive;
    long long         dropped;    /* 因为队列满而丢掉的条数 */
    volatile LONG     running;

    /*
     * 抑制表：我们自己刚动过的路径。
     *
     * 为什么需要：整理完一批文件后，那些 MOVE 动作本身也是"文件变化"，
     * 会立刻被记回队列 —— 用户刚点完「整理」，队列里马上又冒出 6 条，
     * 看着像功能坏了。
     *
     * 做法是按**具体路径**抑制，而不是"静默 N 秒"。
     * 静默 N 秒的问题是：那段时间里用户自己真的改了别的文件就漏掉了。
     * 按路径抑制只挡住我们动过的那些，其余照常记录，没有盲区。
     */
    wchar_t         **supp;
    long long        *supp_until;
    size_t            n_supp, cap_supp;
};

/* 等待/停止用的信号：既能在超时后重试，也能被主动唤醒 */
static const DWORD WATCH_WAIT_MS = 500;

/* ==================================================== 队列操作 ========= */

static int kind_rank(int k) {
    /* 同一个文件既被改又被删时，以"更靠后发生的事"为准 */
    switch (k) {
        case 0: return 0;   /* 新增 */
        case 1: return 1;   /* 修改 */
        case 3: return 2;   /* 改名 */
        case 2: return 3;   /* 删除 */
        default: return 0;
    }
}

static int supp_hit(Watcher *w, const wchar_t *rel) {
    /* 调用方已持锁 */
    if (!w->n_supp) return 0;
    wchar_t full[MAX_PATH * 2];
    swprintf(full, MAX_PATH * 2, L"%ls\\%ls", w->root, rel);
    long long now = lc_now_unix();

    for (size_t i = 0; i < w->n_supp; ++i) {
        if (w->supp_until[i] < now) continue;          /* 过期了，忽略 */
        if (lc_same_path(w->supp[i], full)) return 1;
        /* 一条 MOVE 会同时产生"旧路径消失"和"新路径出现"两个事件，
           还要覆盖目录本身被创建/删除的情况 */
        if (lc_is_within(w->supp[i], full) || lc_is_within(full, w->supp[i]))
            return 1;
    }
    return 0;
}

static void queue_push(Watcher *w, const wchar_t *rel, int kind) {
    if (!rel || !rel[0]) return;

    EnterCriticalSection(&w->lock);

    /* 我们自己刚动过的路径：不记 */
    if (supp_hit(w, rel)) {
        LeaveCriticalSection(&w->lock);
        return;
    }

    /* 去重：同一个路径已经排过队就只更新状态和计数 */
    for (size_t i = 0; i < w->n; ++i) {
        if (lc_wcs_ieq(w->items[i].path, rel)) {
            w->items[i].count++;
            w->items[i].when = lc_now_unix();
            /* 后来的事件优先级更高（改 → 删，最终状态应是"没了"） */
            if (kind_rank(kind) > kind_rank(w->items[i].kind))
                w->items[i].kind = kind;
            LeaveCriticalSection(&w->lock);
            return;
        }
    }

    if (w->n + 1 > w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 256;
        if (cap > WATCH_QUEUE_MAX) cap = WATCH_QUEUE_MAX;
        WatchEntry *ni = (WatchEntry *)realloc(w->items, cap * sizeof(WatchEntry));
        if (!ni) { LeaveCriticalSection(&w->lock); return; }
        w->items = ni;
        w->cap = cap;
    }

    if (w->n >= WATCH_QUEUE_MAX) {
        /* 满了：丢掉最早的一条，给新的腾位置 */
        free(w->items[0].path);
        memmove(&w->items[0], &w->items[1], (w->n - 1) * sizeof(WatchEntry));
        w->n--;
        w->dropped++;
    }

    w->items[w->n].path  = _wcsdup(rel);
    w->items[w->n].kind  = kind;
    w->items[w->n].when  = lc_now_unix();
    w->items[w->n].count = 1;
    w->n++;

    LeaveCriticalSection(&w->lock);
}

/* 编辑器写盘时的中间文件、系统临时文件 —— 报出来只是噪音 */
static int is_noise(const wchar_t *rel) {
    if (!rel || !rel[0]) return 1;
    const wchar_t *base = lc_basename(rel);

    /* 常见编辑器/系统的中间产物 */
    if (base[0] == L'~') return 1;                       /* ~$文档.docx */
    if (base[0] == L'.' && base[1] == L'_') return 1;    /* ._macOS  */
    if (wcsstr(base, L".tmp") || wcsstr(base, L".temp")) return 1;
    if (wcsstr(base, L".crdownload")) return 1;          /* Chrome 下载中 */
    if (wcsstr(base, L".part")) return 1;                /* 下载中 */
    if (wcsstr(base, L".partial")) return 1;
    if (wcsstr(base, L"~")) return 1;
    if (wcsstr(base, L"Thumbs.db")) return 1;
    if (wcsstr(base, L"desktop.ini")) return 1;
    return 0;
}

/* ==================================================== 监控线程 ========= */

static DWORD WINAPI watch_thread(LPVOID param) {
    Watcher *w = (Watcher *)param;

    unsigned char *buf = (unsigned char *)malloc(WATCH_BUF_SIZE);
    if (!buf) {
        InterlockedExchange(&w->running, 0);
        return 0;
    }

    while (InterlockedCompareExchange(&w->running, 1, 1) == 1) {
        DWORD bytes = 0;
        /*
         * 用同步（重叠）方式读，但加一个超时 —— 这样停止信号能被及时响应。
         * ReadDirectoryChangesW 本身不支持超时，所以用 overlapped + WaitForSingleObject。
         */
        OVERLAPPED ov;
        memset(&ov, 0, sizeof(ov));
        ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!ov.hEvent) break;

        BOOL ok = ReadDirectoryChangesW(
            w->dir, buf, WATCH_BUF_SIZE, w->recursive ? TRUE : FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
            NULL, &ov, NULL);

        if (!ok && GetLastError() != ERROR_IO_PENDING) {
            CloseHandle(ov.hEvent);
            break;
        }

        /* 同时等"有变化"和"要停止" */
        HANDLE waits[2] = { ov.hEvent, w->stop_ev };
        DWORD wr = WaitForMultipleObjects(2, waits, FALSE, WATCH_WAIT_MS);

        if (wr == WAIT_OBJECT_0 + 1) {          /* 停止 */
            CancelIo(w->dir);
            CloseHandle(ov.hEvent);
            break;
        }
        if (wr == WAIT_TIMEOUT) {
            /* 超时：还没变化就再等一轮（这样停止信号最多延迟 500ms） */
            if (!GetOverlappedResult(w->dir, &ov, &bytes, FALSE) &&
                GetLastError() == ERROR_IO_INCOMPLETE) {
                CancelIo(w->dir);
                CloseHandle(ov.hEvent);
                continue;
            }
        } else if (wr == WAIT_OBJECT_0) {
            if (!GetOverlappedResult(w->dir, &ov, &bytes, FALSE)) {
                CloseHandle(ov.hEvent);
                break;
            }
        } else {
            CloseHandle(ov.hEvent);
            break;
        }
        CloseHandle(ov.hEvent);
        if (bytes == 0) continue;

        /* 解析 FILE_NOTIFY_INFORMATION 链 */
        size_t off = 0;
        while (off + sizeof(FILE_NOTIFY_INFORMATION) <= bytes) {
            FILE_NOTIFY_INFORMATION *fni = (FILE_NOTIFY_INFORMATION *)(buf + off);
            wchar_t rel[MAX_PATH * 2];
            DWORD chars = fni->FileNameLength / sizeof(wchar_t);
            if (chars >= MAX_PATH * 2) chars = MAX_PATH * 2 - 1;
            memcpy(rel, fni->FileName, chars * sizeof(wchar_t));
            rel[chars] = 0;

            int kind;
            switch (fni->Action) {
                case FILE_ACTION_ADDED:            kind = 0; break;
                case FILE_ACTION_MODIFIED:         kind = 1; break;
                case FILE_ACTION_REMOVED:          kind = 2; break;
                case FILE_ACTION_RENAMED_OLD_NAME: kind = 2; break;  /* 旧名字消失 */
                case FILE_ACTION_RENAMED_NEW_NAME: kind = 3; break;
                default: kind = 1; break;
            }

            /* 目录的增删也报 —— 目录本身不处理，但下面的文件会跟着来 */
            if (!is_noise(rel)) queue_push(w, rel, kind);

            if (fni->NextEntryOffset == 0) break;
            off += fni->NextEntryOffset;
        }
    }

    free(buf);
    InterlockedExchange(&w->running, 0);
    return 0;
}

/* ==================================================== 对外接口 ========= */

Watcher *watch_start(const wchar_t *dir, int recursive, wchar_t *err, size_t errcch) {
    if (err && errcch) err[0] = 0;
    if (!dir || !dir[0]) { swprintf(err, errcch, L"请指定要监控的文件夹"); return NULL; }
    if (!lc_is_dir(dir)) { swprintf(err, errcch, L"文件夹不存在：%ls", dir); return NULL; }

    Watcher *w = (Watcher *)calloc(1, sizeof(Watcher));
    if (!w) { swprintf(err, errcch, L"内存不足"); return NULL; }

    w->root = _wcsdup(dir);
    w->recursive = recursive ? 1 : 0;
    w->cap = 256;
    w->items = (WatchEntry *)calloc(w->cap, sizeof(WatchEntry));
    if (!w->items) { free(w->root); free(w); swprintf(err, errcch, L"内存不足"); return NULL; }
    InitializeCriticalSection(&w->lock);

    /* 目录句柄必须带 FILE_FLAG_BACKUP_SEMANTICS，否则打不开目录 */
    wchar_t *lp = lc_long_path(dir);
    w->dir = CreateFileW(lp ? lp : dir, FILE_LIST_DIRECTORY,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    free(lp);
    if (w->dir == INVALID_HANDLE_VALUE) {
        swprintf(err, errcch, L"无法监视该文件夹（可能没有权限）");
        DeleteCriticalSection(&w->lock);
        free(w->root); free(w->items); free(w);
        return NULL;
    }

    w->stop_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!w->stop_ev) {
        CloseHandle(w->dir);
        DeleteCriticalSection(&w->lock);
        free(w->root); free(w->items); free(w);
        swprintf(err, errcch, L"创建事件失败");
        return NULL;
    }

    InterlockedExchange(&w->running, 1);
    w->thread = CreateThread(NULL, 0, watch_thread, w, 0, NULL);
    if (!w->thread) {
        CloseHandle(w->stop_ev);
        CloseHandle(w->dir);
        DeleteCriticalSection(&w->lock);
        free(w->root); free(w->items); free(w);
        swprintf(err, errcch, L"无法启动监控线程");
        return NULL;
    }
    return w;
}

/*
 * 记下"这个路径是我们自己动的"，在接下来的一段时间内不再把它记进队列。
 *
 * 主要用途：整理完一批文件后，把这些文件的源路径和目标路径登记进来，
 * 免得我们自己的移动动作立刻把队列又填满。
 * ttl_sec 给得不用太长 —— 只是盖住"执行刚结束"那一小段。
 */
void watch_suppress(Watcher *w, const wchar_t *abs_path, int ttl_sec) {
    if (!w || !abs_path || !abs_path[0]) return;
    if (ttl_sec <= 0) ttl_sec = 60;

    EnterCriticalSection(&w->lock);

    /* 已经登记过就只续期 */
    for (size_t i = 0; i < w->n_supp; ++i) {
        if (lc_same_path(w->supp[i], abs_path)) {
            w->supp_until[i] = lc_now_unix() + ttl_sec;
            LeaveCriticalSection(&w->lock);
            return;
        }
    }

    /* 顺便清掉过期的，免得表无限长 */
    long long now = lc_now_unix();
    size_t k = 0;
    for (size_t i = 0; i < w->n_supp; ++i) {
        if (w->supp_until[i] >= now) {
            w->supp[k] = w->supp[i];
            w->supp_until[k] = w->supp_until[i];
            k++;
        } else {
            free(w->supp[i]);
        }
    }
    w->n_supp = k;

    if (w->n_supp + 1 > w->cap_supp) {
        size_t cap = w->cap_supp ? w->cap_supp * 2 : 64;
        wchar_t **ns = (wchar_t **)realloc(w->supp, cap * sizeof(wchar_t *));
        if (!ns) { LeaveCriticalSection(&w->lock); return; }
        w->supp = ns;
        long long *nu = (long long *)realloc(w->supp_until, cap * sizeof(long long));
        if (!nu) { LeaveCriticalSection(&w->lock); return; }
        w->supp_until = nu;
        w->cap_supp = cap;
    }
    w->supp[w->n_supp] = _wcsdup(abs_path);
    w->supp_until[w->n_supp] = now + ttl_sec;
    w->n_supp++;

    LeaveCriticalSection(&w->lock);
}
void watch_stop(Watcher *w) {
    if (!w) return;
    if (w->stop_ev) SetEvent(w->stop_ev);
    if (w->thread) {
        WaitForSingleObject(w->thread, 3000);
        CloseHandle(w->thread);
    }
    if (w->dir && w->dir != INVALID_HANDLE_VALUE) CloseHandle(w->dir);
    if (w->stop_ev) CloseHandle(w->stop_ev);

    for (size_t i = 0; i < w->n; ++i) free(w->items[i].path);
    free(w->items);
    for (size_t i = 0; i < w->n_supp; ++i) free(w->supp[i]);
    free(w->supp);
    free(w->supp_until);
    free(w->root);
    DeleteCriticalSection(&w->lock);
    free(w);
}

size_t watch_count(Watcher *w) {
    if (!w) return 0;
    EnterCriticalSection(&w->lock);
    size_t n = w->n;
    LeaveCriticalSection(&w->lock);
    return n;
}

long long watch_dropped(Watcher *w) {
    if (!w) return 0;
    EnterCriticalSection(&w->lock);
    long long d = w->dropped;
    LeaveCriticalSection(&w->lock);
    return d;
}

const wchar_t *watch_root(Watcher *w) { return w ? w->root : NULL; }

int watch_item_at(Watcher *w, size_t i, const wchar_t **rel, int *kind,
                  long long *when, long long *count) {
    if (!w) return -1;
    int rc = -1;
    EnterCriticalSection(&w->lock);
    if (i < w->n) {
        if (rel)   *rel   = w->items[i].path;
        if (kind)  *kind  = w->items[i].kind;
        if (when)  *when  = w->items[i].when;
        if (count) *count = w->items[i].count;
        rc = 0;
    }
    LeaveCriticalSection(&w->lock);
    return rc;
}

/* 取出快照：把当前队列拷一份出来（顺便清空），交给上层去建预览。
   拷贝而不是直接交出去，是为了避免上层处理期间监控线程又往里加东西。 */
int watch_take_all(Watcher *w, wchar_t ***paths, size_t *n_out,
                   long long *dropped_out) {
    if (!w || !paths || !n_out) return -1;
    *paths = NULL;
    *n_out = 0;
    if (dropped_out) *dropped_out = 0;

    EnterCriticalSection(&w->lock);
    size_t n = w->n;
    if (n) {
        wchar_t **arr = (wchar_t **)calloc(n, sizeof(wchar_t *));
        if (!arr) { LeaveCriticalSection(&w->lock); return -1; }
        size_t k = 0;
        for (size_t i = 0; i < n; ++i) {
            /* 只交出"新增"和"修改"的 —— 已删除的文件没什么可整理的 */
            if (w->items[i].kind == 2) continue;
            arr[k++] = _wcsdup(w->items[i].path);
        }
        *paths = arr;
        *n_out = k;
        if (dropped_out) *dropped_out = w->dropped;
    }
    for (size_t i = 0; i < w->n; ++i) free(w->items[i].path);
    w->n = 0;
    w->dropped = 0;
    LeaveCriticalSection(&w->lock);
    return 0;
}

void watch_clear(Watcher *w) {
    if (!w) return;
    EnterCriticalSection(&w->lock);
    for (size_t i = 0; i < w->n; ++i) free(w->items[i].path);
    w->n = 0;
    w->dropped = 0;
    for (size_t i = 0; i < w->n_supp; ++i) free(w->supp[i]);
    w->n_supp = 0;
    LeaveCriticalSection(&w->lock);
}
