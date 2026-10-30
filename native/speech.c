#include "speech.h"

#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { uint8_t *data; size_t size, capacity; } Buffer;

static int fail(SpeechEngine *e, const char *format, ...) {
    va_list args; va_start(args, format); int n = vsnprintf(e->error, sizeof e->error, format, args); va_end(args);
    if (n >= (int)sizeof e->error) { // drop a final UTF-8 character that was cut short
        size_t end = sizeof e->error - 1, start = end;
        while (start && ((unsigned char)e->error[start - 1] & 0xc0) == 0x80) --start;
        if (start) {
            unsigned char lead = (unsigned char)e->error[start - 1];
            size_t width = lead < 0x80 ? 1 : lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : 2;
            if (start - 1 + width > end) e->error[start - 1] = 0;
        }
    }
    return -1;
}
static int append(Buffer *b, const void *data, size_t n) {
    if (n > b->capacity - b->size) return -1;
    memcpy(b->data + b->size, data, n); b->size += n; return 0;
}
static int varint(Buffer *b, size_t n) {
    do { uint8_t c = (uint8_t)(n & 0x7f); n >>= 7; if (n) c |= 0x80; if (append(b, &c, 1)) return -1; } while (n);
    return 0;
}
static int field_bytes(Buffer *b, unsigned field, const void *data, size_t n) {
    return varint(b, (field << 3) | 2) || varint(b, n) || append(b, data, n);
}
static int field_float(Buffer *b, unsigned field, float f) {
    return varint(b, (field << 3) | 5) || append(b, &f, sizeof f);
}
static int call(Guest *g, const char *name, const uint64_t args[8], uint64_t *result) {
    uint64_t address = guest_symbol(g, name);
    if (!address) { fprintf(stderr, "Missing engine function: %s\n", name); return -1; }
    return guest_call(g, address, args, result);
}
static void *guest_string(Guest *g, const char *s) {
    size_t len = strlen(s) + 1; void *p = guest_heap_alloc(g, len);
    if (p) memcpy(p, s, len);
    return p;
}
int writable_output(const char *path) {
    char root[PATH_MAX], parent[PATH_MAX], resolved[PATH_MAX];
    const char *data=getenv("GTTS_DATA");
    if (!data || !realpath(data, root) || !path || !*path || strlen(path) >= sizeof(parent)) return 0;
    strcpy(parent, path); char *slash = strrchr(parent, '/');
    if (!slash) strcpy(parent, "."); else if (slash == parent) slash[1] = '\0'; else *slash = '\0';
    if (!realpath(parent, resolved)) return 0;
    size_t n = strlen(root); struct stat st;
    if (!lstat(path, &st) && (S_ISLNK(st.st_mode) || (S_ISREG(st.st_mode) && st.st_nlink > 1))) return 0;
    return !strncmp(root, resolved, n) && (!resolved[n] || resolved[n] == '/');
}

int speech_open(SpeechEngine *e, Guest *g, const char *pack) {
    memset(e, 0, sizeof *e); e->guest = g; double start = guest_now();
    FILE *in = fopen(pack, "rb"); struct stat st;
    if (!in || fstat(fileno(in), &st) || st.st_size <= 0 || st.st_size > INT32_MAX) {
        if (in) fclose(in); return fail(e, "Cannot read voice pack: %s", pack);
    }
    CC_SHA256_CTX sha; CC_SHA256_Init(&sha); uint8_t block[65536], digest[CC_SHA256_DIGEST_LENGTH]; size_t bytes;
    while ((bytes = fread(block, 1, sizeof block, in)) != 0) CC_SHA256_Update(&sha, block, (CC_LONG)bytes);
    if (ferror(in) || fseek(in, 0, SEEK_SET)) { fclose(in); return fail(e, "Cannot read voice pack: %s", pack); }
    CC_SHA256_Final(digest, &sha);
    char hex[CC_SHA256_DIGEST_LENGTH * 2 + 1]; static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof digest; ++i) { hex[2*i] = digits[digest[i] >> 4]; hex[2*i+1] = digits[digest[i] & 15]; }
    hex[sizeof hex - 1] = 0; double hashed = guest_now();
    const char *data_root=getenv("GTTS_DATA");char installed_dir[PATH_MAX];
    if (!data_root || snprintf(installed_dir,sizeof installed_dir,"%s/installed",data_root)>=(int)sizeof installed_dir ||
        !writable_output(installed_dir) || (mkdir(installed_dir, 0700) && errno != EEXIST)) {
        fclose(in); return fail(e, "Cannot create voice installation directory");
    }
    char directory[PATH_MAX], pipeline[PATH_MAX], marker[PATH_MAX], lock_path[PATH_MAX];
    if (snprintf(directory, sizeof directory, "%s/voice-%s", installed_dir, hex) >= (int)sizeof directory ||
        snprintf(pipeline, sizeof pipeline, "%s/pipeline.pb", directory) >= (int)sizeof pipeline ||
        snprintf(marker, sizeof marker, "%s/.complete", directory) >= (int)sizeof marker ||
        snprintf(lock_path, sizeof lock_path, "%s/.lock", directory) >= (int)sizeof lock_path || !writable_output(directory) ||
        (mkdir(directory, 0700) && errno != EEXIST) || !writable_output(lock_path)) {
        fclose(in); return fail(e, "Cannot prepare voice installation directory");
    }
    int lock_fd = open(lock_path, O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX)) {
        if (lock_fd >= 0) close(lock_fd); fclose(in); return fail(e, "Cannot lock voice installation");
    }
    uint64_t a[8] = {guest_import(g, "__guest_logger")}, result = 0;
    if (!a[0] || call(g, "GoogleTtsSetLogger", a, &result) || call(g, "GoogleTtsPreSandboxInit", NULL, &result)) {
        close(lock_fd); fclose(in); return fail(e, "Engine pre-initialization failed");
    }
    if (access(pipeline, F_OK) != 0 || access(marker, F_OK) != 0) {
        void *data = guest_heap_alloc(g, (size_t)st.st_size); void *dir = guest_string(g, directory);
        if (!data || !dir || fread(data, 1, (size_t)st.st_size, in) != (size_t)st.st_size) {
            guest_heap_free(g, data); guest_heap_free(g, dir); close(lock_fd); fclose(in); return fail(e, "Cannot load voice pack");
        }
        a[0] = (uintptr_t)dir; a[1] = (uintptr_t)data; a[2] = (uint64_t)st.st_size;
        int failed = call(g, "GoogleTtsInstallVoice", a, &result) || !(result & 0xff);
        guest_heap_free(g, data); guest_heap_free(g, dir);
        if (failed) { close(lock_fd); fclose(in); return fail(e, "GoogleTtsInstallVoice failed for %s", pack); }
        if (!writable_output(marker)) { close(lock_fd); fclose(in); return fail(e, "Invalid voice installation marker"); }
        int complete = open(marker, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
        if (complete < 0) { close(lock_fd); fclose(in); return fail(e, "Cannot mark voice installation complete"); }
        if (close(complete)) { close(lock_fd); fclose(in); return fail(e, "Cannot finish voice installation"); }
    }
    close(lock_fd); fclose(in); double installed = guest_now();
    e->pipeline = guest_string(g, pipeline); e->directory = guest_string(g, directory);
    a[0] = (uintptr_t)e->pipeline; a[1] = (uintptr_t)e->directory;
    if (!a[0] || !a[1] || call(g, "GoogleTtsInit", a, &result) || !(result & 0xff))
        return fail(e, "GoogleTtsInit failed for %s", pack);
    if(!g->speaker_count && g->fallback_speaker_count){g->speaker_count=g->fallback_speaker_count;
        memcpy(g->speakers,g->fallback_speakers,g->speaker_count*sizeof(Speaker));}
    e->ready = 1;
    if (g->profile) fprintf(stderr, "PROFILE pack_hash=%.3fs presandbox_install=%.3fs voice_init=%.3fs arena_used=%zu\n",
                            hashed-start, installed-hashed, guest_now()-installed, g->arena_used);
    return 0;
}

static int make_request(Guest *g, const char *text, const char *speaker, const char *gender, double rate, double pitch,
                        void **request, size_t *request_len, void **spk, size_t *spk_len) {
    size_t n = strlen(text); if (n > 16 * 1024 * 1024) return -1;
    size_t cap = n + 128; uint8_t *storage = malloc(cap * 3 + 512); if (!storage) return -1;
    Buffer params = {storage, 0, 64}, body = {storage + 64, 0, cap}, inner = {storage + 64 + cap, 0, cap};
    Buffer outer = {storage + 64 + cap*2, 0, cap}, speaker_buf = {storage + 64 + cap*3, 0, 448};
    int error = field_float(&params, 2, (float)pitch) || field_float(&params, 3, (float)(1.0/rate)) ||
                field_bytes(&body, 1, text, n) || field_bytes(&body, 20, params.data, params.size) ||
                field_bytes(&inner, 1, body.data, body.size) || field_bytes(&outer, 1, inner.data, inner.size) ||
                field_bytes(&speaker_buf, 1, speaker, strlen(speaker)) || field_bytes(&speaker_buf, 2, gender, strlen(gender));
    *request = error ? NULL : guest_heap_alloc(g, outer.size); *spk = error ? NULL : guest_heap_alloc(g, speaker_buf.size);
    if (!*request || !*spk) error = 1;
    if (!error) { memcpy(*request, outer.data, outer.size); memcpy(*spk, speaker_buf.data, speaker_buf.size);
        *request_len = outer.size; *spk_len = speaker_buf.size; }
    free(storage); return error ? -1 : 0;
}

static int speech_synth_part(SpeechEngine *e, const char *text, const char *speaker, const char *gender, double rate, double pitch,
                             float **audio, size_t *count) {
    Guest *g = e->guest; e->error[0] = 0; *audio = NULL; *count = 0;
    if (__atomic_load_n(&g->master->broken, __ATOMIC_ACQUIRE)) { fail(e, "Engine worker is broken"); e->broken = 1; return SPEECH_ENGINE_BROKEN; }
    if (!e->ready || !text || !speaker || !isfinite(rate) || rate <= 0 || !isfinite((float)(1.0/rate)) ||
        !isfinite(pitch) || pitch <= 0 || !isfinite((float)pitch)) return fail(e, "Invalid synthesis parameters");
    const char *detected = NULL;
    for (size_t i = 0; i < g->speaker_count; ++i) if (!strcmp(g->speakers[i].name, speaker)) { detected = g->speakers[i].gender; break; }
    if (g->speaker_count && !detected) return fail(e, "Speaker '%s' is not in this voice pack", speaker);
    if (!gender || !*gender) gender = detected && *detected ? detected : "female";
    void *req = NULL, *spk = NULL, *samples = NULL, *written = NULL;
    size_t req_len = 0, spk_len = 0, total = 0, capacity = 0; float *output = NULL; int rc = -1, entered = 0;
    if (make_request(g, text, speaker, gender, rate, pitch, &req, &req_len, &spk, &spk_len)) {
        fail(e, "Text is too long or guest memory is exhausted"); goto cleanup;
    }
    uint64_t calls_before = 0;
    if (g->profile) for (size_t i = 0; i < MAX_IMPORTS; ++i)
        calls_before += __atomic_load_n(&g->master->hostcall_counts[i], __ATOMIC_RELAXED);
    double start = guest_now(); uint64_t a[8] = {(uintptr_t)req, (uintptr_t)spk, req_len, spk_len}, result = 0;
    entered = 1;
    if (call(g, "GoogleTtsInitBuffered", a, &result) || !(result & 0xff)) { fail(e, "GoogleTtsInitBuffered failed"); goto cleanup; }
    if (call(g, "GoogleTtsGetFramesInAudioBuffer", NULL, &result) || !result || result > 10000000) {
        fail(e, "Invalid audio buffer size"); goto cleanup;
    }
    size_t frames = (size_t)result; samples = guest_heap_alloc(g, frames * sizeof(float)); written = guest_heap_alloc(g, sizeof(size_t));
    if (!samples || !written) { fail(e, "Guest memory exhausted"); goto cleanup; }
    int status, empty = 0;
    do {
        *(size_t *)written = 0; a[0] = (uintptr_t)samples; a[1] = (uintptr_t)written;
        if (call(g, "GoogleTtsReadBuffered", a, &result) || *(size_t *)written > frames) {
            fail(e, "GoogleTtsReadBuffered failed"); goto cleanup;
        }
        status = (int32_t)result; size_t amount = *(size_t *)written;
        if (status < 0 || total + amount > (UINT32_MAX - 44) / sizeof(float) || (!amount && status > 0 && ++empty > 1000)) {
            fail(e, "GoogleTtsReadBuffered returned invalid audio (status %d)", status); goto cleanup;
        }
        if (amount) empty = 0;
        if (total + amount > capacity) {
            size_t next = capacity ? capacity : 16384;
            while (next < total + amount) next *= 2;
            float *p = realloc(output, next * sizeof(float)); if (!p) { fail(e, "Host memory exhausted"); goto cleanup; }
            output = p; capacity = next;
        }
        if (amount) memcpy(output + total, samples, amount * sizeof(float)); total += amount;
    } while (status > 0);
    if (g->profile) {
        double elapsed = guest_now() - start;
        uint64_t calls = 0;
        for (size_t i = 0; i < MAX_IMPORTS; ++i)
            calls += __atomic_load_n(&g->master->hostcall_counts[i], __ATOMIC_RELAXED);
        calls -= calls_before;
        fprintf(stderr, "PROFILE backend=%s synth=%.3fs audio=%.3fs rtf=%.3f hostcalls=%llu hostcalls_per_s=%.0f arena_used=%zu\n",
                g->macho_mode ? "macho" : g->native_mode ? "inprocess" : "vm", elapsed, (double)total/24000,
                total ? elapsed/((double)total/24000) : 0, (unsigned long long)calls,
                elapsed ? calls/elapsed : 0, g->arena_used);
    }
    if (__atomic_load_n(&g->master->broken, __ATOMIC_ACQUIRE)) { fail(e, "Engine worker is broken"); goto cleanup; }
    *audio = output; *count = total; output = NULL; rc = 0;
cleanup:
    free(output); guest_heap_free(g, written); guest_heap_free(g, samples); guest_heap_free(g, spk); guest_heap_free(g, req);
    if (rc && (entered || __atomic_load_n(&g->master->broken, __ATOMIC_ACQUIRE))) {e->broken = 1; rc = SPEECH_ENGINE_BROKEN;}
    return rc;
}

size_t speech_chunk_end(const char *text, size_t offset, size_t length) {
    size_t end=offset+1024<length?offset+1024:length;
    if(end==length)return end;
    size_t sentence=0,space=0;
    for(size_t i=offset;i<end;) {
        const unsigned char *p=(const unsigned char *)text+i;unsigned char c=p[0];
        size_t width=c<0x80?1:(c&0xe0)==0xc0?2:(c&0xf0)==0xe0?3:(c&0xf8)==0xf0?4:1;
        if(width>end-i)break;
        if(width==1 && (c==' ' || c=='\t' || c=='\n' || c=='\r')) {
            space=i+1;if(c=='\n' || (i>offset && strchr(".!?",text[i-1])))sentence=i+1;
        } else if(width==3 && ((c==0xe3 && p[1]==0x80 && p[2]==0x82) ||
                  (c==0xe0 && p[1]==0xa5 && (p[2]==0xa4 || p[2]==0xa5)) ||
                  (c==0xef && p[1]==0xbc && (p[2]==0x81 || p[2]==0x9f))))sentence=i+width;
        else if(width==3 && c==0xe3 && p[1]==0x80 && (p[2]==0x80 || p[2]==0x81))space=i+width;
        i+=width;
    }
    if(sentence>offset+256)return sentence;
    if(space>offset+256)return space;
    while(end>offset && (((unsigned char)text[end]&0xc0)==0x80))--end;
    return end;
}

// Long text is spoken in segments; each segment's audio goes to the sink as soon as
// it is ready, so a chapter never has to be held in memory as a whole.
int speech_synth_stream(SpeechEngine *e, const char *text, const char *speaker, const char *gender, double rate, double pitch,
                        SpeechSink sink, void *context, size_t *count) {
    size_t length = text ? strlen(text) : 0, offset = 0, total = 0;
    *count = 0;
    if (length > 16 * 1024 * 1024) return fail(e, "Text exceeds 16 MiB");
    do {
        size_t end = length <= 1024 ? length : speech_chunk_end(text, offset, length);
        if (length > 1024 && end == offset) return fail(e, "Cannot split UTF-8 text");
        float *chunk = NULL; size_t frames = 0; int status;
        if (length <= 1024) status = speech_synth_part(e, text, speaker, gender, rate, pitch, &chunk, &frames);
        else {
            char *part = malloc(end - offset + 1);
            if (!part) return fail(e, "Host memory exhausted");
            memcpy(part, text + offset, end - offset); part[end - offset] = 0;
            status = speech_synth_part(e, part, speaker, gender, rate, pitch, &chunk, &frames); free(part);
        }
        if (status) { free(chunk); return status; }
        if (frames > (UINT32_MAX - 44) / sizeof(float) - total) { free(chunk); return fail(e, "Audio is too long"); }
        int delivered = sink(context, chunk, frames); free(chunk);
        if (delivered) return fail(e, "Cannot deliver audio");
        total += frames; offset = end;
    } while (offset < length);
    *count = total; return 0;
}

typedef struct { float *data; size_t total, capacity; } AudioBuffer;
static int append_audio(void *context, const float *samples, size_t frames) {
    AudioBuffer *b = context;
    if (b->total + frames > b->capacity) {
        size_t next = b->capacity ? b->capacity : 16384; while (next < b->total + frames) next *= 2;
        float *grown = realloc(b->data, next * sizeof(float)); if (!grown) return -1;
        b->data = grown; b->capacity = next;
    }
    if (frames) memcpy(b->data + b->total, samples, frames * sizeof(float));
    b->total += frames; return 0;
}
int speech_synth(SpeechEngine *e, const char *text, const char *speaker, const char *gender, double rate, double pitch,
                 float **audio, size_t *count) {
    AudioBuffer b = {0};
    int status = speech_synth_stream(e, text, speaker, gender, rate, pitch, append_audio, &b, count);
    if (status) { free(b.data); *audio = NULL; *count = 0; return status; }
    *audio = b.data; return 0;
}
void speech_close(SpeechEngine *e) {
    if (!e->guest) return;
    uint64_t result = 0;
    if (e->ready && !e->broken && call(e->guest, "GoogleTtsShutdown", NULL, &result)) fprintf(stderr, "Warning: engine shutdown failed\n");
    guest_heap_free(e->guest, e->directory); guest_heap_free(e->guest, e->pipeline); e->ready = 0;
}
