#include "util.h"
#include <string.h>
#include <stdlib.h>

static log_level_t g_level = LOG_INFO;
static FILE* g_sink = NULL;

void log_set_level(log_level_t lvl){ g_level = lvl; }
void log_set_sink(FILE* sink){ g_sink = sink; }

void log_msg(log_level_t lvl, const char* fmt, ...){
  if (lvl > g_level) return;
  FILE* s = g_sink ? g_sink : stderr;
  const char* tag = (lvl==LOG_ERROR?"ERROR":lvl==LOG_WARN?"WARN":lvl==LOG_INFO?"INFO":lvl==LOG_DEBUG?"DEBUG":"TRACE");
  fprintf(s, "[%s] ", tag);
  va_list ap; va_start(ap, fmt); vfprintf(s, fmt, ap); va_end(ap);
  fprintf(s, "\n");
}

char* read_whole_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buf = (char*)malloc(size + 1);
    if (!buf) { fclose(f); return NULL; }

    fread(buf, 1, size, f);
    buf[size] = 0;

    fclose(f);
    return buf;
}

char* util_join(const char* sep, int argc, char** argv) {
    size_t len = 0;
    for (int i = 0; i < argc; i++)
        len += strlen(argv[i]) + strlen(sep);

    char* out = (char*)malloc(len + 1);
    out[0] = 0;

    for (int i = 0; i < argc; i++) {
        strcat(out, argv[i]);
        if (i + 1 < argc) strcat(out, sep);
    }

    return out;
}

char* util_format(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    char tmp[8192];
    vsnprintf(tmp, sizeof(tmp), fmt, ap);

    va_end(ap);
    return strdup(tmp);
}



