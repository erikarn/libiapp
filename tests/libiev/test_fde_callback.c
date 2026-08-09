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
#include <sys/types.h>
#include <sys/event.h>
#include <sys/queue.h>
#include <sys/time.h>

#include "libiev/fde.h"
#include "minunit.h"

static struct fde_head *cb_fh;
static int cb_fired_count;
static struct fde_head *cb_fh_ref;

static void
cb_setup(void)
{
	cb_fh = fde_ctx_new();
	cb_fh_ref = cb_fh;
	cb_fired_count = 0;
}

static void
cb_teardown(void)
{
	if (cb_fh != NULL) {
		close(cb_fh->kqfd);
		free(cb_fh);
		cb_fh = NULL;
	}
}

static void
cb_simple_cb(int fd, struct fde *f, void *arg, fde_cb_status status)
{
	(void) fd;
	(void) f;
	(void) arg;
	(void) status;
	cb_fired_count++;
}

static void
cb_nested_cb(int fd, struct fde *f, void *arg, fde_cb_status status)
{
	struct fde *nested;

	(void) fd;
	(void) f;
	(void) arg;
	(void) status;
	cb_fired_count++;

	/* Schedule a new callback from within a callback */
	nested = fde_create(cb_fh_ref, -1, FDE_T_CALLBACK, 0,
	    cb_simple_cb, NULL);
	if (nested != NULL)
		fde_add(cb_fh_ref, nested);
}

MU_TEST(test_callback_fires)
{
	struct fde *f;
	struct timeval timeout;

	f = fde_create(cb_fh, -1, FDE_T_CALLBACK, 0, cb_simple_cb, NULL);
	mu_assert(f != NULL, "callback create failed");

	fde_add(cb_fh, f);

	timeout.tv_sec = 0;
	timeout.tv_usec = 10000;
	fde_runloop(cb_fh, &timeout);

	mu_assert_int_eq(1, cb_fired_count);
}

MU_TEST(test_callback_genid)
{
	struct fde *f;
	struct timeval timeout;

	/*
	 * Schedule a callback that itself schedules another callback.
	 * The nested callback should NOT fire in the same runloop
	 * iteration due to the genid guard.
	 */
	f = fde_create(cb_fh, -1, FDE_T_CALLBACK, 0, cb_nested_cb, NULL);
	mu_assert(f != NULL, "callback create failed");

	fde_add(cb_fh, f);

	timeout.tv_sec = 0;
	timeout.tv_usec = 10000;
	fde_runloop(cb_fh, &timeout);

	/* Only the outer callback should have fired */
	mu_assert_int_eq(1, cb_fired_count);

	/* Run again; now the nested callback should fire */
	timeout.tv_sec = 0;
	timeout.tv_usec = 10000;
	fde_runloop(cb_fh, &timeout);

	mu_assert_int_eq(2, cb_fired_count);
}

void
fde_callback_suite(void)
{
	MU_SUITE_CONFIGURE(cb_setup, cb_teardown);

	MU_RUN_TEST(test_callback_fires);
	MU_RUN_TEST(test_callback_genid);
}
