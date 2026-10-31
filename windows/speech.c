// Drives the engine's C API: install a voice pack, initialise it, and turn text
// into 24 kHz float samples.
#include "speech.h"
#include "engine.h"

#include <windows.h>
#include <bcrypt.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef SYSV void (*LogFunction)(int, const char *);
typedef SYSV void (*SetLogger)(LogFunction);
typedef SYSV void (*VoidCall)(void);
typedef SYSV uint8_t (*InstallVoice)(const char *, const void *, int);
typedef SYSV uint8_t (*Init)(const char *, const char *);
typedef SYSV uint8_t (*InitBuffered)(const void *, const void *, int, int);
typedef SYSV size_t (*FramesInBuffer)(void);
typedef SYSV int (*ReadBuffered)(float *, size_t *);

static char error_text[512];
const char *speech_error(void) { return error_text; }
static int fail(const char *format, ...) {
    va_list ap; va_start(ap, format);
    vsnprintf(error_text, sizeof error_text, format, ap);
    va_end(ap);
    return -1;
}

static void remember(const char *name, size_t n, const char *gender, size_t m) {
    if (!n || n >= sizeof engine_speakers[0].name || m >= sizeof engine_speakers[0].gender) return;
    for (unsigned i = 0; i < engine_speaker_count; ++i)
        if (strlen(engine_speakers[i].name) == n && !memcmp(engine_speakers[i].name, name, n)) {
            if (m && !engine_speakers[i].gender[0]) memcpy(engine_speakers[i].gender, gender, m);
            return;
        }
    if (engine_speaker_count >= sizeof engine_speakers / sizeof engine_speakers[0]) return;
    Speaker *s = &engine_speakers[engine_speaker_count++];
    memset(s, 0, sizeof *s);
    memcpy(s->name, name, n);
    if (m) memcpy(s->gender, gender, m);
}

// The pack lists its speakers in the log while it initialises.
static SYSV void engine_log(int severity, const char *message) {
    for (const char *p = message; p && (p = strstr(p, "speaker: \""));) {
        const char *start = p + 10, *end = strchr(start, '"');
        if (!end) break;
        const char *q = end + 1, *gender = NULL, *gender_end = NULL;
        while (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t') ++q;
        if (q > end + 1 && !strncmp(q, "gender: \"", 9)) { gender = q + 9; gender_end = strchr(gender, '"'); }
        remember(start, (size_t)(end - start), gender, gender_end ? (size_t)(gender_end - gender) : 0);
        p = end + 1;
    }
    if (engine_verbose) fprintf(stderr, "[engine %d] %s\n", severity, message);
}

static void *function(const char *name) {
    void *p = elf_symbol(name);
    if (!p) fail("The engine has no %s", name);
    return p;
}

int sha256_file(const char *path, char hex[65]) {
    wchar_t *w = utf8_to_wide(path);
    HANDLE h = w ? CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL) : INVALID_HANDLE_VALUE;
    free(w);
    if (h == INVALID_HANDLE_VALUE) return -1;
    BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE hash; uint8_t digest[32];
    static uint8_t block[1 << 16];
    int rc = -1;
    if (!BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)) {
        if (!BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0)) {
            DWORD got; rc = 0;
            for (;;) {
                if (!ReadFile(h, block, sizeof block, &got, NULL)) { rc = -1; break; }
                if (!got) break;
                BCryptHashData(hash, block, got, 0);
            }
            BCryptFinishHash(hash, digest, 32, 0);
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    CloseHandle(h);
    for (int i = 0; i < 32; ++i) sprintf(hex + 2 * i, "%02x", digest[i]);
    return rc;
}

static int exists(const char *path) {
    wchar_t *w = utf8_to_wide(path);
    DWORD a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    free(w);
    return a != INVALID_FILE_ATTRIBUTES;
}
static void make_dir(const char *path) { wchar_t *w = utf8_to_wide(path); if (w) CreateDirectoryW(w, NULL); free(w); }

int speech_open(const char *data_dir, const char *pack, const char *engine_path) {
    if (elf_load_all(data_dir, engine_path)) return fail("Cannot load the engine");
    SetLogger set_logger = function("GoogleTtsSetLogger");
    VoidCall presandbox = function("GoogleTtsPreSandboxInit");
    InstallVoice install = function("GoogleTtsInstallVoice");
    Init init = function("GoogleTtsInit");
    if (!set_logger || !presandbox || !install || !init) return -1;
    set_logger(engine_log);
    presandbox();

    char hex[65], dir[1024], pipeline[1100], marker[1100], lock[1100];
    if (sha256_file(pack, hex)) return fail("Cannot read voice pack %s", pack);
    snprintf(dir, sizeof dir, "%s/installed", data_dir);
    make_dir(dir);
    snprintf(dir, sizeof dir, "%s/installed/voice-%s", data_dir, hex);
    make_dir(dir);
    snprintf(pipeline, sizeof pipeline, "%s/pipeline.pb", dir);
    snprintf(marker, sizeof marker, "%s/.complete", dir);
    snprintf(lock, sizeof lock, "%s/.lock", dir);
    // Several workers may start at once; one installs while the others wait.
    wchar_t *wlock = utf8_to_wide(lock);
    HANDLE lock_file = INVALID_HANDLE_VALUE;
    for (int tries = 0; wlock && tries < 600; ++tries) {
        lock_file = CreateFileW(wlock, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (lock_file != INVALID_HANDLE_VALUE) break;
        Sleep(100);
    }
    free(wlock);
    if (lock_file == INVALID_HANDLE_VALUE) return fail("Cannot lock the voice installation");
    if (!exists(pipeline) || !exists(marker)) {
        wchar_t *w = utf8_to_wide(pack);
        HANDLE h = w ? CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL) : INVALID_HANDLE_VALUE;
        free(w);
        LARGE_INTEGER size = {0};
        uint8_t *data = NULL; DWORD got = 0;
        if (h != INVALID_HANDLE_VALUE && GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < INT32_MAX &&
            (data = malloc((size_t)size.QuadPart)) && ReadFile(h, data, (DWORD)size.QuadPart, &got, NULL) && got == size.QuadPart) {
            CloseHandle(h);
            int ok = install(dir, data, (int)size.QuadPart);
            free(data);
            if (!ok) { CloseHandle(lock_file); return fail("GoogleTtsInstallVoice failed for %s", pack); }
            FILE *f = fopen(marker, "wb");
            if (f) fclose(f);
        } else {
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            free(data); CloseHandle(lock_file);
            return fail("Cannot read voice pack %s", pack);
        }
    }
    CloseHandle(lock_file);
    if (!init(pipeline, dir)) return fail("GoogleTtsInit failed for %s", pack);
    return 0;
}

typedef struct { uint8_t *data; size_t size, capacity; } Bytes;
static int put(Bytes *b, const void *data, size_t n) {
    if (b->size + n > b->capacity) {
        size_t cap = b->capacity ? b->capacity * 2 : 256;
        while (cap < b->size + n) cap *= 2;
        uint8_t *p = realloc(b->data, cap);
        if (!p) return -1;
        b->data = p; b->capacity = cap;
    }
    memcpy(b->data + b->size, data, n); b->size += n;
    return 0;
}
static int varint(Bytes *b, size_t n) {
    do { uint8_t c = (uint8_t)(n & 0x7f); n >>= 7; if (n) c |= 0x80; if (put(b, &c, 1)) return -1; } while (n);
    return 0;
}
static int field_bytes(Bytes *b, unsigned field, const void *data, size_t n) {
    return varint(b, (field << 3) | 2) || varint(b, n) || put(b, data, n);
}
static int field_float(Bytes *b, unsigned field, float f) { return varint(b, (field << 3) | 5) || put(b, &f, 4); }

const char *speech_gender(const char *speaker) {
    for (unsigned i = 0; i < engine_speaker_count; ++i)
        if (!strcmp(engine_speakers[i].name, speaker)) return engine_speakers[i].gender;
    return NULL;
}

static int synth_part(const char *text, size_t length, const char *speaker, const char *gender, double rate, double pitch,
                      SpeechSink sink, void *context, size_t *total) {
    InitBuffered init = function("GoogleTtsInitBuffered");
    FramesInBuffer frames_in = function("GoogleTtsGetFramesInAudioBuffer");
    ReadBuffered read = function("GoogleTtsReadBuffered");
    if (!init || !frames_in || !read) return -1;
    // Field 3 scales duration, so speed 2.5 is sent as 0.4.
    Bytes params = {0}, body = {0}, inner = {0}, outer = {0}, spk = {0};
    int bad = field_float(&params, 2, (float)pitch) || field_float(&params, 3, (float)(1.0 / rate)) ||
              field_bytes(&body, 1, text, length) || field_bytes(&body, 20, params.data, params.size) ||
              field_bytes(&inner, 1, body.data, body.size) || field_bytes(&outer, 1, inner.data, inner.size) ||
              field_bytes(&spk, 1, speaker, strlen(speaker)) || field_bytes(&spk, 2, gender, strlen(gender));
    int rc = -1;
    float *samples = NULL;
    if (bad) { fail("Out of memory"); goto done; }
    if (!init(outer.data, spk.data, (int)outer.size, (int)spk.size)) { fail("GoogleTtsInitBuffered failed"); goto done; }
    size_t frames = frames_in();
    if (!frames || frames > 10000000) { fail("Invalid audio buffer size"); goto done; }
    samples = malloc(frames * sizeof(float));
    if (!samples) { fail("Out of memory"); goto done; }
    int status, empty = 0;
    do {
        size_t written = 0;
        status = read(samples, &written);
        if (status < 0 || written > frames || (!written && status > 0 && ++empty > 1000)) {
            fail("GoogleTtsReadBuffered failed (status %d)", status); goto done;
        }
        if (written) { empty = 0; if (sink(context, samples, written)) { fail("Cannot write audio"); goto done; } }
        *total += written;
    } while (status > 0);
    rc = 0;
done:
    free(samples); free(params.data); free(body.data); free(inner.data); free(outer.data); free(spk.data);
    return rc;
}

// Long text is spoken in segments of about a kilobyte, split at a sentence end
// or a space where possible, the same way as on macOS.
size_t speech_chunk_end(const char *text, size_t offset, size_t length) {
    size_t end = offset + 1024 < length ? offset + 1024 : length;
    if (end == length) return end;
    size_t sentence = 0, space = 0;
    for (size_t i = offset; i < end;) {
        const unsigned char *p = (const unsigned char *)text + i; unsigned char c = p[0];
        size_t width = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1;
        if (width > end - i) break;
        if (width == 1 && (c == ' ' || c == '\t' || c == '\n' || c == '\r')) {
            space = i + 1;
            if (c == '\n' || (i > offset && strchr(".!?", text[i - 1]))) sentence = i + 1;
        } else if (width == 3 && ((c == 0xe3 && p[1] == 0x80 && p[2] == 0x82) ||
                                  (c == 0xe0 && p[1] == 0xa5 && (p[2] == 0xa4 || p[2] == 0xa5)) ||
                                  (c == 0xef && p[1] == 0xbc && (p[2] == 0x81 || p[2] == 0x9f)))) sentence = i + width;
        else if (width == 3 && c == 0xe3 && p[1] == 0x80 && (p[2] == 0x80 || p[2] == 0x81)) space = i + width;
        i += width;
    }
    if (sentence > offset + 256) return sentence;
    if (space > offset + 256) return space;
    while (end > offset && (((unsigned char)text[end] & 0xc0) == 0x80)) --end;
    return end;
}

int speech_synth(const char *text, const char *speaker, const char *gender, double rate, double pitch,
                 SpeechSink sink, void *context, size_t *count) {
    *count = 0;
    if (!isfinite(rate) || rate <= 0 || !isfinite(pitch) || pitch <= 0) return fail("rate and pitch must be positive numbers");
    const char *known = speech_gender(speaker);
    if (engine_speaker_count && !known) return fail("No speaker '%s' in this pack", speaker);
    if (!gender || !*gender) gender = known && *known ? known : "female";
    size_t length = strlen(text), offset = 0;
    if (length > 16u << 20) return fail("Text exceeds 16 MiB");
    while (offset < length) {
        size_t end = speech_chunk_end(text, offset, length);
        if (synth_part(text + offset, end - offset, speaker, gender, rate, pitch, sink, context, count)) return -1;
        offset = end;
    }
    return 0;
}
