// Texture packs in Dolphin's format. Aurora does the work (lib/gfx/texture_replacement.cpp):
// it names each texture the game loads like Dolphin does - tex1_<w>x<h>[_m]_<XXH64 of the
// level-0 data>[_<XXH64 of the palette range it uses>]_<GX format>[_arb] - looks the name up in
// the registered folders (.dds or .png, _mipN files, "$" wildcards), and streams the files in on
// worker threads (a 64-pixel mip tail first, then the full texture). The bridge hands it each
// texture's guest memory (gx_bridge.cpp, LOAD_TEXOBJ), so the hashes match Dolphin's.
#include "mods.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>
#ifdef __ANDROID__
#include <SDL3/SDL_system.h>
#endif
#include <dolphin/gx/GXStruct.h>
#include <aurora/texture.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "runtime.h"
#include "settings.h"

// Aurora internals (lib/gx/texture.hpp, which needs Aurora's private include paths): the same
// definition, so the replacement counters can be logged.
namespace aurora::gx {
struct TextureStats {
  uint64_t uploadBytes = 0;
  uint64_t hashedBytes = 0;
  uint64_t objectHits = 0;
  uint64_t contentHits = 0;
  uint64_t replacementHits = 0;
  uint64_t misses = 0;
  uint64_t evictions = 0;
  uint64_t contentCacheBytes = 0;
  uint64_t contentCacheEntries = 0;
  uint64_t copyCacheBytes = 0;
  uint64_t copyCacheEntries = 0;
  uint64_t copyCacheHits = 0;
  uint64_t copyCacheMisses = 0;
  uint64_t copyCacheEvictions = 0;
  uint64_t currentCopyBytes = 0;
  uint64_t pendingLoads = 0;
  uint64_t publishes = 0;
  uint64_t publishBytes = 0;
};
const TextureStats& texture_stats() noexcept;
}  // namespace aurora::gx

namespace rt::mods {
namespace fs = std::filesystem;

namespace {

fs::path u8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string str(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}
bool ieq(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; i++)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return i == a.size() && !b[i];
}

bool automated() { return envFlag("R2_BACKGROUND") || envFlag("R2_HEADLESS") || getenv("R2_INPUT"); }

std::vector<PackInfo> s_packs;
std::vector<aurora::texture::ReplacementGroup> s_groups;
bool s_aurora = false;  // Aurora is up (textures can be registered)
bool s_packsHidden = false;  // hidden by the hotkey (toggleTexturesShown)

void showPacks(bool show) {
    if (s_packsHidden == !show) return;
    s_packsHidden = !show;
    // (packs register at priority 0, the button prompts at 1000)
    aurora::texture::set_replacement_priority_floor(show ? INT32_MIN : 1);
}

// the textures root this run uses ("" = none)
std::string texturesRoot() {
    if (const char* d = getenv("R2_TEXTURES_DIR")) return d;
    if (automated() && !envFlag("R2_TEXTURES")) return {};
    return texturesDir();
}

void unload() {
    for (auto& g : s_groups) aurora::texture::unregister_replacements(g);
    aurora::texture::clear_prefetched_replacements();
    s_groups.clear();
    s_packs.clear();
}

void load() {
    unload();
    // Red Two's own textures folder (Dolphin's rules), then the folder chosen in the menu
    std::vector<std::pair<std::string, bool>> dirs;  // pack folder, chosen
    if (const std::string root = texturesRoot(); !root.empty())
        for (const std::string& dir : packFolders(root)) dirs.emplace_back(dir, false);
    if (const std::string& chosen = settings::get().texturesFolder; !chosen.empty()) {
        std::string pack;
        if (textureFolderKind(chosen, &pack) == FolderKind::Missing) logf("mods", "texture folder %s: not found", chosen.c_str());
        else dirs.emplace_back(pack, true);
    }
    for (size_t i = 0; i < dirs.size(); i++) {
        const auto& [dir, chosen] = dirs[i];
        bool seen = false;  // (the chosen folder may be Red Two's own pack)
        for (size_t j = 0; j < i; j++) seen |= ieq(dirs[j].first, dir.c_str());
        if (seen) continue;
        auto group = aurora::texture::load_replacement_directory(u8(dir));
        PackInfo info{dir, group.registrations.size(), chosen};
        logf("mods", "texture pack %s: %zu textures%s", dir.c_str(), info.textures, chosen ? " (chosen folder)" : "");
        s_groups.push_back(std::move(group));
        s_packs.push_back(std::move(info));
    }
    // Prefetch (Dolphin's "Prefetch Custom Textures", the player's setting there too): the packs'
    // textures read into memory by Aurora's texture workers, up to half the RAM as Dolphin does;
    // a texture then appears sharp the first time it is used (no preview while it loads from disk).
    // The game waits for it at startup (gx_bridge.cpp precompileShaders).
    if (settings::get().prefetchTextures && !s_packs.empty()) {
#ifdef _WIN32
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof ms;
        const u64 budget = GlobalMemoryStatusEx(&ms) ? ms.ullTotalPhys / 2 : (u64(4) << 30);
#else
        const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGE_SIZE);
        const u64 budget = pages > 0 && page > 0 ? u64(pages) * u64(page) / 2 : (u64(4) << 30);
#endif
        aurora::texture::prefetch_replacements(budget);
    }
}

}  // namespace

#ifdef _WIN32
namespace {
namespace fs = std::filesystem;
std::string s_userDirNote;  // what portableDir found or did, for the session log (userDirNote)
bool s_portable = false;

std::string utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}
fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

// A new portable folder takes over a previous install's small files: settings.ini and input.json
// from %APPDATA%\Red Two (with its texture packs kept where they are, as the textures folder),
// and the memory card from saves\ in the program's or the working folder (where 1.0 kept it).
// Copies: the old ones stay.
void migrate(const fs::path& user, const fs::path& appData, const fs::path& base) {
    std::error_code ec;
    std::string copied;
    auto note = [&](const char* what) { copied += copied.empty() ? what : std::string(", ") + what; };
    if (!fs::exists(user / "settings.ini", ec) && fs::exists(appData / "settings.ini", ec)) {
        std::string text;
        if (FILE* f = _wfopen((appData / "settings.ini").c_str(), L"rb")) {
            char buf[4096];
            for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
            fclose(f);
        }
        const fs::path packs = appData / "textures";
        if (fs::is_directory(packs / "GSWE64", ec) || fs::is_directory(packs / "GSW", ec)) {
            const std::string key = "\ntextures_folder=";
            const size_t at = text.rfind(key), v = at + key.size();
            const bool unset = at == std::string::npos || v >= text.size() || text[v] == '\r' || text[v] == '\n';
            if (unset) {
                text += "\ntextures_folder=" + utf8(packs) + "\n";  // (a later line wins)
                note("texture packs (left in place)");
            }
        }
        if (FILE* f = _wfopen((user / "settings.ini").c_str(), L"wb")) {
            fwrite(text.data(), 1, text.size(), f);
            fclose(f);
            note("settings");
        }
    }
    if (!fs::exists(user / "input.json", ec) && fs::exists(appData / "input.json", ec) &&
        fs::copy_file(appData / "input.json", user / "input.json", ec))
        note("controls");
    if (!fs::exists(user / "saves" / "GSWE64_A.raw", ec))
        for (const fs::path& dir : {base, fs::current_path(ec)}) {
            const fs::path card = dir / "saves" / "GSWE64_A.raw";
            if (dir.empty() || !fs::exists(card, ec)) continue;
            fs::create_directories(user / "saves", ec);
            if (fs::copy_file(card, user / "saves" / "GSWE64_A.raw", ec)) {
                note("memory card");
                break;
            }
        }
    if (!copied.empty()) s_userDirNote += "; copied from the previous install: " + copied;
}

// Windows: portable. A folder named "user" next to red_two.exe (the release zip has one) holds
// settings, the memory card, save states, session logs, the shader cache, texture packs and
// codes, so the whole program folder can live anywhere (a NAS, a USB stick: issue #13). Without
// one, an existing %APPDATA%\Red Two (an install from before) stays in use; a new install gets
// the user folder, or %APPDATA%\Red Two where the program's folder cannot be written.
std::string portableDir() {
    const char* baseUtf8 = SDL_GetBasePath();
    if (!baseUtf8 || !*baseUtf8) return {};
    const fs::path base = u8path(baseUtf8);
    const fs::path user = base / "user";
    fs::path appData;
    if (const wchar_t* a = _wgetenv(L"APPDATA"); a && *a) appData = fs::path(a) / "Red Two";
    std::error_code ec;
    bool fresh = false;
    if (!fs::is_directory(user, ec)) {
        if (!appData.empty() && fs::is_directory(appData, ec)) {
            s_userDirNote = "user folder: " + utf8(appData) + " (an existing install; a folder named user next to red_two.exe makes it portable)";
            return {};
        }
        if (!fs::create_directory(user, ec)) {
            s_userDirNote = "user folder: cannot create " + utf8(user) + " (" + ec.message() + "): " + utf8(appData);
            return {};
        }
        fresh = true;
    }
    s_portable = true;
    s_userDirNote = "user folder: " + utf8(user) + " (portable)";
    if (!appData.empty() && (fresh || !fs::exists(user / "settings.ini", ec))) migrate(user, appData, base);
    return utf8(user) + "\\";
}
}  // namespace

bool portable() {
    userDir();
    return s_portable;
}
std::string userDirNote() {
    userDir();
    return s_userDirNote;
}
#else
bool portable() { return false; }
std::string userDirNote() { return {}; }
#endif

std::string userDir() {
    static const std::string dir = [] {
        std::string d;
#ifdef _WIN32
        if (d = portableDir(); !d.empty()) return d;
#endif
#ifdef __ANDROID__
        // Android: the app's folder in shared storage (Android/data/<package>/files), which a PC
        // sees over USB, so texture packs, codes, saves and logs can be put there and taken
        // out; the private internal folder only if there is no shared storage.
        if (const char* ext = SDL_GetAndroidExternalStoragePath(); ext && *ext) {
            d = std::string(ext) + "/";
            return d;
        }
#endif
        if (char* p = SDL_GetPrefPath(nullptr, "Red Two")) {
            d = p;
            SDL_free(p);
        }
        return d;
    }();
    return dir;
}

std::string texturesDir() {
    const std::string u = userDir();
#ifdef _WIN32
    return u.empty() ? std::string("textures\\") : u + "textures\\";
#else
    return u.empty() ? std::string("textures/") : u + "textures/";
#endif
}

std::vector<std::string> packFolders(const std::string& root) {
    std::vector<std::string> out;
    std::error_code ec;
    const fs::path r = u8(root);
    if (!fs::is_directory(r, ec)) return out;
    auto add = [&](const fs::path& p) {
        const std::string s = str(p.lexically_normal());
        for (const auto& o : out)
            if (ieq(o, s.c_str())) return;
        out.push_back(s);
    };
    if (fs::is_directory(r / "GSWE64", ec)) add(r / "GSWE64");
    else if (fs::is_directory(r / "GSW", ec)) add(r / "GSW");
    // marker files: <root>/<pack>/.../GSWE64.txt (or GSW.txt, all.txt) claims <root>/<pack>
    for (fs::recursive_directory_iterator it(r, fs::directory_options::skip_permission_denied, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string name = str(it->path().filename());
        if (!ieq(name, "GSWE64.txt") && !ieq(name, "GSW.txt") && !ieq(name, "all.txt")) continue;
        const fs::path rel = it->path().lexically_relative(r);
        if (std::distance(rel.begin(), rel.end()) < 2) continue;  // a marker in the root itself
        add(r / *rel.begin());
    }
    return out;
}

FolderKind textureFolderKind(const std::string& dir, std::string* packDir) {
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(u8(dir), ec)) return FolderKind::Missing;
    fs::path p = u8(dir).lexically_normal();
    if (!p.has_filename() && p != p.root_path()) p = p.parent_path();  // (a trailing separator)
    const auto found = [&](const fs::path& pack, FolderKind kind) {
        if (packDir) *packDir = str(pack);
        return kind;
    };
    const std::string name = str(p.filename());
    if (ieq(name, "GSWE64") || ieq(name, "GSW")) return found(p, FolderKind::Pack);
    if (fs::is_directory(p / "GSWE64", ec)) return found(p / "GSWE64", FolderKind::HoldsPack);
    if (fs::is_directory(p / "GSW", ec)) return found(p / "GSW", FolderKind::HoldsPack);
    return found(p, FolderKind::Other);
}

void texturesInit() {
    s_aurora = true;
    if (settings::get().customTextures) load();
}

void setTexturesEnabled(bool on) {
    if (!s_aurora) return;
    showPacks(true);
    if (on) load();
    else unload();
}

// The hotkey (hotkey_toggle_textures, Dolphin's "Toggle Custom Textures"): the packs hidden or
// shown again at once, to compare. Aurora patch 0029 ignores registrations below a priority
// floor: the packs' 0 while the button prompts (1000) stay; nothing is unloaded and prefetched
// textures stay in memory. With custom textures off in the settings it turns them on (loads).
std::string toggleTexturesShown() {
    if (!s_aurora) return "TEXTURE PACKS: NOT AVAILABLE";
    settings::Settings& s = settings::get();
    if (!s.customTextures) {
        s.customTextures = true;
        settings::save();
        setTexturesEnabled(true);
        logf("mods", "texture packs: turned on by the hotkey");
        return s_packs.empty() ? "NO TEXTURE PACK FOUND" : "TEXTURE PACKS ON";
    }
    if (s_packs.empty()) return "NO TEXTURE PACK FOUND";
    showPacks(s_packsHidden);
    logf("mods", "texture packs %s by the hotkey", s_packsHidden ? "hidden" : "shown");
    return s_packsHidden ? "TEXTURE PACKS OFF" : "TEXTURE PACKS ON";
}

bool texturesHidden() { return s_packsHidden; }

void reloadTextures() {
    if (s_aurora && settings::get().customTextures) load();
}

const std::vector<PackInfo>& loadedPacks() { return s_packs; }

std::string findPackFile(const std::string& name) {
    std::error_code ec;
    for (const PackInfo& p : s_packs)
        for (fs::recursive_directory_iterator it(u8(p.path), fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec) && ieq(str(it->path().filename()), name.c_str())) return str(it->path());
        }
    return {};
}

size_t loadedTextureCount() {
    size_t n = 0;
    for (const auto& p : s_packs) n += p.textures;
    return n;
}

namespace {
u64 s_hits = 0, s_published = 0, s_publishedBytes = 0, s_pending = 0;
}

void textureFrame(bool ended) {
    if (s_packs.empty()) return;
    const auto& st = aurora::gx::texture_stats();  // (the GX thread is drained by now)
    if (!ended) {
        s_hits += st.replacementHits;
    } else {
        s_published += st.publishes;
        s_publishedBytes += st.publishBytes;
        s_pending = st.pendingLoads;
    }
}

std::string textureStats() {
    char b[160];
    snprintf(b, sizeof b, "replacements: %llu bound, %llu loaded (%llu MiB), %llu loading",
             (unsigned long long)s_hits, (unsigned long long)s_published,
             (unsigned long long)(s_publishedBytes >> 20), (unsigned long long)s_pending);
    return b;
}

}  // namespace rt::mods
