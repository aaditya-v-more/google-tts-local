// gtts for Windows: the command line, the engine workers and the HTTP server.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <psapi.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "json.h"
#include "speech.h"

#ifndef GTTS_VERSION
#define GTTS_VERSION "0.4.0"
#endif

static const char *const PACKAGE_URL = "https://storage.googleapis.com/chromeos-localmirror/distfiles/googletts-35.3.tar.xz";
static const char *const PACKAGE_SHA256 = "51785a17918e2c4d9aa2f4cc17ed8ee672b0127501a58b457fdd7876755d046a";
static const char *const ENGINE_SHA256 = "b1fef77d0149c167c49a9eb5325ea9f46eb7ef31fa51ca43033a086aacdb1404";
static const struct { const char *lang, *sha256; } PACKS[] = {
    {"en-us", "5fe8cf314d68ef47a10d7ae6dc369ed4ab4975bb7d1809b952d57ccf7284ed6f"},
    {"es-es", "9d9c81453aec389643f8c385ca02308ff462cb0cf0164116e5bdca898d3b1df4"},
    {"fr-fr", "f6130d59e7b9200f07fe6ef4c99658171db79e104a04e1189c7905acb87830d4"},
    {"hi-in", "75de7d82e3d71dc764b877d09fca9b1b54424f6e9af9ce3ab9ffa41b9c552b35"},
    {"ja-jp", "28eadf2d7473abcaeccc609ecd16e897f8cceb78988a2750616e6b0b1a6db4f4"},
    {"nl-nl", "d4a927483d563a06464f33357436ac0aab8dc1b8d1cc64a91742e4cbc912810b"},
};
enum { PACK_COUNT = sizeof PACKS / sizeof PACKS[0] };
// The C++ runtime the engine was built against, from Ubuntu 24.04 (amd64).
// SHA-256 values are from Ubuntu's package indices and of the extracted libraries.
static const struct { const char *name, *deb, *sha256, *member, *lib_sha256, *copyright; } RUNTIME[] = {
    {"libc++.so.1", "pool/universe/l/llvm-toolchain-18/libc%2B%2B1-18_18.1.3-1_amd64.deb",
     "b7b2577ba8395eb81af6fed22c318265ac27327d840e912fe6a375f20f27fc3d", "./usr/lib/llvm-18/lib/libc++.so.1.0",
     "9541a671f61fdc0a5b41db8b7023a9482309d11a56eb51998795346219dff76a", "./usr/share/doc/libc++1-18/copyright"},
    {"libc++abi.so.1", "pool/universe/l/llvm-toolchain-18/libc%2B%2Babi1-18_18.1.3-1_amd64.deb",
     "f2c72b1013a3feeea1f8c4861c595008baa74f83a1450b3c9fb48b3d0b4e6b5c", "./usr/lib/llvm-18/lib/libc++abi.so.1.0",
     "f64e89cd0a48d3826ed89746df2854671dd6a0b4dba4f8d461f24dcc65a37ef4", "./usr/share/doc/libc++abi1-18/copyright"},
    {"libunwind.so.1", "pool/universe/l/llvm-toolchain-18/libunwind-18_18.1.3-1_amd64.deb",
     "4019c3dabf8a6079b0edf4a0cb74b9da25f665e0a677267959a134ab4e4ea889", "./usr/lib/llvm-18/lib/libunwind.so.1.0",
     "1d552b7d50207fee79ee5a676306b3a05848c6be3f8f6869bb4d67bce28fbc5d", "./usr/share/doc/libunwind-18/copyright"},
};
static const char *const UBUNTU_MIRROR = "https://archive.ubuntu.com/ubuntu/";

// ---- small helpers -------------------------------------------------------------

static void fail_with(int code, const char *format, ...) {
    va_list ap; va_start(ap, format);
    fputs("error: ", stderr); vfprintf(stderr, format, ap); fputc('\n', stderr);
    va_end(ap);
    exit(code);
}
#define die(...) fail_with(1, __VA_ARGS__)
#define usage_error(...) fail_with(2, __VA_ARGS__)

static char *format_string(const char *format, ...) {
    va_list ap; va_start(ap, format);
    int n = vsnprintf(NULL, 0, format, ap);
    va_end(ap);
    char *s = malloc((size_t)n + 1);
    if (!s) die("Out of memory");
    va_start(ap, format); vsnprintf(s, (size_t)n + 1, format, ap); va_end(ap);
    return s;
}

static wchar_t *wide(const char *s) {
    wchar_t *w = utf8_to_wide(s);
    if (!w) die("Out of memory");
    return w;
}

static int file_exists(const char *path) {
    wchar_t *w = wide(path);
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Compares the files themselves, so a path through a junction (Scoop's "current")
// matches the real one.
static int same_file(const wchar_t *a, const wchar_t *b) {
    BY_HANDLE_FILE_INFORMATION ia, ib;
    HANDLE ha = CreateFileW(a, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE hb = CreateFileW(b, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    int same = ha != INVALID_HANDLE_VALUE && hb != INVALID_HANDLE_VALUE &&
               GetFileInformationByHandle(ha, &ia) && GetFileInformationByHandle(hb, &ib) &&
               ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber &&
               ia.nFileIndexHigh == ib.nFileIndexHigh && ia.nFileIndexLow == ib.nFileIndexLow;
    if (ha != INVALID_HANDLE_VALUE) CloseHandle(ha);
    if (hb != INVALID_HANDLE_VALUE) CloseHandle(hb);
    return same;
}

static void ensure_dir(const char *path) {
    char *copy = format_string("%s", path);
    for (char *p = copy + 1; *p; ++p) if (*p == '/' || *p == '\\') {
        if (p[-1] == ':') continue;
        char c = *p; *p = 0;
        wchar_t *w = wide(copy); CreateDirectoryW(w, NULL); free(w);
        *p = c;
    }
    wchar_t *w = wide(copy);
    if (!CreateDirectoryW(w, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) die("Cannot create %s", path);
    free(w); free(copy);
}

static int move_file(const char *from, const char *to) {
    wchar_t *a = wide(from), *b = wide(to);
    BOOL ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    free(a); free(b);
    return ok ? 0 : -1;
}

static void delete_file(const char *path) { wchar_t *w = wide(path); DeleteFileW(w); free(w); }

static void delete_tree(const char *path) {
    char *pattern = format_string("%s\\*", path);
    wchar_t *w = wide(pattern);
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(w, &data);
    free(w); free(pattern);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
            char *name = wide_to_utf8(data.cFileName), *child = format_string("%s\\%s", path, name);
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) delete_tree(child); else delete_file(child);
            free(name); free(child);
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    w = wide(path); RemoveDirectoryW(w); free(w);
}

static char *exe_path(void) {
    wchar_t buffer[32768];
    DWORD n = GetModuleFileNameW(NULL, buffer, 32768);
    if (!n || n >= 32768) die("Cannot locate gtts.exe");
    return wide_to_utf8(buffer);
}

static char *exe_sibling(const char *name) {
    char *self = exe_path(), *slash = strrchr(self, '\\');
    if (slash) slash[1] = 0;
    char *path = format_string("%s%s", self, name);
    free(self);
    return path;
}

static char *absolute(const char *path) {
    wchar_t *w = wide(path), full[32768];
    DWORD n = GetFullPathNameW(w, 32768, full, NULL);
    free(w);
    if (!n || n >= 32768) die("Invalid path: %s", path);
    return wide_to_utf8(full);
}

// Quotes one argument for CreateProcess the way the C runtime parses it back.
static void append_argument(JsonOut *line, const char *arg) {
    if (line->size) json_out_raw(line, " ");
    if (*arg && !strpbrk(arg, " \t\"")) { json_out_raw(line, arg); return; }
    json_out_raw(line, "\"");
    for (const char *p = arg;; ++p) {
        size_t slashes = 0;
        while (*p == '\\') { ++slashes; ++p; }
        if (!*p) { for (size_t i = 0; i < slashes * 2; ++i) json_out_raw(line, "\\"); break; }
        if (*p == '"') { for (size_t i = 0; i < slashes * 2 + 1; ++i) json_out_raw(line, "\\"); json_out_raw(line, "\""); }
        else { for (size_t i = 0; i < slashes; ++i) json_out_raw(line, "\\"); char c[2] = {*p, 0}; json_out_raw(line, c); }
    }
    json_out_raw(line, "\"");
}

static int run_command(const char *const *args) {
    JsonOut line = {0};
    for (size_t i = 0; args[i]; ++i) append_argument(&line, args[i]);
    wchar_t *w = wide(line.data);
    STARTUPINFOW si = {sizeof si}; PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(NULL, w, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    free(w); free(line.data);
    if (!ok) die("Cannot run %s", args[0]);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
}

static char *system_tool(const char *name) {
    wchar_t dir[MAX_PATH];
    UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (!n || n >= MAX_PATH) die("Cannot locate the Windows system folder");
    char *system = wide_to_utf8(dir), *path = format_string("%s\\%s", system, name);
    free(system);
    if (!file_exists(path)) die("%s is missing; it ships with Windows 10 1803 and later.", path);
    return path;
}

// ---- fetch -------------------------------------------------------------------------

static char *data_dir;

static void download(const char *url, const char *dest, const char *sha256) {
    char *curl = system_tool("curl.exe"), *part = format_string("%s.part", dest), hex[65];
    if (file_exists(dest) && !sha256_file(dest, hex) && !strcmp(hex, sha256)) { free(curl); free(part); return; }
    fprintf(stderr, "Downloading %s\n", url);
    const char *args[] = {curl, "-fL", "--proto", "=https", "--proto-redir", "=https", "--retry", "3",
                          "--connect-timeout", "30", "--progress-bar", "-o", part, url, NULL};
    if (run_command(args)) { delete_file(part); die("Download failed: %s", url); }
    if (sha256_file(part, hex) || strcmp(hex, sha256)) { delete_file(part); die("SHA-256 mismatch for %s", url); }
    if (move_file(part, dest)) die("Cannot save %s", dest);
    free(curl); free(part);
}

static void extract(const char *archive, const char *dir, const char *const *members) {
    char *tar = system_tool("tar.exe");
    const char *args[24] = {tar, "-xf", archive, "-C", dir};
    size_t n = 5;
    for (size_t i = 0; members[i] && n < 23; ++i) args[n++] = members[i];
    args[n] = NULL;
    if (run_command(args)) die("Cannot extract %s", archive);
    free(tar);
}

static void install_verified(const char *from, const char *to, const char *sha256) {
    char hex[65];
    if (sha256_file(from, hex) || strcmp(hex, sha256)) die("SHA-256 mismatch for %s", from);
    if (move_file(from, to)) die("Cannot install %s", to);
}

static int verified(const char *path, const char *sha256) {
    char hex[65];
    return file_exists(path) && !sha256_file(path, hex) && !strcmp(hex, sha256);
}

static void fetch(const char **langs, size_t lang_count) {
    char *tmp = format_string("%s\\tmp", data_dir), *voices = format_string("%s\\voices", data_dir);
    char *runtime = format_string("%s\\runtime", data_dir), *stage = format_string("%s\\stage", tmp);
    ensure_dir(tmp); ensure_dir(voices); ensure_dir(runtime);
    delete_tree(stage); ensure_dir(stage);

    char *engine = format_string("%s\\libchrometts.so", data_dir);
    const char *members[PACK_COUNT + 2];
    char *pack_names[PACK_COUNT];
    size_t member_count = 0;
    int need_package = !verified(engine, ENGINE_SHA256);
    if (need_package) members[member_count++] = "libchrometts_x86_64.so";
    for (size_t i = 0; i < lang_count; ++i) {
        size_t k = 0;
        while (k < PACK_COUNT && strcmp(PACKS[k].lang, langs[i])) ++k;
        pack_names[i] = format_string("%s-x-multi.zvoice", PACKS[k].lang);
        char *dest = format_string("%s\\%s", voices, pack_names[i]);
        if (!verified(dest, PACKS[k].sha256)) { members[member_count++] = pack_names[i]; need_package = 1; }
        free(dest);
    }
    members[member_count] = NULL;
    if (need_package) {
        char *tarball = format_string("%s\\googletts-35.3.tar.xz", tmp);
        download(PACKAGE_URL, tarball, PACKAGE_SHA256);
        fprintf(stderr, "Extracting the engine and voices\n");
        extract(tarball, stage, members);
        for (size_t i = 0; members[i]; ++i) {
            char *from = format_string("%s\\%s", stage, members[i]);
            if (!strcmp(members[i], "libchrometts_x86_64.so")) install_verified(from, engine, ENGINE_SHA256);
            else {
                size_t k = 0;
                while (k < PACK_COUNT && strncmp(PACKS[k].lang, members[i], 5)) ++k;
                char *dest = format_string("%s\\%s", voices, members[i]);
                install_verified(from, dest, PACKS[k].sha256);
                free(dest);
            }
            free(from);
        }
        delete_file(tarball);
        free(tarball);
    }
    for (size_t i = 0; i < sizeof RUNTIME / sizeof RUNTIME[0]; ++i) {
        char *dest = format_string("%s\\%s", runtime, RUNTIME[i].name);
        if (verified(dest, RUNTIME[i].lib_sha256)) { free(dest); continue; }
        char *deb = format_string("%s\\%s.deb", tmp, RUNTIME[i].name), *url = format_string("%s%s", UBUNTU_MIRROR, RUNTIME[i].deb);
        char *inner = format_string("%s\\data.tar.zst", stage);
        download(url, deb, RUNTIME[i].sha256);
        const char *outer[] = {"data.tar.zst", NULL};
        extract(deb, stage, outer);
        const char *files[] = {RUNTIME[i].member, RUNTIME[i].copyright, NULL};
        extract(inner, stage, files);
        char *lib = format_string("%s\\%s", stage, RUNTIME[i].member + 2), *notice = format_string("%s\\%s", stage, RUNTIME[i].copyright + 2);
        char *notice_dest = format_string("%s.copyright", dest);
        install_verified(lib, dest, RUNTIME[i].lib_sha256);
        move_file(notice, notice_dest);
        delete_file(deb); delete_file(inner);
        free(deb); free(url); free(inner); free(lib); free(notice); free(notice_dest); free(dest);
    }
    delete_tree(stage);
    JsonOut list = {0};
    for (size_t i = 0; i < lang_count; ++i) { if (i) json_out_raw(&list, ", "); json_out_raw(&list, langs[i]); }
    fprintf(stderr, "Ready: %s and %s voices\n", engine, list.data);
    free(list.data); free(engine); free(tmp); free(voices); free(runtime); free(stage);
}

// ---- workers -------------------------------------------------------------------------
//
// The engine holds one pack per process, so each pack gets its own worker
// process. Workers start on first use and exit after sitting idle.

typedef struct {
    char *pack, *path;
    HANDLE process, in, out;
    CRITICAL_SECTION lock;
    ULONGLONG used;
    size_t speaker_count;
    char **speakers, **genders;
    uint8_t buffer[65536];
    size_t buffered, offset;
} Worker;

static const char *engine_lib;
static int verbose;

static int read_exact(Worker *w, void *data, size_t n) {
    uint8_t *p = data;
    while (n) {
        if (w->offset == w->buffered) {
            DWORD got = 0;
            if (!ReadFile(w->out, w->buffer, sizeof w->buffer, &got, NULL) || !got) return -1;
            w->buffered = got; w->offset = 0;
        }
        size_t k = w->buffered - w->offset < n ? w->buffered - w->offset : n;
        memcpy(p, w->buffer + w->offset, k);
        w->offset += k; p += k; n -= k;
    }
    return 0;
}

static Json *read_header(Worker *w) {
    char line[65536]; size_t n = 0;
    for (;;) {
        char c;
        if (read_exact(w, &c, 1)) return NULL;
        if (c == '\n') break;
        if (n + 1 >= sizeof line) return NULL;
        line[n++] = c;
    }
    return json_parse(line, n, NULL, 0);
}

static int write_all(HANDLE h, const void *data, size_t n) {
    const uint8_t *p = data;
    while (n) {
        DWORD put = 0;
        if (!WriteFile(h, p, (DWORD)(n > (1u << 30) ? 1u << 30 : n), &put, NULL) || !put) return -1;
        p += put; n -= put;
    }
    return 0;
}

static void worker_stop(Worker *w) {
    if (w->in) CloseHandle(w->in);
    if (w->out) CloseHandle(w->out);
    if (w->process) {
        if (WaitForSingleObject(w->process, 2000) != WAIT_OBJECT_0) TerminateProcess(w->process, 1);
        CloseHandle(w->process);
    }
    w->in = w->out = w->process = NULL;
}

static int worker_start(Worker *w, char *error, size_t error_size) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE child_in, child_out, err = (HANDLE)_get_osfhandle(_fileno(stderr));
    if (!CreatePipe(&child_in, &w->in, &sa, 0) || !CreatePipe(&w->out, &child_out, &sa, 0)) {
        snprintf(error, error_size, "Cannot create worker pipes"); return -1;
    }
    SetHandleInformation(w->in, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(w->out, HANDLE_FLAG_INHERIT, 0);
    char *self = exe_sibling("gtts.exe");
    JsonOut line = {0};
    append_argument(&line, self); append_argument(&line, "worker");
    append_argument(&line, "--data-dir"); append_argument(&line, data_dir);
    append_argument(&line, "--pack"); append_argument(&line, w->path);
    if (engine_lib) { append_argument(&line, "--lib"); append_argument(&line, engine_lib); }
    if (verbose) append_argument(&line, "--verbose");
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = child_in; si.hStdOutput = child_out;
    si.hStdError = err != INVALID_HANDLE_VALUE && err != (HANDLE)-2 ? err : NULL;
    HANDLE null_err = INVALID_HANDLE_VALUE;
    if (!si.hStdError) {
        null_err = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
        si.hStdError = null_err;
    } else SetHandleInformation(err, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    PROCESS_INFORMATION pi;
    wchar_t *cmd = wide(line.data);
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    free(cmd); free(line.data); free(self);
    CloseHandle(child_in); CloseHandle(child_out);
    if (null_err != INVALID_HANDLE_VALUE) CloseHandle(null_err);
    if (!ok) { worker_stop(w); snprintf(error, error_size, "Cannot start the engine worker"); return -1; }
    // Workers die with the process that started them, so stopping the server leaves none behind.
    static HANDLE job;
    if (!job) {
        HANDLE created = CreateJobObjectW(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (created) SetInformationJobObject(created, JobObjectExtendedLimitInformation, &limits, sizeof limits);
        if (created && InterlockedCompareExchangePointer(&job, created, NULL)) CloseHandle(created);
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    CloseHandle(pi.hThread);
    w->process = pi.hProcess;
    w->buffered = w->offset = 0;
    Json *hello = read_header(w);
    const char *message = json_string(hello, "error");
    Json *speakers = json_get(hello, "speakers"), *order = json_get(hello, "speaker_order");
    if (!hello || message || !speakers || speakers->type != JSON_OBJECT || !order || order->type != JSON_ARRAY) {
        snprintf(error, error_size, "%s", message ? message : "Could not load the voice pack");
        json_free(hello); worker_stop(w); return -1;
    }
    if (!w->speakers) {
        w->speakers = calloc(order->length + 1, sizeof *w->speakers);
        w->genders = calloc(order->length + 1, sizeof *w->genders);
        for (size_t i = 0; i < order->length; ++i) if (order->items[i]->type == JSON_STRING) {
            const char *gender = json_string(speakers, order->items[i]->string);
            w->speakers[w->speaker_count] = _strdup(order->items[i]->string);
            w->genders[w->speaker_count++] = _strdup(gender ? gender : "");
        }
    }
    json_free(hello);
    w->used = GetTickCount64();
    return 0;
}

typedef int (*AudioSink)(void *context, const float *samples, size_t count);

// Sends one job; audio arrives in frames until a closing header.
static int worker_synth(Worker *w, const char *text, const char *speaker, const char *gender, double rate, double pitch,
                        AudioSink sink, void *context, size_t *samples, char *error, size_t error_size) {
    EnterCriticalSection(&w->lock);
    int rc = -1;
    *samples = 0;
    if (!w->process || WaitForSingleObject(w->process, 0) == WAIT_OBJECT_0) {
        worker_stop(w);
        if (worker_start(w, error, error_size)) goto done;
    }
    w->used = GetTickCount64();
    JsonOut job = {0};
    char number[96];
    json_out_raw(&job, "{\"text\":"); json_out_string(&job, text);
    json_out_raw(&job, ",\"speaker\":"); json_out_string(&job, speaker);
    json_out_raw(&job, ",\"gender\":"); json_out_string(&job, gender ? gender : "");
    snprintf(number, sizeof number, ",\"rate\":%.17g,\"pitch\":%.17g}\n", rate, pitch);
    json_out_raw(&job, number);
    int sent = write_all(w->in, job.data, job.size);
    free(job.data);
    if (sent) { worker_stop(w); snprintf(error, error_size, "The engine worker stopped unexpectedly"); goto done; }
    float *chunk = NULL; size_t chunk_capacity = 0;
    for (;;) {
        Json *header = read_header(w);
        if (!header) { worker_stop(w); snprintf(error, error_size, "The engine worker stopped unexpectedly"); break; }
        const char *message = json_string(header, "error");
        Json *bytes = json_get(header, "bytes");
        if (message) {
            snprintf(error, error_size, "%s", message);
            Json *broken = json_get(header, "broken");
            if (broken && broken->type == JSON_TRUE) worker_stop(w);
            json_free(header); break;
        }
        if (json_get(header, "done")) { json_free(header); rc = 0; break; }
        if (!bytes || bytes->type != JSON_NUMBER || bytes->number < 0 || bytes->number > 128.0 * 1024 * 1024 || fmod(bytes->number, 4)) {
            json_free(header); worker_stop(w); snprintf(error, error_size, "Invalid reply from the engine worker"); break;
        }
        size_t n = (size_t)bytes->number;
        json_free(header);
        if (n > chunk_capacity) { free(chunk); chunk = malloc(n); chunk_capacity = chunk ? n : 0; }
        if (!chunk || read_exact(w, chunk, n)) { worker_stop(w); snprintf(error, error_size, "The engine worker stopped unexpectedly"); break; }
        *samples += n / 4;
        if (sink(context, chunk, n / 4)) {
            // Drain the rest of this job so the worker stays usable.
            snprintf(error, error_size, "Audio is too long or cannot be written");
            sink = NULL;
        }
        if (!sink) {
            for (;;) {
                Json *h = read_header(w);
                if (!h) { worker_stop(w); break; }
                Json *b = json_get(h, "bytes");
                int end = json_get(h, "done") || json_get(h, "error") || !b || b->type != JSON_NUMBER;
                size_t skip = !end ? (size_t)b->number : 0;
                json_free(h);
                if (end) break;
                if (skip > chunk_capacity) { free(chunk); chunk = malloc(skip); chunk_capacity = chunk ? skip : 0; }
                if (!chunk || read_exact(w, chunk, skip)) { worker_stop(w); break; }
            }
            break;
        }
    }
    free(chunk);
    w->used = GetTickCount64();
done:
    LeaveCriticalSection(&w->lock);
    return rc;
}

// ---- voices -------------------------------------------------------------------------------

typedef struct { char *id, *name, *language, *gender; size_t pack; } Voice;
typedef struct {
    size_t pack_count, voice_count;
    char **pack_names;
    Worker *workers;
    Voice *voices;
    SRWLOCK lock;
} Voices;

static int compare_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static char *pack_language(const char *pack) {
    char lang[16] = {0};
    const char *dash = strchr(pack, '-');
    if (!dash || dash - pack > 3) return _strdup(pack);
    size_t n = (size_t)(dash - pack);
    memcpy(lang, pack, n);
    lang[n] = '-';
    size_t k = 0;
    for (const char *p = dash + 1; *p && *p != '-' && k < 8; ++p) lang[n + 1 + k++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
    return _strdup(lang);
}

static void voices_load(Voices *v, const char *single_pack) {
    memset(v, 0, sizeof *v);
    InitializeSRWLock(&v->lock);
    if (single_pack) {
        char *path = absolute(single_pack), *base = strrchr(path, '\\');
        char *name = _strdup(base ? base + 1 : path), *dot = strrchr(name, '.');
        if (dot) *dot = 0;
        v->pack_names = malloc(sizeof *v->pack_names);
        v->workers = calloc(1, sizeof *v->workers);
        v->pack_names[0] = name; v->workers[0].pack = name; v->workers[0].path = path;
        InitializeCriticalSection(&v->workers[0].lock);
        v->pack_count = 1;
    } else {
        char *pattern = format_string("%s\\voices\\*.zvoice", data_dir);
        wchar_t *w = wide(pattern);
        WIN32_FIND_DATAW data;
        HANDLE find = FindFirstFileW(w, &data);
        free(w); free(pattern);
        size_t capacity = 0;
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (v->pack_count == capacity) {
                    capacity = capacity ? capacity * 2 : 8;
                    v->pack_names = realloc(v->pack_names, capacity * sizeof *v->pack_names);
                    if (!v->pack_names) die("Out of memory");
                }
                char *name = wide_to_utf8(data.cFileName);
                name[strlen(name) - 7] = 0;
                v->pack_names[v->pack_count++] = name;
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
        if (!v->pack_count) die("No voice packs installed. Run gtts fetch.");
        qsort(v->pack_names, v->pack_count, sizeof *v->pack_names, compare_names);
        v->workers = calloc(v->pack_count, sizeof *v->workers);
        for (size_t i = 0; i < v->pack_count; ++i) {
            v->workers[i].pack = v->pack_names[i];
            v->workers[i].path = format_string("%s\\voices\\%s.zvoice", data_dir, v->pack_names[i]);
            InitializeCriticalSection(&v->workers[i].lock);
        }
    }
    // Each pack's speakers are cached, keyed by its size and modification time.
    char *cache_dir = format_string("%s\\speaker-cache", data_dir);
    ensure_dir(cache_dir);
    size_t capacity = 0;
    for (size_t i = 0; i < v->pack_count; ++i) {
        Worker *w = &v->workers[i];
        WIN32_FILE_ATTRIBUTE_DATA info;
        wchar_t *wp = wide(w->path);
        if (!GetFileAttributesExW(wp, GetFileExInfoStandard, &info)) die("Cannot read %s", w->path);
        free(wp);
        char *cache = format_string("%s\\%s-%lu%08lx-%lu%08lx.json", cache_dir, w->pack, info.nFileSizeHigh, info.nFileSizeLow,
                                    info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime);
        FILE *f = _wfopen(wp = wide(cache), L"rb");
        free(wp);
        if (f) {
            char text[16384]; size_t n = fread(text, 1, sizeof text, f);
            fclose(f);
            Json *saved = json_parse(text, n, NULL, 0);
            Json *names = json_get(saved, "speaker_order"), *genders = json_get(saved, "speakers");
            if (names && names->type == JSON_ARRAY && genders && genders->type == JSON_OBJECT) {
                w->speakers = calloc(names->length + 1, sizeof *w->speakers);
                w->genders = calloc(names->length + 1, sizeof *w->genders);
                for (size_t k = 0; k < names->length; ++k) if (names->items[k]->type == JSON_STRING) {
                    const char *g = json_string(genders, names->items[k]->string);
                    w->speakers[w->speaker_count] = _strdup(names->items[k]->string);
                    w->genders[w->speaker_count++] = _strdup(g ? g : "");
                }
            }
            json_free(saved);
        }
        if (!w->speaker_count) {
            char error[512];
            if (worker_start(w, error, sizeof error)) die("%s", error);
            JsonOut out = {0};
            json_out_raw(&out, "{\"speakers\":{");
            for (size_t k = 0; k < w->speaker_count; ++k) {
                if (k) json_out_raw(&out, ",");
                json_out_string(&out, w->speakers[k]); json_out_raw(&out, ":"); json_out_string(&out, w->genders[k]);
            }
            json_out_raw(&out, "},\"speaker_order\":[");
            for (size_t k = 0; k < w->speaker_count; ++k) { if (k) json_out_raw(&out, ","); json_out_string(&out, w->speakers[k]); }
            json_out_raw(&out, "]}");
            f = _wfopen(wp = wide(cache), L"wb");
            free(wp);
            if (f) { fwrite(out.data, 1, out.size, f); fclose(f); }
            free(out.data);
        }
        free(cache);
        char *language = pack_language(w->pack);
        for (size_t k = 0; k < w->speaker_count; ++k) {
            if (v->voice_count == capacity) {
                capacity = capacity ? capacity * 2 : 64;
                v->voices = realloc(v->voices, capacity * sizeof *v->voices);
                if (!v->voices) die("Out of memory");
            }
            Voice *voice = &v->voices[v->voice_count++];
            voice->id = format_string("%s/%s", w->pack, w->speakers[k]);
            voice->name = w->speakers[k]; voice->language = language; voice->gender = w->genders[k]; voice->pack = i;
        }
    }
    free(cache_dir);
}

static Worker *find_pack(Voices *v, const char *pack) {
    for (size_t i = 0; i < v->pack_count; ++i) if (!strcmp(v->pack_names[i], pack)) return &v->workers[i];
    return NULL;
}

typedef struct { const char *voice, *speaker, *gender; double rate, pitch; } VoiceOptions;

// Resolves a request to a worker and speaker, the same way on every path.
static Worker *resolve(Voices *v, const char *voice, const char *speaker, const char **gender, char *error, size_t error_size) {
    char pack[256];
    if (voice) {
        const char *slash = strchr(voice, '/');
        if (!slash) { snprintf(error, error_size, "voice must have the form pack/speaker"); return NULL; }
        snprintf(pack, sizeof pack, "%.*s", (int)(slash - voice), voice);
        speaker = slash + 1;
    } else snprintf(pack, sizeof pack, "%s", find_pack(v, "en-us-x-multi") ? "en-us-x-multi" : v->pack_names[0]);
    if (!speaker) speaker = "sfg";
    Worker *w = find_pack(v, pack);
    if (!w) { snprintf(error, error_size, "No pack '%s'", pack); return NULL; }
    const char *known = NULL;
    for (size_t i = 0; i < w->speaker_count; ++i) if (!strcmp(w->speakers[i], speaker)) known = w->genders[i];
    if (w->speaker_count && !known) { snprintf(error, error_size, "No speaker '%s' in pack '%s'", speaker, pack); return NULL; }
    if (!*gender || !**gender) *gender = known;
    return w;
}

static DWORD idle_limit_ms(void) {
    const char *value = getenv("GTTS_WORKER_TIMEOUT_MS");
    if (value) { char *end; unsigned long n = strtoul(value, &end, 10); if (!*end && n >= 100 && n <= 3600000) return (DWORD)n; }
    return 600000;
}

static DWORD WINAPI reaper(void *param) {
    Voices *v = param;
    DWORD limit = idle_limit_ms();
    for (;;) {
        Sleep(limit < 60000 ? limit / 2 + 1 : 30000);
        for (size_t i = 0; i < v->pack_count; ++i) {
            Worker *w = &v->workers[i];
            if (!TryEnterCriticalSection(&w->lock)) continue;
            if (w->process && GetTickCount64() - w->used > limit) worker_stop(w);
            LeaveCriticalSection(&w->lock);
        }
    }
    return 0;
}

// ---- WAV output ---------------------------------------------------------------------------

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static void wav_header(uint8_t *p, size_t samples) {
    memcpy(p, "RIFF", 4); put32(p + 4, (uint32_t)(36 + samples * 2)); memcpy(p + 8, "WAVEfmt ", 8); put32(p + 16, 16);
    put16(p + 20, 1); put16(p + 22, 1); put32(p + 24, 24000); put32(p + 28, 48000); put16(p + 32, 2); put16(p + 34, 16);
    memcpy(p + 36, "data", 4); put32(p + 40, (uint32_t)(samples * 2));
}
static void pcm16(uint8_t *p, const float *src, size_t samples) {
    for (size_t i = 0; i < samples; ++i) {
        double n = (double)src[i] * 32767.0;
        int value = !isfinite(n) ? 0 : n >= 32767 ? 32767 : n <= -32768 ? -32768 : (int)n;
        put16(p + i * 2, (uint16_t)(int16_t)value);
    }
}

typedef struct { uint8_t *data; size_t size, capacity; } Bytes;
static int bytes_put(Bytes *b, const void *data, size_t n) {
    if (b->size + n > b->capacity) {
        size_t cap = b->capacity ? b->capacity * 2 : 1 << 16;
        while (cap < b->size + n) cap *= 2;
        uint8_t *p = realloc(b->data, cap);
        if (!p) return -1;
        b->data = p; b->capacity = cap;
    }
    memcpy(b->data + b->size, data, n); b->size += n;
    return 0;
}
static int memory_sink(void *context, const float *samples, size_t count) {
    Bytes *b = context;
    if (b->size + count * 4 > 128u << 20) return -1;   // one reply holds at most 128 MiB of float audio
    return bytes_put(b, samples, count * 4);
}

typedef struct { HANDLE file; size_t samples; uint8_t *scratch; size_t scratch_size; } WavFile;
static int wav_sink(void *context, const float *samples, size_t count) {
    WavFile *f = context;
    if (f->samples + count > (UINT32_MAX - 36) / 2) return -1;
    if (count * 2 > f->scratch_size) { free(f->scratch); f->scratch = malloc(count * 2); f->scratch_size = f->scratch ? count * 2 : 0; }
    if (!f->scratch) return -1;
    pcm16(f->scratch, samples, count);
    f->samples += count;
    return write_all(f->file, f->scratch, count * 2);
}

static wchar_t *pending_output;
static BOOL WINAPI on_interrupt(DWORD type) {
    (void)type;
    if (pending_output) DeleteFileW(pending_output);
    return FALSE;
}

typedef int (*Producer)(void *context, AudioSink sink, void *sink_context, size_t *samples, char *error, size_t error_size);

// Writes a WAV file next to the destination, then moves it into place.
static size_t write_wav(const char *out, Producer produce, void *context) {
    char *full = absolute(out), *slash = strrchr(full, '\\');
    if (!slash) die("Invalid output path: %s", out);
    *slash = 0;
    char *temp = format_string("%s\\.gtts-output-%lu-%lu.wav", full, GetCurrentProcessId(), GetTickCount());
    *slash = '\\';
    wchar_t *wtemp = wide(temp);
    WavFile f = {CreateFileW(wtemp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL), 0, NULL, 0};
    if (f.file == INVALID_HANDLE_VALUE) die("Cannot write %s", out);
    pending_output = wtemp;
    uint8_t header[44] = {0};
    char error[512] = "";
    size_t samples = 0;
    int rc = write_all(f.file, header, 44) ? -1 : produce(context, wav_sink, &f, &samples, error, sizeof error);
    if (!rc) {
        wav_header(header, f.samples);
        LARGE_INTEGER zero = {0};
        if (!SetFilePointerEx(f.file, zero, NULL, FILE_BEGIN) || write_all(f.file, header, 44)) { rc = -1; snprintf(error, sizeof error, "Cannot write %s", out); }
    }
    CloseHandle(f.file);
    free(f.scratch);
    if (rc || move_file(temp, full)) {
        DeleteFileW(wtemp);
        pending_output = NULL;
        die("%s", *error ? error : "Cannot write the output file");
    }
    pending_output = NULL;
    free(wtemp); free(temp); free(full);
    return f.samples;
}

// ---- the running server, used by speak and batch -------------------------------------------

static int connect_local(int port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_port = htons((u_short)port); addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    DWORD timeout = 600000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
    if (connect(s, (struct sockaddr *)&addr, sizeof addr)) { closesocket(s); return -1; }
    return (int)s;
}

static int send_all(SOCKET s, const void *data, size_t n) {
    const char *p = data;
    while (n) { int k = send(s, p, (int)(n > (1u << 30) ? 1u << 30 : n), 0); if (k <= 0) return -1; p += k; n -= (size_t)k; }
    return 0;
}

// Returns the body of a 200 reply carrying this server's token, or NULL.
static Bytes *server_request(int port, const char *token, const char *path, const char *body) {
    int s = connect_local(port);
    if (s < 0) return NULL;
    char *request = format_string("%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                                  body ? "POST" : "GET", path, body ? strlen(body) : 0, body ? body : "");
    int failed = send_all((SOCKET)s, request, strlen(request));
    free(request);
    Bytes *reply = calloc(1, sizeof *reply);
    char chunk[65536];
    int n;
    while (!failed && (n = recv((SOCKET)s, chunk, sizeof chunk, 0)) > 0)
        if (bytes_put(reply, chunk, (size_t)n) || reply->size > (256u << 20)) failed = 1;
    closesocket((SOCKET)s);
    uint8_t *end = NULL;
    for (size_t i = 0; !failed && i + 4 <= reply->size; ++i) if (!memcmp(reply->data + i, "\r\n\r\n", 4)) { end = reply->data + i; break; }
    if (failed || !end || reply->size < 12 || memcmp(reply->data + 9, "200", 3)) { free(reply->data); free(reply); return NULL; }
    *end = 0;
    char *expected = format_string("\r\nX-Gtts-Token: %s\r\n", token);
    int trusted = strstr((char *)reply->data, expected) != NULL;
    free(expected);
    if (!trusted) { free(reply->data); free(reply); return NULL; }
    size_t head = (size_t)(end - reply->data) + 4;
    memmove(reply->data, reply->data + head, reply->size - head);
    reply->size -= head;
    return reply;
}

static char *token_path(int port) { return format_string("%s\\serve-%d.token", data_dir, port); }

static char *running_server(int port) {
    char *path = token_path(port);
    FILE *f = _wfopen(wide(path), L"rb");
    free(path);
    if (!f) return NULL;
    char token[128] = {0};
    size_t n = fread(token, 1, sizeof token - 1, f);
    fclose(f);
    while (n && (token[n - 1] == '\n' || token[n - 1] == '\r' || token[n - 1] == ' ')) token[--n] = 0;
    if (n != 64) return NULL;
    Bytes *health = server_request(port, token, "/health", NULL);
    if (!health) return NULL;
    free(health->data); free(health);
    return _strdup(token);
}

// ---- command line ---------------------------------------------------------------------------

typedef struct { const char *text, *voice, *speaker, *gender; double rate, pitch; Voices *voices; } SpeakJob;

static int produce_local(void *context, AudioSink sink, void *sink_context, size_t *samples, char *error, size_t error_size) {
    SpeakJob *job = context;
    const char *gender = job->gender;
    Worker *w = resolve(job->voices, job->voice, job->speaker, &gender, error, error_size);
    if (!w) return -1;
    return worker_synth(w, job->text, job->voice ? strchr(job->voice, '/') + 1 : job->speaker ? job->speaker : "sfg",
                        gender, job->rate, job->pitch, sink, sink_context, samples, error, error_size);
}

typedef struct { const float *samples; size_t count; } Prepared;
static int produce_prepared(void *context, AudioSink sink, void *sink_context, size_t *samples, char *error, size_t error_size) {
    Prepared *p = context;
    *samples = p->count;
    if (sink(sink_context, p->samples, p->count)) { snprintf(error, error_size, "Cannot write the output file"); return -1; }
    return 0;
}

static void print_result(const char *out, size_t samples) {
    JsonOut line = {0};
    char seconds[64];
    json_out_raw(&line, "{\"out\":"); json_out_string(&line, out);
    snprintf(seconds, sizeof seconds, ",\"seconds\":%.2f}\n", (double)samples / 24000);
    json_out_raw(&line, seconds);
    fputs(line.data, stdout); fflush(stdout);
    free(line.data);
}

static char *read_stdin(void) {
    Bytes b = {0};
    char chunk[16384];
    size_t n;
    _setmode(_fileno(stdin), _O_BINARY);
    while ((n = fread(chunk, 1, sizeof chunk, stdin)) > 0) {
        if (b.size + n > (16u << 20)) die("Text exceeds 16 MiB");
        bytes_put(&b, chunk, n);
    }
    bytes_put(&b, "", 1);
    return (char *)b.data;
}

static Voices *local_voices;
static const char *single_pack;
static Voices *get_voices(void) {
    if (!local_voices) { local_voices = calloc(1, sizeof *local_voices); voices_load(local_voices, single_pack); }
    return local_voices;
}

static size_t speak_to(const char *text, const char *out, const VoiceOptions *o, int port, const char *token) {
    if (token) {
        JsonOut body = {0};
        char number[96];
        json_out_raw(&body, "{\"format\":\"f32\",\"text\":"); json_out_string(&body, text);
        if (o->voice) { json_out_raw(&body, ",\"voice\":"); json_out_string(&body, o->voice); }
        if (o->speaker) { json_out_raw(&body, ",\"speaker\":"); json_out_string(&body, o->speaker); }
        snprintf(number, sizeof number, ",\"rate\":%.17g,\"pitch\":%.17g}", o->rate, o->pitch);
        json_out_raw(&body, number);
        Bytes *audio = strlen(text) <= 65536 ? server_request(port, token, "/speak", body.data) : NULL;
        free(body.data);
        if (audio && audio->size % 4 == 0) {
            Prepared p = {(const float *)audio->data, audio->size / 4};
            size_t samples = write_wav(out, produce_prepared, &p);
            free(audio->data); free(audio);
            return samples;
        }
        if (audio) { free(audio->data); free(audio); }
    }
    SpeakJob job = {text, o->voice, o->speaker, o->gender, o->rate, o->pitch, get_voices()};
    return write_wav(out, produce_local, &job);
}

// ---- worker side ------------------------------------------------------------------------------

typedef struct { const char *pack; } WorkerArgs;

static int frame_sink(void *context, const float *samples, size_t count) {
    (void)context;
    char header[64];
    int n = snprintf(header, sizeof header, "{\"bytes\":%zu}\n", count * 4);
    return fwrite(header, 1, (size_t)n, stdout) != (size_t)n || fwrite(samples, 4, count, stdout) != count || fflush(stdout);
}

static void reply_error(const char *message, int broken) {
    JsonOut out = {0};
    json_out_raw(&out, "{\"error\":"); json_out_string(&out, message);
    json_out_raw(&out, broken ? ",\"broken\":true}\n" : "}\n");
    fputs(out.data, stdout); fflush(stdout);
    free(out.data);
}

static DWORD WINAPI worker_thread(void *param) {
    WorkerArgs *args = param;
    if (speech_open(data_dir, args->pack, engine_lib)) { reply_error(speech_error(), 1); return 1; }
    JsonOut hello = {0};
    json_out_raw(&hello, "{\"speakers\":{");
    for (unsigned i = 0; i < engine_speaker_count; ++i) {
        if (i) json_out_raw(&hello, ",");
        json_out_string(&hello, engine_speakers[i].name); json_out_raw(&hello, ":"); json_out_string(&hello, engine_speakers[i].gender);
    }
    json_out_raw(&hello, "},\"speaker_order\":[");
    for (unsigned i = 0; i < engine_speaker_count; ++i) { if (i) json_out_raw(&hello, ","); json_out_string(&hello, engine_speakers[i].name); }
    json_out_raw(&hello, "]}\n");
    fputs(hello.data, stdout); fflush(stdout);
    free(hello.data);
    Bytes line = {0};
    int c;
    while ((c = fgetc(stdin)) != EOF) {
        if (c != '\n') { if (bytes_put(&line, &(char){(char)c}, 1)) return 1; continue; }
        char error[256];
        Json *job = json_parse((const char *)line.data, line.size, error, sizeof error);
        line.size = 0;
        const char *text = json_string(job, "text"), *speaker = json_string(job, "speaker"), *gender = json_string(job, "gender");
        Json *rate = json_get(job, "rate"), *pitch = json_get(job, "pitch");
        if (!job || !text || !speaker) { reply_error(job ? "text and speaker must be strings without NUL characters" : error, 0); json_free(job); continue; }
        double r = rate && rate->type == JSON_NUMBER ? rate->number : 1, p = pitch && pitch->type == JSON_NUMBER ? pitch->number : 1;
        size_t count = 0;
        const char *known = speech_gender(speaker);
        if (!isfinite(r) || r <= 0 || !isfinite(p) || p <= 0) reply_error("rate and pitch must be positive numbers", 0);
        else if (engine_speaker_count && !known) { snprintf(error, sizeof error, "No speaker '%s' in this pack", speaker); reply_error(error, 0); }
        else if (speech_synth(text, speaker, gender, r, p, frame_sink, NULL, &count)) { reply_error(speech_error(), 1); json_free(job); return 1; }
        else { printf("{\"done\":true,\"samples\":%zu}\n", count); fflush(stdout); }
        json_free(job);
    }
    free(line.data);
    return 0;
}

static int run_worker(const char *pack) {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
    if (!pack) usage_error("worker requires --pack");
    WorkerArgs args = {pack};
    // Linux code expects a large stack and does not probe it page by page, so
    // the engine runs on a thread whose whole stack is committed.
    HANDLE thread = CreateThread(NULL, 16u << 20, worker_thread, &args, 0, NULL);
    if (!thread) die("Cannot start the engine thread");
    WaitForSingleObject(thread, INFINITE);
    DWORD code = 1; GetExitCodeThread(thread, &code);
    fflush(stdout);
    ExitProcess(code);
}

// ---- HTTP server --------------------------------------------------------------------------------

static Voices *server_voices;
static char server_token[65];
static VoiceOptions server_defaults;
static HANDLE client_slots;

static void reply(SOCKET s, int code, const char *type, const void *body, size_t length) {
    const char *reason = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 404 ? "Not Found" : code == 501 ? "Not Implemented" : "Internal Server Error";
    char *head = format_string("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nX-Gtts-Version: %s\r\nX-Gtts-Token: %s\r\nConnection: close\r\n\r\n",
                               code, reason, type, length, GTTS_VERSION, server_token);
    if (!send_all(s, head, strlen(head)) && length) send_all(s, body, length);
    free(head);
}

static void reply_json_error(SOCKET s, const char *message) {
    JsonOut out = {0};
    json_out_raw(&out, "{\"error\":"); json_out_string(&out, message); json_out_raw(&out, "}");
    reply(s, 400, "application/json", out.data, out.size);
    free(out.data);
}

static int parse_number(Json *job, const char *key, double fallback, double *out) {
    Json *v = json_get(job, key);
    if (!v || v->type == JSON_NULL) { *out = fallback; return 0; }
    if (v->type == JSON_NUMBER) *out = v->number;
    else if (v->type == JSON_TRUE || v->type == JSON_FALSE) *out = v->type == JSON_TRUE;
    else if (v->type == JSON_STRING) { char *end; *out = strtod(v->string, &end); if (*end || end == v->string) return -1; }
    else return -1;
    return isfinite(*out) && *out > 0 ? 0 : -1;
}

static void handle_speak(SOCKET s, const char *body, size_t length) {
    char error[512];
    Json *job = json_parse(body, length, error, sizeof error);
    if (!job) { reply_json_error(s, error); return; }
    if (job->type != JSON_OBJECT) { json_free(job); reply_json_error(s, "Expected a JSON object"); return; }
    const char *text = json_string(job, "text");
    Json *voice = json_get(job, "voice"), *speaker = json_get(job, "speaker"), *format = json_get(job, "format");
    double rate, pitch;
    const char *gender = NULL;
    Worker *w = NULL;
    const char *message = NULL;
    if (!text) message = "text must be a string";
    else if (voice && voice->type != JSON_NULL && (voice->type != JSON_STRING || strlen(voice->string) != voice->length)) message = "voice must be a string";
    else if (speaker && speaker->type != JSON_NULL && (speaker->type != JSON_STRING || strlen(speaker->string) != speaker->length)) message = "speaker must be a string";
    else if (parse_number(job, "rate", server_defaults.rate, &rate) || parse_number(job, "pitch", server_defaults.pitch, &pitch))
        message = "rate and pitch must be positive numbers";
    if (message) { json_free(job); reply_json_error(s, message); return; }
    const char *voice_id = voice && voice->type == JSON_STRING ? voice->string : NULL;
    const char *speaker_name = speaker && speaker->type == JSON_STRING ? speaker->string : NULL;
    w = resolve(server_voices, voice_id, speaker_name, &gender, error, sizeof error);
    if (!w) { json_free(job); reply_json_error(s, error); return; }
    Bytes audio = {0};
    size_t samples = 0;
    const char *name = voice_id ? strchr(voice_id, '/') + 1 : speaker_name ? speaker_name : "sfg";
    if (worker_synth(w, text, name, gender, rate, pitch, memory_sink, &audio, &samples, error, sizeof error)) {
        free(audio.data); json_free(job); reply_json_error(s, error); return;
    }
    if (format && format->type == JSON_STRING && !strcmp(format->string, "f32")) reply(s, 200, "audio/L32; rate=24000", audio.data, audio.size);
    else {
        size_t count = audio.size / 4;
        uint8_t *wav = malloc(44 + count * 2);
        if (!wav) reply_json_error(s, "Out of memory");
        else { wav_header(wav, count); pcm16(wav + 44, (const float *)audio.data, count); reply(s, 200, "audio/wav", wav, 44 + count * 2); }
        free(wav);
    }
    free(audio.data);
    json_free(job);
}

static char *catalog_json(Voices *v) {
    JsonOut out = {0};
    json_out_raw(&out, "[");
    for (size_t i = 0; i < v->voice_count; ++i) {
        if (i) json_out_raw(&out, ",");
        json_out_raw(&out, "{\"id\":"); json_out_string(&out, v->voices[i].id);
        json_out_raw(&out, ",\"name\":"); json_out_string(&out, v->voices[i].name);
        json_out_raw(&out, ",\"language\":"); json_out_string(&out, v->voices[i].language);
        json_out_raw(&out, ",\"gender\":"); json_out_string(&out, v->voices[i].gender);
        json_out_raw(&out, "}");
    }
    json_out_raw(&out, "]");
    return out.data;
}
static char *server_catalog;

static DWORD WINAPI client_thread(void *param) {
    SOCKET s = (SOCKET)(uintptr_t)param;
    DWORD timeout = 10000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof timeout);
    Bytes request = {0};
    char chunk[8192];
    size_t head = 0;
    ULONGLONG deadline = GetTickCount64() + 10000;
    const char *problem = NULL;
    while (!head) {
        if (GetTickCount64() > deadline) { problem = "HTTP request timed out"; break; }
        int n = recv(s, chunk, sizeof chunk, 0);
        if (n <= 0) { problem = "Incomplete HTTP headers"; break; }
        bytes_put(&request, chunk, (size_t)n);
        for (size_t i = request.size >= (size_t)n + 3 ? request.size - (size_t)n - 3 : 0; i + 4 <= request.size; ++i)
            if (!memcmp(request.data + i, "\r\n\r\n", 4)) { head = i + 4; break; }
        if (!head && request.size > 65536) { problem = "Request headers are too large"; break; }
    }
    size_t content_length = 0;
    char method[16] = "", path[1024] = "";
    if (!problem) {
        request.data[head - 2] = 0;
        char *text = (char *)request.data;
        if (sscanf(text, "%15s %1023s", method, path) != 2) problem = "Invalid HTTP request";
        for (char *line = strstr(text, "\r\n"); !problem && line && line[2]; line = strstr(line + 2, "\r\n")) {
            char *name = line + 2;
            if (!_strnicmp(name, "transfer-encoding:", 18)) problem = "Transfer encoding is not supported";
            else if (!_strnicmp(name, "content-length:", 15)) {
                char *end; unsigned long long n = strtoull(name + 15, &end, 10);
                while (*end == ' ' || *end == '\t') ++end;
                if (end == name + 15 || (*end != '\r' && *end) || n > 1024 * 1024) problem = "Invalid Content-Length (maximum 1 MiB)";
                content_length = (size_t)n;
            }
        }
    }
    while (!problem && request.size - head < content_length) {
        if (GetTickCount64() > deadline) { problem = "HTTP request timed out"; break; }
        int n = recv(s, chunk, sizeof chunk, 0);
        if (n <= 0) { problem = "Incomplete HTTP body"; break; }
        bytes_put(&request, chunk, (size_t)n);
    }
    if (problem) reply_json_error(s, problem);
    else if (!strcmp(method, "GET") && !strcmp(path, "/health")) reply(s, 200, "application/json", "{\"ok\": true}", 12);
    else if (!strcmp(method, "GET") && !strcmp(path, "/voices")) reply(s, 200, "application/json", server_catalog, strlen(server_catalog));
    else if (!strcmp(method, "POST") && !strcmp(path, "/speak")) handle_speak(s, (const char *)request.data + head, content_length);
    else if (!strcmp(method, "GET") || !strcmp(method, "POST")) reply(s, 404, "text/plain", "", 0);
    else reply(s, 501, "text/plain", "", 0);
    shutdown(s, SD_SEND);
    closesocket(s);
    free(request.data);
    ReleaseSemaphore(client_slots, 1, NULL);
    return 0;
}

static void serve(const char *host, int port) {
    server_voices = get_voices();
    server_catalog = catalog_json(server_voices);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) usage_error("--host must be an IPv4 address");
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL exclusive = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof exclusive);
    if (listener == INVALID_SOCKET || bind(listener, (struct sockaddr *)&addr, sizeof addr) || listen(listener, 16))
        die("Cannot listen on %s:%d (error %d)", host, port, WSAGetLastError());
    SetHandleInformation((HANDLE)listener, HANDLE_FLAG_INHERIT, 0);
    // speak and batch talk to this server only when its reply carries the
    // token written here, so they never mistake another program for it.
    uint8_t random[32];
    BCryptGenRandom(NULL, random, sizeof random, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    for (int i = 0; i < 32; ++i) sprintf(server_token + i * 2, "%02x", random[i]);
    if (!strcmp(host, "127.0.0.1") && !engine_lib && !single_pack && !server_defaults.gender) {
        char *path = token_path(port);
        FILE *f = _wfopen(wide(path), L"wb");
        if (f) { fputs(server_token, f); fclose(f); }
        free(path);
    }
    client_slots = CreateSemaphoreW(NULL, 16, 16, NULL);
    CreateThread(NULL, 0, reaper, server_voices, 0, NULL);
    fprintf(stderr, "%zu voices from %zu packs. Listening on %s:%d\n", server_voices->voice_count, server_voices->pack_count, host, port);
    fflush(stderr);
    for (;;) {
        WaitForSingleObject(client_slots, INFINITE);
        SOCKET client = accept(listener, NULL, NULL);
        if (client == INVALID_SOCKET) { ReleaseSemaphore(client_slots, 1, NULL); Sleep(10); continue; }
        SetHandleInformation((HANDLE)client, HANDLE_FLAG_INHERIT, 0);
        HANDLE t = CreateThread(NULL, 256 * 1024, client_thread, (void *)(uintptr_t)client, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (t) CloseHandle(t); else { closesocket(client); ReleaseSemaphore(client_slots, 1, NULL); }
    }
}

// ---- start at login --------------------------------------------------------------------------------

static const wchar_t *const RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t *const RUN_VALUE = L"google-tts-local";

static int service(const char *action, const char *host, int port) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, NULL, &key, NULL)) die("Cannot open the Run registry key");
    if (!strcmp(action, "install")) {
        char *quiet = exe_sibling("gttsw.exe");
        if (!file_exists(quiet)) die("%s is missing; reinstall gtts.", quiet);
        if (!file_exists(format_string("%s\\libchrometts.so", data_dir))) die("Run gtts fetch first.");
        JsonOut line = {0};
        char number[16];
        snprintf(number, sizeof number, "%d", port);
        append_argument(&line, quiet); append_argument(&line, "serve");
        append_argument(&line, "--data-dir"); append_argument(&line, data_dir);
        append_argument(&line, "--host"); append_argument(&line, host);
        append_argument(&line, "--port"); append_argument(&line, number);
        wchar_t *value = wide(line.data);
        if (RegSetValueExW(key, RUN_VALUE, 0, REG_SZ, (const BYTE *)value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)))) die("Cannot write the Run registry value");
        STARTUPINFOW si = {sizeof si}; PROCESS_INFORMATION pi;
        if (CreateProcessW(NULL, value, NULL, NULL, FALSE, DETACHED_PROCESS, NULL, NULL, &si, &pi)) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
        fprintf(stderr, "gtts serve now starts when you sign in, and is running on %s:%d.\nLog: %s\\serve.log\n", host, port, data_dir);
        free(value); free(line.data); free(quiet);
    } else if (!strcmp(action, "uninstall")) {
        RegDeleteValueW(key, RUN_VALUE);
        // Stop the background server started for this user.
        char *quiet = exe_sibling("gttsw.exe");
        wchar_t *wquiet = wide(quiet);
        DWORD pids[4096], bytes;
        if (EnumProcesses(pids, sizeof pids, &bytes)) for (DWORD i = 0; i < bytes / sizeof(DWORD); ++i) {
            HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pids[i]);
            if (!p) continue;
            wchar_t image[32768]; DWORD size = 32768;
            if (QueryFullProcessImageNameW(p, 0, image, &size) && (!_wcsicmp(image, wquiet) || same_file(image, wquiet))) TerminateProcess(p, 0);
            CloseHandle(p);
        }
        fprintf(stderr, "Removed the sign-in entry and stopped the background server. Downloaded data is kept in %s\n", data_dir);
        free(wquiet); free(quiet);
    } else if (!strcmp(action, "status")) {
        wchar_t value[4096]; DWORD size = sizeof value;
        if (RegQueryValueExW(key, RUN_VALUE, NULL, NULL, (BYTE *)value, &size)) printf("Not set to start at sign-in.\n");
        else { char *u = wide_to_utf8(value); printf("Starts at sign-in: %s\n", u); free(u); }
    } else usage_error("service takes install, uninstall or status");
    RegCloseKey(key);
    return 0;
}

// ---- main ---------------------------------------------------------------------------------------------

static void usage(const char *command) {
    if (!command) {
        printf("Usage: gtts [global options] COMMAND [options]\n\n"
               "Commands:\n  fetch    Download and verify the engine, runtimes, and voices\n"
               "  voices   List installed voices\n  speak    Synthesize text to a WAV file\n"
               "  batch    Synthesize one JSON job per input line\n  serve    Run the HTTP service\n"
               "  service  Start the HTTP service when you sign in (install, uninstall, status)\n"
               "  worker   Run the internal JSON/binary worker protocol\n\n"
               "Use gtts COMMAND --help for command-specific options.\n"
               "Global: --data-dir DIR (overrides GTTS_DATA), --version, --help\n"
               "Default data: %%LOCALAPPDATA%%\\google-tts-local\n");
        return;
    }
    if (!strcmp(command, "fetch")) printf("Usage: gtts fetch [--lang LANG ...] [--data-dir DIR]\nDownload and verify the engine, voices, and runtimes.\nLanguages: en-us, es-es, fr-fr, hi-in, ja-jp, nl-nl (default: all).\n");
    else if (!strcmp(command, "voices")) printf("Usage: gtts voices [--data-dir DIR] [--lib FILE] [--pack FILE] [--verbose]\nList voice IDs, languages, and genders. Run gtts fetch first.\n");
    else if (!strcmp(command, "speak")) printf("Usage: gtts speak --out FILE [--text TEXT] [voice options]\nRead UTF-8 text from stdin if --text is omitted and write a WAV file.\nIf gtts serve is running on 127.0.0.1 (port 5591, or --port) with the default data, the text is sent there;\nset GTTS_NO_SERVER=1 to always start a local engine.\n");
    else if (!strcmp(command, "batch")) printf("Usage: gtts batch [voice options]\nRead one JSON object per line with text and out; voice, speaker, rate, and pitch are optional.\nIf gtts serve is running on 127.0.0.1 (port 5591, or --port) with the default data, the text is sent there;\nset GTTS_NO_SERVER=1 to always start a local engine.\n");
    else if (!strcmp(command, "serve")) printf("Usage: gtts serve [--host IPv4] [--port PORT] [voice options]\nServe GET /health, GET /voices, and POST /speak (default 127.0.0.1:5591).\nPOST accepts text, voice, speaker, rate, pitch, and format f32 for raw float audio.\nHTTP voice and speaker come from the request, not CLI defaults.\n");
    else if (!strcmp(command, "service")) printf("Usage: gtts service install|uninstall|status [--host IPv4] [--port PORT] [--data-dir DIR]\nRun gtts serve in the background whenever you sign in to Windows.\n");
    else if (!strcmp(command, "worker")) printf("Usage: gtts worker --pack FILE [--lib FILE] [--data-dir DIR] [--verbose]\nInternal protocol: JSON lines in; JSON headers and float32 audio out.\n");
    if (!strcmp(command, "speak") || !strcmp(command, "batch") || !strcmp(command, "serve"))
        printf("Voice options: --voice PACK/SPEAKER, --pack FILE, --speaker NAME, --gender GENDER,\n"
               "               --rate N, --pitch N, --lib FILE, --data-dir DIR, --verbose\n");
    printf("--data-dir overrides GTTS_DATA; default is %%LOCALAPPDATA%%\\google-tts-local.\n");
}

static char **utf8_arguments(int *argc) {
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), argc);
    char **argv = calloc((size_t)*argc + 1, sizeof *argv);
    for (int i = 0; i < *argc; ++i) argv[i] = wide_to_utf8(wargv[i]);
    LocalFree(wargv);
    return argv;
}

static double positive(const char *name, const char *value) {
    char *end;
    double n = strtod(value, &end);
    if (end == value || *end || !isfinite(n) || n <= 0) usage_error("Invalid %s: %s", name, value);
    return n;
}

int gtts_main(void) {
    int argc;
    char **argv = utf8_arguments(&argc);
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) { usage(NULL); return 2; }
    if (argc == 2 && !strcmp(argv[1], "--version")) { printf("gtts %s\n", GTTS_VERSION); return 0; }
    static const char *commands[] = {"fetch", "voices", "speak", "batch", "serve", "worker", "service", NULL};
    const char *command = NULL, *data = NULL, *text = NULL, *out = NULL, *host = "127.0.0.1", *action = NULL;
    const char *langs[16]; size_t lang_count = 0;
    VoiceOptions o = {NULL, NULL, NULL, 1.0, 1.0};
    int port = 5591, help = 0;
    for (int i = 1; i < argc; ++i) {
        char *arg = argv[i], *value = NULL;
        if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) { help = 1; continue; }
        int is_command = 0;
        for (size_t k = 0; commands[k]; ++k) if (!command && !strcmp(arg, commands[k])) { command = commands[k]; is_command = 1; }
        if (is_command) continue;
        if (command && !strcmp(command, "service") && !action && arg[0] != '-') { action = arg; continue; }
        if (!strcmp(arg, "--verbose")) { verbose = 1; continue; }
        char *equals = strncmp(arg, "--", 2) ? NULL : strchr(arg, '=');
        if (equals) { *equals = 0; value = equals + 1; }
        else { if (i + 1 >= argc) usage_error("Missing value for %s", arg); value = argv[++i]; }
        if (!strcmp(arg, "--data-dir")) data = value;
        else if (!strcmp(arg, "--lang")) {
            size_t k = 0;
            while (k < PACK_COUNT && strcmp(PACKS[k].lang, value)) ++k;
            if (k == PACK_COUNT) die("Unsupported language: %s", value);
            int seen = 0;
            for (size_t j = 0; j < lang_count; ++j) if (!strcmp(langs[j], value)) seen = 1;
            if (!seen && lang_count < 16) langs[lang_count++] = value;
        }
        else if (!strcmp(arg, "--lib")) engine_lib = value;
        else if (!strcmp(arg, "--voice")) o.voice = value;
        else if (!strcmp(arg, "--pack")) single_pack = value;
        else if (!strcmp(arg, "--speaker")) o.speaker = value;
        else if (!strcmp(arg, "--gender")) o.gender = value;
        else if (!strcmp(arg, "--text")) text = value;
        else if (!strcmp(arg, "--out")) out = value;
        else if (!strcmp(arg, "--host")) host = value;
        else if (!strcmp(arg, "--rate")) o.rate = positive("--rate", value);
        else if (!strcmp(arg, "--pitch")) o.pitch = positive("--pitch", value);
        else if (!strcmp(arg, "--port")) {
            char *end; long n = strtol(value, &end, 10);
            if (end == value || *end || n < 1 || n > 65535) usage_error("Invalid port");
            port = (int)n;
        } else usage_error("Unknown option: %s", arg);
    }
    if (help) { usage(command); return 0; }
    if (!command) { usage(NULL); return 2; }
    if (!data) data = getenv("GTTS_DATA");
    char *default_data = NULL;
    if (!data || !*data) {
        const char *local = getenv("LOCALAPPDATA");
        if (!local) die("LOCALAPPDATA is not set; use --data-dir.");
        data = default_data = format_string("%s\\google-tts-local", local);
    }
    data_dir = absolute(data);
    if (engine_lib) engine_lib = absolute(engine_lib);
    engine_verbose = verbose;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    if (!strcmp(command, "fetch")) {
        ensure_dir(data_dir);
        if (!lang_count) for (size_t k = 0; k < PACK_COUNT; ++k) langs[lang_count++] = PACKS[k].lang;
        fetch(langs, lang_count);
        return 0;
    }
    if (!strcmp(command, "service")) return service(action ? action : "", host, port);
    char *lib = engine_lib ? _strdup(engine_lib) : format_string("%s\\libchrometts.so", data_dir);
    if (!file_exists(lib)) die("%s is missing. Run gtts fetch.", lib);
    for (size_t i = 0; i < sizeof RUNTIME / sizeof RUNTIME[0]; ++i) {
        char *path = format_string("%s\\runtime\\%s", data_dir, RUNTIME[i].name);
        if (!file_exists(path)) die("Runtime library %s is missing. Run gtts fetch.", path);
        free(path);
    }
    if (!strcmp(command, "worker")) return run_worker(single_pack ? absolute(single_pack) : NULL);
    ensure_dir(data_dir);

    if (!strcmp(command, "voices")) {
        Voices *v = get_voices();
        for (size_t i = 0; i < v->voice_count; ++i) printf("%-24s %-6s %s\n", v->voices[i].id, v->voices[i].language, v->voices[i].gender);
        return 0;
    }
    if (!strcmp(command, "serve")) {
        server_defaults = o;
        serve(host, port);
        return 0;
    }
    SetConsoleCtrlHandler(on_interrupt, TRUE);
    int plain = !engine_lib && !single_pack && !o.gender;
    char *token = plain && !getenv("GTTS_NO_SERVER") ? running_server(port) : NULL;
    if (!strcmp(command, "batch")) {
        _setmode(_fileno(stdin), _O_BINARY);
        Bytes line = {0};
        int c;
        for (;;) {
            c = fgetc(stdin);
            if (c != '\n' && c != EOF) { bytes_put(&line, &(char){(char)c}, 1); continue; }
            size_t start = 0, end = line.size;
            while (start < end && strchr(" \t\r", line.data[start])) ++start;
            while (end > start && strchr(" \t\r", line.data[end - 1])) --end;
            if (end > start) {
                char error[256];
                Json *job = json_parse((const char *)line.data + start, end - start, error, sizeof error);
                if (!job) die("%s", error);
                const char *job_out = json_string(job, "out"), *job_text = json_string(job, "text");
                if (job->type != JSON_OBJECT || !job_out) die("batch requires a string out path");
                if (!job_text) die("text must be a string");
                VoiceOptions jo = o;
                const char *s;
                if ((s = json_string(job, "voice"))) jo.voice = s;
                if ((s = json_string(job, "speaker"))) jo.speaker = s;
                if (parse_number(job, "rate", o.rate, &jo.rate) || parse_number(job, "pitch", o.pitch, &jo.pitch)) die("rate and pitch must be positive numbers");
                size_t samples = speak_to(job_text, job_out, &jo, port, token);
                print_result(job_out, samples);
                json_free(job);
            }
            line.size = 0;
            if (c == EOF) break;
        }
        return 0;
    }
    if (!out) die("speak needs --out");
    char *input = text ? (char *)text : read_stdin();
    size_t samples = speak_to(input, out, &o, port, token);
    print_result(out, samples);
    free(default_data);
    return 0;
}

#ifdef GTTS_WINDOWLESS
// gttsw.exe runs the server without a console; its messages go to serve.log.
int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command_line, int show) {
    (void)instance; (void)previous; (void)command_line; (void)show;
    const char *local = getenv("GTTS_DATA");
    char *log = NULL;
    int argc;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i + 1 < argc; ++i) if (!wcscmp(wargv[i], L"--data-dir")) log = wide_to_utf8(wargv[i + 1]);
    LocalFree(wargv);
    if (!log) log = local ? format_string("%s", local) : format_string("%s\\google-tts-local", getenv("LOCALAPPDATA"));
    ensure_dir(log);
    char *path = format_string("%s\\serve.log", log);
    wchar_t *w = wide(path);
    _wfreopen(w, L"a", stderr);
    _wfreopen(L"NUL", L"w", stdout);
    return gtts_main();
}
#else
int main(void) { return gtts_main(); }
#endif
