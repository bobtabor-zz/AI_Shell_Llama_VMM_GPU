#ifndef UTIL_H
#define UTIL_H
#include <stdio.h>
#include <stdarg.h>
//#pragma message("util.h INCLUDED")
#pragma message("USING util.h FROM: " __FILE__)

typedef enum { LOG_ERROR=0, LOG_WARN=1, LOG_INFO=2, LOG_DEBUG=3, LOG_TRACE=4 } log_level_t;

void log_set_level(log_level_t lvl);
void log_set_sink(FILE* sink);
void log_msg(log_level_t lvl, const char* fmt, ...);

char* read_whole_file(const char* path);
char* util_join(const char* sep, int argc, char** argv);
char* util_format(const char* fmt, ...);


#endif
