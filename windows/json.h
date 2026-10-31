#ifndef GTTS_WIN_JSON_H
#define GTTS_WIN_JSON_H
#include <stddef.h>

typedef enum { JSON_NULL, JSON_FALSE, JSON_TRUE, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } JsonType;
typedef struct Json {
    JsonType type;
    double number;
    char *string;           // UTF-8, may contain no NUL
    size_t length;          // string length, or number of items
    char **keys;            // object keys
    struct Json **items;    // array items or object values
} Json;

Json *json_parse(const char *text, size_t length, char *error, size_t error_size);
void json_free(Json *value);
Json *json_get(const Json *object, const char *key);
const char *json_string(const Json *object, const char *key);   // NULL unless a string without NUL

typedef struct { char *data; size_t size, capacity; } JsonOut;
void json_out_raw(JsonOut *out, const char *text);
void json_out_string(JsonOut *out, const char *text);
#endif
