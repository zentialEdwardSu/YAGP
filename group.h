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

/* Imports a group described by a *.compose file. The file's directory is the
 * group root. On failure err describes why and nothing is left behind. */
bool group_import_compose(const wchar_t *compose_path, wchar_t *err, size_t cap);

/* Writes a commented template compose file to path. The path must end in
 * .compose; an existing file is replaced. */
bool group_create_template(const wchar_t *path, wchar_t *err, size_t cap);

#endif
