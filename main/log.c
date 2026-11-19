/* log.c - the firmware log, on the serial console. The drivers log here too. */
#include <stdarg.h>
#include <stdio.h>
#include "esp_log.h"
#include "leadin.h"

#define LINE_LEN    120

static const char *TAG = "leadin";

void nlog_init(void)
{
    hal_log = nlog;                     /* driver messages go to this log too */
}

void nlog(const char *fmt, ...)
{
    char buf[LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", buf);
}
