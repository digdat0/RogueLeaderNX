#pragma once
// In-app updates (update.cpp): at start Red Two asks GitHub for the latest release (setting
// check_updates); the menu shows a newer one, and on request downloads it, checks it against the
// release's SHA256SUMS.txt, puts it in place and restarts into it. Windows: the release zip over
// the program's folder (release builds only: -DR2_RELEASE=ON; the user folder is left alone);
// Linux: the AppImage file itself (same name: shortcuts keep working). Other builds (from source)
// get the notice and a link to the release page.
#include <string>

#include "runtime.h"

namespace rt::update {

const char* version();  // this build's ("1.2"; the repository's VERSION file)

// At start (after the settings): what an update just installed left behind is removed, and the
// check starts on a worker thread when the setting allows it.
void init();
void check();  // ask GitHub again (the menu)

enum class Phase { Idle, Checking, Current, Available, Downloading, Installing, Ready, Failed, Updated };
struct Status {
    Phase phase = Phase::Idle;
    std::string latest;    // the newest release's version ("1.3")
    std::string from;      // Updated: the version this one replaced
    std::string pageUrl;   // its page on GitHub (the notes)
    std::string asset;     // the file this platform downloads
    std::string error;     // Failed: what went wrong (a sentence)
    u64 got = 0, total = 0;  // Downloading: bytes
    bool canInstall = false;  // this copy can replace itself
};
Status status();

void install();  // Available / Failed: download, check and put in place (a worker); then Ready
// Ready: the program to start instead of this one (main.cpp restartGame takes it from here)
std::string restartProgram();

}  // namespace rt::update
