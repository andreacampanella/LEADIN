/* hal_common.c - the driver log */
#include <stdarg.h>
#include <stdio.h>
#include "hal_drivers.h"

static void default_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

void (*hal_log)(const char *fmt, ...) = default_log;
