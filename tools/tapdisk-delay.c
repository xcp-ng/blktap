/* Send a per-disk completion-delay control signal to a patched tapdisk.
 * Build: cc -Wall -Wextra -o tapdisk-delay tapdisk-delay.c
 * Usage: ./tapdisk-delay PID MINOR on|off
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long number(const char *s, long low, long high)
{
    char *end;
    long value;

    errno = 0;
    value = strtol(s, &end, 10);
    if (errno || end == s || *end || value < low || value > high) {
        fprintf(stderr, "Invalid number: %s\n", s);
        exit(2);
    }
    return value;
}

int main(int argc, char **argv)
{
    union sigval value;
    pid_t pid;
    int signo;

    if (argc != 4 || (strcmp(argv[3], "on") && strcmp(argv[3], "off"))) {
        fprintf(stderr, "Usage: %s PID MINOR on|off\n", argv[0]);
        return 2;
    }
    pid = (pid_t)number(argv[1], 1, INT_MAX);
    value.sival_int = (int)number(argv[2], 0, 65535);
    signo = SIGRTMIN + (!strcmp(argv[3], "off"));
    if (sigqueue(pid, signo, value)) {
        perror("sigqueue");
        return 1;
    }
    return 0;
}
