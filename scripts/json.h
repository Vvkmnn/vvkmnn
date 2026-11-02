/*
 * json.h — minimal JSON parser (RFC 8259 subset sufficient for GitHub/npm/Claude data).
 *
 * Parses a whole document into a tree of JVal. Objects keep keys in order; lookups are
 * linear (documents here are small). Strings are decoded (escapes, \uXXXX -> UTF-8).
 * Memory: every node/string comes from malloc and lives until process exit — these are
 * short-lived CLI programs, so there is deliberately no free routine.
 *
 * Usage:
 *   JVal v; const char *end = jparse(text, &v);   // end == NULL on syntax error
 *   const JVal *name = jget(&v, "name");          // NULL if missing or not an object
 *   for (size_t i = 0; i < arr->n; i++) jat(arr, i) ...
 *
 * Also: jparse_all() parses several concatenated documents (gh --paginate output).
 */
#ifndef JSON_H
#define JSON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JVal {
    JType type;
    double num;           /* J_NUM, J_BOOL (0/1) */
    char *str;            /* J_STR */
    size_t n;             /* J_ARR/J_OBJ element count */
    struct JVal *items;   /* J_ARR/J_OBJ values */
    char **keys;          /* J_OBJ keys, parallel to items */
} JVal;

static inline void *jalloc(size_t size) {
    void *p = malloc(size ? size : 1);
    if (!p) { fputs("json: out of memory\n", stderr); exit(1); }
    return p;
}

static inline const char *jws(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    return s;
}

static inline void jutf8(char **o, unsigned cp) {
    if (cp < 0x80) { *(*o)++ = (char)cp; }
    else if (cp < 0x800) { *(*o)++ = (char)(0xC0 | cp >> 6); *(*o)++ = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        *(*o)++ = (char)(0xE0 | cp >> 12); *(*o)++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *(*o)++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *(*o)++ = (char)(0xF0 | cp >> 18); *(*o)++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *(*o)++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *(*o)++ = (char)(0x80 | (cp & 0x3F));
    }
}

static inline int jhex4(const char *s, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

/** Parses a string starting at the opening quote; returns pointer after the closing quote. */
static inline const char *jstring(const char *s, char **out) {
    const char *p = s + 1;
    size_t cap = 0;
    while (p[cap] && p[cap] != '"') cap += (p[cap] == '\\' && p[cap + 1]) ? 2 : 1;
    char *buf = jalloc(cap * 2 + 1), *o = buf; /* escapes never expand beyond 2x */
    while (*p && *p != '"') {
        if (*p != '\\') { *o++ = *p++; continue; }
        p++;
        switch (*p) {
        case '"': *o++ = '"'; break;
        case '\\': *o++ = '\\'; break;
        case '/': *o++ = '/'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'n': *o++ = '\n'; break;
        case 'r': *o++ = '\r'; break;
        case 't': *o++ = '\t'; break;
        case 'u': {
            unsigned cp, lo;
            if (!jhex4(p + 1, &cp)) return NULL;
            p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && p[1] == '\\' && p[2] == 'u' && jhex4(p + 3, &lo)) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); /* surrogate pair */
                p += 6;
            }
            jutf8(&o, cp);
            break;
        }
        default: return NULL;
        }
        p++;
    }
    if (*p != '"') return NULL;
    *o = '\0';
    *out = buf;
    return p + 1;
}

static inline const char *jvalue(const char *s, JVal *v);

/** Shared array/object body parser; keys != NULL means object. */
static inline const char *jcontainer(const char *s, JVal *v, int is_obj) {
    size_t cap = 8;
    v->type = is_obj ? J_OBJ : J_ARR;
    v->n = 0;
    v->items = jalloc(cap * sizeof *v->items);
    v->keys = is_obj ? jalloc(cap * sizeof *v->keys) : NULL;
    s = jws(s + 1);
    if (*s == (is_obj ? '}' : ']')) return s + 1;
    for (;;) {
        if (v->n == cap) {
            cap *= 2;
            v->items = realloc(v->items, cap * sizeof *v->items);
            if (is_obj) v->keys = realloc(v->keys, cap * sizeof *v->keys);
            if (!v->items || (is_obj && !v->keys)) { fputs("json: out of memory\n", stderr); exit(1); }
        }
        if (is_obj) {
            if (*s != '"' || !(s = jstring(s, &v->keys[v->n]))) return NULL;
            s = jws(s);
            if (*s++ != ':') return NULL;
        }
        if (!(s = jvalue(jws(s), &v->items[v->n]))) return NULL;
        v->n++;
        s = jws(s);
        if (*s == ',') { s = jws(s + 1); continue; }
        if (*s == (is_obj ? '}' : ']')) return s + 1;
        return NULL;
    }
}

static inline const char *jvalue(const char *s, JVal *v) {
    memset(v, 0, sizeof *v);
    switch (*s) {
    case '{': return jcontainer(s, v, 1);
    case '[': return jcontainer(s, v, 0);
    case '"': v->type = J_STR; return jstring(s, &v->str);
    case 't': if (strncmp(s, "true", 4)) return NULL; v->type = J_BOOL; v->num = 1; return s + 4;
    case 'f': if (strncmp(s, "false", 5)) return NULL; v->type = J_BOOL; return s + 5;
    case 'n': if (strncmp(s, "null", 4)) return NULL; v->type = J_NULL; return s + 4;
    default: {
        char *end;
        v->num = strtod(s, &end);
        if (end == s) return NULL;
        v->type = J_NUM;
        return end;
    }
    }
}

/** Parses one document; returns pointer after it, or NULL on error. */
static inline const char *jparse(const char *text, JVal *v) {
    const char *end = jvalue(jws(text), v);
    return end ? jws(end) : NULL;
}

/** Parses concatenated documents (e.g. `gh api --paginate`) into an array of them. */
static inline int jparse_all(const char *text, JVal *out) {
    size_t cap = 4;
    out->type = J_ARR;
    out->n = 0;
    out->keys = NULL;
    out->items = jalloc(cap * sizeof *out->items);
    for (const char *s = jws(text); *s; s = jws(s)) {
        if (out->n == cap) {
            cap *= 2;
            out->items = realloc(out->items, cap * sizeof *out->items);
            if (!out->items) { fputs("json: out of memory\n", stderr); exit(1); }
        }
        if (!(s = jparse(s, &out->items[out->n]))) return 0;
        out->n++;
    }
    return 1;
}

static inline const JVal *jget(const JVal *v, const char *key) {
    if (!v || v->type != J_OBJ) return NULL;
    for (size_t i = 0; i < v->n; i++)
        if (!strcmp(v->keys[i], key)) return &v->items[i];
    return NULL;
}

static inline const JVal *jat(const JVal *v, size_t i) {
    return v && (v->type == J_ARR || v->type == J_OBJ) && i < v->n ? &v->items[i] : NULL;
}

/** Follows a dotted path of object keys, e.g. jpath(v, "data.user.repositories"). */
static inline const JVal *jpath(const JVal *v, const char *path) {
    char key[128];
    while (v && *path) {
        size_t len = strcspn(path, ".");
        if (len >= sizeof key) return NULL;
        memcpy(key, path, len);
        key[len] = '\0';
        v = jget(v, key);
        path += len + (path[len] == '.');
    }
    return v;
}

static inline double jnum(const JVal *v, double fallback) { return v && v->type == J_NUM ? v->num : fallback; }
static inline const char *jstr(const JVal *v) { return v && v->type == J_STR ? v->str : NULL; }

#endif
