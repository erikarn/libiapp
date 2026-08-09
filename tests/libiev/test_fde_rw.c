/*-
 * Copyright (c) 2026 Adrian Chadd <adrian@FreeBSD.org>.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "libiev/fde.h"
#include "minunit.h"

static struct fde_head *rw_fh;
static int rw_sv[2];
static int rw_read_fired;
static int rw_write_fired;
static int rw_drain_on_read;
static int rw_delete_after_fire;
static struct fde_head *rw_fh_ref;

static void
rw_setup(void)
{
	rw_fh = fde_ctx_new();
	rw_fh_ref = rw_fh;
	socketpair(AF_UNIX, SOCK_STREAM, 0, rw_sv);
	rw_read_fired = 0;
	rw_write_fired = 0;
	rw_drain_on_read = 0;
	rw_delete_after_fire = 0;
}

static void
rw_teardown(void)
{
	if (rw_fh != NULL) {
		close(rw_fh->kqfd);
		free(rw_fh);
		rw_fh = NULL;
	}
	close(rw_sv[0]);
	close(rw_sv[1]);
}

static void
rw_read_cb(int fd, struct fde *f, void *arg, fde_cb_status status)
{
	char drainbuf[256];
	ssize_t n;

	(void) arg;
	(void) status;
	rw_read_fired++;
	if (rw_drain_on_read) {
		/*
		 * Single read to consume available data.
		 * Cannot loop because the socket is blocking.
		 */
		n = read(fd, drainbuf, sizeof(drainbuf));
		(void) n;
	}
	if (rw_delete_after_fire)
		fde_delete(rw_fh_ref, f);
}

static void
rw_write_cb(int fd, struct fde *f, void *arg, fde_cb_status status)
{
	(void) fd;
	(void) f;
	(void) arg;
	(void) status;
	rw_write_fired++;
}

MU_TEST(test_read_event_fires)
{
	struct fde *f;
	struct timeval timeout;
	char buf[] = "hello";

	/* Write data to one end so the other becomes readable */
	write(rw_sv[1], buf, sizeof(buf));

	f = fde_create(rw_fh, rw_sv[0], FDE_T_READ, 0, rw_read_cb, NULL);
	mu_assert(f != NULL, "fde_create READ failed");

	fde_add(rw_fh, f);

	timeout.tv_sec = 0;
	timeout.tv_usec = 100000;
	fde_runloop(rw_fh, &timeout);

	mu_assert_int_eq(1, rw_read_fired);

	fde_free(rw_fh, f);
}

MU_TEST(test_write_event_fires)
{
	struct fde *f;
	struct timeval timeout;

	/* Fresh socket should be writable immediately */
	f = fde_create(rw_fh, rw_sv[0], FDE_T_WRITE, 0, rw_write_cb, NULL);
	mu_assert(f != NULL, "fde_create WRITE failed");

	fde_add(rw_fh, f);

	timeout.tv_sec = 0;
	timeout.tv_usec = 100000;
	fde_runloop(rw_fh, &timeout);

	mu_assert_int_eq(1, rw_write_fired);

	fde_free(rw_fh, f);
}

MU_TEST(test_oneshot_deactivates)
{
	struct fde *f;
	struct timeval timeout;
	char buf[] = "test";

	write(rw_sv[1], buf, sizeof(buf));

	f = fde_create(rw_fh, rw_sv[0], FDE_T_READ, 0, rw_read_cb, NULL);
	mu_assert(f != NULL, "fde_create READ failed");

	fde_add(rw_fh, f);
	mu_check(f->is_active == 1);

	timeout.tv_sec = 0;
	timeout.tv_usec = 100000;
	fde_runloop(rw_fh, &timeout);

	/* Oneshot event should be deactivated after firing */
	mu_check(f->is_active == 0);

	fde_free(rw_fh, f);
}

MU_TEST(test_persist_stays_active)
{
	struct fde *f;
	struct timeval timeout;
	char buf[] = "persist_test_data";

	write(rw_sv[1], buf, sizeof(buf));

	rw_drain_on_read = 1;
	f = fde_create(rw_fh, rw_sv[0], FDE_T_READ, FDE_F_PERSIST,
	    rw_read_cb, NULL);
	mu_assert(f != NULL, "fde_create READ PERSIST failed");

	fde_add(rw_fh, f);
	mu_check(f->is_active == 1);

	timeout.tv_sec = 0;
	timeout.tv_usec = 100000;
	fde_runloop(rw_fh, &timeout);

	/* Persistent event should remain active after firing */
	mu_check(f->is_active == 1);

	/* Clean up: delete the persistent event so runloop can exit */
	fde_delete(rw_fh, f);
	fde_free(rw_fh, f);
}

void
fde_rw_suite(void)
{
	MU_SUITE_CONFIGURE(rw_setup, rw_teardown);

	MU_RUN_TEST(test_read_event_fires);
	MU_RUN_TEST(test_write_event_fires);
	MU_RUN_TEST(test_oneshot_deactivates);
	MU_RUN_TEST(test_persist_stays_active);
}
