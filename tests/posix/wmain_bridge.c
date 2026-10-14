/* Calls the test's wmain() with UTF-8 argv converted to wide strings. */
#include <windows.h>

#include <stdlib.h>

int wmain(int argc, wchar_t **argv);

int main(int argc, char **argv)
{
    wchar_t **wargv = (wchar_t **)calloc((size_t)argc + 1, sizeof(wchar_t *));
    if (!wargv) return 99;
    for (int i = 0; i < argc; ++i) {
        int n = MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, NULL, 0);
        wargv[i] = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
        if (!wargv[i]) return 99;
        MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, wargv[i], n);
    }
    int rc = wmain(argc, wargv);
    for (int i = 0; i < argc; ++i) free(wargv[i]);
    free(wargv);
    return rc;
}
