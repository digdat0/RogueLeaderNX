// Small libc bridges required by the Switch graphics executable.
#include <sys/types.h>
#include <unistd.h>

// Horizon homebrew has no set-user-ID execution or separate effective identity.
// SQLite's Unix VFS queries this before its ownership-preservation operation.
// Keep it consistent with the process identity supplied by libnx.
uid_t geteuid(void) { return getuid(); }

// RL-NX: newlib has no strnlen (POSIX 2008), and card_image.cpp uses it to read the 32-byte name
// field of a memory-card directory entry.
size_t strnlen(const char* s, size_t maxlen) {
  size_t n = 0;
  while (n < maxlen && s[n] != '\0') n++;
  return n;
}

#include <errno.h>
#include <string.h>  // RL-NX: memcpy (pread bounce buffer)
#include <switch.h>  // RL-NX: Mutex (pread bounce buffer)

// RL-NX: newlib has no execvp/waitpid. Aurora's updater and the desktop restart path use them; on
// Horizon there is no process to start or wait for, so both fail the way an unimplemented call
// should rather than pull those paths into the build.
int execvp(const char* file, char* const argv[]) {
  (void)file;
  (void)argv;
  errno = ENOSYS;
  return -1;
}

pid_t waitpid(pid_t pid, int* status, int options) {
  (void)pid;
  (void)status;
  (void)options;
  errno = ECHILD;
  return -1;
}

// RL-NX: newlib has no pread (POSIX), and src/common/disc_image.cpp reads the disc with it. Built on
// lseek+read, restoring the file position afterwards; the disc is read from one thread, and the
// compression readers keep their own state per image.
// RL-NX: the read goes through a bounce buffer, never straight into the caller's memory. The disc is read
// into guest RAM, and on Horizon a file read hands its destination pages to the fs service for the length
// of the IPC: until it returns, every other thread that touches those pages takes a data abort. Hardware
// 10:48 crashed exactly so on a level load - the main thread in fsdev_read, the GP worker hashing a
// texture in the same pages (2168-0002 in textureProbe). The copy costs a memcpy per 256 KiB.
static u8 s_preadBounce[256 << 10] __attribute__((aligned(0x1000)));
static Mutex s_preadLock;  // RL-NX: zero-initialised = unlocked; pread has one caller today, but be safe
ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
  mutexLock(&s_preadLock);
  const off_t cur = lseek(fd, 0, SEEK_CUR);
  if (cur < 0 || lseek(fd, offset, SEEK_SET) < 0) {
    mutexUnlock(&s_preadLock);
    return -1;
  }
  ssize_t total = 0;
  while ((size_t)total < count) {
    size_t want = count - (size_t)total;
    if (want > sizeof s_preadBounce) want = sizeof s_preadBounce;
    const ssize_t n = read(fd, s_preadBounce, want);
    if (n <= 0) {
      if (total == 0) total = n;  // RL-NX: nothing read: pass the error or EOF through
      break;
    }
    memcpy((u8*)buf + total, s_preadBounce, (size_t)n);
    total += n;
    if ((size_t)n < want) break;  // RL-NX: short read (end of file)
  }
  lseek(fd, cur, SEEK_SET);
  mutexUnlock(&s_preadLock);
  return total;
}

// RL-NX: newlib has no sysconf (POSIX), and library code - Aurora's pipeline cache worker pool,
// zstd - asks for these. Horizon has one 4 KiB page size and always four cores.
// Written as an if-chain, not a switch: newlib defines _SC_PAGESIZE and _SC_PAGE_SIZE with the same
// value, which makes duplicate case labels.
long sysconf(int name) {
#ifdef _SC_PAGESIZE
  if (name == _SC_PAGESIZE) return 4096;
#endif
#ifdef _SC_PAGE_SIZE
  if (name == _SC_PAGE_SIZE) return 4096;
#endif
#ifdef _SC_NPROCESSORS_ONLN
  if (name == _SC_NPROCESSORS_ONLN) return 4;
#endif
#ifdef _SC_NPROCESSORS_CONF
  if (name == _SC_NPROCESSORS_CONF) return 4;
#endif
#ifdef _SC_OPEN_MAX
  if (name == _SC_OPEN_MAX) return 256;
#endif
  return -1;
}
