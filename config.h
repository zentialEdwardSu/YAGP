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
    uint64_t uid;         /* Stable identity; 0 = allocate on load. */
    uint64_t leader_uid;  /* 0 = not a follower. */
    uint64_t group_uid;   /* 0 = no group. */
    bool defer_script;    /* Restore phase: create the overlay before starting it. */
} OverlayConfig;

typedef struct GroupConfig {
    uint64_t uid;
    const wchar_t *name;
    const wchar_t *folder;
    bool hidden;
} GroupConfig;

typedef struct ConfigSave {
    const GroupConfig *groups;
    int group_count;
    const OverlayConfig *overlays;
    int overlay_count;
} ConfigSave;

typedef void (*ConfigEntryFn)(const OverlayConfig *entry, void *ctx);
typedef void (*ConfigGroupFn)(const GroupConfig *entry, void *ctx);

/* %APPDATA%\YAGP; returns false if the folder cannot be resolved. */
bool config_dir(wchar_t *out, size_t cap);
/* Calls group_fn for v4 group records, then entry_fn once per saved overlay.
 * Missing file is not an error. v1-v3 overlay lines are accepted. */
void config_load(ConfigEntryFn fn, ConfigGroupFn group_fn, void *ctx);
/* Atomically replaces %APPDATA%\YAGP\config.txt (v4). */
bool config_save(const ConfigSave *save);
/* Recursively removes %APPDATA%\YAGP. */
bool config_delete_all(void);

#ifdef YAGP_CONFIG_TEST
/* Redirects config_dir() to a test folder; does not affect production. */
void config_test_set_dir(const wchar_t *dir);
#endif

#endif
