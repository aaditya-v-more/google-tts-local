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
typedef int (*SpeechSink)(void *context, const float *samples, size_t count);
int speech_synth_stream(SpeechEngine *engine, const char *text, const char *speaker,
                        const char *gender, double rate, double pitch,
                        SpeechSink sink, void *context, size_t *count);
int speech_synth(SpeechEngine *engine, const char *text, const char *speaker,
                 const char *gender, double rate, double pitch,
                 float **audio, size_t *count);
size_t speech_chunk_end(const char *text, size_t offset, size_t length);
void speech_close(SpeechEngine *engine);
int writable_output(const char *path);
#endif
