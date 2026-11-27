// RL-NX: <dlfcn.h> for Horizon. There is no dlopen on the Switch; http.cpp uses it only to find a
// system libcurl at run time (Linux/Android). Failing the load makes that path report "no curl"
// instead of failing to link, which is the right answer here: the Switch build links curl
// statically when the updater/achievements are enabled (devkitPro portlibs).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_LOCAL 0
#define RTLD_GLOBAL 0

static inline void* dlopen(const char* file, int mode) {
    (void)file, (void)mode;
    return 0;
}

static inline void* dlsym(void* handle, const char* name) {
    (void)handle, (void)name;
    return 0;
}

static inline int dlclose(void* handle) {
    (void)handle;
    return 0;
}

static inline const char* dlerror(void) { return "dlopen is not available on the Switch"; }

#ifdef __cplusplus
}
#endif
