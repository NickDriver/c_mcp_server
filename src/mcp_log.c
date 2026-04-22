#define _POSIX_C_SOURCE 200809L

#include "mcp_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

void mcp_log(const char *level, const char *fmt, ...)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    (void)gmtime_r(&ts.tv_sec, &tmv);

    char tbuf[32];
    (void)strftime(tbuf, sizeof tbuf, "%Y-%m-%dT%H:%M:%SZ", &tmv);

    (void)fprintf(stderr, "[%s] [%s] ", tbuf, level);
    va_list ap;
    va_start(ap, fmt);
    (void)vfprintf(stderr, fmt, ap);
    va_end(ap);
    (void)fputc('\n', stderr);
    (void)fflush(stderr);
}
