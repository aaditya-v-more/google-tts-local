#ifndef GTTS_WIN_SPEECH_H
#define GTTS_WIN_SPEECH_H
#include <stddef.h>

typedef int (*SpeechSink)(void *context, const float *samples, size_t count);

int speech_open(const char *data_dir, const char *pack, const char *engine_path);
int speech_synth(const char *text, const char *speaker, const char *gender, double rate, double pitch,
                 SpeechSink sink, void *context, size_t *count);
const char *speech_gender(const char *speaker);
const char *speech_error(void);
size_t speech_chunk_end(const char *text, size_t offset, size_t length);
int sha256_file(const char *path, char hex[65]);
#endif
