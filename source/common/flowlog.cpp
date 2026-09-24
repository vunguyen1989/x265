/*****************************************************************************
 * Temporary structural flow trace used by the Step 1 encoder walkthrough.
 *****************************************************************************/

#include "flowlog.h"
#include "threading.h"

#include <stdarg.h>
#include <time.h>

namespace X265_NS {

#if CHECKED_BUILD || _DEBUG

static Lock s_flowLogLock;
static FILE* s_flowLogFile;
static bool s_flowLogOpenAttempted;
static uint64_t s_flowLogEventId;

static const char* flowLogBaseName(const char* path)
{
    const char* base = path;
    for (const char* p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    return base;
}

static uint64_t flowLogThreadId()
{
#if _WIN32
    return (uint64_t)GetCurrentThreadId();
#else
    return (uint64_t)(uintptr_t)pthread_self();
#endif
}

void x265_flow_log(const char* threadName, const char* sourceFile, int sourceLine,
                   const char* fmt, ...)
{
    ScopedLock lock(s_flowLogLock);

    if (!s_flowLogOpenAttempted)
    {
        s_flowLogOpenAttempted = true;
        const char* path = getenv("X265_FLOW_LOG");
        if (path && path[0])
            s_flowLogFile = x265_fopen(path, "w");
        if (path && path[0] && !s_flowLogFile)
            x265_log(NULL, X265_LOG_ERROR, "unable to open flow log %s\n", path);
    }

    if (!s_flowLogFile)
        return;

    /* The lock makes this a single total order across every traced thread. */
    uint64_t eventId = ++s_flowLogEventId;

    int64_t now = x265_mdate();
    time_t seconds = (time_t)(now / 1000000);
    struct tm wallTime;
#if _WIN32
    localtime_s(&wallTime, &seconds);
#else
    localtime_r(&seconds, &wallTime);
#endif

    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &wallTime);
    fprintf(s_flowLogFile, "[event=%06llu] %s.%06d [thread=%s id=%llu] %s:%d ",
            (unsigned long long)eventId, timestamp, (int)(now % 1000000), threadName,
            (unsigned long long)flowLogThreadId(), flowLogBaseName(sourceFile), sourceLine);

    va_list args;
    va_start(args, fmt);
    vfprintf(s_flowLogFile, fmt, args);
    va_end(args);

    fputc('\n', s_flowLogFile);
    fflush(s_flowLogFile);
}

#endif

}
