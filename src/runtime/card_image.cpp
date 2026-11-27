#include "card_image.h"

#include <algorithm>
#include <cstring>

namespace rt::card {
namespace {

constexpr uint32_t kSystemBlocks = 5;
constexpr uint32_t kMaxFiles = 127;
constexpr uint32_t kDirCheckCode = 0x1FFA, kDirSum = 0x1FFC;  // in a directory block
constexpr uint32_t kFatSum = 0, kFatCheckCode = 4, kFatFree = 6, kFatLast = 8;  // byte offsets in a table block

uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void put16(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}

// __CARDCheckSum: sum and inverted sum of big-endian halfwords; 0xFFFF is stored as 0
void checksum(const uint8_t* p, uint32_t len, uint16_t& sum, uint16_t& inv) {
    sum = inv = 0;
    for (uint32_t i = 0; i + 1 < len; i += 2) {
        const uint16_t w = be16(p + i);
        sum = uint16_t(sum + w);
        inv = uint16_t(inv + uint16_t(~w));
    }
    if (sum == 0xFFFF) sum = 0;
    if (inv == 0xFFFF) inv = 0;
}
bool checksumOk(const uint8_t* p, uint32_t len, const uint8_t* stored) {
    uint16_t s, i;
    checksum(p, len, s, i);
    return be16(stored) == s && be16(stored + 2) == i;
}
void setChecksum(uint8_t* p, uint32_t len, uint8_t* stored) {
    uint16_t s, i;
    checksum(p, len, s, i);
    put16(stored, s);
    put16(stored + 2, i);
}

bool dirValid(const uint8_t* d) { return checksumOk(d, kDirSum, d + kDirSum); }
bool fatValid(const uint8_t* f) { return checksumOk(f + 4, kBlockSize - 4, f + kFatSum); }

// the current copy (1 or 2 for the directory, 3 or 4 for the table), 0 if neither is valid
uint32_t current(const std::vector<uint8_t>& img, uint32_t first, bool (*valid)(const uint8_t*), uint32_t counterOff) {
    const uint8_t* a = img.data() + first * kBlockSize;
    const uint8_t* b = a + kBlockSize;
    const bool va = valid(a), vb = valid(b);
    if (va && vb) return int16_t(be16(b + counterOff) - be16(a + counterOff)) > 0 ? first + 1 : first;
    if (va) return first;
    if (vb) return first + 1;
    return 0;
}

uint32_t blockCount(const std::vector<uint8_t>& img) { return uint32_t(img.size() / kBlockSize); }

bool sameFile(const uint8_t* a, const uint8_t* b) {
    return memcmp(a, b, 6) == 0 && memcmp(a + 8, b + 8, 32) == 0;  // game code, company, file name
}

}  // namespace

bool isFormatted(const std::vector<uint8_t>& img) {
    if (img.size() < kSystemBlocks * kBlockSize) return false;
    const uint8_t* h = img.data();
    return be16(h + 0x20) == 0 && checksumOk(h, 0x1FC, h + 0x1FC);
}

void format(std::vector<uint8_t>& img, uint64_t time) {
    std::fill(img.begin(), img.end(), uint8_t(0xFF));
    uint8_t* h = img.data();
    // CARDFormat: serial = the console's flash ID scrambled with a generator seeded by the
    // format time (the runtime recovers the flash ID from it, hw.cpp cardSyncSram), SRAM's
    // counter bias and language, the format time, device 0, size in Mbit, encoding (0 ANSI).
    int64_t rnd = int64_t(time);
    for (int i = 0; i < 12; i++) {
        rnd = int64_t(uint64_t(rnd) * 1103515245u + 12345u) >> 16;
        h[i] = uint8_t(rnd);  // flash ID 0
        rnd = (int64_t(uint64_t(rnd) * 1103515245u + 12345u) >> 16) & 0x7FFF;
    }
    for (int i = 0; i < 8; i++) h[12 + i] = uint8_t(time >> (56 - 8 * i));
    memset(h + 20, 0, 12);  // counter bias, language, video mode
    put16(h + 0x20, 0);
    put16(h + 0x22, uint32_t(img.size() * 8 / (1024 * 1024)));
    put16(h + 0x24, 0);
    setChecksum(h, 0x1FC, h + 0x1FC);
    for (uint32_t i = 0; i < 2; i++) {
        uint8_t* d = h + (1 + i) * kBlockSize;
        memset(d, 0xFF, kBlockSize);
        put16(d + kDirCheckCode, i);
        setChecksum(d, kDirSum, d + kDirSum);
        uint8_t* f = h + (3 + i) * kBlockSize;
        memset(f, 0, kBlockSize);
        put16(f + kFatCheckCode, i);
        put16(f + kFatFree, blockCount(img) - kSystemBlocks);
        put16(f + kFatLast, kSystemBlocks - 1);
        setChecksum(f + 4, kBlockSize - 4, f + kFatSum);
    }
}

FileInfo info(const uint8_t* e) {
    FileInfo fi;
    fi.gameCode.assign(reinterpret_cast<const char*>(e), 4);
    fi.company.assign(reinterpret_cast<const char*>(e + 4), 2);
    const char* name = reinterpret_cast<const char*>(e + 8);
    fi.fileName.assign(name, strnlen(name, 32));
    fi.modTime = be32(e + 0x28);
    fi.blocks = be16(e + 0x38);
    return fi;
}

bool validGci(const std::vector<uint8_t>& gci, std::string& err) {
    if (gci.size() < kEntrySize + kBlockSize) {
        err = "not a GCI file (too small)";
        return false;
    }
    const uint16_t blocks = be16(gci.data() + 0x38);
    if (blocks == 0 || gci.size() < kEntrySize + size_t(blocks) * kBlockSize) {
        err = "not a GCI file (its size does not match its block count)";
        return false;
    }
    if (be32(gci.data()) == 0xFFFFFFFFu) {
        err = "not a GCI file (empty directory entry)";
        return false;
    }
    return true;
}

bool listFiles(const std::vector<uint8_t>& img, std::vector<std::vector<uint8_t>>& gcis, std::string& err) {
    if (!isFormatted(img)) {
        err = "the card is not formatted";
        return false;
    }
    const uint32_t dirBlk = current(img, 1, dirValid, kDirCheckCode);
    const uint32_t fatBlk = current(img, 3, fatValid, kFatCheckCode);
    if (!dirBlk || !fatBlk) {
        err = "the card's directory is damaged";
        return false;
    }
    const uint8_t* dir = img.data() + dirBlk * kBlockSize;
    const uint8_t* fat = img.data() + fatBlk * kBlockSize;
    const uint32_t nBlocks = blockCount(img);
    for (uint32_t i = 0; i < kMaxFiles; i++) {
        const uint8_t* e = dir + i * kEntrySize;
        if (be32(e) == 0xFFFFFFFFu) continue;
        const uint16_t len = be16(e + 0x38);
        uint16_t blk = be16(e + 0x36);
        std::vector<uint8_t> gci(e, e + kEntrySize);
        gci.reserve(kEntrySize + size_t(len) * kBlockSize);
        bool ok = len > 0;
        for (uint16_t n = 0; ok && n < len; n++) {
            if (blk < kSystemBlocks || blk >= nBlocks) {
                ok = false;
                break;
            }
            const uint8_t* src = img.data() + size_t(blk) * kBlockSize;
            gci.insert(gci.end(), src, src + kBlockSize);
            blk = be16(fat + size_t(blk) * 2);
        }
        if (ok) gcis.push_back(std::move(gci));
    }
    return true;
}

bool putFile(std::vector<uint8_t>& img, const std::vector<uint8_t>& gci, std::string& err) {
    if (!validGci(gci, err)) return false;
    if (!isFormatted(img)) {
        err = "the card is not formatted";
        return false;
    }
    const uint32_t dirBlk = current(img, 1, dirValid, kDirCheckCode);
    const uint32_t fatBlk = current(img, 3, fatValid, kFatCheckCode);
    if (!dirBlk || !fatBlk) {
        err = "the card's directory is damaged";
        return false;
    }
    std::vector<uint8_t> dir(img.begin() + dirBlk * kBlockSize, img.begin() + (dirBlk + 1) * kBlockSize);
    std::vector<uint8_t> fat(img.begin() + fatBlk * kBlockSize, img.begin() + (fatBlk + 1) * kBlockSize);
    const uint32_t nBlocks = blockCount(img);
    auto fatAt = [&](uint32_t b) { return be16(&fat[b * 2]); };
    auto setFat = [&](uint32_t b, uint32_t v) { put16(&fat[b * 2], v); };
    uint32_t freeBlocks = be16(&fat[kFatFree]);

    // drop the file it replaces
    int slot = -1;
    for (uint32_t i = 0; i < kMaxFiles; i++) {
        uint8_t* e = &dir[i * kEntrySize];
        if (be32(e) == 0xFFFFFFFFu || !sameFile(e, gci.data())) continue;
        uint32_t blk = be16(e + 0x36);
        for (uint32_t n = be16(e + 0x38); n > 0 && blk >= kSystemBlocks && blk < nBlocks; n--) {
            const uint32_t next = fatAt(blk);
            setFat(blk, 0);
            freeBlocks++;
            blk = next;
        }
        memset(e, 0xFF, kEntrySize);
        if (slot < 0) slot = int(i);
    }
    if (slot < 0)
        for (uint32_t i = 0; i < kMaxFiles && slot < 0; i++)
            if (be32(&dir[i * kEntrySize]) == 0xFFFFFFFFu) slot = int(i);
    if (slot < 0) {
        err = "the memory card has no free directory entry";
        return false;
    }
    const uint32_t len = be16(gci.data() + 0x38);
    if (len > freeBlocks) {
        err = "not enough free blocks on the memory card (" + std::to_string(len) + " needed, " +
              std::to_string(freeBlocks) + " free)";
        return false;
    }

    // allocate like __CARDAllocBlock: from the block after the last one allocated, wrapping
    std::vector<uint32_t> chain;
    uint32_t b = be16(&fat[kFatLast]);
    for (uint32_t tries = 0; chain.size() < len && tries < nBlocks; tries++) {
        if (++b < kSystemBlocks || b >= nBlocks) b = kSystemBlocks;
        if (fatAt(b) == 0) chain.push_back(b);
    }
    if (chain.size() < len) {
        err = "the memory card's allocation table is inconsistent";
        return false;
    }
    for (size_t i = 0; i < chain.size(); i++) {
        setFat(chain[i], i + 1 < chain.size() ? chain[i + 1] : 0xFFFF);
        memcpy(&img[size_t(chain[i]) * kBlockSize], &gci[kEntrySize + i * kBlockSize], kBlockSize);
    }
    put16(&fat[kFatFree], freeBlocks - len);
    put16(&fat[kFatLast], chain.back());

    uint8_t* e = &dir[size_t(slot) * kEntrySize];
    memcpy(e, gci.data(), kEntrySize);
    put16(e + 0x36, chain.front());

    // both copies get the new contents with a newer update counter
    put16(&dir[kDirCheckCode], be16(&dir[kDirCheckCode]) + 1u);
    setChecksum(dir.data(), kDirSum, &dir[kDirSum]);
    put16(&fat[kFatCheckCode], be16(&fat[kFatCheckCode]) + 1u);
    setChecksum(fat.data() + 4, kBlockSize - 4, &fat[kFatSum]);
    for (uint32_t i = 0; i < 2; i++) {
        std::copy(dir.begin(), dir.end(), img.begin() + (1 + i) * kBlockSize);
        std::copy(fat.begin(), fat.end(), img.begin() + (3 + i) * kBlockSize);
    }
    return true;
}

}  // namespace rt::card
