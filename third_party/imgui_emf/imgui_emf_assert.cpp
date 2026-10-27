#include <windows.h>

#include <cstdio>

#include "imgui_emf_config.h"

namespace {
char g_last[512] = "";
}

extern "C" void emf_imgui_assert_failed(const char* expr, const char* file, int line) {
    const char* base = file;
    for (const char* p = file; *p; ++p)
        if (*p == '\\' || *p == '/') base = p + 1;
    std::snprintf(g_last, sizeof(g_last), "IM_ASSERT(%s) failed at %s:%d", expr, base, line);
    RaiseException(0xE0EF1A55u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

extern "C" const char* emf_imgui_last_assert(void) { return g_last; }
