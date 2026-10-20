#include "still_image.h"

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const wchar_t *path, size_t *size)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER n;
    if (!GetFileSizeEx(f, &n) || n.QuadPart <= 0 || n.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(f);
        return NULL;
    }
    uint8_t *data = (uint8_t *)malloc((size_t)n.QuadPart);
    DWORD got = 0;
    if (!data || !ReadFile(f, data, (DWORD)n.QuadPart, &got, NULL) || got != (DWORD)n.QuadPart) {
        free(data);
        CloseHandle(f);
        return NULL;
    }
    CloseHandle(f);
    *size = got;
    return data;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) return 2;
    size_t len = 0;
    uint8_t *data = read_file(argv[1], &len);
    if (!data) return 1;
    StillImage img;
    if (still_image_open_memory(&img, data, len) != 0) {
        free(data);
        return 1;
    }
    free(data);
    if (argc >= 4) {
        uint32_t rgb = (uint32_t)wcstoul(argv[2], NULL, 16);
        int tol = (int)wcstol(argv[3], NULL, 10);
        still_image_set_color_key(&img, true, rgb, tol);
    }
    if (still_image_set_output_size(&img, img.width, img.height) != 0) {
        still_image_close(&img);
        return 1;
    }
    wprintf(L"%d %d %d %d %d %d %d %d\n", img.width, img.height, img.out_w, img.out_h,
            img.opaque[0], img.opaque[1], img.opaque[2], img.opaque[3]);
    still_image_close(&img);
    return 0;
}
