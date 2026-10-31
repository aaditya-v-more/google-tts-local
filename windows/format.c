// printf and scanf for Linux callers. Arguments arrive in a System V va_list,
// which is read here directly; each conversion is then formatted by the C
// runtime one at a time.
#include "engine.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static uint64_t arg_int(SysvVaList *a) {
    uint64_t v;
    if (a->gp_offset < 48) { memcpy(&v, a->reg_save + a->gp_offset, 8); a->gp_offset += 8; }
    else { memcpy(&v, a->overflow, 8); a->overflow += 8; }
    return v;
}
static double arg_double(SysvVaList *a) {
    double v;
    if (a->fp_offset < 176) { memcpy(&v, a->reg_save + a->fp_offset, 8); a->fp_offset += 16; }
    else { memcpy(&v, a->overflow, 8); a->overflow += 8; }
    return v;
}
static long double arg_long_double(SysvVaList *a) {
    a->overflow = (uint8_t *)(((uintptr_t)a->overflow + 15) & ~(uintptr_t)15);
    long double v; memcpy(&v, a->overflow, 10); a->overflow += 16;
    return v;
}

static int append(TextBuffer *b, const void *data, size_t n) {
    if (b->size + n + 1 > b->capacity) {
        size_t cap = b->capacity ? b->capacity * 2 : 256;
        while (cap < b->size + n + 1) cap *= 2;
        char *p = realloc(b->data, cap);
        if (!p) return -1;
        b->data = p; b->capacity = cap;
    }
    memcpy(b->data + b->size, data, n);
    b->size += n;
    b->data[b->size] = 0;
    return 0;
}

static int append_formatted(TextBuffer *b, const char *spec, ...) {
    char small[512];
    va_list ap; va_start(ap, spec);
    int n = __mingw_vsnprintf(small, sizeof small, spec, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n < sizeof small) return append(b, small, (size_t)n);
    char *big = malloc((size_t)n + 1);
    if (!big) return -1;
    va_start(ap, spec);
    __mingw_vsnprintf(big, (size_t)n + 1, spec, ap);
    va_end(ap);
    int r = append(b, big, (size_t)n);
    free(big);
    return r;
}

static size_t wide_length(const uint32_t *s) { size_t n = 0; while (s[n]) ++n; return n; }

// Encodes 32-bit characters as UTF-8.
static char *wide_to_utf8_32(const uint32_t *s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) ++n;
    char *out = malloc(n * 4 + 1), *p = out;
    if (!out) return NULL;
    for (size_t i = 0; i < n; ++i) {
        uint32_t c = s[i];
        if (c < 0x80) *p++ = (char)c;
        else if (c < 0x800) { *p++ = (char)(0xc0 | c >> 6); *p++ = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { *p++ = (char)(0xe0 | c >> 12); *p++ = (char)(0x80 | ((c >> 6) & 63)); *p++ = (char)(0x80 | (c & 63)); }
        else { *p++ = (char)(0xf0 | c >> 18); *p++ = (char)(0x80 | ((c >> 12) & 63)); *p++ = (char)(0x80 | ((c >> 6) & 63)); *p++ = (char)(0x80 | (c & 63)); }
    }
    *p = 0;
    return out;
}

int sysv_format(TextBuffer *out, const char *fmt, SysvVaList *args) {
    if (append(out, "", 0)) return -1;
    for (const char *p = fmt; *p;) {
        if (*p != '%') {
            const char *q = strchr(p, '%');
            size_t n = q ? (size_t)(q - p) : strlen(p);
            if (append(out, p, n)) return -1;
            p += n; continue;
        }
        const char *start = p++;
        if (*p == '%') { if (append(out, "%", 1)) return -1; ++p; continue; }
        char spec[64]; size_t k = 0;
        spec[k++] = '%';
        while (*p && strchr("-+ #0'", *p)) { if (*p != '\'' && k < 40) spec[k++] = *p; ++p; }
        int star_width = 0, star_precision = 0, width = 0, precision = 0;
        if (*p == '*') { star_width = 1; width = (int)arg_int(args); ++p; spec[k++] = '*'; }
        else while (*p >= '0' && *p <= '9') { if (k < 50) spec[k++] = *p; ++p; }
        if (*p == '.') {
            if (k < 52) spec[k++] = '.';
            ++p;
            if (*p == '*') { star_precision = 1; precision = (int)arg_int(args); ++p; spec[k++] = '*'; }
            else while (*p >= '0' && *p <= '9') { if (k < 58) spec[k++] = *p; ++p; }
        }
        int length = 0;   // 1 hh, 2 h, 3 l, 4 ll, 5 L, 6 size_t / intmax / ptrdiff
        if (*p == 'h') { length = p[1] == 'h' ? 1 : 2; p += p[1] == 'h' ? 2 : 1; }
        else if (*p == 'l') { length = p[1] == 'l' ? 4 : 3; p += p[1] == 'l' ? 2 : 1; }
        else if (*p == 'q') { length = 4; ++p; }
        else if (*p == 'L') { length = 5; ++p; }
        else if (*p == 'j' || *p == 'z' || *p == 't' || *p == 'Z') { length = 6; ++p; }
        char conv = *p;
        if (!conv) { append(out, start, strlen(start)); break; }
        ++p;
        int r = 0;
        switch (conv) {
        case 'd': case 'i': {
            int64_t v = (int64_t)arg_int(args);
            if (length == 1) v = (signed char)v; else if (length == 2) v = (short)v; else if (length == 0) v = (int)v;
            strcpy(spec + k, "lld");
            r = star_width && star_precision ? append_formatted(out, spec, width, precision, (long long)v)
              : star_width ? append_formatted(out, spec, width, (long long)v)
              : star_precision ? append_formatted(out, spec, precision, (long long)v) : append_formatted(out, spec, (long long)v);
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            uint64_t v = arg_int(args);
            if (length == 1) v = (unsigned char)v; else if (length == 2) v = (unsigned short)v; else if (length == 0) v = (unsigned)v;
            spec[k] = 'l'; spec[k + 1] = 'l'; spec[k + 2] = conv; spec[k + 3] = 0;
            r = star_width && star_precision ? append_formatted(out, spec, width, precision, (unsigned long long)v)
              : star_width ? append_formatted(out, spec, width, (unsigned long long)v)
              : star_precision ? append_formatted(out, spec, precision, (unsigned long long)v) : append_formatted(out, spec, (unsigned long long)v);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            if (length == 5) {
                long double v = arg_long_double(args);
                spec[k] = 'L'; spec[k + 1] = conv; spec[k + 2] = 0;
                r = star_width && star_precision ? append_formatted(out, spec, width, precision, v)
                  : star_width ? append_formatted(out, spec, width, v)
                  : star_precision ? append_formatted(out, spec, precision, v) : append_formatted(out, spec, v);
            } else {
                double v = arg_double(args);
                spec[k] = conv; spec[k + 1] = 0;
                r = star_width && star_precision ? append_formatted(out, spec, width, precision, v)
                  : star_width ? append_formatted(out, spec, width, v)
                  : star_precision ? append_formatted(out, spec, precision, v) : append_formatted(out, spec, v);
            }
            break;
        }
        case 'c': {
            uint32_t c = (uint32_t)arg_int(args);
            char text[8] = {0};
            if (length == 3 && c >= 0x80) { uint32_t w[2] = {c, 0}; char *u = wide_to_utf8_32(w, 1); if (u) { strncpy(text, u, 7); free(u); } }
            else text[0] = (char)c;
            spec[k] = 's'; spec[k + 1] = 0;
            if (!text[0]) { r = append(out, "", 1); break; }
            r = star_width ? append_formatted(out, spec, width, text) : append_formatted(out, spec, text);
            break;
        }
        case 's': {
            const char *s = (const char *)(uintptr_t)arg_int(args);
            char *owned = NULL;
            if (length == 3 && s) { owned = wide_to_utf8_32((const uint32_t *)s, star_precision ? (size_t)precision : wide_length((const uint32_t *)s)); s = owned; }
            if (!s) s = "(null)";
            spec[k] = 's'; spec[k + 1] = 0;
            r = star_width && star_precision ? append_formatted(out, spec, width, precision, s)
              : star_width ? append_formatted(out, spec, width, s)
              : star_precision ? append_formatted(out, spec, precision, s) : append_formatted(out, spec, s);
            free(owned);
            break;
        }
        case 'p': {
            uint64_t v = arg_int(args);
            r = v ? append_formatted(out, "0x%llx", (unsigned long long)v) : append(out, "(nil)", 5);
            break;
        }
        case 'n': {
            void *where = (void *)(uintptr_t)arg_int(args);
            if (length == 4 || length == 3 || length == 6) *(int64_t *)where = (int64_t)out->size;
            else *(int *)where = (int)out->size;
            break;
        }
        case 'm': r = append(out, "error", 5); break;
        default: r = append(out, start, (size_t)(p - start)); break;
        }
        if (r) return -1;
    }
    return out->size > 0x7fffffff ? -1 : (int)out->size;
}

// swprintf: the 32-bit format is narrowed (it is ASCII in practice), formatted,
// then widened again.
int sysv_wformat(TextBuffer *out, const uint32_t *format, SysvVaList *args) {
    size_t n = wide_length(format);
    char *narrow = malloc(n + 1);
    if (!narrow) return -1;
    for (size_t i = 0; i <= n; ++i) narrow[i] = format[i] < 128 ? (char)format[i] : '?';
    TextBuffer bytes = {0};
    int r = sysv_format(&bytes, narrow, args);
    free(narrow);
    if (r < 0) { free(bytes.data); return -1; }
    for (size_t i = 0; i < bytes.size; ++i) {
        uint32_t c = (unsigned char)bytes.data[i];
        if (append(out, &c, 4)) { free(bytes.data); return -1; }
    }
    free(bytes.data);
    return (int)(out->size / 4);
}

// sscanf: literal text and whitespace are matched here, and each conversion is
// handed to the C runtime with %n to learn how much input it consumed.
int sysv_scan(const char *input, const char *fmt, SysvVaList *args) {
    const char *s = input;
    int assigned = 0;
    for (const char *p = fmt; *p;) {
        if (*p == ' ' || (*p >= 9 && *p <= 13)) {
            while (*s == ' ' || (*s >= 9 && *s <= 13)) ++s;
            ++p; continue;
        }
        if (*p != '%') { if (*s != *p) return assigned; ++s; ++p; continue; }
        if (p[1] == '%') { while (*s == ' ' || (*s >= 9 && *s <= 13)) ++s; if (*s != '%') return assigned; ++s; p += 2; continue; }
        const char *start = p++;
        int suppress = 0;
        if (*p == '*') { suppress = 1; ++p; }
        while (*p >= '0' && *p <= '9') ++p;
        int length = 0;
        if (*p == 'h') { length = p[1] == 'h' ? 1 : 2; p += p[1] == 'h' ? 2 : 1; }
        else if (*p == 'l') { length = p[1] == 'l' ? 4 : 3; p += p[1] == 'l' ? 2 : 1; }
        else if (*p == 'L' || *p == 'q') { length = p[0] == 'L' ? 5 : 4; ++p; }
        else if (*p == 'j' || *p == 'z' || *p == 't') { length = 4; ++p; }
        if (*p == '[') { ++p; if (*p == '^') ++p; if (*p == ']') ++p; while (*p && *p != ']') ++p; }
        char conv = *p;
        if (!conv) return assigned;
        ++p;
        if (!*s && conv != 'n') return assigned ? assigned : -1;
        char spec[128];
        size_t len = (size_t)(p - start);
        if (len > 100) return assigned;
        memcpy(spec, start, len); spec[len] = 0;
        int consumed = 0, got = 0;
        char rebuilt[160];
        // Keep flags and width, then use a length modifier the Windows runtime
        // understands (long is 32 bits there).
        size_t head = 1; while (head < len && (spec[head] == '*' || (spec[head] >= '0' && spec[head] <= '9'))) ++head;
        const char *tail = spec + len - 1;
        if (conv == ']') { tail = strchr(spec, '['); }
        char modifier[4] = "";
        if (strchr("diouxX", conv)) strcpy(modifier, length == 1 ? "hh" : length == 2 ? "h" : length >= 3 ? "ll" : "");
        else if (strchr("eEfgGaA", conv)) strcpy(modifier, length == 5 ? "L" : length == 3 ? "l" : "");
        snprintf(rebuilt, sizeof rebuilt, "%.*s%s%s%%n", (int)head, spec, modifier, tail);
        if (conv == 'n') {
            if (!suppress) { void *where = (void *)(uintptr_t)arg_int(args); if (length >= 3) *(int64_t *)where = s - input; else *(int *)where = (int)(s - input); }
            continue;
        }
        if (suppress) got = __mingw_sscanf(s, rebuilt, &consumed);
        else {
            void *where = (void *)(uintptr_t)arg_int(args);
            if ((conv == 's' || conv == 'c' || conv == ']') && length == 3) return assigned;   // wide strings are not used
            got = __mingw_sscanf(s, rebuilt, where, &consumed);
            if (got == 1) ++assigned;
        }
        if (got < (suppress ? 0 : 1) || consumed == 0) return assigned;
        s += consumed;
    }
    return assigned;
}
