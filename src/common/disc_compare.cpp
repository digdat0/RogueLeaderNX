// disc_compare <a> <b>: reads two disc images (ISO/RVZ/WIA) end to end and reports the first
// difference. Checks the RVZ reader against an ISO made by tools/rvz.py (or Dolphin).
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "disc_image.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: disc_compare <image a> <image b>\n");
        return 2;
    }
    std::string err;
    auto a = disc::open(argv[1], err);
    if (!a) return fprintf(stderr, "%s: %s\n", argv[1], err.c_str()), 1;
    auto b = disc::open(argv[2], err);
    if (!b) return fprintf(stderr, "%s: %s\n", argv[2], err.c_str()), 1;
    printf("%s: %s, %llu bytes\n%s: %s, %llu bytes\n", argv[1], a->kind(), (unsigned long long)a->size(), argv[2],
           b->kind(), (unsigned long long)b->size());
    const uint64_t size = std::min(a->size(), b->size());
    std::vector<uint8_t> x(1 << 20), y(1 << 20);
    const auto t0 = std::chrono::steady_clock::now();
    for (uint64_t off = 0; off < size; off += x.size()) {
        const uint32_t n = uint32_t(std::min<uint64_t>(x.size(), size - off));
        if (!a->read(off, x.data(), n) || !b->read(off, y.data(), n)) {
            printf("read failed at %llX\n", (unsigned long long)off);
            return 1;
        }
        if (memcmp(x.data(), y.data(), n) != 0) {
            uint32_t i = 0;
            while (x[i] == y[i]) i++;
            printf("first difference at %llX: %02X vs %02X\n", (unsigned long long)(off + i), x[i], y[i]);
            return 1;
        }
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("identical (%llu bytes, %.1f s)\n", (unsigned long long)size, s);
    return a->size() == b->size() ? 0 : 1;
}
