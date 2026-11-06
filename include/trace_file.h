#ifndef TRACE_FILE_H
#define TRACE_FILE_H

/* DEBUG=yes trace files shared by both backends (src/trace_file.c). */

#include <stdio.h>
#include <stddef.h>

/* New ./trace/<base><ext> file, 0600, never reusing or following an existing name. */
FILE *open_trace_file(const char *base, const char *ext, char *path, size_t path_size);

/* Trace file base name from the request's timestamp and uuid. */
void trace_base(char *out, size_t size, const char *timestamp, const char *uuid);

#endif /* TRACE_FILE_H */
