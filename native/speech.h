#ifndef GTTS_SPEECH_H
#define GTTS_SPEECH_H
#include "guest.h"

typedef struct {
    Guest *guest;
    void *directory;
    void *pipeline;
    int ready;
    int broken;
    char error[512];
} SpeechEngine;

enum { SPEECH_ERROR = -1, SPEECH_ENGINE_BROKEN = -2 };

int speech_open(SpeechEngine *engine, Guest *guest, const char *pack);
int speech_synth(SpeechEngine *engine, const char *text, const char *speaker,
                 const char *gender, double rate, double pitch,
                 float **audio, size_t *count);
size_t speech_chunk_end(const char *text, size_t offset, size_t length);
void speech_close(SpeechEngine *engine);
int writable_output(const char *path);
#endif
