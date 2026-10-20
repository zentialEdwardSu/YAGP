#ifndef YAGP_GROUP_H
#define YAGP_GROUP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

typedef struct Group {
    struct Group *next;
    uint64_t uid;
    wchar_t *name;
    wchar_t *folder;
    bool hidden;
    bool removing;       /* group_remove is deleting members; don't auto-delete early. */
} Group;

typedef struct GroupMember {
    wchar_t *media;
    wchar_t *script;
} GroupMember;

typedef struct GroupScan {
    GroupMember *items;
    int count;
} GroupScan;

Group *group_create(const wchar_t *name, const wchar_t *folder);
void group_destroy(Group *g);
Group *group_find_uid(uint64_t uid);
Group *group_find_folder(const wchar_t *folder);
int group_member_count(const Group *g);
bool group_is_empty(const Group *g);
void group_set_hidden(Group *g, bool hidden);
void group_remove(Group *g);
/* Called by app_remove_overlay after a member leaves: destroys an empty group. */
void group_after_member_removed(Group *g);

/* Recursively scans folder; only same-directory, same-stem media + .lua pairs
 * are returned. */
bool group_scan_folder(const wchar_t *folder, GroupScan *out, wchar_t *err, size_t cap);
void group_scan_free(GroupScan *s);

/* Scans and creates a group plus its overlays. On failure err describes why. */
bool group_import_folder(const wchar_t *folder, wchar_t *err, size_t cap);

#endif
