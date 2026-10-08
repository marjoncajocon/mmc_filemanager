/* fjson.h -- a small JSON reader: parse once into a node array, then walk.
**
** All nodes and strings live in one allocation owned by FmJson, so a whole
** API response is freed with one call. Strings are unescaped to UTF-8
** (\uXXXX and surrogate pairs included). Depth and size are bounded.
**
**   FmJson j;
**   if (json_parse(&j, text, len) == FM_OK) {
**     const FmJsonNode *items = json_get(json_root(&j), "items");
**     for (const FmJsonNode *it = json_first(items); it; it = json_next(it))
**       puts(json_str(json_path(it, "snippet.title"), ""));
**     json_free(&j);
**   }
*/
#ifndef FJSON_H
#define FJSON_H

#include "fcore.h"

enum { JSON_NULL, JSON_FALSE, JSON_TRUE, JSON_NUM, JSON_STR, JSON_ARR, JSON_OBJ };

typedef struct FmJsonNode {
  u8 type;
  int next;            /* offset to the next sibling from this node, 0 = last */
  int child;           /* offset to the first child (array/object), 0 = none */
  int count;           /* children (array/object) */
  const char *key;     /* member name inside an object, else NULL */
  const char *s;       /* string value (JSON_STR) */
  double n;            /* number value (JSON_NUM) */
} FmJsonNode;

typedef struct FmJson {
  FmJsonNode *nodes;
  int count;
  char *strings;       /* unescaped string pool */
  char error[96];
} FmJson;

FmErr json_parse(FmJson *j, const char *text, size_t len);
void  json_free(FmJson *j);

const FmJsonNode *json_root(const FmJson *j);
const FmJsonNode *json_get(const FmJsonNode *obj, const char *key);    /* NULL if missing */
const FmJsonNode *json_path(const FmJsonNode *n, const char *dotted);  /* "a.b.0.c" */
const FmJsonNode *json_first(const FmJsonNode *arr_or_obj);
const FmJsonNode *json_next(const FmJsonNode *n);
const FmJsonNode *json_at(const FmJsonNode *arr, int i);

const char *json_str(const FmJsonNode *n, const char *def);     /* strings only */
double      json_num(const FmJsonNode *n, double def);          /* numbers, or numeric strings */
bool        json_bool(const FmJsonNode *n, bool def);

#endif
