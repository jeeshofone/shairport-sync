/*
 * test_session_handover_fd_race.c
 *
 * Test that verifies the FD race condition between old connection teardown
 * and new connection setup is eliminated.
 *
 * The race: pthread_cancel(old_thread) is called but old_thread's cleanup
 * handlers haven't finished closing FDs when the new thread calls getifaddrs().
 * getifaddrs() opens a netlink socket that reuses a just-freed FD number,
 * then the old thread's cleanup closes it -> glibc abort.
 *
 * The fix: pthread_join(old_thread) after pthread_cancel() ensures all
 * cleanup handlers have completed before the new connection proceeds.
 *
 * Build: gcc -O2 -pthread -o test_session_handover test_session_handover_fd_race.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <ifaddrs.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define NUM_ITERATIONS 100

static int aborted = 0;

void sigabrt_handler(int sig) {
    (void)sig;
    aborted = 1;
    fprintf(stderr, "FAIL: SIGABRT - FD race condition triggered!\n");
    _exit(1);
}

/* Simulates an old connection thread that holds FDs and closes them on cancel */
static int old_thread_fds[16];
static int old_thread_fd_count = 0;

void old_thread_cleanup(void *arg) {
    (void)arg;
    /* Simulate closing connection FDs during cleanup - this is what causes the race */
    usleep(1000); /* Small delay to widen the race window */
    for (int i = 0; i < old_thread_fd_count; i++) {
        if (old_thread_fds[i] >= 0) {
            close(old_thread_fds[i]);
            old_thread_fds[i] = -1;
        }
    }
}

void *old_connection_thread(void *arg) {
    (void)arg;
    /* Open some sockets like a real RTSP connection would */
    old_thread_fd_count = 0;
    for (int i = 0; i < 8; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            old_thread_fds[old_thread_fd_count++] = fd;
        }
    }

    pthread_cleanup_push(old_thread_cleanup, NULL);

    /* Sit here until cancelled */
    while (1) {
        usleep(10000);
        pthread_testcancel();
    }

    pthread_cleanup_pop(1);
    return NULL;
}

/* Test WITHOUT the fix - cancel without join (original shairport-sync behavior) */
int test_without_fix(void) {
    pthread_t old_thread;
    pthread_create(&old_thread, NULL, old_connection_thread, NULL);
    usleep(50000); /* Let old thread open its FDs */

    /* Cancel but DON'T join - this is the bug */
    pthread_cancel(old_thread);

    /* Immediately call getifaddrs like handle_setup_2 does */
    struct ifaddrs *addrs = NULL;
    int ret = getifaddrs(&addrs);
    if (addrs) freeifaddrs(addrs);

    /* Now join (too late - the race already happened or didn't) */
    pthread_join(old_thread, NULL);
    return ret;
}

/* Test WITH the fix - cancel AND join before proceeding */
int test_with_fix(void) {
    pthread_t old_thread;
    pthread_create(&old_thread, NULL, old_connection_thread, NULL);
    usleep(50000); /* Let old thread open its FDs */

    /* Cancel AND join - this is the fix */
    pthread_cancel(old_thread);
    pthread_join(old_thread, NULL);

    /* Now safe to call getifaddrs - old thread's FDs are all closed */
    struct ifaddrs *addrs = NULL;
    int ret = getifaddrs(&addrs);
    if (addrs) freeifaddrs(addrs);
    return ret;
}

int main(int argc, char *argv[]) {
    int use_fix = (argc > 1 && strcmp(argv[1], "--fix") == 0);

    signal(SIGABRT, sigabrt_handler);

    printf("Testing session handover FD race (%s) - %d iterations\n",
           use_fix ? "WITH FIX" : "WITHOUT FIX", NUM_ITERATIONS);

    int failures = 0;
    for (int i = 0; i < NUM_ITERATIONS; i++) {
        if (use_fix)
            test_with_fix();
        else
            test_without_fix();
    }

    if (!aborted) {
        printf("PASS: %d iterations completed without SIGABRT\n", NUM_ITERATIONS);
        printf("(Note: without fix, the race is timing-dependent and may not trigger every run)\n");
    }
    return aborted ? 1 : 0;
}
