/*****************************************************************************
 * Temporary structural flow trace used by the Step 1 encoder walkthrough.
 *****************************************************************************/

#ifndef X265_FLOWLOG_H
#define X265_FLOWLOG_H

#include "common.h"

namespace X265_NS {

#if CHECKED_BUILD || _DEBUG
void x265_flow_log(const char* threadName, const char* sourceFile, int sourceLine,
                   const char* fmt, ...);

#define X265_FLOW_LOG(threadName, ...) \
    x265_flow_log(threadName, __FILE__, __LINE__, __VA_ARGS__)
#else
#define X265_FLOW_LOG(threadName, ...) do { } while (0)
#endif

}

#endif // X265_FLOWLOG_H
