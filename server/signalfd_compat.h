/* signalfd compatibility for macOS */
#ifndef SIGNALFD_COMPAT_H
#define SIGNALFD_COMPAT_H

#ifdef __APPLE__
/* Stub struct - not actually used on macOS in thread mode */
struct signalfd_siginfo {
    uint32_t ssi_signo;
    uint32_t ssi_pid;
};
#define SFD_NONBLOCK 0
#define SFD_CLOEXEC 0
static inline int signalfd(int fd, const sigset_t *mask, int flags) {
    (void)fd; (void)mask; (void)flags;
    return -1;
}
#endif

#endif
