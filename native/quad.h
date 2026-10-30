#ifndef GTTS_QUAD_H
#define GTTS_QUAD_H

#include <stdint.h>

/* Convert a validated strtod subject sequence to IEEE binary128. */
int guest_quad_parse(const char *start, const char *end, const char *decimal,
                     unsigned rounding, uint64_t result[2], int *range_error);

#endif
