// RL-NX: the NRO's entry point.
//
// The boot sequence is the one in src/runtime/main.cpp - the desktop app this port parks - with the
// desktop parts left out: no launcher, no window, no settings file. What the game needs is the disc,
// the guest memory the apploader would have set up, and the guest's first fiber.
#include <cstdio>
#include <cstdlib>  // RL-NX: getenv/atoi for R2_PROFILE
#include <cstring>
#include <string>

#include <fcntl.h>
#include <switch.h>
#include <sys/stat.h>
#include <unistd.h>  // RL-NX: write/dup for the boot report (banner goes to the screen, not to stdout)

#include "hw/musyx_dsp.h"  // rt::audio::init (the MusyX/DSP interface this file provides)
#include "log_switch.h"  // melee_nx_log_capture_stdio: stdout/stderr -> logs/log.txt on the SD card
#include "rl_boot.h"      // rt::findDiscSwitch, rt::bootDiscSwitch
#include "runtime.h"
#include "savestate.h"
#include "table.h"

// RL-NX: switch/src/perf_switch.cpp (thread placement policy from config/perf.cfg) and prof_switch.cpp
// (the 1 kHz sampling profiler). Declared here: rl_entry is built with Clang, apart from those files.
extern "C" void melee_nx_perf_init(void);
extern "C" void rlProfStartFor(int seconds);
namespace rt::gfx {
void menuLoadPrefs();       // RL-NX: menu_switch.cpp - config/menu.cfg, the - menu's saved rows
void menuApplyBootPrefs();  // RL-NX: the saved rows that are switched by a call (profiler, perf log, GPU timing)
}

#define RL_SD_ROOT "sdmc:/switch/RL-NX"
#define RL_LOG_PATH RL_SD_ROOT "/logs/log.txt"

// The build stamp: build.sh touches this file so the log's first line carries this build's time (an
// old .nro on the SD card is the most common "it still crashes").
const char rl_build_stamp[] = __DATE__ " " __TIME__;
// RL-NX: the same stamp under the C name prof_switch.cpp reads for the profile header (a namespace-
// scope const has internal linkage in C++, so it is exported explicitly).
extern "C" const char rl_nx_build_stamp[];
extern "C" const char rl_nx_build_stamp[] = __DATE__ " " __TIME__;

namespace {

// RL-NX: the console's own stdout, kept before the log takes fd 1. melee_nx_log_capture_stdio
// dup2's the log over STDOUT_FILENO and STDERR_FILENO, so from then on printf reaches only
// logs/log.txt - on a console that was a black screen with no boot report at all. Writes through
// this saved descriptor still reach the console device, and consoleUpdate draws them.
int s_screenFd = -1;

// RL-NX: options. Red Two's switches are environment variables (R2_INPUT, R2_NO_XFB, R2_GX_DUMP,
// R2_UBER, ...) and a console has no shell to set them from, so the same names are read from
// boot.cfg in the port's folder: one KEY=VALUE per line, # starts a comment. Loaded before anything
// reads them, and every line that is set is logged, so a log always says how a build was run.
void loadOptions() {
    // RL-NX: config/boot.cfg since the SD layout change (2026-11-26). Optional and for diagnostics only: normal
    // play needs no file (the zfreeze guard is on by default and the - menu rows are saved in config/menu.cfg).
    FILE* f = fopen(RL_SD_ROOT "/config/boot.cfg", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#' || *p == '\n' || *p == '\r') continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = p;
        char* val = eq + 1;
        for (char* e = key + strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t');) *--e = '\0';
        for (char* e = val + strlen(val); e > val && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ');) *--e = '\0';
        if (!*key) continue;
        setenv(key, val, 1);
        rt::logf("opts", "%s=%s", key, val);
    }
    fclose(f);
}

// The boot report: the lines the screen shows, mirrored into the log through printf once stdout is
// the port's buffered writer (which is where every rt::logf line goes too).
void banner(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    // RL-NX: and onto the screen: through the descriptor saved before the log took stdout, then
    // one update, because libnx's console draws nothing until consoleUpdate is called.
    if (s_screenFd >= 0) {
        const size_t len = strlen(buf);
        write(s_screenFd, buf, len);
        write(s_screenFd, "\n", 1);
        consoleUpdate(NULL);
    }
}

}  // namespace

extern "C" void rlUpdaterSetNroPath(const char* path);  // RL-NX (2026-11-27): updater_switch.cpp
extern "C" void rlUpdaterApplyPending(void);

int main(int argc, char** argv) {
    consoleInit(NULL);
    // RL-NX: before anything redirects stdout (see s_screenFd): the boot report has to be readable
    // on the console, where a user with a black screen can see how far the boot got.
    s_screenFd = dup(STDOUT_FILENO);
    printf("RL-NX (Rogue Leader) %s\n", "0.1");
    printf("build %s\n\n", rl_build_stamp);
    consoleUpdate(NULL);  // RL-NX: the console draws nothing until an update - show the two lines above

    // The SD card is on the console too (from here on everything printf'd is there, and this is
    // where the boot's own log lines go: a console screen cannot be attached to a bug report).
    mkdir(RL_SD_ROOT, 0777);
    mkdir(RL_SD_ROOT "/logs", 0777);
    mkdir(RL_SD_ROOT "/config", 0777);        // RL-NX: boot.cfg, menu.cfg, Aurora's input.json
    mkdir(RL_SD_ROOT "/cache", 0777);         // RL-NX: deko/ shaders, aurora/ caches, update.nro
    mkdir(RL_SD_ROOT "/cache/aurora", 0777);  // Aurora's pipeline cache (dawn_cache.db) and bindings
    // RL-NX (2026-11-27): the in-game updater: where this NRO lives (argv[0]), and the leftovers of the last update
    rlUpdaterSetNroPath(argc > 0 ? argv[0] : nullptr);
    rlUpdaterApplyPending();
    const int logFd = open(RL_LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (logFd >= 0 && melee_nx_log_capture_stdio(logFd))
        printf("RL-NX (Rogue Leader) %s, build %s\n", "0.1", rl_build_stamp);
    else
        printf("warning: no log file at %s\n", RL_LOG_PATH);

    // Title takeover: applet mode has too little memory for the 256 MiB guest arena.
    const AppletType applet = appletGetAppletType();
    if (applet != AppletType_Application && applet != AppletType_SystemApplication)
        banner("WARNING: applet mode. Use title takeover (hold R while starting a game).");
    else
        banner("launch mode: title takeover (ok)");

    loadOptions();  // before anything reads an R2_* switch
    rt::gfx::menuLoadPrefs();  // RL-NX: config/menu.cfg, the - menu's saved rows (over boot.cfg)
    // RL-NX: R2_ICOUNT=1 (guest instruction metering) and R2_STATE_HASH=<frame>[,...] (the
    // differential harness) are read here, before the guest boots. Both are inert otherwise.
    rt::meterInit();
    // RL-NX: R2_DETERMINISTIC=1 - the guest's clock becomes a virtual one that moves one frame period per
    // presented frame (runtime/events.cpp), which is what makes a state hash at frame N comparable between
    // two runs, and a performance A/B the same work each time.
    rt::detInit();
    // RL-NX: R2_ORACLE=<n> - the oracle test (runtime/oracle.cpp): n guest functions are run under the
    // interpreter and under the recompiled code from identical bytes, and must agree.
    rt::oracleInit();
    // RL-NX: thread placement. perf.cfg (optional) picks the policy; the default "tuned" puts the game
    // thread alone on core 1 and Aurora's threads on the others, applied as each thread names itself
    // (Aurora's thread::set_current, deko seam in integrate-deko.py). Before this, nothing placed
    // them: Aurora's placement code is Linux/Windows only, so the render worker started on the
    // process's default core, which is the game thread's (to be confirmed by the THREAD log lines).
    melee_nx_perf_init();
    // RL-NX: boot.cfg R2_PROFILE=<seconds> samples the game thread (and placed Aurora threads) at
    // 1 kHz from here, then writes logs/profile-NNN.txt (switch/tools/prof_report.py reads it).
    if (const char* prof = getenv("R2_PROFILE"); prof && *prof) rlProfStartFor(atoi(prof));
    rt::gfx::menuApplyBootPrefs();  // RL-NX: saved PROFILER / PERF LOG / GPU TIMING rows

    rt::eventsInit();
    if (!rt::memInit()) rt::fatal("could not reserve the guest address space");
    rt::tableInit();

    const std::string disc = rt::findDiscSwitch();
    if (disc.empty()) {
        banner("no disc image: put your Rogue Leader .iso/.rvz in");
        banner("  %s", RL_SD_ROOT);
        banner("(the game reads the disc; nothing is bundled with this build)");
        PadState pad = {};
        padInitializeDefault(&pad);
        banner("press + to exit");
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
            consoleUpdate(NULL);  // RL-NX: keep the console drawing while it waits
        }
        consoleExit(NULL);
        return 0;
    }
    banner("disc: %s", disc.c_str());
    if (!rt::discOpen(disc)) rt::fatal("cannot open the disc image %s", disc.c_str());

    rt::Ctx* boot = nullptr;
    rt::threadsInit(boot);
    const u32 entry = rt::bootDiscSwitch();
    // RL-NX: the renderer needs the window, and the console's framebuffer holds its buffer slots.
    // deko3d's swapchain configures the same slots and refuses a window that already has them
    // (nwindowConfigureBuffer answers LibnxError_AlreadyInitialized, and the release library aborts
    // inside dkSwapchainCreate), so the swapchain was skipped on a console - the game ran and the
    // screen stayed black. The boot report has been shown; everything from here is in logs/log.txt.
    if (s_screenFd >= 0) {
        close(s_screenFd);
        s_screenFd = -1;
    }
    consoleExit(NULL);
    rt::gfx::switchInit();  // Aurora: GX runs, the swapchain takes the window it just got back (§E 4)
    rt::audio::init();      // audout output, before the DSP starts producing blocks
    rt::hwInit();
    boot->msr = 0x2032;  // FP, IR, DR, RI; interrupts off until the guest's OS enables them
    rt::logf("boot", "%u recompiled functions; jumping to %s", rt::g_funcCount, rt::describeAddr(entry).c_str());
    rt::threadsRun(entry);  // the boot fiber runs the game; this thread only takes save states now
}
