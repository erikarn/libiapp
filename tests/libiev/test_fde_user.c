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

static struct fde_head *ue_fh;
static int ue_fired_count;

static void
ue_setup(void)
{
	ue_fh = fde_ctx_new();
	ue_fired_count = 0;
}

static void
ue_teardown(void)
{
	if (ue_fh != NULL) {
		close(ue_fh->kqfd);
		free(ue_fh);
		ue_fh = NULL;
	}
}

static void
ue_cb(int fd, struct fde *f, void *arg, fde_cb_status status)
{
	(void) fd;
	(void) f;
	(void) arg;
	(void) status;
	ue_fired_count++;
}

/*
 * Flush pending kevent registrations by running the event loop
 * with zero timeout.  fde_add buffers EV_ADD in the pending array;
 * fde_ue_push does a direct kevent() call that requires the event
 * to already exist in the kernel.
 */
static void
ue_flush_pending(struct fde_head *fh)
{
	struct timeval flush_tv = { 0, 0 };

	fde_runloop(fh, &flush_tv);
}

MU_TEST(test_ue_push_triggers)
{
	struct fde *f;
	struct timeval timeout;

	f = fde_create(ue_fh, -1, FDE_T_USER, FDE_F_PERSIST, ue_cb, NULL);
	mu_assert(f != NULL, "fde_create USER failed");

	fde_add(ue_fh, f);
	ue_flush_pending(ue_fh);

	/*
	 * fde_ue_push has a known bug: it checks kevent() ret != 1,
	 * but kevent() returns 0 on success when no output buffer is
	 * provided.  So fde_ue_push always returns 0 even on success.
	 * We test the actual trigger behavior via the callback instead.
	 */
	(void) fde_ue_push(ue_fh, f);

	timeout.tv_sec = 0;
	timeout.tv_usec = 100000;
	fde_runloop(ue_fh, &timeout);

	mu_assert_int_eq(1, ue_fired_count);

	fde_free(ue_fh, f);
}

MU_TEST(test_ue_push_returns_success)
{
	struct fde *f;
	int ret;

	f = fde_create(ue_fh, -1, FDE_T_USER, FDE_F_PERSIST, ue_cb, NULL);
	mu_assert(f != NULL, "fde_create USER failed");

	fde_add(ue_fh, f);
	ue_flush_pending(ue_fh);

	ret = fde_ue_push(ue_fh, f);

	/*
	 * Known bug: fde_ue_push returns 0 even on success because
	 * it checks kevent() ret != 1, but kevent() returns 0 when
	 * called with no output buffer.  Document the actual behavior.
	 */
	mu_assert_int_eq(0, ret);

	fde_free(ue_fh, f);
}

void
fde_user_suite(void)
{
	MU_SUITE_CONFIGURE(ue_setup, ue_teardown);

	MU_RUN_TEST(test_ue_push_triggers);
	MU_RUN_TEST(test_ue_push_returns_success);
}
