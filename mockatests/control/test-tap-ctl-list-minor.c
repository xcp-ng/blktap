/*
 * Copyright (c) 2026, Cloud Software Group, Inc.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <cmocka.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <wrappers.h>
#include "control-wrappers.h"
#include "util.h"
#include "test-suites.h"

#include "tap-ctl.h"
#include "blktap.h"

/*
 * tap_ctl_list_minor() locates the tapdisk serving a minor from the flock it
 * holds on the minor's marker file, discovered via /proc/locks. These tests
 * drive the marker stat, the /proc/locks read and (on the fast path) a single
 * control-socket round-trip.
 *
 * The marker's device is fabricated as makedev(0, 0x45), so /proc/locks entries
 * use "00:45" as the major:minor (hex) field.
 */

#define TEST_MINOR	5
#define TEST_MARKER	"/run/blktap-control/tapdisk/tapdisk-5"
#define TEST_INO	230

/* Sets up the marker stat() to succeed with our fabricated dev/inode. */
static void
expect_marker_stat(struct stat *st)
{
	memset(st, 0, sizeof(*st));
	st->st_dev = makedev(0, 0x45);
	st->st_ino = TEST_INO;

	expect_string(__wrap_stat, pathname, TEST_MARKER);
	will_return(__wrap_stat, 0);
	will_return(__wrap_stat, st);
}

/*
 * Points the code's fopen("/proc/locks") at an in-memory buffer holding the
 * supplied text, so the real fgets()-based parser runs against it. The buffer
 * (text) must outlive the call under test.
 */
static void
expect_proc_locks(const char *text)
{
	FILE *f = fmemopen((void *)text, strlen(text), "r");

	will_return(__wrap_fopen, f);
	expect_value(__wrap_fclose, fp, f);
}

/*
 * Fast path: the marker is held, /proc/locks names the owning tapdisk, and a
 * single LIST round-trip to that tapdisk returns the minor. A leading POSIX
 * lock and a FLOCK on a different inode must both be ignored.
 */
void test_tap_ctl_list_minor_served(void **state)
{
	int err;
	tap_list_t *entry;
	struct list_head list = LIST_HEAD_INIT(list);
	struct stat st;
	int ipc_socket = 7;
	char *expected_sock_name = "/run/blktap-control/ctl1236";
	tapdisk_message_t write_message;
	tapdisk_message_t *read_message;
	struct mock_ipc_params *list_ipc_params;
	char *vdi_path = "vhd:/dev/VG/some-vhd";
	char locks[] =
		"1: POSIX  ADVISORY  WRITE 999 fd:01:111 0 EOF\n"
		"2: FLOCK  ADVISORY  WRITE 4242 00:45:999 0 EOF\n"
		"3: FLOCK  ADVISORY  WRITE 1236 00:45:230 0 EOF\n";

	expect_marker_stat(&st);
	expect_proc_locks(locks);

	/* Single LIST round-trip to the owning tapdisk (pid 1236) */
	memset(&write_message, 0, sizeof(write_message));
	write_message.type = TAPDISK_MESSAGE_LIST;
	write_message.cookie = -1;

	read_message = test_malloc(sizeof(*read_message) * 2);
	memset(read_message, 0, sizeof(*read_message) * 2);
	read_message[0].type = TAPDISK_MESSAGE_LIST_RSP;
	read_message[0].u.list.count = 1;
	read_message[0].u.list.minor = TEST_MINOR;
	read_message[0].u.list.state = 0;
	strcpy(read_message[0].u.list.path, vdi_path);
	read_message[1].type = TAPDISK_MESSAGE_LIST_RSP;
	read_message[1].u.list.count = 0;
	read_message[1].u.list.minor = -1;
	read_message[1].u.list.state = -1;
	read_message[1].u.list.path[0] = 0;

	list_ipc_params = setup_ipc(
		expected_sock_name, ipc_socket,
		&write_message, read_message, 2);

	test_free(read_message);

	err = tap_ctl_list_minor(TEST_MINOR, &list);

	assert_int_equal(0, err);
	assert_true(list_is_singular(&list));

	tap_list_for_each_entry(entry, &list) {
		assert_int_equal(TEST_MINOR, entry->minor);
		assert_int_equal(1236, entry->pid);
		assert_string_equal("vhd", entry->type);
		assert_string_equal("/dev/VG/some-vhd", entry->path);
	}

	tap_ctl_list_free(&list);
	free_ipc_params(list_ipc_params);
}

/*
 * The marker exists but no tapdisk holds an flock on it (allocated but
 * unserved): an orphan entry is reported and no tapdisk is contacted.
 */
void test_tap_ctl_list_minor_orphan(void **state)
{
	int err;
	tap_list_t *entry;
	struct list_head list = LIST_HEAD_INIT(list);
	struct stat st;
	/* A FLOCK for a different inode must not match. */
	char locks[] =
		"1: POSIX  ADVISORY  WRITE 999 fd:01:111 0 EOF\n"
		"2: FLOCK  ADVISORY  WRITE 4242 00:45:999 0 EOF\n";

	expect_marker_stat(&st);
	expect_proc_locks(locks);

	err = tap_ctl_list_minor(TEST_MINOR, &list);

	assert_int_equal(0, err);
	assert_true(list_is_singular(&list));

	tap_list_for_each_entry(entry, &list) {
		assert_int_equal(TEST_MINOR, entry->minor);
		assert_int_equal(-1, entry->pid);
		assert_int_equal(-1, entry->state);
	}

	tap_ctl_list_free(&list);
}

/* The minor is not allocated: the marker stat fails with ENOENT, empty list. */
void test_tap_ctl_list_minor_absent(void **state)
{
	int err;
	struct list_head list = LIST_HEAD_INIT(list);

	expect_string(__wrap_stat, pathname, TEST_MARKER);
	will_return(__wrap_stat, -1);
	will_return(__wrap_stat, ENOENT);

	err = tap_ctl_list_minor(TEST_MINOR, &list);

	assert_int_equal(0, err);
	assert_true(list_empty(&list));
}

/*
 * A matching flock exists but its holder is invisible in our pid namespace
 * (pid 0). This falls back to the full tap_ctl_list() scan, which here finds
 * nothing.
 */
void test_tap_ctl_list_minor_fallback(void **state)
{
	int err;
	struct list_head list = LIST_HEAD_INIT(list);
	struct stat st;
	char locks[] = "3: FLOCK  ADVISORY  WRITE 0 00:45:230 0 EOF\n";

	expect_marker_stat(&st);
	expect_proc_locks(locks);

	/* Fallback: full scan finds no minors and no tapdisks. */
	expect_string(__wrap_glob, pattern, "/run/blktap-control/tapdisk/tapdisk*");
	will_return(__wrap_glob, GLOB_NOMATCH);
	expect_string(__wrap_glob, pattern, "/run/blktap-control/ctl*");
	will_return(__wrap_glob, GLOB_NOMATCH);

	err = tap_ctl_list_minor(TEST_MINOR, &list);

	assert_int_equal(0, err);
	assert_true(list_empty(&list));
}
