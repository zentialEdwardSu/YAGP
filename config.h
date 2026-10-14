#ifndef YAGP_CONFIG_H
#define YAGP_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

typedef struct OverlayConfig {
    const wchar_t *path;
    int x, y;
    int scale_pct;
    bool paused, topmost, locked, clickthrough;
    bool hidden;
    bool key_enabled;
    uint32_t key_rgb;
    int key_tolerance;
    /* Motion script: "" (none), "builtin:<name>" or an absolute .lua path. */
    const wchar_t *script;
} OverlayConfig;

typedef void (*ConfigEntryFn)(const OverlayConfig *entry, void *ctx);

/* %APPDATA%\YAGP; returns false if the folder cannot be resolved. */
bool config_dir(wchar_t *out, size_t cap);
/* Calls fn once per saved overlay. Missing file is not an error. */
void config_load(ConfigEntryFn fn, void *ctx);
/* Atomically replaces %APPDATA%\YAGP\config.txt. */
bool config_save(const OverlayConfig *entries, int count);
/* Recursively removes %APPDATA%\YAGP. */
bool config_delete_all(void);

#endif
