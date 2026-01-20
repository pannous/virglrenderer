/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "render_common.h"

#include <stdarg.h>
#include <stdio.h>
#include <syslog.h>

void
render_log_init(void)
{
   openlog(NULL, LOG_NDELAY | LOG_PERROR | LOG_PID, LOG_USER);
}

void
render_log(const char *fmt, ...)
{
   va_list va;

   /* Use both syslog and stderr for better visibility */
   va_start(va, fmt);
   vsyslog(LOG_DEBUG, fmt, va);
   va_end(va);

#ifdef __APPLE__
   /* Also print to stderr on macOS for debugging */
   va_start(va, fmt);
   fprintf(stderr, "[virgl_render_server] ");
   vfprintf(stderr, fmt, va);
   fprintf(stderr, "\n");
   fflush(stderr);
   va_end(va);
#endif
}
