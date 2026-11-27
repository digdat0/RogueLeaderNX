// GameCube memory card images (raw dumps, the layout of Dolphin's .raw cards and of
// saves/GSWE64_A.raw): the file system the SDK's CARD library keeps on them, enough to list
// files as GCI (directory entry + data blocks) and to put a GCI file onto a card.
//
// Layout, in 8 KiB blocks: 0 header (serial, format time, size, encoding, checksums),
// 1-2 directory (127 entries of 64 bytes, update counter and checksums at the end; two copies,
// the valid one with the newer counter is current), 3-4 block allocation table (same scheme),
// 5.. file data, chained through the table.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace rt::card {

constexpr uint32_t kBlockSize = 0x2000;
constexpr uint32_t kEntrySize = 0x40;  // directory entry, also the GCI header

struct FileInfo {
    std::string gameCode;  // "GSWE"
    std::string company;   // "64"
    std::string fileName;  // "RogueLeader"
    uint32_t modTime = 0;  // seconds since 2000-01-01
    uint16_t blocks = 0;
};

bool isFormatted(const std::vector<uint8_t>& img);
// a freshly formatted card of img.size() bytes (a multiple of 8 KiB, 0.5-16 MiB)
void format(std::vector<uint8_t>& img, uint64_t formatTimeTicks);
// every file on the card as a GCI image; false (with err) if the card is not readable
bool listFiles(const std::vector<uint8_t>& img, std::vector<std::vector<uint8_t>>& gcis, std::string& err);
// directory entry fields of a GCI (or of a 64-byte entry)
FileInfo info(const uint8_t* entry);
bool validGci(const std::vector<uint8_t>& gci, std::string& err);
// writes the file, replacing one with the same game code, company and file name
bool putFile(std::vector<uint8_t>& img, const std::vector<uint8_t>& gci, std::string& err);

}  // namespace rt::card
