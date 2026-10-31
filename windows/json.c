#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *p, *end; int depth; const char *error; } Parser;

static void skip_space(Parser *s) { while (s->p < s->end && (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r')) ++s->p; }

static Json *make(JsonType type) { Json *v = calloc(1, sizeof *v); if (v) v->type = type; return v; }

static int hex4(const char *p, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; ++i) {
        char c = p[i]; v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *out = v; return 0;
}

static char *parse_string(Parser *s, size_t *length) {
    ++s->p;   // opening quote
    size_t cap = 16, n = 0;
    char *out = malloc(cap);
    while (out && s->p < s->end && *s->p != '"') {
        unsigned char c = (unsigned char)*s->p++;
        unsigned code = c;
        if (c < 0x20) { s->error = "Control character in string"; free(out); return NULL; }
        if (c == '\\') {
            if (s->p >= s->end) break;
            char e = *s->p++;
            switch (e) {
            case '"': code = '"'; break; case '\\': code = '\\'; break; case '/': code = '/'; break;
            case 'b': code = 8; break; case 'f': code = 12; break; case 'n': code = 10; break;
            case 'r': code = 13; break; case 't': code = 9; break;
            case 'u':
                if (s->end - s->p < 4 || hex4(s->p, &code)) { s->error = "Invalid \\u escape"; free(out); return NULL; }
                s->p += 4;
                if (code >= 0xd800 && code < 0xdc00 && s->end - s->p >= 6 && s->p[0] == '\\' && s->p[1] == 'u') {
                    unsigned low;
                    if (!hex4(s->p + 2, &low) && low >= 0xdc00 && low < 0xe000) { code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00); s->p += 6; }
                }
                if (code >= 0xd800 && code < 0xe000) code = 0xfffd;
                break;
            default: s->error = "Invalid escape"; free(out); return NULL;
            }
        } else {
            if (n + 2 > cap) { cap *= 2; char *p = realloc(out, cap); if (!p) { free(out); return NULL; } out = p; }
            out[n++] = (char)c;
            continue;
        }
        if (n + 5 > cap) { cap *= 2; char *p = realloc(out, cap); if (!p) { free(out); return NULL; } out = p; }
        if (code < 0x80) out[n++] = (char)code;
        else if (code < 0x800) { out[n++] = (char)(0xc0 | code >> 6); out[n++] = (char)(0x80 | (code & 63)); }
        else if (code < 0x10000) { out[n++] = (char)(0xe0 | code >> 12); out[n++] = (char)(0x80 | ((code >> 6) & 63)); out[n++] = (char)(0x80 | (code & 63)); }
        else { out[n++] = (char)(0xf0 | code >> 18); out[n++] = (char)(0x80 | ((code >> 12) & 63)); out[n++] = (char)(0x80 | ((code >> 6) & 63)); out[n++] = (char)(0x80 | (code & 63)); }
    }
    if (!out) return NULL;
    if (s->p >= s->end) { s->error = "Unterminated string"; free(out); return NULL; }
    ++s->p;
    out[n] = 0;
    *length = n;
    return out;
}

static Json *parse_value(Parser *s) {
    skip_space(s);
    if (s->p >= s->end) { s->error = "Unexpected end of JSON"; return NULL; }
    if (++s->depth > 64) { s->error = "JSON is nested too deeply"; return NULL; }
    Json *v = NULL;
    char c = *s->p;
    if (c == '"') {
        v = make(JSON_STRING);
        if (v && !(v->string = parse_string(s, &v->length))) { free(v); v = NULL; }
    } else if (c == '{' || c == '[') {
        int object = c == '{';
        v = make(object ? JSON_OBJECT : JSON_ARRAY);
        ++s->p;
        skip_space(s);
        if (s->p < s->end && *s->p == (object ? '}' : ']')) { ++s->p; --s->depth; return v; }
        while (v) {
            char *key = NULL; size_t key_length;
            if (object) {
                skip_space(s);
                if (s->p >= s->end || *s->p != '"' || !(key = parse_string(s, &key_length))) { if (!s->error) s->error = "Expected a key"; json_free(v); return NULL; }
                skip_space(s);
                if (s->p >= s->end || *s->p != ':') { s->error = "Expected ':'"; free(key); json_free(v); return NULL; }
                ++s->p;
            }
            Json *item = parse_value(s);
            if (!item) { free(key); json_free(v); return NULL; }
            Json **items = realloc(v->items, (v->length + 1) * sizeof *items);
            if (items) v->items = items;
            char **keys = object && items ? realloc(v->keys, (v->length + 1) * sizeof *keys) : NULL;
            if (keys) v->keys = keys;
            if (!items || (object && !keys)) { free(key); json_free(item); json_free(v); return NULL; }
            if (object) keys[v->length] = key;
            items[v->length++] = item;
            skip_space(s);
            if (s->p < s->end && *s->p == ',') { ++s->p; continue; }
            if (s->p < s->end && *s->p == (object ? '}' : ']')) { ++s->p; break; }
            s->error = "Expected ',' or a closing bracket"; json_free(v); return NULL;
        }
    } else if (!strncmp(s->p, "true", 4) && s->end - s->p >= 4) { v = make(JSON_TRUE); s->p += 4; }
    else if (!strncmp(s->p, "false", 5) && s->end - s->p >= 5) { v = make(JSON_FALSE); s->p += 5; }
    else if (!strncmp(s->p, "null", 4) && s->end - s->p >= 4) { v = make(JSON_NULL); s->p += 4; }
    else if (c == '-' || (c >= '0' && c <= '9')) {
        char buffer[64]; size_t n = 0;
        while (s->p < s->end && n < 63 && strchr("+-0123456789.eE", *s->p)) buffer[n++] = *s->p++;
        buffer[n] = 0;
        char *end;
        double d = strtod(buffer, &end);
        if (*end) { s->error = "Invalid number"; return NULL; }
        v = make(JSON_NUMBER);
        if (v) v->number = d;
    } else s->error = "Unexpected character in JSON";
    --s->depth;
    return v;
}

Json *json_parse(const char *text, size_t length, char *error, size_t error_size) {
    Parser s = {text, text + length, 0, NULL};
    Json *v = parse_value(&s);
    if (v) { skip_space(&s); if (s.p != s.end) { s.error = "Trailing characters after JSON"; json_free(v); v = NULL; } }
    if (!v && error) snprintf(error, error_size, "%s", s.error ? s.error : "Out of memory");
    return v;
}

void json_free(Json *v) {
    if (!v) return;
    for (size_t i = 0; (v->type == JSON_ARRAY || v->type == JSON_OBJECT) && i < v->length; ++i) {
        json_free(v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items); free(v->keys); free(v->string); free(v);
}

Json *json_get(const Json *o, const char *key) {
    if (!o || o->type != JSON_OBJECT) return NULL;
    for (size_t i = o->length; i > 0; --i) if (!strcmp(o->keys[i - 1], key)) return o->items[i - 1];
    return NULL;
}

const char *json_string(const Json *o, const char *key) {
    Json *v = json_get(o, key);
    return v && v->type == JSON_STRING && strlen(v->string) == v->length ? v->string : NULL;
}

void json_out_raw(JsonOut *out, const char *text) {
    size_t n = strlen(text);
    if (out->size + n + 1 > out->capacity) {
        size_t cap = out->capacity ? out->capacity * 2 : 256;
        while (cap < out->size + n + 1) cap *= 2;
        char *p = realloc(out->data, cap);
        if (!p) abort();
        out->data = p; out->capacity = cap;
    }
    memcpy(out->data + out->size, text, n + 1);
    out->size += n;
}

void json_out_string(JsonOut *out, const char *text) {
    json_out_raw(out, "\"");
    char piece[8];
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p == '"') json_out_raw(out, "\\\"");
        else if (*p == '\\') json_out_raw(out, "\\\\");
        else if (*p == '\n') json_out_raw(out, "\\n");
        else if (*p == '\r') json_out_raw(out, "\\r");
        else if (*p == '\t') json_out_raw(out, "\\t");
        else if (*p < 0x20) { snprintf(piece, sizeof piece, "\\u%04x", *p); json_out_raw(out, piece); }
        else { piece[0] = (char)*p; piece[1] = 0; json_out_raw(out, piece); }
    }
    json_out_raw(out, "\"");
}
