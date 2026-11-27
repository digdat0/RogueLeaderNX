// Windows: a CPU without the x86-64 level the program was built for (R2_ARCH, x86-64-v3: AVX2,
// FMA, BMI2, MOVBE) stopped at the first such instruction with nothing on screen - not even a
// session log (issue #14: "it doesn't display anything"). This file is compiled for the baseline
// (CMakeLists.txt) and runs as a C initializer, before the C++ initializers and main, which may
// already use those instructions: it says what is missing in a message box and exits.
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__)) && defined(R2_CPU_CHECK)
#include <windows.h>
#include <intrin.h>

#include <cwchar>

namespace {

int checkCpu() {
    int r[4];
    __cpuid(r, 0);
    const int maxLeaf = r[0];
    __cpuid(r, 1);
    const unsigned ecx1 = unsigned(r[2]);
    unsigned ebx7 = 0;
    if (maxLeaf >= 7) {
        __cpuidex(r, 7, 0);
        ebx7 = unsigned(r[1]);
    }
    __cpuid(r, int(0x80000001u));
    const unsigned ecxExt = unsigned(r[2]);
    // AVX state saved by the OS (OSXSAVE, XCR0 bits 1 and 2)
    const bool osAvx = (ecx1 & (1u << 27)) && (_xgetbv(0) & 6) == 6;
    struct Need {
        bool ok;
        const wchar_t* name;
    } const needs[] = {
        {(ecx1 & (1u << 28)) && osAvx, L"AVX"},
        {(ebx7 & (1u << 5)) && osAvx, L"AVX2"},
        {(ecx1 & (1u << 12)) != 0, L"FMA"},
        {(ebx7 & (1u << 3)) && (ebx7 & (1u << 8)), L"BMI1/BMI2"},
        {(ecx1 & (1u << 22)) != 0, L"MOVBE"},
        {(ecxExt & (1u << 5)) != 0, L"LZCNT"},
        {(ecx1 & (1u << 29)) != 0, L"F16C"},
    };
    wchar_t missing[128] = L"";
    for (const Need& n : needs)
        if (!n.ok) {
            if (missing[0]) wcscat_s(missing, L", ");
            wcscat_s(missing, n.name);
        }
    // R2_CPU_CHECK_TEST=1: the failure path as if something were missing, to stderr instead of a
    // message box, exit code 3 (proves the check runs before anything else)
    const bool test = GetEnvironmentVariableW(L"R2_CPU_CHECK_TEST", nullptr, 0) != 0;
    if (!missing[0] && !test) return 0;
    if (!missing[0]) wcscpy_s(missing, L"nothing (R2_CPU_CHECK_TEST)");
    wchar_t msg[512];
    swprintf_s(msg,
               L"Red Two needs a processor with AVX2 (Intel Haswell / 4th generation Core, AMD Ryzen or "
               L"newer).\n\nThis processor lacks: %ls.",
               missing);
    if (test) {
        char line[600];
        const int n = WideCharToMultiByte(CP_UTF8, 0, msg, -1, line, sizeof line - 2, nullptr, nullptr);
        DWORD written = 0;
        if (n > 0) WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(n - 1), &written, nullptr);
        ExitProcess(3);
    }
    MessageBoxW(nullptr, msg, L"Red Two", MB_OK | MB_ICONERROR);
    ExitProcess(1);
}

}  // namespace

// C initializers (.CRT$XI*) run before the C++ ones (.CRT$XC*) and main
#pragma section(".CRT$XIY", long, read)
extern "C" __declspec(allocate(".CRT$XIY")) int (*const r2CpuCheck)(void) = checkCpu;
#endif
