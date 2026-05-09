# Fix: getifaddrs() SIGABRT on netlink FD race (Issue #2184)

## Status: Testing — awaiting real-world confirmation before PR

## Summary

ShairportSync crashes with `SIGABRT` when `getifaddrs()` is called in `handle_setup_2` (rtsp.c:2834) during AirPlay 2 connection setup. The crash is caused by glibc's internal `abort()` when it detects an unexpected error on the netlink socket used by `getifaddrs()`.

## Root Cause

1. An old AirPlay connection thread tears down and closes file descriptors
2. A new connection arrives and `handle_setup_2` calls `getifaddrs()`
3. `getifaddrs()` internally opens a netlink socket, which gets assigned an FD number recently freed by the old thread
4. The old thread's cleanup races with the new netlink socket — closing the FD from under `getifaddrs()`
5. glibc's `__netlink_assert_response` detects EBADF (error 9) on the netlink descriptor and calls `abort()`
6. The process dies with SIGABRT

## Why a simple return-value check doesn't work

`getifaddrs()` never returns — glibc calls `abort()` internally before the function can return an error code. The crash happens inside glibc's `__netlink_request` → `__netlink_assert_response` → `__libc_fatal` → `abort()`.

## The Fix

We wrap `getifaddrs()` in `safe_getifaddrs()` which uses `sigsetjmp`/`siglongjmp` to catch the SIGABRT signal and recover gracefully. When caught, it returns -1 and the caller continues with `self_ip_string` only (already added to the PTP timing response before `getifaddrs` is called).

## Reproduction

The bug is trivially reproducible with a test program that:
1. Spawns a thread calling `getifaddrs()` in a loop
2. Spawns another thread closing FDs in the range where netlink sockets get allocated

On Rocky Linux 9.7 (glibc 2.34, kernel 5.14.0-611.54.1), the unfixed code crashes within milliseconds. The fixed code survives indefinitely (84,000+ abort signals caught and recovered in 10 seconds of stress testing).

### Reproducer (reproduce_getifaddrs_crash_v2.c)

```c
// Build: gcc -O2 -pthread -o reproduce reproduce_getifaddrs_crash_v2.c
// Crashes instantly without the fix

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <ifaddrs.h>
#include <signal.h>
#include <dirent.h>

volatile int running = 1;

int get_max_fd(void) {
    DIR *d = opendir("/proc/self/fd");
    if (!d) return 64;
    struct dirent *de;
    int max = 0;
    while ((de = readdir(d)) != NULL) {
        int fd = atoi(de->d_name);
        if (fd > max) max = fd;
    }
    closedir(d);
    return max;
}

void *closer_thread(void *arg) {
    while (running) {
        int base = get_max_fd();
        for (int fd = base - 2; fd <= base + 5; fd++)
            if (fd > 2) close(fd);
        usleep(10);
    }
    return NULL;
}

void *getifaddrs_thread(void *arg) {
    while (running) {
        struct ifaddrs *addrs = NULL;
        getifaddrs(&addrs);  // CRASHES HERE - glibc abort()
        if (addrs) freeifaddrs(addrs);
    }
    return NULL;
}

void handler(int sig) {
    fprintf(stderr, "SIGABRT - bug reproduced!\n");
    _exit(1);
}

int main(void) {
    signal(SIGABRT, handler);
    pthread_t t1, t2;
    pthread_create(&t1, NULL, closer_thread, NULL);
    pthread_create(&t2, NULL, getifaddrs_thread, NULL);
    sleep(10);
    running = 0;
    return 0;
}
```

## Environment

- ShairportSync 5.0.4 (also affects 5.0.2, 5.0.3)
- Rocky Linux 9.7, kernel 5.14.0-611.54.1
- glibc 2.34
- Crash observed during real AirPlay 2 session handover from iPhone

## Real-world crash log

```
May 09 19:39:04 livingroom-media shairport-sync[272394]: Unexpected error 9 on netlink descriptor 11.
May 09 19:39:05 livingroom-media systemd-coredump[344263]: Process 272394 (shairport-sync) of user 0 dumped core.
  Stack trace of thread 344261:
  #0  __pthread_kill_implementation (libc.so.6)
  #1  raise (libc.so.6)
  #2  abort (libc.so.6)
  #3  __libc_message.cold (libc.so.6)
  #4  __libc_fatal (libc.so.6)
  #5  __netlink_assert_response (libc.so.6)
  #6  __netlink_request (libc.so.6)
  #7  getifaddrs_internal (libc.so.6)
  #8  getifaddrs (libc.so.6)
  #9  handle_setup_2 (shairport-sync)
  #10 rtsp_conversation_thread_func (shairport-sync)
```

## Branch

`fix/safe-getifaddrs-netlink-race` on `jeeshofone/shairport-sync`

## Related

- https://github.com/mikebrady/shairport-sync/issues/2184
- https://github.com/zeromq/libzmq/issues/2051 (same glibc behavior)
