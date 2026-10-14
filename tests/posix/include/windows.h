/* Minimal Win32 stand-in so the platform-independent modules (gif_stream,
 * frame_cache, script, contacts, monitors) and their tests build and run on
 * Linux/macOS under sanitizers. Not used by the real build. Only what those
 * files call is provided; see win32_shim.c.
 *
 * Differences from Windows that matter here: wchar_t is 32-bit, paths are
 * converted to UTF-8 with '\' mapped to '/', and the MSVC wide-printf "%s"
 * (wide) / "%hs" (narrow) convention is translated for glibc. */
#ifndef YAGP_POSIX_WINDOWS_H
#define YAGP_POSIX_WINDOWS_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define WINAPI
#define CALLBACK
#define TRUE 1
#define FALSE 0
#define MAX_PATH 260

typedef int BOOL;
typedef unsigned char BYTE;
typedef uint16_t WORD;
typedef unsigned long DWORD; /* Wider than on Windows so "%lu" stays well-defined. */
typedef long LONG; /* 'long' as on Windows, so "%ld" matches. */
typedef int64_t LONGLONG;
typedef unsigned int UINT;
typedef intptr_t LPARAM;
typedef uintptr_t WPARAM;
typedef intptr_t LRESULT;
typedef intptr_t INT_PTR;
typedef uintptr_t ULONG_PTR;
typedef uintptr_t DWORD_PTR;
typedef long HRESULT;
typedef void *HANDLE;
typedef void *HMONITOR;
typedef void *HDC;
typedef void *HWND;
typedef void *HINSTANCE;
typedef wchar_t WCHAR;
typedef wchar_t *LPWSTR, *PWSTR;
typedef const wchar_t *LPCWSTR, *PCWSTR;

typedef union LARGE_INTEGER { LONGLONG QuadPart; } LARGE_INTEGER;
typedef struct RECT { LONG left, top, right, bottom; } RECT, *LPRECT;
typedef struct POINT { LONG x, y; } POINT;
typedef struct FILETIME { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;

#define S_OK ((HRESULT)0)
#define E_FAIL ((HRESULT)0x80004005L)
#define FAILED(hr) ((HRESULT)(hr) < 0)
#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)

/* ---- files ---------------------------------------------------------------- */
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define GENERIC_READ 0x80000000ul
#define GENERIC_WRITE 0x40000000ul
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define FILE_SHARE_DELETE 4
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_FLAG_RANDOM_ACCESS 0x10000000
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define FILE_CURRENT 1
#define PAGE_READONLY 2
#define FILE_MAP_READ 4
#define MOVEFILE_REPLACE_EXISTING 1
#define MOVEFILE_WRITE_THROUGH 8

typedef enum GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard } GET_FILEEX_INFO_LEVELS;
typedef struct WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;

HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, void *sa, DWORD disposition, DWORD flags,
                   HANDLE tmpl);
BOOL CloseHandle(HANDLE h);
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size);
BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, void *ov);
BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *wrote, void *ov);
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER *pos, DWORD method);
BOOL FlushFileBuffers(HANDLE h);
HANDLE CreateFileMappingW(HANDLE file, void *sa, DWORD protect, DWORD hi, DWORD lo, LPCWSTR name);
void *MapViewOfFile(HANDLE mapping, DWORD access, DWORD hi, DWORD lo, size_t n);
BOOL UnmapViewOfFile(const void *view);
DWORD GetFileAttributesW(LPCWSTR path);
BOOL GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, void *out);
BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags);
BOOL DeleteFileW(LPCWSTR path);
BOOL CreateDirectoryW(LPCWSTR path, void *sa);

/* ---- misc ----------------------------------------------------------------- */
#define CP_UTF8 65001
int MultiByteToWideChar(UINT cp, DWORD flags, const char *src, int n, wchar_t *dst, int cap);
int WideCharToMultiByte(UINT cp, DWORD flags, const wchar_t *src, int n, char *dst, int cap,
                        const char *def, BOOL *used_def);
BOOL QueryPerformanceCounter(LARGE_INTEGER *t);
BOOL QueryPerformanceFrequency(LARGE_INTEGER *f);
void OutputDebugStringW(LPCWSTR s);
DWORD GetCurrentProcessId(void);
DWORD GetCurrentThreadId(void);
LPWSTR CharLowerW(LPWSTR s);
void Sleep(DWORD ms);

BOOL SetRect(RECT *r, int l, int t, int rr, int b);
BOOL UnionRect(RECT *out, const RECT *a, const RECT *b);
BOOL IntersectRect(RECT *out, const RECT *a, const RECT *b);
BOOL IsRectEmpty(const RECT *r);

/* ---- monitors (monitors.c; tests use monitors_set_fake) --------------------- */
#define MONITORINFOF_PRIMARY 1
#define SPI_GETWORKAREA 0x30
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
typedef struct MONITORINFO { DWORD cbSize; RECT rcMonitor, rcWork; DWORD dwFlags; } MONITORINFO;
typedef BOOL (*MONITORENUMPROC)(HMONITOR, HDC, LPRECT, LPARAM);
BOOL EnumDisplayMonitors(HDC dc, const RECT *clip, MONITORENUMPROC fn, LPARAM lp);
BOOL GetMonitorInfoW(HMONITOR m, MONITORINFO *mi);
BOOL SystemParametersInfoW(UINT action, UINT param, void *pv, UINT ini);
int GetSystemMetrics(int index);

/* ---- MSVC CRT "secure" functions ------------------------------------------- */
#define _TRUNCATE ((size_t)-1)
#define STRUNCATE 80
int _snwprintf_s(wchar_t *buf, size_t size, size_t count, const wchar_t *fmt, ...);
int _snprintf_s(char *buf, size_t size, size_t count, const char *fmt, ...);
int strncpy_s(char *dst, size_t size, const char *src, size_t count);
int wcsncpy_s(wchar_t *dst, size_t size, const wchar_t *src, size_t count);
FILE *_wfopen(const wchar_t *path, const wchar_t *mode);
int _wtoi(const wchar_t *s);
/* glibc's swprintf reads "%s" as narrow; MSVC (and the test sources) mean wide. */
int yagp_swprintf(wchar_t *buf, size_t size, const wchar_t *fmt, ...);
#define swprintf yagp_swprintf

/* Path conversion used by the shim, exposed for the test drivers. */
char *yagp_path_utf8(const wchar_t *path);

#endif
