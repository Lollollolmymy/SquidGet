#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/* ── parser state ── */
typedef struct { const char *p; } P;

static void ws(P *p) {
    while (*p->p && isspace((unsigned char)*p->p)) p->p++;
}

/* ── UTF-8 encode a codepoint into buf, returns bytes written ── */
static int utf8_encode(unsigned int cp, char *buf) {
    if (cp < 0x80) {
        buf[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else if (cp <= 0x10FFFF) {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6)  & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    /* invalid codepoint — emit replacement character U+FFFD */
    buf[0] = (char)0xEF; buf[1] = (char)0xBF; buf[2] = (char)0xBD;
    return 3;
}

/* Parse 4 hex digits at *pp (advances *pp by 4), returns codepoint or -1 */
static int parse_hex4(const char **pp) {
    unsigned val = 0;
    for (int i = 0; i < 4; i++) {
        if (!**pp) return -1;
        unsigned char c = (unsigned char)**pp;
        val <<= 4;
        if      (c >= '0' && c <= '9') val |= c - '0';
        else if (c >= 'a' && c <= 'f') val |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') val |= c - 'A' + 10;
        else return -1;
        (*pp)++;
    }
    return (int)val;
}

/* returns newly malloc'd, unescaped string; advances p past closing '"' */
static char *parse_str(P *p) {
    if (*p->p != '"') return NULL;
    p->p++;
    size_t cap = 64, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    while (*p->p && *p->p != '"') {
        /* grow buffer: worst case is 4 bytes per character + null */
        if (len + 8 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }

        if (*p->p == '\\') {
            p->p++;
            switch (*p->p) {
                case '"':  buf[len++] = '"';  break;
                case '\\': buf[len++] = '\\'; break;
                case '/':  buf[len++] = '/';  break;
                case 'b':  buf[len++] = '\b'; break;
                case 'f':  buf[len++] = '\f'; break;
                case 'n':  buf[len++] = '\n'; break;
                case 'r':  buf[len++] = '\r'; break;
                case 't':  buf[len++] = '\t'; break;
                case 'u': {
                    p->p++;
                    int hi = parse_hex4(&p->p);
                    if (hi < 0) { buf[len++] = '?'; break; }

                    unsigned int codepoint = (unsigned int)hi;

                    /* FIX: handle UTF-16 surrogate pairs (\uD800–\uDBFF followed
                       by \uDC00–\uDFFF).  Previously each half was decoded
                       independently, producing two invalid UTF-8 sequences. */
                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                        /* high surrogate — expect \uXXXX low surrogate */
                        if (p->p[0] == '\\' && p->p[1] == 'u') {
                            p->p += 2;
                            int lo = parse_hex4(&p->p);
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                codepoint = 0x10000u
                                    + ((codepoint - 0xD800u) << 10)
                                    + ((unsigned int)lo - 0xDC00u);
                            } else {
                                /* malformed pair — emit replacement and rewind lo */
                                codepoint = 0xFFFD;
                                /* lo is already consumed, treat as isolated */
                            }
                        } else {
                            /* isolated high surrogate */
                            codepoint = 0xFFFD;
                        }
                    } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
                        /* isolated low surrogate */
                        codepoint = 0xFFFD;
                    }

                    /* ensure buffer has room for up to 4 UTF-8 bytes */
                    if (len + 4 >= cap) {
                        cap *= 2;
                        char *nb = realloc(buf, cap);
                        if (!nb) { free(buf); return NULL; }
                        buf = nb;
                    }
                    len += (size_t)utf8_encode(codepoint, buf + len);
                    /* p->p is already past the escape — skip the
                       unconditional p->p++ at the bottom of the loop. */
                    continue;
                }
                default: buf[len++] = *p->p; break;
            }
        } else {
            /* pass UTF-8 bytes through unchanged */
            buf[len++] = *p->p;
        }
        if (*p->p) p->p++;
    }
    if (*p->p == '"') p->p++;
    buf[len] = '\0';
    return buf;
}

static JNode *parse_value(P *p);  /* forward decl */

static JNode *parse_object(P *p) {
    p->p++; /* skip '{' */
    JNode *n = calloc(1, sizeof(JNode));
    if (!n) return NULL;
    n->type = J_OBJ;
    size_t cap = 8;
    n->arr.items = malloc(cap * sizeof(JNode *));
    if (!n->arr.items) { free(n); return NULL; }
    while (1) {
        ws(p);
        if (!*p->p || *p->p == '}') { if (*p->p) p->p++; break; }
        if (*p->p == ',') { p->p++; continue; }
        char *key = parse_str(p);
        if (!key) break;
        ws(p);
        if (*p->p == ':') p->p++;
        ws(p);
        JNode *child = parse_value(p);
        if (!child) { free(key); break; }  /* unrecognised token — stop, don't loop */
        child->key = key;
        if ((size_t)n->arr.len >= cap) {
            cap *= 2;
            JNode **ni = realloc(n->arr.items, cap * sizeof(JNode *));
            if (!ni) { json_free(child); free(key); break; }
            n->arr.items = ni;
        }
        n->arr.items[n->arr.len++] = child;
    }
    return n;
}

static JNode *parse_array(P *p) {
    p->p++; /* skip '[' */
    JNode *n = calloc(1, sizeof(JNode));
    if (!n) return NULL;
    n->type = J_ARR;
    size_t cap = 8;
    n->arr.items = malloc(cap * sizeof(JNode *));
    if (!n->arr.items) { free(n); return NULL; }
    while (1) {
        ws(p);
        if (!*p->p || *p->p == ']') { if (*p->p) p->p++; break; }
        if (*p->p == ',') { p->p++; continue; }
        JNode *child = parse_value(p);
        if (!child) break;
        if ((size_t)n->arr.len >= cap) {
            cap *= 2;
            JNode **ni = realloc(n->arr.items, cap * sizeof(JNode *));
            if (!ni) { json_free(child); break; }
            n->arr.items = ni;
        }
        n->arr.items[n->arr.len++] = child;
    }
    return n;
}

static JNode *parse_value(P *p) {
    ws(p);
    if (!*p->p) return NULL;
    if (*p->p == '{') return parse_object(p);
    if (*p->p == '[') return parse_array(p);
    if (*p->p == '"') {
        JNode *n = calloc(1, sizeof(JNode));
        if (!n) return NULL;
        n->type = J_STR;
        n->s = parse_str(p);
        return n;
    }
    if (strncmp(p->p, "true",  4) == 0) { p->p += 4; JNode *n = calloc(1, sizeof(JNode)); if (!n) return NULL; n->type = J_BOOL; n->b = 1; return n; }
    if (strncmp(p->p, "false", 5) == 0) { p->p += 5; JNode *n = calloc(1, sizeof(JNode)); if (!n) return NULL; n->type = J_BOOL; n->b = 0; return n; }
    if (strncmp(p->p, "null",  4) == 0) { p->p += 4; JNode *n = calloc(1, sizeof(JNode)); if (!n) return NULL; n->type = J_NULL; return n; }
    if (*p->p == '-' || isdigit((unsigned char)*p->p)) {
        JNode *n = calloc(1, sizeof(JNode));
        if (!n) return NULL;
        n->type = J_NUM;
        char *end;
        n->n = strtod(p->p, &end);
        p->p = end;
        return n;
    }
    return NULL; /* unknown token — skip */
}

/* ── public API ── */

JNode *json_parse(const char *src) {
    if (!src) return NULL;
    P p = {src};
    return parse_value(&p);
}

void json_free(JNode *n) {
    if (!n) return;
    free(n->key);
    switch (n->type) {
        case J_STR:
            free(n->s);
            break;
        case J_ARR:
        case J_OBJ:
            for (int i = 0; i < n->arr.len; i++) json_free(n->arr.items[i]);
            free(n->arr.items);
            break;
        default: break;
    }
    free(n);
}

JNode *jobj_get(JNode *obj, const char *key) {
    if (!obj || obj->type != J_OBJ || !key) return NULL;
    for (int i = 0; i < obj->arr.len; i++) {
        JNode *m = obj->arr.items[i];
        if (m->key && strcmp(m->key, key) == 0) return m;
    }
    return NULL;
}

JNode *jarr_get(JNode *arr, int idx) {
    if (!arr || arr->type != J_ARR) return NULL;
    if (idx < 0 || idx >= arr->arr.len) return NULL;
    return arr->arr.items[idx];
}

const char *jstr(JNode *n) {
    if (!n || n->type != J_STR) return "";
    return n->s ? n->s : "";
}

double jnum(JNode *n) {
    if (!n) return 0.0;
    if (n->type == J_NUM)  return n->n;
    if (n->type == J_STR && n->s) { char *e; double v = strtod(n->s, &e); if (e != n->s) return v; }
    return 0.0;
}

int jbool(JNode *n) {
    if (!n || n->type != J_BOOL) return 0;
    return n->b;
}
