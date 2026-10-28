/*
 * debuglog.h — log opcional para diagnóstico.
 *
 * Compile com -DVW_DEBUG (MinGW) ou /DVW_DEBUG (MSVC) para gravar em
 * %TEMP%\videowallpaper_debug.log e no OutputDebugString (DebugView).
 * Sem a macro, DbgLog() some na compilação.
 */
#ifndef VW_DEBUGLOG_H
#define VW_DEBUGLOG_H

#ifdef VW_DEBUG
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

static void DbgLog(const char *fmt, ...)
{
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    OutputDebugStringA(buf);

    char path[MAX_PATH + 32];
    DWORD n = GetTempPathA(MAX_PATH, path);
    if (n == 0 || n > MAX_PATH) return;
    strcat(path, "videowallpaper_debug.log");
    FILE *f = fopen(path, "a");
    if (f) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(f, "[%02u:%02u:%02u.%03u] %s", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
        fclose(f);
    }
}
#else
#define DbgLog(...) ((void)0)
#endif

#endif
