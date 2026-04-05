#pragma once
#include <stddef.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JNode {
    JType type;
    char *key;   /* non-NULL for object members */
    union {
        int    b;    /* J_BOOL  */
        double n;    /* J_NUM   */
        char  *s;    /* J_STR   */
        struct {
            struct JNode **items;
            int            len;
        } arr;       /* J_ARR, J_OBJ */
    };
} JNode;

JNode      *json_parse(const char *src);
void        json_free(JNode *n);

JNode      *jobj_get(JNode *obj, const char *key);
const char *jstr(JNode *n);           /* "" on miss */
double      jnum(JNode *n);           /*  0 on miss */
