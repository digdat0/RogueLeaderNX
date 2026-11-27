// r2recomp: translates the game's main.dol into C++ (one function per guest entry point).
//
//   r2recomp <main.dol> <config dir> <out dir> [--files N]
//
// Writes <out>/funcs.h, <out>/code_NNN.cpp, <out>/table.cpp and <out>/functions.txt.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "analysis.h"
#include "emit.h"

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: r2recomp <main.dol> <config dir> <out dir> [--files N]\n");
        return 2;
    }
    int files = 48;
    for (int i = 4; i < argc; i++)
        if (!strcmp(argv[i], "--files") && i + 1 < argc) files = atoi(argv[++i]);

    const auto t0 = std::chrono::steady_clock::now();
    Image img;
    if (!img.load(argv[1])) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    Config cfg;
    if (!cfg.load(argv[2])) return 1;
    for (uint32_t at : cfg.embedded) {
        if (!img.addEmbedded(at)) {
            fprintf(stderr, "no embedded image at %08X\n", at);
            return 1;
        }
    }
    for (auto& s : img.sections)
        printf("  %-4s %08X-%08X %s\n", s.name.c_str(), s.addr, s.end(), s.text ? "text" : "data");
    Analysis an(img, cfg);
    an.run();

    std::filesystem::create_directories(argv[3]);
    Emitter em(img, an, cfg);
    if (!em.write(argv[3], files)) return 1;

    size_t insns = 0, tables = 0;
    for (auto& [e, f] : an.funcs) {
        insns += f.insns.size();
        tables += f.tables.size();
    }
    {
        std::ofstream lst(std::string(argv[3]) + "/functions.txt");
        for (auto& [e, f] : an.funcs) {
            char buf[128];
            snprintf(buf, sizeof buf, "%08X %6zu", e, f.insns.size());
            lst << buf;
            if (auto it = cfg.names.find(e); it != cfg.names.end()) lst << " " << it->second;
            lst << "\n";
        }
        for (auto& n : an.notes) lst << "# " << n << "\n";
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("r2recomp: %zu functions, %zu instructions (with overlap), %zu jump tables, %zu notes, %.1fs\n",
           an.funcs.size(), insns, tables, an.notes.size(), secs);
    for (auto& [s, end] : an.codeLimit) printf("  code %08X-%08X\n", s, end);
    return 0;
}
