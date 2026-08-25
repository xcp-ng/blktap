/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2.1 only
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "tapdisk.h"
#include "tapdisk-utils.h"
#include "tapdisk-server.h"
#include "tapdisk-control.h"
#include "tapdisk-metrics.h"

#ifdef HAVE_LTTNG
# include <signal.h>
# include <lttng/ust-version.h>
# if LTTNG_UST_MAJOR_VERSION > 2 || \
     (LTTNG_UST_MAJOR_VERSION == 2 && LTTNG_UST_MINOR_VERSION >= 13)
#  include <lttng/ust-fork.h>
# else
#  include <lttng/ust.h>
#  define lttng_ust_before_fork		ust_before_fork
#  define lttng_ust_after_fork_child	ust_after_fork_child
#  define lttng_ust_after_fork_parent	ust_after_fork_parent
# endif
#endif

void tdnbd_fdreceiver_start();
void tdnbd_fdreceiver_stop();

static void
usage(const char *app, int err)
{
	fprintf(stderr, "usage: %s <-u uuid> <-c control socket>\n", app);
	exit(err);
}

/*
 * Tracing across a fork(2) without exec(3) requires the LTTng-UST handlers
 * below, otherwise the child loses its tracing threads. Calling them here
 * spares us from LD_PRELOADing liblttng-ust-fork.so. See lttng-ust(3),
 * section "Using LTTng-UST with daemons".
 */
static int
daemonize(int nochdir, int noclose)
{
#ifdef HAVE_LTTNG
	sigset_t sigset;
	int err;

	lttng_ust_before_fork(&sigset);

	err = daemon(nochdir, noclose);

	if (err == 0)
		lttng_ust_after_fork_child(&sigset);
	else
		lttng_ust_after_fork_parent(&sigset);

	return err;
#else
	return daemon(nochdir, noclose);
#endif
}

static FILE *
fdup(FILE *stream, const char *mode)
{
	int fd, err;
	FILE *f;

	fd = dup(STDOUT_FILENO);
	if (fd < 0)
		goto fail;

	f = fdopen(fd, mode);
	if (!f)
		goto fail;

	return f;

fail:
	err = -errno;
	if (fd >= 0)
		close(fd);
	errno = -err;

	return NULL;
}

int
main(int argc, char *argv[])
{
	char *control;
	int c, err, nodaemon;
	FILE *out;

	control  = NULL;
	nodaemon = 0;

	while ((c = getopt(argc, argv, "Dh")) != -1) {
		switch (c) {
		case 'D':
			nodaemon = 1;
			break;
		case 'h':
			usage(argv[0], 0);
			break;
		default:
			usage(argv[0], EINVAL);
		}
	}

	if (optind != argc)
		usage(argv[0], EINVAL);

	err = tapdisk_server_init();
	if (err) {
		DPRINTF("failed to initialize server: %d\n", err);
		goto out;
	}

	out = fdup(stdout, "w");
	if (!out) {
		err = -errno;
		DPRINTF("failed to dup stdout: %d\n", err);
		goto out;
	}

	if (!nodaemon) {
		err = daemonize(0, 0);
		if (err) {
			DPRINTF("failed to daemonize: %d\n", errno);
			goto out;
		}
	}

	tapdisk_start_logging("tapdisk", NULL);

	err = tapdisk_control_open(&control);
	if (err) {
		DPRINTF("failed to open control socket: %d\n", err);
		goto out;
	}

	err = tapdisk_server_complete();
	if (err) {
		DPRINTF("failed to complete server: %d\n", err);
		goto out;
	}

	DPRINTF("Tapdisk running, control on %s\n", control);

	fprintf(out, "%s\n", control);
	fclose(out);

	err = td_metrics_start();
	if (err) {
		DPRINTF("failed to create metrics folder: %d\n", err);
		goto out;
	}
	/*
	 * NB: We're unconditionally starting the FD receiver here - this is 
	 * for the block-nbd driver. In the future we may want to start this as 
	 * a response to a tap-ctl message
	 */
	tdnbd_fdreceiver_start();

	err = tapdisk_server_run();

out:
	if (err) {
		EPRINTF("Tapdisk exiting with error %d\n", err);
	}
	td_metrics_stop();
	tdnbd_fdreceiver_stop();
	tapdisk_control_close();
	tapdisk_stop_logging();
	return -err;
}
