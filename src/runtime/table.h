// Tables emitted by the recompiler (gen/table.cpp).
#pragma once
#include "ctx.h"

namespace rt {

struct FuncEntry {
    u32 addr;
    GuestFn fn;
};
struct SymName {
    u32 addr;
    const char* name;
};

extern const FuncEntry g_funcTable[];
extern const u32 g_funcCount;
extern const SymName g_symNames[];  // terminated by {0, nullptr}
extern const FuncEntry g_origTable[];  // translated bodies of hooked functions, {0, nullptr}-terminated

}  // namespace rt
