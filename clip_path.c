#include "clip_path.h"

#include <windows.h>
#include <string.h>

/* Reserved DOS device names, which open the device whatever the folder or
 * extension ("sub\con.gif"). */
static bool is_device_name(const wchar_t *seg, size_t n)
{
    static const wchar_t *const names[] = {L"con", L"prn", L"aux", L"nul", L"conin$", L"conout$"};
    size_t stem = 0;
    while (stem < n && seg[stem] != L'.') ++stem;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (wcslen(names[i]) == stem && _wcsnicmp(seg, names[i], stem) == 0) return true;
    }
    /* COM0-9 / LPT0-9, also with superscript digits. */
    if (stem == 4 && (_wcsnicmp(seg, L"com", 3) == 0 || _wcsnicmp(seg, L"lpt", 3) == 0)) {
        wchar_t d = seg[3];
        return (d >= L'0' && d <= L'9') || d == 0xB9 || d == 0xB2 || d == 0xB3;
    }
    return false;
}

static bool valid_segment(const wchar_t *seg, size_t n)
{
    if (n == 0 || n > 255) return false;
    if (seg[0] == L'.' && (n == 1 || (n == 2 && seg[1] == L'.'))) return false; /* "." / ".." */
    if (seg[n - 1] == L'.' || seg[n - 1] == L' ') return false; /* Trimmed by Win32: "a.gif." */
    for (size_t i = 0; i < n; ++i) {
        wchar_t c = seg[i];
        if (c < 32 || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' ||
            c == L'|') return false;
    }
    return !is_device_name(seg, n);
}

bool clip_resolve_path(const wchar_t *base_gif, const char *rel_utf8, wchar_t *out, size_t cap)
{
    if (!base_gif || !rel_utf8 || !out || cap == 0) return false;
    out[0] = 0;
    wchar_t rel[MAX_PATH];
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, rel_utf8, -1, rel, MAX_PATH);
    if (n <= 1) return false; /* Invalid UTF-8, too long or empty. */
    size_t len = (size_t)n - 1;
    for (size_t i = 0; i < len; ++i) {
        if (rel[i] == L'/') rel[i] = L'\\';
    }
    /* Absolute, rooted ("\x") or drive-relative ("C:x") paths. ':' is also
     * caught per segment; this keeps the intent obvious. */
    if (rel[0] == L'\\' || (len >= 2 && rel[1] == L':')) return false;
    for (size_t i = 0; i < len;) {
        size_t j = i;
        while (j < len && rel[j] != L'\\') ++j;
        if (!valid_segment(rel + i, j - i)) return false; /* Also rejects "a\\b" and a trailing '\'. */
        i = j + 1;
        if (j == len - 1) return false; /* Trailing separator. */
    }
    if (len < 5 || _wcsicmp(rel + len - 4, L".gif") != 0) return false;

    /* Folder of the main GIF, including its final separator. */
    const wchar_t *slash = wcsrchr(base_gif, L'\\');
    const wchar_t *fwd = wcsrchr(base_gif, L'/');
    if (fwd > slash) slash = fwd;
    if (!slash) return false;
    size_t dir = (size_t)(slash - base_gif) + 1;
    if (dir + len + 1 > cap || dir + len >= MAX_PATH * 4) return false;
    memcpy(out, base_gif, dir * sizeof(wchar_t));
    memcpy(out + dir, rel, (len + 1) * sizeof(wchar_t));
    /* Every segment was checked, so no normalisation can climb out of the
     * folder: the result is the folder followed by plain names. */
    return true;
}
