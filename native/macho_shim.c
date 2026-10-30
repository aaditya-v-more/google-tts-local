#include <stdint.h>

/* These are addressed by dyld binds in converted images. They are populated
   by the host before any guest initializer or engine code is called. */
unsigned gtts_tls_key;
uint64_t gtts_slot_guard, gtts_slot_stdin, gtts_slot_stdout, gtts_slot_stderr;
uint64_t gtts_slot_environ, gtts_slot_tzname[2], gtts_slot_timezone;
