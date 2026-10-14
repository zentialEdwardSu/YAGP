/* POSIX implementation of the Win32 subset declared in include/windows.h.
 * Test-only: just enough fidelity for gif_stream, frame_cache, script,
 * contacts and monitors to behave as they do on Windows. */
#include <windows.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>

#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wctype.h>

/* ---- UTF-8 <-> UTF-32 ------------------------------------------------------ */

static size_t utf8_encode(uint32_t c, char out[4])
{
    if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = 0xFFFD;
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) { out[0] = (char)(0xC0 | c >> 6); out[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | c >> 12); out[1] = (char)(0x80 | ((c >> 6) & 63));
        out[2] = (char)(0x80 | (c & 63));
        return 3;
    }
    out[0] = (char)(0xF0 | c >> 18); out[1] = (char)(0x80 | ((c >> 12) & 63));
    out[2] = (char)(0x80 | ((c >> 6) & 63)); out[3] = (char)(0x80 | (c & 63));
    return 4;
}

/* Decodes one code point; invalid sequences yield U+FFFD and consume one byte,
 * like MultiByteToWideChar without MB_ERR_INVALID_CHARS. */
static uint32_t utf8_decode(const unsigned char *s, size_t n, size_t *used)
{
    unsigned char b = s[0];
    int len = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 0;
    *used = 1;
    if (len == 1) return b;
    if (len == 0 || (size_t)len > n) return 0xFFFD;
    uint32_t c = b & (0x7F >> len);
    for (int i = 1; i < len; ++i) {
        if ((s[i] & 0xC0) != 0x80) return 0xFFFD;
        c = c << 6 | (s[i] & 63);
    }
    static const uint32_t min[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (c < min[len] || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return 0xFFFD;
    *used = (size_t)len;
    return c;
}

int MultiByteToWideChar(UINT cp, DWORD flags, const char *src, int n, wchar_t *dst, int cap)
{
    (void)cp; (void)flags;
    size_t len = n < 0 ? strlen(src) + 1 : (size_t)n;
    int out = 0;
    for (size_t i = 0; i < len;) {
        size_t used;
        uint32_t c = utf8_decode((const unsigned char *)src + i, len - i, &used);
        i += used;
        if (cap) {
            if (out >= cap) return 0; /* ERROR_INSUFFICIENT_BUFFER */
            dst[out] = (wchar_t)c;
        }
        ++out;
    }
    return out;
}

int WideCharToMultiByte(UINT cp, DWORD flags, const wchar_t *src, int n, char *dst, int cap,
                        const char *def, BOOL *used_def)
{
    (void)cp; (void)flags; (void)def;
    if (used_def) *used_def = FALSE;
    size_t len = n < 0 ? wcslen(src) + 1 : (size_t)n;
    int out = 0;
    for (size_t i = 0; i < len; ++i) {
        char buf[4];
        size_t k = utf8_encode((uint32_t)src[i], buf);
        if (cap) {
            if (out + (int)k > cap) return 0;
            memcpy(dst + out, buf, k);
        }
        out += (int)k;
    }
    return out;
}

char *yagp_path_utf8(const wchar_t *path)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, NULL, 0, NULL, NULL);
    char *s = (char *)malloc((size_t)n);
    if (!s) abort();
    WideCharToMultiByte(CP_UTF8, 0, path, -1, s, n, NULL, NULL);
    for (char *p = s; *p; ++p) {
        if (*p == '\\') *p = '/';
    }
    return s;
}

/* ---- MSVC printf conventions ----------------------------------------------- */

/* MSVC wide printf: %s / %c are wide, %hs / %hc narrow. glibc: %s narrow, %ls wide. */
static wchar_t *translate_wfmt(const wchar_t *fmt)
{
    size_t n = wcslen(fmt);
    wchar_t *out = (wchar_t *)malloc((2 * n + 1) * sizeof(wchar_t));
    if (!out) abort();
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        if (fmt[i] != L'%') { out[o++] = fmt[i++]; continue; }
        out[o++] = fmt[i++];
        if (i < n && fmt[i] == L'%') { out[o++] = fmt[i++]; continue; }
        size_t spec = o;
        while (i < n && wcschr(L"-+ #0123456789.*", fmt[i])) out[o++] = fmt[i++];
        size_t len_at = o;
        while (i < n && wcschr(L"hlLjzt", fmt[i])) out[o++] = fmt[i++];
        if (i >= n) break;
        wchar_t conv = fmt[i++];
        if (conv == L's' || conv == L'c') {
            bool narrow = o - len_at == 1 && out[len_at] == L'h';
            o = len_at;
            if (!narrow) out[o++] = L'l';
        }
        (void)spec;
        out[o++] = conv;
    }
    out[o] = 0;
    return out;
}

/* MSVC _TRUNCATE semantics: writes what fits (always terminated), returns the
 * length, or -1 if truncated. size 0 is an invalid parameter (abort). */
static int vsnwprintf_trunc(wchar_t *buf, size_t size, const wchar_t *fmt, va_list ap)
{
    if (!buf || size == 0) abort();
    wchar_t *f = translate_wfmt(fmt);
    size_t cap = 256;
    for (;;) {
        wchar_t *tmp = (wchar_t *)malloc(cap * sizeof(wchar_t));
        if (!tmp) abort();
        va_list cp;
        va_copy(cp, ap);
        int n = vswprintf(tmp, cap, f, cp);
        va_end(cp);
        if (n >= 0) {
            int result = (size_t)n < size ? n : -1;
            size_t keep = (size_t)n < size ? (size_t)n : size - 1;
            memcpy(buf, tmp, keep * sizeof(wchar_t));
            buf[keep] = 0;
            free(tmp);
            free(f);
            return result;
        }
        free(tmp);
        if (cap > (1u << 24)) abort();
        cap *= 4;
    }
}

int _snwprintf_s(wchar_t *buf, size_t size, size_t count, const wchar_t *fmt, ...)
{
    (void)count;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnwprintf_trunc(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int yagp_swprintf(wchar_t *buf, size_t size, const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnwprintf_trunc(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int _snprintf_s(char *buf, size_t size, size_t count, const char *fmt, ...)
{
    (void)count;
    if (!buf || size == 0) abort();
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n >= 0 && (size_t)n < size ? n : -1;
}

int strncpy_s(char *dst, size_t size, const char *src, size_t count)
{
    (void)count;
    if (!dst || size == 0) abort();
    size_t n = strlen(src);
    size_t keep = n < size ? n : size - 1;
    memcpy(dst, src, keep);
    dst[keep] = 0;
    return keep == n ? 0 : STRUNCATE;
}

int wcsncpy_s(wchar_t *dst, size_t size, const wchar_t *src, size_t count)
{
    (void)count;
    if (!dst || size == 0) abort();
    size_t n = wcslen(src);
    size_t keep = n < size ? n : size - 1;
    memcpy(dst, src, keep * sizeof(wchar_t));
    dst[keep] = 0;
    return keep == n ? 0 : STRUNCATE;
}

FILE *_wfopen(const wchar_t *path, const wchar_t *mode)
{
    char *p = yagp_path_utf8(path), *m = yagp_path_utf8(mode);
    FILE *f = fopen(p, m);
    free(p);
    free(m);
    return f;
}

int _wtoi(const wchar_t *s)
{
    return (int)wcstol(s, NULL, 10);
}

/* ---- files ------------------------------------------------------------------ */

enum { KIND_FILE = 0x46494C45, KIND_MAPPING = 0x4D415050 };
typedef struct Handle { int kind; int fd; } Handle;

typedef struct View { void *addr; size_t size; struct View *next; } View;
static View *s_views;

HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, void *sa, DWORD disposition, DWORD flags,
                   HANDLE tmpl)
{
    (void)share; (void)sa; (void)flags; (void)tmpl;
    int mode = (access & GENERIC_READ) && (access & GENERIC_WRITE) ? O_RDWR
               : (access & GENERIC_WRITE) ? O_WRONLY : O_RDONLY;
    if (disposition == CREATE_ALWAYS) mode |= O_CREAT | O_TRUNC;
    char *p = yagp_path_utf8(path);
    int fd = open(p, mode | O_CLOEXEC, 0644);
    free(p);
    if (fd < 0) return INVALID_HANDLE_VALUE;
    struct stat st;
    if (fstat(fd, &st) != 0 || S_ISDIR(st.st_mode)) { close(fd); return INVALID_HANDLE_VALUE; }
    Handle *h = (Handle *)malloc(sizeof(Handle));
    if (!h) abort();
    h->kind = KIND_FILE;
    h->fd = fd;
    return h;
}

BOOL CloseHandle(HANDLE handle)
{
    Handle *h = (Handle *)handle;
    if (!h || handle == INVALID_HANDLE_VALUE) return FALSE;
    if (h->kind != KIND_FILE && h->kind != KIND_MAPPING) abort(); /* Not ours: a real bug. */
    close(h->fd);
    h->kind = 0;
    free(h);
    return TRUE;
}

BOOL GetFileSizeEx(HANDLE handle, LARGE_INTEGER *size)
{
    struct stat st;
    if (fstat(((Handle *)handle)->fd, &st) != 0) return FALSE;
    size->QuadPart = st.st_size;
    return TRUE;
}

BOOL ReadFile(HANDLE handle, void *buf, DWORD n, DWORD *got, void *ov)
{
    (void)ov;
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(((Handle *)handle)->fd, (char *)buf + done, n - done);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return FALSE;
        if (r == 0) break;
        done += (size_t)r;
    }
    if (got) *got = (DWORD)done;
    return TRUE;
}

BOOL WriteFile(HANDLE handle, const void *buf, DWORD n, DWORD *wrote, void *ov)
{
    (void)ov;
    size_t done = 0;
    while (done < n) {
        ssize_t r = write(((Handle *)handle)->fd, (const char *)buf + done, n - done);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return FALSE;
        done += (size_t)r;
    }
    if (wrote) *wrote = (DWORD)done;
    return TRUE;
}

BOOL SetFilePointerEx(HANDLE handle, LARGE_INTEGER dist, LARGE_INTEGER *pos, DWORD method)
{
    off_t r = lseek(((Handle *)handle)->fd, (off_t)dist.QuadPart, method == FILE_CURRENT ? SEEK_CUR : SEEK_SET);
    if (r < 0) return FALSE;
    if (pos) pos->QuadPart = r;
    return TRUE;
}

BOOL FlushFileBuffers(HANDLE handle)
{
    return fsync(((Handle *)handle)->fd) == 0;
}

HANDLE CreateFileMappingW(HANDLE file, void *sa, DWORD protect, DWORD hi, DWORD lo, LPCWSTR name)
{
    (void)sa; (void)protect; (void)hi; (void)lo; (void)name;
    Handle *h = (Handle *)malloc(sizeof(Handle));
    if (!h) abort();
    h->kind = KIND_MAPPING;
    h->fd = dup(((Handle *)file)->fd);
    return h;
}

void *MapViewOfFile(HANDLE mapping, DWORD access, DWORD hi, DWORD lo, size_t n)
{
    (void)access; (void)hi; (void)lo; (void)n;
    struct stat st;
    int fd = ((Handle *)mapping)->fd;
    if (fstat(fd, &st) != 0 || st.st_size == 0) return NULL;
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) return NULL;
    View *v = (View *)malloc(sizeof(View));
    if (!v) abort();
    v->addr = p;
    v->size = (size_t)st.st_size;
    v->next = s_views;
    s_views = v;
    return p;
}

BOOL UnmapViewOfFile(const void *view)
{
    for (View **v = &s_views; *v; v = &(*v)->next) {
        if ((*v)->addr == view) {
            View *dead = *v;
            munmap(dead->addr, dead->size);
            *v = dead->next;
            free(dead);
            return TRUE;
        }
    }
    abort(); /* Unmapping something never mapped. */
}

DWORD GetFileAttributesW(LPCWSTR path)
{
    char *p = yagp_path_utf8(path);
    struct stat st;
    int r = stat(p, &st);
    free(p);
    if (r != 0) return INVALID_FILE_ATTRIBUTES;
    return S_ISDIR(st.st_mode) ? 0x10 : 0x80;
}

BOOL GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, void *out)
{
    (void)level;
    char *p = yagp_path_utf8(path);
    struct stat st;
    int r = stat(p, &st);
    free(p);
    if (r != 0) return FALSE;
    WIN32_FILE_ATTRIBUTE_DATA *a = (WIN32_FILE_ATTRIBUTE_DATA *)out;
    memset(a, 0, sizeof(*a));
    uint64_t size = (uint64_t)st.st_size;
    uint64_t ft = ((uint64_t)st.st_mtim.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)st.st_mtim.tv_nsec / 100;
    a->nFileSizeHigh = (DWORD)(size >> 32);
    a->nFileSizeLow = (DWORD)(size & 0xFFFFFFFFu);
    a->ftLastWriteTime.dwHighDateTime = (DWORD)(ft >> 32);
    a->ftLastWriteTime.dwLowDateTime = (DWORD)(ft & 0xFFFFFFFFu);
    a->dwFileAttributes = S_ISDIR(st.st_mode) ? 0x10 : 0x80;
    return TRUE;
}

BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags)
{
    (void)flags;
    char *a = yagp_path_utf8(from), *b = yagp_path_utf8(to);
    int r = rename(a, b);
    free(a);
    free(b);
    return r == 0;
}

BOOL DeleteFileW(LPCWSTR path)
{
    char *p = yagp_path_utf8(path);
    int r = unlink(p);
    free(p);
    return r == 0;
}

BOOL CreateDirectoryW(LPCWSTR path, void *sa)
{
    (void)sa;
    char *p = yagp_path_utf8(path);
    int r = mkdir(p, 0755);
    free(p);
    return r == 0;
}

int SHCreateDirectoryExW(HWND hwnd, LPCWSTR path, void *sa)
{
    (void)hwnd; (void)sa;
    char *p = yagp_path_utf8(path);
    for (char *s = p + 1; *s; ++s) {
        if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    }
    int r = mkdir(p, 0755) == 0 || errno == EEXIST ? 0 : 1;
    free(p);
    return r;
}

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st; (void)flag; (void)ftw;
    return remove(path);
}

int SHFileOperationW(SHFILEOPSTRUCTW *op)
{
    if (op->wFunc != FO_DELETE) return 1;
    op->fAnyOperationsAborted = FALSE;
    char *p = yagp_path_utf8(op->pFrom);
    int r = nftw(p, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
    free(p);
    return r == 0 ? 0 : 1;
}

/* ---- known folders ---------------------------------------------------------- */

const KNOWNFOLDERID FOLDERID_LocalAppData = 1, FOLDERID_RoamingAppData = 2, FOLDERID_Documents = 3;

HRESULT SHGetKnownFolderPath(const KNOWNFOLDERID *id, DWORD flags, HANDLE token, PWSTR *out)
{
    (void)flags; (void)token;
    const char *env = *id == FOLDERID_LocalAppData ? "YAGP_LOCALAPPDATA"
                      : *id == FOLDERID_RoamingAppData ? "YAGP_APPDATA" : "YAGP_DOCUMENTS";
    const char *v = getenv(env);
    if (!v) v = "/tmp";
    int n = MultiByteToWideChar(CP_UTF8, 0, v, -1, NULL, 0);
    *out = (PWSTR)malloc((size_t)n * sizeof(wchar_t));
    if (!*out) return E_FAIL;
    MultiByteToWideChar(CP_UTF8, 0, v, -1, *out, n);
    return S_OK;
}

void CoTaskMemFree(void *p)
{
    free(p);
}

/* ---- misc ------------------------------------------------------------------- */

BOOL QueryPerformanceCounter(LARGE_INTEGER *t)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t->QuadPart = (LONGLONG)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return TRUE;
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER *f)
{
    f->QuadPart = 1000000000LL;
    return TRUE;
}

void OutputDebugStringW(LPCWSTR s)
{
    if (getenv("YAGP_DEBUG")) fprintf(stderr, "%ls", s);
}

DWORD GetCurrentProcessId(void)
{
    return (DWORD)getpid();
}

DWORD GetCurrentThreadId(void)
{
    return (DWORD)gettid();
}

LPWSTR CharLowerW(LPWSTR s)
{
    if (((uintptr_t)s >> 16) == 0) return (LPWSTR)(uintptr_t)towlower((wint_t)(uintptr_t)s);
    for (wchar_t *p = s; *p; ++p) *p = (wchar_t)towlower((wint_t)*p);
    return s;
}

void Sleep(DWORD ms)
{
    usleep((useconds_t)ms * 1000);
}

BOOL SetRect(RECT *r, int l, int t, int rr, int b)
{
    r->left = l; r->top = t; r->right = rr; r->bottom = b;
    return TRUE;
}

BOOL IsRectEmpty(const RECT *r)
{
    return r->right <= r->left || r->bottom <= r->top;
}

BOOL UnionRect(RECT *out, const RECT *a, const RECT *b)
{
    if (IsRectEmpty(a)) { *out = *b; return !IsRectEmpty(b); }
    if (IsRectEmpty(b)) { *out = *a; return TRUE; }
    RECT r = {a->left < b->left ? a->left : b->left, a->top < b->top ? a->top : b->top,
              a->right > b->right ? a->right : b->right, a->bottom > b->bottom ? a->bottom : b->bottom};
    *out = r;
    return TRUE;
}

BOOL IntersectRect(RECT *out, const RECT *a, const RECT *b)
{
    RECT r = {a->left > b->left ? a->left : b->left, a->top > b->top ? a->top : b->top,
              a->right < b->right ? a->right : b->right, a->bottom < b->bottom ? a->bottom : b->bottom};
    if (IsRectEmpty(&r)) { memset(out, 0, sizeof(*out)); return FALSE; }
    *out = r;
    return TRUE;
}

BOOL EnumDisplayMonitors(HDC dc, const RECT *clip, MONITORENUMPROC fn, LPARAM lp)
{
    (void)dc; (void)clip; (void)fn; (void)lp;
    return TRUE; /* No real displays: monitors_refresh falls back, tests use monitors_set_fake. */
}

BOOL GetMonitorInfoW(HMONITOR m, MONITORINFO *mi)
{
    (void)m; (void)mi;
    return FALSE;
}

BOOL SystemParametersInfoW(UINT action, UINT param, void *pv, UINT ini)
{
    (void)param; (void)ini;
    if (action == SPI_GETWORKAREA) SetRect((RECT *)pv, 0, 0, 1920, 1040);
    return TRUE;
}

int GetSystemMetrics(int index)
{
    return index == SM_CXSCREEN ? 1920 : 1080;
}

HRESULT GetDpiForMonitor(HMONITOR m, MONITOR_DPI_TYPE type, UINT *x, UINT *y)
{
    (void)m; (void)type;
    *x = *y = 96;
    return S_OK;
}
