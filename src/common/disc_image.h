#pragma once
// GameCube disc images: plain ISO/GCM, or RVZ/WIA (Dolphin's compressed formats; GameCube
// discs, zstd or no compression). Shared by the recompiler (main.dol straight from the disc)
// and the runtime (the DVD drive).
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace disc {

class Image {
public:
    virtual ~Image() = default;
    virtual bool read(uint64_t off, void* dst, uint32_t len) = 0;
    virtual uint64_t size() const = 0;
    virtual const char* kind() const = 0;
};

// Opens an ISO/GCM or RVZ/WIA file; nullptr (and a message in `error`) if it cannot.
std::unique_ptr<Image> open(const std::string& path, std::string& error);

// main.dol from a disc image (the header's DOL offset; size from the DOL's own sections).
bool readDol(Image& img, std::string& out, std::string& error);

}  // namespace disc
