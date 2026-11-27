// GameCube disc images: ISO/GCM and RVZ/WIA. The RVZ/WIA layout follows Dolphin's format
// description (docs/WiaAndRvz.md); no Dolphin code. Only what a GameCube disc needs: no Wii
// partitions, zstd or no compression (what Dolphin writes by default). Mirrors tools/rvz.py.
#include "disc_image.h"

#include <zstd.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <list>
#include <unordered_map>
#include <vector>

namespace disc {

namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }

#ifdef _WIN32
bool seekRead(FILE* f, uint64_t off, void* dst, size_t len) {
    if (_fseeki64(f, int64_t(off), SEEK_SET) != 0) return false;
    return fread(dst, 1, len, f) == len;
}
uint64_t fileSize(FILE* f) {
    _fseeki64(f, 0, SEEK_END);
    return uint64_t(_ftelli64(f));
}
#else
// POSIX: positioned reads, which leave the file offset alone - an image opened from a file
// descriptor ("fd:<n>", Android's document picker) shares its offset with every dup of it.
bool seekRead(FILE* f, uint64_t off, void* dst, size_t len) {
    auto* p = static_cast<uint8_t*>(dst);
    while (len) {
        const ssize_t n = pread(fileno(f), p, len, off_t(off));
        if (n <= 0) return false;
        p += n, off += uint64_t(n), len -= size_t(n);
    }
    return true;
}
uint64_t fileSize(FILE* f) {
    struct stat st{};
    return fstat(fileno(f), &st) == 0 ? uint64_t(st.st_size) : 0;
}
#endif

class Iso final : public Image {
public:
    explicit Iso(FILE* f) : f_(f) { size_ = fileSize(f_); }
    ~Iso() override { fclose(f_); }
    bool read(uint64_t off, void* dst, uint32_t len) override {
        return off + len <= size_ && seekRead(f_, off, dst, len);
    }
    uint64_t size() const override { return size_; }
    const char* kind() const override { return "ISO"; }

private:
    FILE* f_;
    uint64_t size_ = 0;
};

// Lagged Fibonacci generator: the "junk" filling unused parts of GameCube discs.
class Lfg {
public:
    explicit Lfg(const uint8_t* seed) {
        for (int i = 0; i < 17; i++) b_[i] = be32(seed + 4 * i);
        for (int i = 17; i < K; i++) b_[i] = (b_[i - 17] << 23) ^ (b_[i - 16] >> 9) ^ b_[i - 1];
        for (int i = 0; i < 4; i++) forward();
    }
    void skip(size_t n) {
        pos_ += n;
        while (pos_ >= K * 4) {
            forward();
            pos_ -= K * 4;
        }
    }
    void get(uint8_t* out, size_t n) {
        while (n) {
            const size_t take = std::min(n, size_t(K * 4) - pos_);
            for (size_t i = 0; i < take; i++) {
                const size_t p = pos_ + i;
                out[i] = uint8_t(b_[p / 4] >> (24 - 8 * (p % 4)));
            }
            out += take;
            pos_ += take;
            n -= take;
            if (pos_ == K * 4) {
                forward();
                pos_ = 0;
            }
        }
    }

private:
    static constexpr int K = 521, J = 32;
    void forward() {
        for (int i = 0; i < J; i++) b_[i] ^= b_[i + K - J];
        for (int i = J; i < K; i++) b_[i] ^= b_[i - J];
    }
    uint32_t b_[K];
    size_t pos_ = 0;
};

constexpr uint64_t kSector = 0x8000;

class Rvz final : public Image {
public:
    ~Rvz() override {
        if (f_) fclose(f_);
    }

    bool init(FILE* f, std::string& error) {
        f_ = f;
        uint8_t h[0x48];
        if (!seekRead(f_, 0, h, sizeof h)) return fail(error, "short file");
        rvz_ = memcmp(h, "RVZ\x01", 4) == 0;
        if (!rvz_ && memcmp(h, "WIA\x01", 4) != 0) return fail(error, "not an RVZ/WIA file");
        const uint32_t discSize = be32(h + 0x0C);
        isoSize_ = be64(h + 0x24);
        std::vector<uint8_t> d(discSize);
        if (!seekRead(f_, 0x48, d.data(), discSize) || discSize < 16 + 0x80 + 16 + 20 + 33)
            return fail(error, "bad disc struct");
        const uint32_t discType = be32(&d[0]);
        compr_ = be32(&d[4]);
        chunk_ = be32(&d[12]);
        memcpy(dhead_, &d[16], 0x80);
        size_t o = 16 + 0x80;
        const uint32_t nPart = be32(&d[o]);
        o += 16 + 20;
        const uint32_t nRaw = be32(&d[o]);
        const uint64_t rawOff = be64(&d[o + 4]);
        const uint32_t rawSize = be32(&d[o + 12]);
        const uint32_t nGroups = be32(&d[o + 16]);
        const uint64_t groupOff = be64(&d[o + 20]);
        const uint32_t groupSize = be32(&d[o + 28]);
        if (discType != 1 || nPart) return fail(error, "only GameCube discs are supported");
        if (compr_ != 0 && compr_ != 5) return fail(error, "unsupported compression (zstd or none only)");
        if (!chunk_) return fail(error, "bad chunk size");
        std::vector<uint8_t> raw, gt;
        if (!table(rawOff, rawSize, size_t(nRaw) * 0x18, raw) || !table(groupOff, groupSize, size_t(nGroups) * 12, gt))
            return fail(error, "cannot read the tables");
        for (uint32_t i = 0; i < nRaw; i++) {
            const uint8_t* e = &raw[size_t(i) * 0x18];
            raw_.push_back({be64(e), be64(e + 8), be32(e + 16), be32(e + 20)});
        }
        for (uint32_t i = 0; i < nGroups; i++) {
            const uint8_t* e = &gt[size_t(i) * 12];
            groups_.push_back({be32(e), be32(e + 4), be32(e + 8)});
        }
        return true;
    }

    bool read(uint64_t off, void* dstv, uint32_t len) override {
        auto* dst = static_cast<uint8_t*>(dstv);
        if (off < 0x80) {
            const uint32_t take = uint32_t(std::min<uint64_t>(len, 0x80 - off));
            memcpy(dst, dhead_ + off, take);
            off += take;
            dst += take;
            len -= take;
        }
        while (len) {
            const RawRange* r = nullptr;
            uint64_t base = 0, end = 0;
            for (const RawRange& x : raw_) {
                base = x.off - x.off % kSector;
                end = x.off + x.size;
                if (off >= base && off < end) {
                    r = &x;
                    break;
                }
            }
            if (!r) {  // outside every raw-data range
                memset(dst, 0, len);
                return true;
            }
            const uint64_t local = (off - base) / chunk_;
            const uint64_t gStart = base + local * chunk_;
            const uint32_t gBytes = uint32_t(std::min<uint64_t>(chunk_, end - gStart));
            const std::vector<uint8_t>* blob = group(r->group + uint32_t(local), gStart, gBytes);
            if (!blob) return false;
            const uint32_t within = uint32_t(off - gStart);
            const uint32_t take = std::min(len, gBytes - within);
            memcpy(dst, blob->data() + within, take);
            off += take;
            dst += take;
            len -= take;
        }
        return true;
    }
    uint64_t size() const override { return isoSize_; }
    const char* kind() const override { return rvz_ ? "RVZ" : "WIA"; }

private:
    struct RawRange {
        uint64_t off, size;
        uint32_t group, count;
    };
    struct Group {
        uint32_t off4, size, packed;
    };

    static bool fail(std::string& error, const char* msg) {
        error = msg;
        return false;
    }

    bool table(uint64_t off, uint32_t size, size_t outSize, std::vector<uint8_t>& out) {
        std::vector<uint8_t> data(size);
        if (!seekRead(f_, off, data.data(), size)) return false;
        if (compr_ == 5) {
            out.resize(outSize);
            const size_t n = ZSTD_decompress(out.data(), outSize, data.data(), size);
            return !ZSTD_isError(n) && n >= outSize;
        }
        out.assign(data.begin(), data.begin() + std::min(outSize, data.size()));
        return out.size() == outSize;
    }

    // Decoded bytes of group gi, covering disc offsets [discOff, discOff + bytes).
    const std::vector<uint8_t>* group(uint32_t gi, uint64_t discOff, uint32_t bytes) {
        if (auto it = cache_.find(gi); it != cache_.end()) return &it->second;
        if (gi >= groups_.size()) return nullptr;
        const Group& g = groups_[gi];
        const bool compressed = rvz_ ? (g.size & 0x80000000u) != 0 : compr_ != 0;
        const uint32_t size = g.size & 0x7FFFFFFFu;
        std::vector<uint8_t> out;
        if (size == 0) {
            out.assign(bytes, 0);
        } else {
            std::vector<uint8_t> data(size);
            if (!seekRead(f_, uint64_t(g.off4) << 2, data.data(), size)) return nullptr;
            if (compressed) {
                std::vector<uint8_t> dec(std::max<size_t>(g.packed, bytes) + 64);
                const size_t n = ZSTD_decompress(dec.data(), dec.size(), data.data(), data.size());
                if (ZSTD_isError(n)) return nullptr;
                dec.resize(n);
                data.swap(dec);
            }
            if (rvz_ && g.packed) unpack(data, discOff, bytes, out);
            else out.assign(data.begin(), data.begin() + std::min<size_t>(bytes, data.size()));
            out.resize(bytes, 0);
        }
        if (order_.size() >= 48) {  // keep a few MiB of decoded groups
            cache_.erase(order_.front());
            order_.pop_front();
        }
        order_.push_back(gi);
        return &(cache_[gi] = std::move(out));
    }

    // RVZ packing: runs of stored bytes and of junk regenerated from a 68-byte LFG seed.
    static void unpack(const std::vector<uint8_t>& data, uint64_t discOff, uint32_t want, std::vector<uint8_t>& out) {
        out.clear();
        size_t p = 0;
        while (out.size() < want && p + 4 <= data.size()) {
            uint32_t n = be32(&data[p]);
            p += 4;
            if (n & 0x80000000u) {
                n &= 0x7FFFFFFFu;
                if (p + 68 > data.size()) break;
                Lfg lfg(&data[p]);
                p += 68;
                lfg.skip(size_t((discOff + out.size()) % kSector));
                const size_t at = out.size();
                out.resize(at + n);
                lfg.get(out.data() + at, n);
            } else {
                n = uint32_t(std::min<size_t>(n, data.size() - p));
                out.insert(out.end(), data.begin() + p, data.begin() + p + n);
                p += n;
            }
        }
        out.resize(want, 0);
    }

    FILE* f_ = nullptr;
    bool rvz_ = false;
    uint32_t compr_ = 0, chunk_ = 0;
    uint64_t isoSize_ = 0;
    uint8_t dhead_[0x80] = {};
    std::vector<RawRange> raw_;
    std::vector<Group> groups_;
    std::unordered_map<uint32_t, std::vector<uint8_t>> cache_;
    std::list<uint32_t> order_;
};

}  // namespace

std::unique_ptr<Image> open(const std::string& path, std::string& error) {
    FILE* f = nullptr;
#ifndef _WIN32
    // "fd:<n>": a file descriptor the caller keeps open (Android hands out documents that way;
    // the image gets its own duplicate)
    if (path.compare(0, 3, "fd:") == 0) {
        const int fd = dup(atoi(path.c_str() + 3));
        if (fd >= 0 && !(f = fdopen(fd, "rb"))) close(fd);
    } else
#endif
        f = fopen(path.c_str(), "rb");
    if (!f) {
        error = "cannot open " + path;
        return nullptr;
    }
    char magic[4] = {};
    seekRead(f, 0, magic, 4);
    if (memcmp(magic, "RVZ\x01", 4) == 0 || memcmp(magic, "WIA\x01", 4) == 0) {
        auto r = std::make_unique<Rvz>();
        if (!r->init(f, error)) return nullptr;  // (the Rvz owns and closes f)
        return r;
    }
    return std::make_unique<Iso>(f);
}

bool readDol(Image& img, std::string& out, std::string& error) {
    uint8_t hdr[0x440];
    if (!img.read(0, hdr, sizeof hdr)) {
        error = "cannot read the disc header";
        return false;
    }
    if (be32(hdr + 0x1C) != 0xC2339F3D) {
        error = "not a GameCube disc (no disc magic)";
        return false;
    }
    const uint32_t dolOff = be32(hdr + 0x420);
    uint8_t dh[0x100];
    if (!img.read(dolOff, dh, sizeof dh)) {
        error = "cannot read the DOL header";
        return false;
    }
    uint32_t size = 0x100;
    for (int i = 0; i < 18; i++) {
        const uint32_t o = be32(dh + 4 * i), s = be32(dh + 0x90 + 4 * i);
        if (s) size = std::max(size, o + s);
    }
    out.resize(size);
    if (!img.read(dolOff, out.data(), size)) {
        error = "cannot read the DOL";
        return false;
    }
    return true;
}

}  // namespace disc
