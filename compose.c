#include "compose.h"

#include <yaml.h>

#include <windows.h>

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ComposeParser {
    yaml_document_t doc;
    ComposeDoc *out;
    const wchar_t *root;
    size_t root_len;
    wchar_t *err;
    size_t err_cap;
    int line;
} ComposeParser;

static void set_error(ComposeParser *p, const wchar_t *fmt, ...)
{
    if (!p->err || !p->err_cap) return;
    wchar_t body[384];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(body, ARRAYSIZE(body), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (p->line > 0)
        _snwprintf_s(p->err, p->err_cap, _TRUNCATE, L"line %d: %ls", p->line, body);
    else
        wcsncpy_s(p->err, p->err_cap, body, _TRUNCATE);
}

static yaml_node_t *node_at(ComposeParser *p, int index)
{
    return yaml_document_get_node(&p->doc, index);
}

static const char *scalar_value(yaml_node_t *n)
{
    return n && n->type == YAML_SCALAR_NODE ? (const char *)n->data.scalar.value : NULL;
}

static bool to_wide(const char *s, wchar_t **out)
{
    *out = NULL;
    if (!s) return false;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0) return false;
    wchar_t *w = (wchar_t *)calloc((size_t)n, sizeof(wchar_t));
    if (!w) return false;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n)) {
        free(w);
        return false;
    }
    *out = w;
    return true;
}

static bool parse_bool(ComposeParser *p, yaml_node_t *n, bool *out)
{
    const char *s = scalar_value(n);
    if (!s) {
        set_error(p, L"expected true or false");
        return false;
    }
    if (strcmp(s, "true") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(s, "false") == 0) {
        *out = false;
        return true;
    }
    set_error(p, L"expected true or false, got '%hs'", s);
    return false;
}

static bool parse_int(ComposeParser *p, yaml_node_t *n, long *out)
{
    const char *s = scalar_value(n);
    if (!s || !*s) {
        set_error(p, L"expected an integer");
        return false;
    }
    wchar_t *w = NULL;
    if (!to_wide(s, &w)) {
        set_error(p, L"invalid UTF-8 integer");
        return false;
    }
    wchar_t *end = NULL;
    long v = wcstol(w, &end, 10);
    bool ok = end && *end == 0;
    free(w);
    if (!ok) {
        set_error(p, L"expected an integer, got '%hs'", s);
        return false;
    }
    *out = v;
    return true;
}

static bool is_within_root(const wchar_t *root, size_t root_len, const wchar_t *path)
{
    return _wcsnicmp(root, path, root_len) == 0;
}

/* Resolves a relative member path. Rejects absolute paths, drive changes and
 * paths that escape the compose directory. */
static wchar_t *resolve_path(ComposeParser *p, const char *value)
{
    wchar_t *rel = NULL;
    if (!to_wide(value, &rel)) {
        set_error(p, L"path is not valid UTF-8");
        return NULL;
    }
    if (!*rel || rel[0] == L'\\' || rel[0] == L'/' ||
        (rel[0] && rel[1] == L':' && ((rel[0] >= L'A' && rel[0] <= L'Z') ||
                                      (rel[0] >= L'a' && rel[0] <= L'z')))) {
        set_error(p, L"member path must be relative: '%ls'", rel);
        free(rel);
        return NULL;
    }
    wchar_t joined[MAX_PATH * 4];
    if (_snwprintf_s(joined, ARRAYSIZE(joined), _TRUNCATE, L"%ls%ls", p->root, rel) <= 0) {
        set_error(p, L"member path is too long");
        free(rel);
        return NULL;
    }
    free(rel);
    wchar_t full[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(joined, ARRAYSIZE(full), full, NULL);
    if (!n || n >= ARRAYSIZE(full)) {
        set_error(p, L"member path is too long");
        return NULL;
    }
    if (!is_within_root(p->root, p->root_len, full)) {
        set_error(p, L"member path escapes the compose directory: '%ls'", full);
        return NULL;
    }
    return _wcsdup(full);
}

static bool file_exists(const wchar_t *path, bool directory_ok)
{
    DWORD a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES) return false;
    return directory_ok || !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static yaml_node_t *find_value(ComposeParser *p, yaml_node_t *map, const char *key, bool *found)
{
    *found = false;
    if (!map || map->type != YAML_MAPPING_NODE) return NULL;
    for (yaml_node_pair_t *pair = map->data.mapping.pairs.start;
         pair < map->data.mapping.pairs.top; ++pair) {
        yaml_node_t *k = node_at(p, pair->key);
        if (k && k->type == YAML_SCALAR_NODE && strcmp(scalar_value(k), key) == 0) {
            *found = true;
            return node_at(p, pair->value);
        }
    }
    return NULL;
}

static bool reject_unknown(ComposeParser *p, yaml_node_t *map, const char *const *known, int n)
{
    if (!map || map->type != YAML_MAPPING_NODE) return false;
    for (yaml_node_pair_t *pair = map->data.mapping.pairs.start;
         pair < map->data.mapping.pairs.top; ++pair) {
        yaml_node_t *k = node_at(p, pair->key);
        if (!k || k->type != YAML_SCALAR_NODE) return false;
        for (yaml_node_pair_t *q = map->data.mapping.pairs.start; q < pair; ++q) {
            yaml_node_t *other = node_at(p, q->key);
            if (other && other->type == YAML_SCALAR_NODE &&
                strcmp(scalar_value(k), scalar_value(other)) == 0) {
                set_error(p, L"duplicate key '%hs'", scalar_value(k));
                return false;
            }
        }
        bool ok = false;
        for (int i = 0; i < n; ++i)
            if (strcmp(scalar_value(k), known[i]) == 0) { ok = true; break; }
        if (!ok) {
            set_error(p, L"unknown key '%hs'", scalar_value(k));
            return false;
        }
    }
    return true;
}

static bool is_still_media(const wchar_t *path)
{
    const wchar_t *dot = wcsrchr(path, L'.');
    if (!dot) return false;
    return _wcsicmp(dot, L".png") == 0 || _wcsicmp(dot, L".jpg") == 0 ||
           _wcsicmp(dot, L".jpeg") == 0;
}

static bool is_lua_script(const wchar_t *path)
{
    const wchar_t *dot = wcsrchr(path, L'.');
    return dot && _wcsicmp(dot, L".lua") == 0;
}

static bool parse_position(ComposeParser *p, yaml_node_t *n, ComposeMember *m)
{
    if (!n || n->type != YAML_SEQUENCE_NODE ||
        n->data.sequence.items.top - n->data.sequence.items.start != 2) {
        set_error(p, L"'position' must be [x, y]");
        return false;
    }
    long x, y;
    yaml_node_t *a = node_at(p, n->data.sequence.items.start[0]);
    yaml_node_t *b = node_at(p, n->data.sequence.items.start[1]);
    if (!parse_int(p, a, &x) || !parse_int(p, b, &y)) return false;
    if (x < INT_MIN || x > INT_MAX || y < INT_MIN || y > INT_MAX) {
        set_error(p, L"'position' is out of range");
        return false;
    }
    m->x = (int)x;
    m->y = (int)y;
    m->has_position = true;
    return true;
}

static bool parse_key(ComposeParser *p, yaml_node_t *n, ComposeMember *m)
{
    static const char *const known[] = {"enabled", "color", "tolerance"};
    if (!n || n->type != YAML_MAPPING_NODE || !reject_unknown(p, n, known, 3)) return false;
    bool found;
    yaml_node_t *v = find_value(p, n, "enabled", &found);
    if (found && !parse_bool(p, v, &m->key_enabled)) return false;
    v = find_value(p, n, "color", &found);
    if (found) {
        const char *s = scalar_value(v);
        wchar_t *w = NULL;
        if (!s || !to_wide(s, &w) || (wcslen(w) != 7 && wcslen(w) != 4) || w[0] != L'#') {
            free(w);
            set_error(p, L"'key.color' must be #RRGGBB");
            return false;
        }
        unsigned rgb = 0;
        bool ok = true;
        if (wcslen(w) == 7) {
            ok = swscanf_s(w + 1, L"%6x", &rgb) == 1;
        } else {
            unsigned short r, g, b;
            ok = swscanf_s(w + 1, L"%1hx%1hx%1hx", &r, &g, &b) == 3;
            if (ok) rgb = ((unsigned)r << 20) | ((unsigned)r << 16) |
                          ((unsigned)g << 12) | ((unsigned)g << 8) |
                          ((unsigned)b << 4) | (unsigned)b;
        }
        free(w);
        if (!ok) {
            set_error(p, L"'key.color' must be #RRGGBB");
            return false;
        }
        m->key_rgb = rgb & 0xFFFFFF;
    }
    v = find_value(p, n, "tolerance", &found);
    if (found) {
        long t;
        if (!parse_int(p, v, &t)) return false;
        if (t < 0 || t > 255) {
            set_error(p, L"'key.tolerance' must be 0..255");
            return false;
        }
        m->key_tolerance = (int)t;
    }
    return true;
}

static bool parse_member(ComposeParser *p, yaml_node_t *n, ComposeMember *m)
{
    static const char *const known[] = {
        "media", "script", "position", "scale", "paused", "hidden",
        "topmost", "locked", "clickthrough", "key", "leader"
    };
    if (!n || n->type != YAML_MAPPING_NODE) {
        set_error(p, L"each member must be a mapping");
        return false;
    }
    p->line = (int)n->start_mark.line + 1;
    if (!reject_unknown(p, n, known, (int)ARRAYSIZE(known))) return false;
    m->leader_index = -1;
    m->scale_pct = 100;
    m->key_tolerance = 16;
    bool found;
    yaml_node_t *v = find_value(p, n, "media", &found);
    if (!found) {
        set_error(p, L"member is missing 'media'");
        return false;
    }
    const char *s = scalar_value(v);
    if (!s || !*s) {
        set_error(p, L"'media' must be a string");
        return false;
    }
    m->media = resolve_path(p, s);
    if (!m->media) return false;
    if (!file_exists(m->media, false)) {
        set_error(p, L"media does not exist: '%ls'", m->media);
        return false;
    }
    v = find_value(p, n, "script", &found);
    if (found) {
        s = scalar_value(v);
        if (!s || !*s) {
            set_error(p, L"'script' must be a string");
            return false;
        }
        m->script = resolve_path(p, s);
        if (!m->script) return false;
        if (!is_lua_script(m->script)) {
            set_error(p, L"'script' must be a .lua file");
            return false;
        }
        if (!file_exists(m->script, false)) {
            set_error(p, L"script does not exist: '%ls'", m->script);
            return false;
        }
    }
    v = find_value(p, n, "position", &found);
    if (found && !parse_position(p, v, m)) return false;
    v = find_value(p, n, "scale", &found);
    if (found) {
        long scale;
        if (!parse_int(p, v, &scale)) return false;
        if (scale < 5 || scale > 250) {
            set_error(p, L"'scale' must be 5..250");
            return false;
        }
        m->scale_pct = (int)scale;
    }
    struct { const char *name; bool *out; } flags[] = {
        {"paused", &m->paused}, {"hidden", &m->hidden}, {"topmost", &m->topmost},
        {"locked", &m->locked}, {"clickthrough", &m->clickthrough},
    };
    for (size_t i = 0; i < ARRAYSIZE(flags); ++i) {
        v = find_value(p, n, flags[i].name, &found);
        if (found && !parse_bool(p, v, flags[i].out)) return false;
    }
    if (m->paused && is_still_media(m->media)) {
        set_error(p, L"still images cannot be paused");
        return false;
    }
    v = find_value(p, n, "key", &found);
    if (found && !parse_key(p, v, m)) return false;
    return true;
}

static bool parse_members(ComposeParser *p, yaml_node_t *n)
{
    if (!n || n->type != YAML_SEQUENCE_NODE) {
        set_error(p, L"'members' must be a list");
        return false;
    }
    int count = (int)(n->data.sequence.items.top - n->data.sequence.items.start);
    if (count <= 0) {
        set_error(p, L"'members' must not be empty");
        return false;
    }
    if (count > 128) {
        set_error(p, L"too many members (max 128)");
        return false;
    }
    p->out->members = (ComposeMember *)calloc((size_t)count, sizeof(*p->out->members));
    if (!p->out->members) {
        set_error(p, L"out of memory");
        return false;
    }
    p->out->count = count;
    for (int i = 0; i < count; ++i) {
        yaml_node_t *m = node_at(p, n->data.sequence.items.start[i]);
        if (!parse_member(p, m, &p->out->members[i])) return false;
        for (int j = 0; j < i; ++j) {
            if (_wcsicmp(p->out->members[j].media, p->out->members[i].media) == 0) {
                set_error(p, L"duplicate media '%ls'", p->out->members[i].media);
                return false;
            }
        }
    }
    return true;
}

static bool resolve_leaders(ComposeParser *p, yaml_node_t *members)
{
    for (int i = 0; i < p->out->count; ++i) {
        yaml_node_t *mn = node_at(p, members->data.sequence.items.start[i]);
        bool found;
        yaml_node_t *v = find_value(p, mn, "leader", &found);
        if (!found) continue;
        const char *s = scalar_value(v);
        if (!s || !*s) {
            set_error(p, L"'leader' must be a string");
            return false;
        }
        wchar_t *leader_path = resolve_path(p, s);
        if (!leader_path) return false;
        int target = -1;
        for (int j = 0; j < p->out->count; ++j)
            if (_wcsicmp(p->out->members[j].media, leader_path) == 0) { target = j; break; }
        if (target < 0) {
            set_error(p, L"leader is not listed in members: '%ls'", leader_path);
            free(leader_path);
            return false;
        }
        if (target == i) {
            set_error(p, L"a member cannot follow itself");
            free(leader_path);
            return false;
        }
        free(leader_path);
        p->out->members[i].leader_index = target;
    }
    for (int i = 0; i < p->out->count; ++i) {
        if (p->out->members[i].leader_index < 0) continue;
        for (int j = 0; j < p->out->count; ++j) {
            if (p->out->members[j].leader_index == i) {
                set_error(p, L"only one leader/follower level is allowed");
                return false;
            }
        }
    }
    return true;
}

bool compose_parse_file(const wchar_t *path, ComposeDoc *out, wchar_t *err, size_t cap)
{
    if (!path || !out) return false;
    memset(out, 0, sizeof(*out));
    if (err && cap) err[0] = 0;

    wchar_t full[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(path, ARRAYSIZE(full), full, NULL);
    if (!n || n >= ARRAYSIZE(full)) {
        if (err) wcsncpy_s(err, cap, L"compose path is too long", _TRUNCATE);
        return false;
    }
    const wchar_t *slash = wcsrchr(full, L'\\');
    if (!slash) {
        if (err) wcsncpy_s(err, cap, L"compose path has no directory", _TRUNCATE);
        return false;
    }
    size_t root_len = (size_t)(slash - full) + 1;
    out->root = (wchar_t *)calloc(root_len + 1, sizeof(wchar_t));
    if (!out->root) {
        if (err) wcsncpy_s(err, cap, L"out of memory", _TRUNCATE);
        return false;
    }
    wcsncpy_s(out->root, root_len + 1, full, root_len);

    FILE *f = _wfopen(full, L"rb");
    if (!f) {
        if (err) _snwprintf_s(err, cap, _TRUNCATE, L"cannot open compose file: %ls", full);
        compose_free(out);
        return false;
    }

    ComposeParser p;
    memset(&p, 0, sizeof(p));
    p.out = out;
    p.root = out->root;
    p.root_len = root_len;
    p.err = err;
    p.err_cap = cap;
    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        fclose(f);
        if (err) wcsncpy_s(err, cap, L"cannot initialize YAML parser", _TRUNCATE);
        compose_free(out);
        return false;
    }
    yaml_parser_set_input_file(&parser, f);
    if (!yaml_parser_load(&parser, &p.doc)) {
        p.line = (int)parser.problem_mark.line + 1;
        set_error(&p, L"%hs", parser.problem ? parser.problem : "invalid YAML");
        yaml_parser_delete(&parser);
        fclose(f);
        compose_free(out);
        return false;
    }
    yaml_parser_delete(&parser);
    fclose(f);

    yaml_node_t *root = yaml_document_get_root_node(&p.doc);
    if (!root || root->type != YAML_MAPPING_NODE) {
        if (err) wcsncpy_s(err, cap, L"compose root must be a mapping", _TRUNCATE);
        yaml_document_delete(&p.doc);
        compose_free(out);
        return false;
    }
    static const char *const known[] = {"version", "name", "members"};
    if (!reject_unknown(&p, root, known, (int)ARRAYSIZE(known))) goto fail;
    bool found;
    yaml_node_t *v = find_value(&p, root, "version", &found);
    long version = 0;
    if (!found || !parse_int(&p, v, &version) || version != 1) {
        if (!found) set_error(&p, L"missing 'version'");
        else if (version != 1) set_error(&p, L"unsupported version (expected 1)");
        goto fail;
    }
    v = find_value(&p, root, "name", &found);
    if (found) {
        const char *s = scalar_value(v);
        if (!s) {
            set_error(&p, L"'name' must be a string");
            goto fail;
        }
        if (!to_wide(s, &out->name)) {
            set_error(&p, L"'name' is not valid UTF-8");
            goto fail;
        }
    }
    v = find_value(&p, root, "members", &found);
    if (!found || !parse_members(&p, v)) {
        if (!found) set_error(&p, L"missing 'members'");
        goto fail;
    }
    if (!resolve_leaders(&p, v)) goto fail;

    yaml_document_delete(&p.doc);
    return true;

fail:
    yaml_document_delete(&p.doc);
    compose_free(out);
    return false;
}

void compose_free(ComposeDoc *doc)
{
    if (!doc) return;
    for (int i = 0; i < doc->count; ++i) {
        free(doc->members[i].media);
        free(doc->members[i].script);
    }
    free(doc->members);
    free(doc->name);
    free(doc->root);
    memset(doc, 0, sizeof(*doc));
}
