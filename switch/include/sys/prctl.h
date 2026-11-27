// RL-NX: <sys/prctl.h> for Horizon. events.cpp uses prctl(PR_SET_TIMERSLACK, 1) to stop the Linux
// kernel from coalescing the guest's event timer; Horizon has no such knob (its timers are already
// serviced per interrupt), so the call is a no-op.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define PR_SET_NAME 15
#define PR_SET_TIMERSLACK 29

static inline int prctl(int option, unsigned long arg2, unsigned long arg3, unsigned long arg4, unsigned long arg5) {
    (void)option, (void)arg2, (void)arg3, (void)arg4, (void)arg5;
    return 0;
}

#ifdef __cplusplus
}
#endif
