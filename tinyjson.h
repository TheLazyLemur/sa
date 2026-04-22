/* tinyjson.h — minimal cJSON-compatible JSON implementation, single-header.
   Zero dep. Supports only what tiny_c uses. */
#ifndef TINYJSON_H
#define TINYJSON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Types — must match cJSON constant values */
#define cJSON_False   0
#define cJSON_True    1
#define cJSON_NULL    2
#define cJSON_Number  3
#define cJSON_String  4
#define cJSON_Array   5
#define cJSON_Object  6
#define cJSON_Raw     7

typedef struct cJSON {
    struct cJSON *next;
    struct cJSON *prev;
    struct cJSON *child;
    struct cJSON *parent;  /* for detach */
    int type;
    char *valuestring;
    int valueint;
    double valuedouble;
    char *string;     /* object key, if child of object */
    int is_ref;       /* 1 = reference, don't delete children */
} cJSON;

/* ---- Create ---- */
static cJSON *cJSON_CreateNull(void);
static cJSON *cJSON_CreateObject(void);
static cJSON *cJSON_CreateArray(void);
static cJSON *cJSON_CreateString(const char *s);
static cJSON *cJSON_CreateNumber(double n);
static cJSON *cJSON_CreateBool(int b);

/* ---- Delete ---- */
static void cJSON_Delete(cJSON *item);

/* ---- Add ---- */
static void cJSON_AddItemToArray(cJSON *array, cJSON *item);
static void cJSON_AddItemToObject(cJSON *object, const char *key, cJSON *item);
static void cJSON_AddItemReferenceToObject(cJSON *object, const char *key, cJSON *item);
static void cJSON_AddStringToObject(cJSON *object, const char *key, const char *val);
static void cJSON_AddNumberToObject(cJSON *object, const char *key, double val);
static void cJSON_AddBoolToObject(cJSON *object, const char *key, int val);

/* ---- Access ---- */
static cJSON *cJSON_GetObjectItem(const cJSON *object, const char *key);
static cJSON *cJSON_GetArrayItem(const cJSON *array, int index);
static int cJSON_GetArraySize(const cJSON *array);
static const char *cJSON_GetStringValue(const cJSON *item);

/* ---- Type ---- */
static int cJSON_IsNumber(const cJSON *item);
static int cJSON_IsArray(const cJSON *item);
static int cJSON_IsString(const cJSON *item);
static int cJSON_IsBool(const cJSON *item);
static int cJSON_IsTrue(const cJSON *item);

/* ---- Mutate ---- */
static char *cJSON_SetValuestring(cJSON *item, const char *valuestring);

/* ---- Parse / Print ---- */
static cJSON *cJSON_Parse(const char *text);
static char *cJSON_PrintUnformatted(const cJSON *item);

/* For cJSON_ArrayForEach compatibility */
#define cJSON_ArrayForEach(el, arr) for (el = (arr) ? (arr)->child : NULL; el; el = el->next)

/* ==================== IMPLEMENTATION ==================== */

static cJSON *cj_new(int type) {
    cJSON *j = calloc(1, sizeof *j);
    if (!j) abort();
    j->type = type;
    return j;
}

static cJSON *cJSON_CreateNull(void) { return cj_new(cJSON_NULL); }
static cJSON *cJSON_CreateObject(void) { return cj_new(cJSON_Object); }
static cJSON *cJSON_CreateArray(void) { return cj_new(cJSON_Array); }

static cJSON *cJSON_CreateString(const char *s) {
    cJSON *j = cj_new(cJSON_String);
    j->valuestring = s ? strdup(s) : strdup("");
    return j;
}

static cJSON *cJSON_CreateNumber(double n) {
    cJSON *j = cj_new(cJSON_Number);
    j->valuedouble = n;
    j->valueint = (int)n;
    return j;
}

static cJSON *cJSON_CreateBool(int b) { return cj_new(b ? cJSON_True : cJSON_False); }

static void cJSON_Delete(cJSON *item) {
    while (item) {
        cJSON *next = item->next;
        if (!item->is_ref && item->child) cJSON_Delete(item->child);
        free(item->valuestring);
        free(item->string);
        free(item);
        item = next;
    }
}

static void cj_append_child(cJSON *parent, cJSON *item) {
    item->parent = parent;
    item->next = NULL;
    if (!parent->child) {
        item->prev = NULL;
        parent->child = item;
    } else {
        cJSON *last = parent->child;
        while (last->next) last = last->next;
        last->next = item;
        item->prev = last;
    }
}

static void cJSON_AddItemToArray(cJSON *array, cJSON *item) {
    if (array && item) cj_append_child(array, item);
}

static void cJSON_AddItemToObject(cJSON *object, const char *key, cJSON *item) {
    if (!object || !item || !key) return;
    free(item->string);
    item->string = strdup(key);
    cj_append_child(object, item);
}

static void cJSON_AddItemReferenceToObject(cJSON *object, const char *key, cJSON *item) {
    if (!object || !item || !key) return;
    cJSON *ref = cj_new(item->type);
    ref->is_ref = 1;
    ref->child = item->child;
    ref->valuestring = item->valuestring;  /* shallow, but ref→no delete */
    ref->valueint = item->valueint;
    ref->valuedouble = item->valuedouble;
    ref->string = strdup(key);
    cj_append_child(object, ref);
}

static void cJSON_AddStringToObject(cJSON *object, const char *key, const char *val) {
    cJSON_AddItemToObject(object, key, cJSON_CreateString(val));
}

static void cJSON_AddNumberToObject(cJSON *object, const char *key, double val) {
    cJSON_AddItemToObject(object, key, cJSON_CreateNumber(val));
}

static void cJSON_AddBoolToObject(cJSON *object, const char *key, int val) {
    cJSON_AddItemToObject(object, key, cJSON_CreateBool(val));
}

static cJSON *cJSON_GetObjectItem(const cJSON *object, const char *key) {
    if (!object || !key) return NULL;
    for (cJSON *c = object->child; c; c = c->next)
        if (c->string && strcmp(c->string, key) == 0) return c;
    return NULL;
}

static cJSON *cJSON_GetArrayItem(const cJSON *array, int index) {
    if (!array) return NULL;
    cJSON *c = array->child;
    while (c && index > 0) { c = c->next; index--; }
    return c;
}

static int cJSON_GetArraySize(const cJSON *array) {
    if (!array) return 0;
    int n = 0;
    for (cJSON *c = array->child; c; c = c->next) n++;
    return n;
}

static const char *cJSON_GetStringValue(const cJSON *item) {
    return (item && item->type == cJSON_String) ? item->valuestring : NULL;
}

static int cJSON_IsNumber(const cJSON *item) { return item && item->type == cJSON_Number; }
static int cJSON_IsArray(const cJSON *item)  { return item && item->type == cJSON_Array; }
static int cJSON_IsString(const cJSON *item) { return item && item->type == cJSON_String; }
static int cJSON_IsBool(const cJSON *item)   { return item && (item->type == cJSON_True || item->type == cJSON_False); }
static int cJSON_IsTrue(const cJSON *item)   { return item && item->type == cJSON_True; }

static char *cJSON_SetValuestring(cJSON *item, const char *v) {
    if (!item || item->type != cJSON_String) return NULL;
    free(item->valuestring);
    item->valuestring = v ? strdup(v) : strdup("");
    return item->valuestring;
}

/* ==================== PARSER ==================== */

typedef struct { const char *p; const char *end; } cj_cursor;

static cJSON *cj_parse_value(cj_cursor *c);

static void cj_skip_ws(cj_cursor *c) {
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) c->p++;
}

static char *cj_parse_string_raw(cj_cursor *c) {
    if (c->p >= c->end || *c->p != '"') return NULL;
    c->p++;
    size_t cap = 32, len = 0;
    char *out = malloc(cap);
    if (!out) abort();
    while (c->p < c->end && *c->p != '"') {
        char ch = *c->p++;
        if (ch == '\\' && c->p < c->end) {
            char esc = *c->p++;
            switch (esc) {
                case '"': ch = '"'; break;
                case '\\': ch = '\\'; break;
                case '/': ch = '/'; break;
                case 'b': ch = '\b'; break;
                case 'f': ch = '\f'; break;
                case 'n': ch = '\n'; break;
                case 'r': ch = '\r'; break;
                case 't': ch = '\t'; break;
                case 'u': {
                    /* Parse 4 hex digits into a codepoint */
                    if (c->end - c->p < 4) { free(out); return NULL; }
                    unsigned cp = 0;
                    for (int i = 0; i < 4; i++) {
                        char h = c->p[i];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= h - '0';
                        else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                        else { free(out); return NULL; }
                    }
                    c->p += 4;
                    /* Encode as UTF-8 */
                    if (len + 4 >= cap) { cap *= 2; out = realloc(out, cap); if (!out) abort(); }
                    if (cp < 0x80) out[len++] = cp;
                    else if (cp < 0x800) { out[len++] = 0xC0 | (cp >> 6); out[len++] = 0x80 | (cp & 0x3F); }
                    else { out[len++] = 0xE0 | (cp >> 12); out[len++] = 0x80 | ((cp >> 6) & 0x3F); out[len++] = 0x80 | (cp & 0x3F); }
                    continue;
                }
                default: free(out); return NULL;
            }
        }
        if (len + 1 >= cap) { cap *= 2; out = realloc(out, cap); if (!out) abort(); }
        out[len++] = ch;
    }
    if (c->p >= c->end) { free(out); return NULL; }
    c->p++;  /* closing " */
    out[len] = 0;
    return out;
}

static cJSON *cj_parse_string(cj_cursor *c) {
    char *s = cj_parse_string_raw(c);
    if (!s) return NULL;
    cJSON *j = cj_new(cJSON_String);
    j->valuestring = s;
    return j;
}

static cJSON *cj_parse_number(cj_cursor *c) {
    const char *start = c->p;
    if (c->p < c->end && (*c->p == '-' || *c->p == '+')) c->p++;
    while (c->p < c->end && (isdigit((unsigned char)*c->p) || *c->p == '.' || *c->p == 'e' || *c->p == 'E' || *c->p == '+' || *c->p == '-')) c->p++;
    if (c->p == start) return NULL;
    char buf[64]; size_t n = c->p - start;
    if (n >= sizeof buf) return NULL;
    memcpy(buf, start, n); buf[n] = 0;
    double d = strtod(buf, NULL);
    return cJSON_CreateNumber(d);
}

static cJSON *cj_parse_array(cj_cursor *c) {
    if (c->p >= c->end || *c->p != '[') return NULL;
    c->p++;
    cJSON *arr = cJSON_CreateArray();
    cj_skip_ws(c);
    if (c->p < c->end && *c->p == ']') { c->p++; return arr; }
    for (;;) {
        cj_skip_ws(c);
        cJSON *item = cj_parse_value(c);
        if (!item) { cJSON_Delete(arr); return NULL; }
        cj_append_child(arr, item);
        cj_skip_ws(c);
        if (c->p >= c->end) { cJSON_Delete(arr); return NULL; }
        if (*c->p == ',') { c->p++; continue; }
        if (*c->p == ']') { c->p++; return arr; }
        cJSON_Delete(arr); return NULL;
    }
}

static cJSON *cj_parse_object(cj_cursor *c) {
    if (c->p >= c->end || *c->p != '{') return NULL;
    c->p++;
    cJSON *obj = cJSON_CreateObject();
    cj_skip_ws(c);
    if (c->p < c->end && *c->p == '}') { c->p++; return obj; }
    for (;;) {
        cj_skip_ws(c);
        char *key = cj_parse_string_raw(c);
        if (!key) { cJSON_Delete(obj); return NULL; }
        cj_skip_ws(c);
        if (c->p >= c->end || *c->p != ':') { free(key); cJSON_Delete(obj); return NULL; }
        c->p++;
        cj_skip_ws(c);
        cJSON *v = cj_parse_value(c);
        if (!v) { free(key); cJSON_Delete(obj); return NULL; }
        v->string = key;
        cj_append_child(obj, v);
        cj_skip_ws(c);
        if (c->p >= c->end) { cJSON_Delete(obj); return NULL; }
        if (*c->p == ',') { c->p++; continue; }
        if (*c->p == '}') { c->p++; return obj; }
        cJSON_Delete(obj); return NULL;
    }
}

static cJSON *cj_parse_value(cj_cursor *c) {
    cj_skip_ws(c);
    if (c->p >= c->end) return NULL;
    char ch = *c->p;
    if (ch == '"') return cj_parse_string(c);
    if (ch == '{') return cj_parse_object(c);
    if (ch == '[') return cj_parse_array(c);
    if (ch == '-' || (ch >= '0' && ch <= '9')) return cj_parse_number(c);
    if (c->end - c->p >= 4 && strncmp(c->p, "true", 4) == 0) { c->p += 4; return cJSON_CreateBool(1); }
    if (c->end - c->p >= 5 && strncmp(c->p, "false", 5) == 0) { c->p += 5; return cJSON_CreateBool(0); }
    if (c->end - c->p >= 4 && strncmp(c->p, "null", 4) == 0) { c->p += 4; return cJSON_CreateNull(); }
    return NULL;
}

static cJSON *cJSON_Parse(const char *text) {
    if (!text) return NULL;
    cj_cursor c = { text, text + strlen(text) };
    return cj_parse_value(&c);
}

/* ==================== SERIALIZER ==================== */

typedef struct { char *data; size_t len, cap; } cj_buf;

static void cj_bput(cj_buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 128;
        while (nc < b->len + n + 1) nc *= 2;
        b->data = realloc(b->data, nc);
        if (!b->data) abort();
        b->cap = nc;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

static void cj_bputc(cj_buf *b, char c) { cj_bput(b, &c, 1); }

static void cj_print_string(cj_buf *b, const char *s) {
    cj_bputc(b, '"');
    for (; *s; s++) {
        unsigned char c = *s;
        switch (c) {
            case '"':  cj_bput(b, "\\\"", 2); break;
            case '\\': cj_bput(b, "\\\\", 2); break;
            case '\b': cj_bput(b, "\\b",  2); break;
            case '\f': cj_bput(b, "\\f",  2); break;
            case '\n': cj_bput(b, "\\n",  2); break;
            case '\r': cj_bput(b, "\\r",  2); break;
            case '\t': cj_bput(b, "\\t",  2); break;
            default:
                if (c < 0x20) {
                    char tmp[8]; int n = snprintf(tmp, sizeof tmp, "\\u%04x", c);
                    cj_bput(b, tmp, n);
                } else {
                    cj_bputc(b, c);
                }
        }
    }
    cj_bputc(b, '"');
}

static void cj_print_value(cj_buf *b, const cJSON *item);

static void cj_print_array(cj_buf *b, const cJSON *item) {
    cj_bputc(b, '[');
    int first = 1;
    for (cJSON *c = item->child; c; c = c->next) {
        if (!first) cj_bputc(b, ',');
        first = 0;
        cj_print_value(b, c);
    }
    cj_bputc(b, ']');
}

static void cj_print_object(cj_buf *b, const cJSON *item) {
    cj_bputc(b, '{');
    int first = 1;
    for (cJSON *c = item->child; c; c = c->next) {
        if (!first) cj_bputc(b, ',');
        first = 0;
        cj_print_string(b, c->string ? c->string : "");
        cj_bputc(b, ':');
        cj_print_value(b, c);
    }
    cj_bputc(b, '}');
}

static void cj_print_value(cj_buf *b, const cJSON *item) {
    if (!item) { cj_bput(b, "null", 4); return; }
    switch (item->type) {
        case cJSON_False:  cj_bput(b, "false", 5); break;
        case cJSON_True:   cj_bput(b, "true",  4); break;
        case cJSON_NULL:   cj_bput(b, "null",  4); break;
        case cJSON_Number: {
            char tmp[32];
            int n = (item->valuedouble == (double)(long long)item->valuedouble)
                    ? snprintf(tmp, sizeof tmp, "%lld", (long long)item->valuedouble)
                    : snprintf(tmp, sizeof tmp, "%g", item->valuedouble);
            cj_bput(b, tmp, n);
            break;
        }
        case cJSON_String: cj_print_string(b, item->valuestring ? item->valuestring : ""); break;
        case cJSON_Array:  cj_print_array(b, item); break;
        case cJSON_Object: cj_print_object(b, item); break;
        default: cj_bput(b, "null", 4);
    }
}

static char *cJSON_PrintUnformatted(const cJSON *item) {
    cj_buf b = {0};
    cj_print_value(&b, item);
    if (!b.data) b.data = strdup("null");
    return b.data;
}

#endif
