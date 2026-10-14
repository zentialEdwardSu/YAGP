#ifndef YAGP_POSIX_SHELLAPI_H
#define YAGP_POSIX_SHELLAPI_H
#include <windows.h>

#define FO_DELETE 3
#define FOF_NO_UI 0x0614
typedef struct SHFILEOPSTRUCTW {
    HWND hwnd;
    UINT wFunc;
    PCWSTR pFrom, pTo;
    WORD fFlags;
    BOOL fAnyOperationsAborted;
    void *hNameMappings;
    PCWSTR lpszProgressTitle;
} SHFILEOPSTRUCTW;
int SHFileOperationW(SHFILEOPSTRUCTW *op);

#endif
