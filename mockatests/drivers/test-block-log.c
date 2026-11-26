/*
 * Copyright (c) 2026, Vates
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

/*
 * Tests of the CBT dirty log driver (block-log.c), exercised through its
 * tap_disk ops with real log files.
 *
 * The bitmap is mmap()ed from the log file with a size computed from the
 * disk size. If the disk is grown (live VDI resize) without growing the
 * log file first, the open must be refused instead of mapping past the
 * end of the file (SIGBUS on the first write to the new area).
 */

#include <stddef.h>
#include <stdarg.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "test-suites.h"
#include "tapdisk.h"
#include "tapdisk-driver.h"
#include "block-log.h"

extern struct tap_disk tapdisk_log;

#define SECS_1G        ((td_sector_t)(1ULL << 21))  /* 1 GiB in 512B sectors */
#define SECS_2G        ((td_sector_t)(1ULL << 22))
#define SECS_PER_BLOCK (CBT_BLOCK_SIZE / SECTOR_SIZE)

struct log_fixture {
	char               path[64];
	struct tdlog_data  data;
	td_driver_t        driver;
};

static uint64_t
log_file_size(td_sector_t disk_secs)
{
	return bitmap_size(disk_secs * SECTOR_SIZE) +
		sizeof(struct cbt_log_metadata);
}

static void
resize_log_file(const char *path, off_t size)
{
	int fd = open(path, O_RDWR);

	assert_true(fd >= 0);
	assert_int_equal(ftruncate(fd, size), 0);
	close(fd);
}

static int
log_open(struct log_fixture *f, td_sector_t disk_secs)
{
	f->driver.info.size = disk_secs;
	return tapdisk_log.td_open(&f->driver, f->path, NULL, 0);
}

static void
log_close(struct log_fixture *f)
{
	tapdisk_log.td_close(&f->driver);
}

/* Write one sector through the driver */
static void
log_write(struct log_fixture *f, td_sector_t sec)
{
	td_request_t treq;

	memset(&treq, 0, sizeof(treq));
	treq.op   = TD_OP_WRITE;
	treq.sec  = sec;
	treq.secs = 1;

	expect_value(__wrap_td_forward_request, treq.sec, sec);
	expect_value(__wrap_td_forward_request, treq.secs, 1);
	tapdisk_log.td_queue_write(&f->driver, treq);
}

/* Same layout as the driver: unsigned longs after the metadata header */
static int
bit_is_set(const void *bitmap, uint64_t nr)
{
	const unsigned long *bits = (const unsigned long *)
		((const char *)bitmap + sizeof(struct cbt_log_metadata));
	const unsigned int bpl = sizeof(unsigned long) * 8;

	return (bits[nr / bpl] >> (nr % bpl)) & 1;
}

int
block_log_setup(void **state)
{
	struct log_fixture *f = calloc(1, sizeof(*f));
	int fd;

	if (!f)
		return -1;

	strcpy(f->path, "/tmp/test-block-log-XXXXXX");
	fd = mkstemp(f->path);
	if (fd < 0) {
		free(f);
		return -1;
	}
	close(fd);

	f->driver.name = f->path;
	f->driver.data = &f->data;

	*state = f;
	return 0;
}

int
block_log_teardown(void **state)
{
	struct log_fixture *f = *state;

	unlink(f->path);
	free(f);
	return 0;
}

/* Live resize sequence: pause, grow disk, grow log, resume */


void
test_block_log_live_resize(void **state)
{
	struct log_fixture *f = *state;
	uint64_t old_block = (SECS_1G - 1) / SECS_PER_BLOCK;
	uint64_t new_block = (SECS_2G - 1) / SECS_PER_BLOCK;

	/* Running VM, 1 GiB disk, one dirty block */
	resize_log_file(f->path, log_file_size(SECS_1G));
	assert_int_equal(log_open(f, SECS_1G), 0);
	log_write(f, SECS_1G - 1);
	log_close(f);                                       /* pause */

	/* Disk grown, log not yet: resume must fail cleanly */
	assert_int_equal(log_open(f, SECS_2G), -EINVAL);
	assert_null(f->data.bitmap);

	/* Log grown (cbt-util set -s): resume succeeds, dirty bits kept */
	resize_log_file(f->path, log_file_size(SECS_2G));
	assert_int_equal(log_open(f, SECS_2G), 0);
	assert_true(bit_is_set(f->data.bitmap, old_block));

	/* The mapping covers the grown area */
	log_write(f, SECS_2G - 1);
	assert_true(bit_is_set(f->data.bitmap, new_block));
	log_close(f);
}
