#ifndef YAGP_POSIX_SHLOBJ_H
#define YAGP_POSIX_SHLOBJ_H
#include <windows.h>

/* SHGetKnownFolderPath reads $YAGP_LOCALAPPDATA / $YAGP_APPDATA (default /tmp). */
typedef int KNOWNFOLDERID;
extern const KNOWNFOLDERID FOLDERID_LocalAppData, FOLDERID_RoamingAppData, FOLDERID_Documents;
HRESULT SHGetKnownFolderPath(const KNOWNFOLDERID *id, DWORD flags, HANDLE token, PWSTR *out);
void CoTaskMemFree(void *p);
int SHCreateDirectoryExW(HWND hwnd, LPCWSTR path, void *sa);

#endif
