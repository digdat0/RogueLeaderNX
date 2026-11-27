// RL-NX: <execinfo.h> for Horizon. newlib has no backtrace(3); the only user is main.cpp's host
// crash report, which on the Switch is replaced by the crash handler in switch/src (Atmosphere
// reports + __nx_exception_handler). Returning no frames makes printHostStack print just its
// heading instead of failing to link. Only this port's Switch include path has this header.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

static inline int backtrace(void** buffer, int size) {
    (void)buffer, (void)size;
    return 0;
}

static inline char** backtrace_symbols(void* const* buffer, int size) {
    (void)buffer, (void)size;
    return 0;
}

static inline void backtrace_symbols_fd(void* const* buffer, int size, int fd) {
    (void)buffer, (void)size, (void)fd;
}

#ifdef __cplusplus
}
#endif
