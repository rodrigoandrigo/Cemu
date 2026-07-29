#include "ExceptionHandler.h"

// UWP doesn't support the desktop dbghelp/minidump exception pipeline. The
// embedding host receives normal lifecycle failures through CemuEmbed instead.
void ExceptionHandler_Init()
{
}
