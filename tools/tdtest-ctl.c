/*
 * Test-only: sends a tdtest control signal to one disk of a patched
 * tapdisk, see TDTEST.md.
 *
 * Usage: tdtest-ctl PID MINOR reload|off
 *
 * PID and MINOR must come from the same "tap-ctl list" row. The signal is
 * queued with sigqueue(), with the minor as its value: SIGRTMIN for reload
 * (read /run/tapdisk-test/MINOR.conf), SIGRTMIN+1 for off. Never send these
 * signals to an unpatched tapdisk: their default action terminates it.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

static long
number(const char *s, long low, long high)
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

int
main(int argc, char **argv)
{
	union sigval value;
	pid_t pid;
	int signo;

	if (argc != 4 ||
	    (strcmp(argv[3], "reload") && strcmp(argv[3], "off"))) {
		fprintf(stderr, "Usage: %s PID MINOR reload|off\n",
			argc > 0 ? argv[0] : "tdtest-ctl");
		return 2;
	}

	pid = (pid_t)number(argv[1], 1, INT_MAX);
	memset(&value, 0, sizeof(value));
	value.sival_int = (int)number(argv[2], 0, 65535);
	signo = strcmp(argv[3], "off") ? SIGRTMIN : SIGRTMIN + 1;

	if (sigqueue(pid, signo, value)) {
		perror("sigqueue");
		return 1;
	}

	return 0;
}
