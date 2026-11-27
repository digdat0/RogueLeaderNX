// Mods: replacement textures in Dolphin's format (mods.cpp, through Aurora's texture
// replacement), cheat / patch codes in Dolphin's format (codes.cpp) and importing those and
// memory card saves from a Dolphin installation (dolphin_import.cpp).
//
// Files live next to settings.ini (the user folder, userDir(): user\ next to red_two.exe):
//   textures\GSWE64\...   texture packs, laid out as in Dolphin's Load\Textures
//   (and wherever the setting textures_folder points: a pack left where it was unpacked)
//   codes.ini             Gecko / Action Replay codes and patches ([Gecko], [Gecko_Enabled], ...)
// Automated runs (R2_BACKGROUND, R2_HEADLESS, R2_INPUT) load neither unless asked:
// R2_TEXTURES=1 (or R2_TEXTURES_DIR=<textures root>) and R2_CODES=<codes.ini>.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace rt::mods {

// Windows: the folder "user" next to red_two.exe (portable; else an existing %APPDATA%\Red Two\),
// ~/.local/share/Red Two/, on Android the app's shared storage folder
// (Android/data/<package>/files/); ends with a separator; "" if unknown
std::string userDir();
bool portable();           // userDir() is the folder next to the program (Windows; mods.cpp portableDir)
std::string userDirNote(); // which user folder and why, for the session log
std::string texturesDir();  // where texture packs go (userDir()/textures/)

// --- texture packs (mods.cpp) ------------------------------------------------------------
// Dolphin's rules for which folders under a textures root belong to the game: <root>/GSWE64
// (else the region-free <root>/GSW), and any top-level folder that contains a GSWE64.txt,
// GSW.txt or all.txt somewhere inside.
std::vector<std::string> packFolders(const std::string& root);

// A folder chosen in the menu (setting textures_folder) can be anything: the downloaded pack's
// own folder (holding GSWE64), GSWE64 itself, or a pack with its files right inside. Its pack:
// the folder itself when it is named GSWE64 / GSW (Pack), else its GSWE64 / GSW subfolder
// (HoldsPack), else the folder itself (Other). No recursive scan: it may be a large folder.
enum class FolderKind { Missing, Pack, HoldsPack, Other };
FolderKind textureFolderKind(const std::string& dir, std::string* packDir = nullptr);

struct PackInfo {
    std::string path;
    size_t textures = 0;  // replacement registrations
    bool chosen = false;  // from the folder chosen in the menu (not Red Two's textures folder)
};
void texturesInit();               // after Aurora is up: load the packs if enabled
void setTexturesEnabled(bool on);  // load / unload now (the menu)
std::string toggleTexturesShown();  // the hotkey: packs hidden / shown at once; the message to show
bool texturesHidden();              // hidden by the hotkey (until pressed again or reloaded)
void reloadTextures();             // after an import: pick up new files
const std::vector<PackInfo>& loadedPacks();
// a replacement file of that name in the loaded packs (case-insensitive), "" if none
std::string findPackFile(const std::string& name);
size_t loadedTextureCount();
std::string textureStats();  // replacement hits / pending loads (R2_GX_LOG)
void textureFrame(bool ended);  // around aurora_end_frame: accumulates Aurora's counters

// --- codes (codes.cpp) -------------------------------------------------------------------
enum class CodeKind { Gecko, ActionReplay, Patch };
struct Code {
    CodeKind kind = CodeKind::Gecko;
    std::string name;                // without the leading '$' (and without the creator)
    std::string creator;             // Gecko: "$Name [creator]"
    std::string notes;               // '*' lines
    std::vector<std::string> lines;  // as written ("04123456 00000000", "0x80001234:dword:0x0")
    bool enabled = false;
    bool supported = true;           // false: it changes game code (not possible here)
    std::string why;                 // why it is unsupported / what it does
};
std::string codesPath();               // codes.ini ("" in automated runs without R2_CODES)
void codesInit();                      // load codes.ini
std::vector<Code> codesSnapshot();     // the list, for the menu
void setCodeEnabled(size_t index, bool on);  // toggles one and saves codes.ini
void mergeCodes(const std::vector<Code>& add);  // imported codes (replacing same kind + name); saves
void codesFrame();                     // once per retrace: apply the enabled codes
// Dolphin's GameSettings sections ([Gecko], [Gecko_Enabled], [Gecko_Disabled], [ActionReplay],
// [OnFrame], ...) merged into `out`: definitions replace those of the same kind and name; the
// _Enabled / _Disabled lists switch codes defined in this text or earlier ones.
void parseCodeIni(const std::string& text, std::vector<Code>& out);
void classifyCode(Code& c);            // fills supported / why

// --- Dolphin import (dolphin_import.cpp) --------------------------------------------------
struct DolphinSave {
    std::string where;        // human-readable source ("Dolphin's card A", ...)
    std::string when;         // its modification time, "2026-01-25 00:03"
    std::string path;         // the file it came from
    std::vector<uint8_t> gci; // 0x40-byte directory entry + data blocks
    uint32_t modTime = 0;     // seconds since 2000-01-01 (directory entry)
};
struct DolphinPack {
    std::string path;     // Dolphin's pack folder
    std::string name;     // its folder name (kept when copied)
    size_t files = 0;
    uint64_t bytes = 0;
    bool imported = false;  // Red Two's textures folder has the same files (by size)
};
struct DolphinScan {
    bool found = false;
    std::string userDir;                 // Dolphin's User folder
    std::string sysDir;                  // Dolphin's Sys folder (built-in codes), may be empty
    std::vector<DolphinSave> saves;      // this game's saves, newest first
    std::vector<DolphinPack> packs;      // this game's texture packs
    std::vector<Code> codes;             // this game's codes (with Dolphin's enabled state)
};
DolphinScan scanDolphin();  // R2_DOLPHIN_DIR=<User folder> overrides the search
// folders holding Dolphin's .ini files (Dolphin.ini, RetroAchievements.ini, ...), most likely first
std::vector<std::string> dolphinConfigDirs();

// Saves: writes the save into Red Two's memory card image (the old image is kept in
// saves/backup). Returns false with a message on failure.
bool importSave(const DolphinSave& save, std::string& message);
// Codes: merged into codes.ini (Dolphin's enabled state is taken over).
bool importCodes(const DolphinScan& scan, std::string& message);
// Texture packs: copied into texturesDir() on a background thread (files already there with
// the same size are skipped).
struct CopyProgress {
    std::atomic<bool> running{false}, done{false}, failed{false};
    std::atomic<uint64_t> bytesDone{0}, bytesTotal{0};
    std::atomic<uint32_t> filesDone{0}, filesTotal{0};
    std::string message;  // set when done
};
CopyProgress& packCopy();
void startPackImport(const std::vector<DolphinPack>& packs);  // no-op while one runs
bool importPacksNow(const std::vector<DolphinPack>& packs, std::string& message);  // blocking

// red_two.exe --import-dolphin[=saves,textures,codes]: import and exit (no window)
int importCommandLine(const std::string& what);

}  // namespace rt::mods
