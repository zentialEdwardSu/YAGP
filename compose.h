#ifndef YAGP_COMPOSE_H
#define YAGP_COMPOSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

typedef struct ComposeMember {
    wchar_t *media;   /* Canonical absolute path. */
    wchar_t *script;  /* Canonical absolute path, or NULL. */
    int x, y;         /* Valid when has_position. */
    bool has_position;
    int scale_pct;
    bool paused, hidden, topmost, locked, clickthrough;
    bool key_enabled;
    uint32_t key_rgb;
    int key_tolerance;
    int leader_index; /* Index in ComposeDoc.members, or -1. */
} ComposeMember;

typedef struct ComposeDoc {
    wchar_t *name;
    ComposeMember *members;
    int count;
    wchar_t *root;    /* Directory containing the compose file. */
} ComposeDoc;

/* Parses and validates a *.compose file. Media/script paths are resolved
 * relative to the file's directory and must stay inside it. On failure returns
 * false and writes a UTF-8-independent (wide) message to err. */
bool compose_parse_file(const wchar_t *path, ComposeDoc *out, wchar_t *err, size_t cap);
void compose_free(ComposeDoc *doc);

#endif
