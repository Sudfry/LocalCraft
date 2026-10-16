/* ==========================================================================
 * backup.c —— 备份 / 镜像（同步复制）
 *
 * 设计上的关键一点：这个模块**不直接搬文件**。
 * 它只做"分析"，产出一张"从哪复制到哪"的清单，然后由上层把这些条目
 * 变成普通的 COPY 动作送进计划里。
 *
 * 这么做的好处是白拿三样东西：
 *   · 预览对照表 —— 执行前能看到每个文件会写到哪
 *   · 冲突检测 —— 目标被占用、体积、路径套娃等
 *   · 一键回退 —— 复制过去的文件进日志，随时能撤
 * 如果这里自己写一套复制逻辑，上面三样都得重新实现一遍，
 * 而且一定会和主流程的行为不一致。
 *
 * 关于"镜像"这个词
 * ----------------
 * 常见的镜像工具会把目标里**多余的文件删掉**，让两边完全一致。
 * 这个程序**不做这件事**，理由是：它从第一天就承诺"没有任何不可逆的删除"。
 * 一次误操作把目标目录设成 D:\，镜像会把整个 D 盘里"不该在"的文件全清掉，
 * 而且恢复不了 —— 这种破坏太大，不该藏在一个复选框里。
 *
 * 所以这里只做"补齐和更新"，界面上也会把名字写成
 * **「同步（只增不删）」**，并明确说明不会删除多余文件。
 * 用户想要真镜像，得自己用别的工具 —— 但至少不会在这里被坑。
 * ========================================================================== */
#include "lc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ==================================================== 结果结构 =========
 *
 * BackupItem / BackupScan 的类型定义在 lc.h 里（上层要遍历它来建计划），
 * 这里只补上内部用的状态常量。 */

enum { BK_NEW = 0, BK_DIFF = 1, BK_SAME = 2 };

/* ==================================================== SHA-256 ========== */
/* 用系统 CryptoAPI（advapi32 本来就在链接列表里）。*/

typedef struct {
    HCRYPTPROV prov;
    HCRYPTHASH hash;
    int        ok;
} Sha256;

static int sha_init(Sha256 *s) {
    s->ok = 0;
    s->prov = 0;
    s->hash = 0;
    if (!CryptAcquireContextW(&s->prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        return -1;
    if (!CryptCreateHash(s->prov, CALG_SHA_256, 0, 0, &s->hash)) {
        CryptReleaseContext(s->prov, 0);
        s->prov = 0;
        return -1;
    }
    s->ok = 1;
    return 0;
}

static int sha_update(Sha256 *s, const void *data, DWORD len) {
    if (!s->ok || len == 0) return 0;
    return CryptHashData(s->hash, (const BYTE *)data, len, 0) ? 0 : -1;
}

static int sha_final(Sha256 *s, unsigned char out[32]) {
    if (!s->ok) return -1;
    DWORD n = 32;
    int rc = CryptGetHashParam(s->hash, HP_HASHVAL, out, &n, 0) ? 0 : -1;
    CryptDestroyHash(s->hash);
    CryptReleaseContext(s->prov, 0);
    s->ok = 0;
    return rc;
}

static void sha_abort(Sha256 *s) {
    if (!s->ok) return;
    CryptDestroyHash(s->hash);
    CryptReleaseContext(s->prov, 0);
    s->ok = 0;
}

/* 算一个文件的 SHA-256。返回 0 成功。 */
static int file_sha256(const wchar_t *path, unsigned char out[32]) {
    wchar_t *lp = lc_long_path(path);
    HANDLE h = CreateFileW(lp ? lp : path, GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) return -1;

    Sha256 s;
    if (sha_init(&s) != 0) { CloseHandle(h); return -1; }

    unsigned char *buf = (unsigned char *)malloc(64 * 1024);
    if (!buf) { sha_abort(&s); CloseHandle(h); return -1; }

    int rc = 0;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf, 64 * 1024, &got, NULL)) { rc = -1; break; }
        if (got == 0) break;
        if (sha_update(&s, buf, got) != 0) { rc = -1; break; }
    }
    free(buf);
    CloseHandle(h);

    if (rc != 0) { sha_abort(&s); return -1; }
    return sha_final(&s, out);
}

/* ==================================================== 小工具 =========== */

static void scan_add(BackupScan *sc, const wchar_t *src, const wchar_t *dst,
                     const wchar_t *rel, long long size, int state) {
    if (sc->n + 1 > sc->cap) {
        size_t cap = sc->cap ? sc->cap * 2 : 256;
        BackupItem *ni = (BackupItem *)realloc(sc->items, cap * sizeof(BackupItem));
        if (!ni) return;
        sc->items = ni;
        sc->cap = cap;
    }
    BackupItem *it = &sc->items[sc->n++];
    it->src = _wcsdup(src);
    it->dst = _wcsdup(dst);
    it->rel = _wcsdup(rel);
    it->size = size;
    it->state = state;
}

void backup_scan_free(BackupScan *sc) {
    if (!sc) return;
    for (size_t i = 0; i < sc->n; ++i) {
        free(sc->items[i].src);
        free(sc->items[i].dst);
        free(sc->items[i].rel);
    }
    free(sc->items);
    memset(sc, 0, sizeof(*sc));
}

/* 求相对路径：src_root 之后的那一段 */
static wchar_t *rel_of(const wchar_t *root, const wchar_t *path) {
    size_t rl = wcslen(root);
    const wchar_t *p = path + rl;
    while (*p == L'\\' || *p == L'/') ++p;
    return _wcsdup(p);
}

/* 文件大小；打不开返回 -1 */
static long long fsize_of(const wchar_t *path) {
    wchar_t *lp = lc_long_path(path);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    long long sz = -1;
    if (GetFileAttributesExW(lp ? lp : path, GetFileExInfoStandard, &fad))
        sz = ((long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    free(lp);
    return sz;
}

/* 修改时间（FILETIME 的 64 位值）。用来做"是否变过"的快速判断。 */
static long long mtime_of(const wchar_t *path) {
    wchar_t *lp = lc_long_path(path);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    long long t = -1;
    if (GetFileAttributesExW(lp ? lp : path, GetFileExInfoStandard, &fad))
        t = ((long long)fad.ftLastWriteTime.dwHighDateTime << 32) |
             fad.ftLastWriteTime.dwLowDateTime;
    free(lp);
    return t;
}

/* 扩展名在逗号分隔的名单里？空名单 = 全部通过 */
static int ext_in_list(const wchar_t *path, const wchar_t *list) {
    if (!list || !list[0]) return 1;
    const wchar_t *e = lc_extname(path);
    if (*e == L'.') ++e;
    const wchar_t *p = list;
    while (*p) {
        while (*p == L' ' || *p == L',' || *p == L';' || *p == L'\t') ++p;
        if (!*p) break;
        const wchar_t *s = p;
        while (*p && *p != L',' && *p != L';') ++p;
        size_t n = (size_t)(p - s);
        while (n > 0 && (s[n-1] == L' ' || s[n-1] == L'\t')) --n;
        if (n > 0 && *s == L'.') { ++s; --n; }
        if (n > 0 && wcslen(e) == n && _wcsnicmp(e, s, n) == 0) return 1;
    }
    return 0;
}

/* ==================================================== 扫描 ============= */

int backup_scan(const wchar_t *src_root, const wchar_t *dst_root,
                const wchar_t *include, const wchar_t *exclude,
                int recursive, int skip_hidden, int strict, long long max_bytes,
                BackupScan *out, wchar_t *err, size_t errcch)
{
    memset(out, 0, sizeof(*out));
    if (err && errcch) err[0] = 0;

    if (!src_root || !src_root[0]) { swprintf(err, errcch, L"请选择要备份的源文件夹"); return -1; }
    if (!dst_root || !dst_root[0]) { swprintf(err, errcch, L"请选择目标文件夹"); return -1; }
    if (!lc_is_dir(src_root))       { swprintf(err, errcch, L"源文件夹不存在：%ls", src_root); return -1; }

    /*
     * 目标不能位于源里面 —— 否则复制进去的文件会被下一轮扫描当作新文件，
     * 无限递归地把目录撑爆。这个检查必须在扫描之前做，不能等执行时才发现。
     */
    if (lc_is_within(dst_root, src_root)) {
        swprintf(err, errcch,
                 L"目标文件夹不能放在源文件夹内部，否则会无限自我复制。");
        return -1;
    }
    if (lc_same_path(src_root, dst_root)) {
        swprintf(err, errcch, L"源和目标不能是同一个文件夹。");
        return -1;
    }

    if (lc_mkdir_p(dst_root) != 0 && !lc_is_dir(dst_root)) {
        swprintf(err, errcch, L"无法创建目标文件夹：%ls", dst_root);
        return -1;
    }

    /* 先收集源里的所有文件 */
    Arena *a = arena_new();
    FileList fl;
    filelist_init(&fl, a);
    if (lc_scan_dir(src_root, recursive, skip_hidden, 1, &fl) != 0) {
        arena_free(a);
        swprintf(err, errcch, L"无法读取源文件夹");
        return -1;
    }

    long long maxb = max_bytes > 0 ? max_bytes : (8ll << 30);

    for (size_t i = 0; i < fl.n; ++i) {
        const wchar_t *src = fl.paths[i];

        if (include && include[0] && !ext_in_list(src, include)) { out->skipped_ext++; continue; }
        if (exclude && exclude[0] &&  ext_in_list(src, exclude)) { out->skipped_ext++; continue; }

        long long sz = fsize_of(src);
        if (sz < 0) continue;
        if (sz > maxb) { out->skipped_size++; continue; }
        out->scanned++;

        wchar_t *rel = rel_of(src_root, src);
        if (!rel) continue;

        WStr dp;
        wstr_init(&dp);
        wstr_puts(&dp, dst_root);
        if (dst_root[wcslen(dst_root) - 1] != L'\\') wstr_putc(&dp, L'\\');
        wstr_puts(&dp, rel);
        wchar_t *dst = wstr_steal(&dp);

        int state;
        long long dsz = fsize_of(dst);
        if (dsz < 0) {
            state = BK_NEW;                       /* 目标没有 */
        } else if (dsz != sz) {
            state = BK_DIFF;                      /* 体积不同 → 一定不样 */
        } else if (!strict) {
            /*
             * 默认按"体积 + 修改时间"判断，和 robocopy 的做法一致。
             * 比逐个算哈希快几个数量级，对备份这个场景够用。
             */
            long long st = mtime_of(src), dt = mtime_of(dst);
            state = (st == dt) ? BK_SAME : BK_DIFF;
        } else {
            /* 严格模式：算内容哈希。慢，但能抓住"体积和时间和一样、内容却不同"的情况 */
            unsigned char hs[32], hd[32];
            if (file_sha256(src, hs) == 0 && file_sha256(dst, hd) == 0)
                state = (memcmp(hs, hd, 32) == 0) ? BK_SAME : BK_DIFF;
            else
                state = BK_DIFF;                  /* 算不出来就当成需要复制 */
        }

        scan_add(out, src, dst, rel, sz, state);
        if (state == BK_NEW)       { out->n_new++;  out->total_bytes += sz; }
        else if (state == BK_DIFF) { out->n_diff++; out->total_bytes += sz; }
        else                       { out->n_same++; }

        free(rel);
        free(dst);
    }

    arena_free(a);
    return 0;
}

/* 给界面用的取值接口 */
int backup_item_at(const BackupScan *sc, size_t i, const wchar_t **src,
                   const wchar_t **dst, const wchar_t **rel,
                   long long *size, int *state) {
    if (!sc || i >= sc->n) return -1;
    if (src)   *src   = sc->items[i].src;
    if (dst)   *dst   = sc->items[i].dst;
    if (rel)   *rel   = sc->items[i].rel;
    if (size)  *size  = sc->items[i].size;
    if (state) *state = sc->items[i].state;
    return 0;
}

/* ==================================================== 复制后校验 ======= */

/*
 * 校验一个已复制的文件：体积一致 + 内容哈希一致。
 * 返回 0 = 一致，1 = 不一致，-1 = 读不了。
 *
 * 为什么要单独做这一步：复制出错**不一定会报错**。
 * 磁盘满、网络盘掉线、U 盘被拔，WriteFile 都可能"成功"地写出一个
 * 长度不对的文件。备份最怕的不是"抄错了"，而是"抄错了还不知道"。
 */
int backup_verify(const wchar_t *src, const wchar_t *dst) {
    if (!src || !dst) return -1;
    long long a = fsize_of(src), b = fsize_of(dst);
    if (a < 0 || b < 0) return -1;
    if (a != b) return 1;

    unsigned char ha[32], hb[32];
    if (file_sha256(src, ha) != 0) return -1;
    if (file_sha256(dst, hb) != 0) return -1;
    return memcmp(ha, hb, 32) == 0 ? 0 : 1;
}
