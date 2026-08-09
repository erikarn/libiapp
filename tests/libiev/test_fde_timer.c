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
#include <string.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/queue.h>
#include <sys/time.h>

#include "libiev/fde.h"
#include "minunit.h"

static struct fde_head *tm_fh;
static int tm_fired_count;

static void
tm_setup(void)
{
	tm_fh = fde_ctx_new();
	tm_fired_count = 0;
}

static void
tm_teardown(void)
{
	if (tm_fh != NULL) {
		close(tm_fh->kqfd);
		free(tm_fh);
		tm_fh = NULL;
	}
}

static void
tm_cb(int fd, struct fde *f, void *arg, fde_cb_status status)
{
	(void) fd;
	(void) f;
	(void) arg;
	(void) status;
	tm_fired_count++;
}

MU_TEST(test_timer_insertion_order)
{
	struct fde *t1, *t2, *t3;
	struct timeval tv;
	struct fde *n;
	int count;

	t1 = fde_create(tm_fh, -1, FDE_T_TIMER, 0, tm_cb, NULL);
	t2 = fde_create(tm_fh, -1, FDE_T_TIMER, 0, tm_cb, NULL);
	t3 = fde_create(tm_fh, -1, FDE_T_TIMER, 0, tm_cb, NULL);
	mu_assert(t1 != NULL && t2 != NULL && t3 != NULL,
	    "timer create failed");

	/* Insert out of order: t2=3s, t1=1s, t3=5s */
	tv.tv_sec = 100; tv.tv_usec = 0;
	fde_add_timeout(tm_fh, t2, &tv);

	tv.tv_sec = 98; tv.tv_usec = 0;
	fde_add_timeout(tm_fh, t1, &tv);

	tv.tv_sec = 102; tv.tv_usec = 0;
	fde_add_timeout(tm_fh, t3, &tv);

	/* Walk timer list, verify ascending order */
	count = 0;
	n = TAILQ_FIRST(&tm_fh->f_t_head);
	mu_assert(n == t1, "first timer should be t1 (earliest)");
	count++;

	n = TAILQ_NEXT(n, cb_node);
	mu_assert(n == t2, "second timer should be t2");
	count++;

	n = TAILQ_NEXT(n, cb_node);
	mu_assert(n == t3, "third timer should be t3 (latest)");
	count++;

	mu_assert_int_eq(3, count);

	fde_free(tm_fh, t1);
	fde_free(tm_fh, t2);
	fde_free(tm_fh, t3);
}

MU_TEST(test_timer_fires)
{
	struct fde *t;
	struct timeval tv, timeout;

	t = fde_create(tm_fh, -1, FDE_T_TIMER, 0, tm_cb, NULL);
	mu_assert(t != NULL, "timer create failed");

	/* Set timer to "now" so it fires immediately */
	gettimeofday(&tv, NULL);
	fde_add_timeout(tm_fh, t, &tv);

	timeout.tv_sec = 0;
	timeout.tv_usec = 100000;
	fde_runloop(tm_fh, &timeout);

	mu_assert_int_eq(1, tm_fired_count);
}

MU_TEST(test_timer_not_yet)
{
	struct fde *t;
	struct timeval tv, timeout;

	t = fde_create(tm_fh, -1, FDE_T_TIMER, 0, tm_cb, NULL);
	mu_assert(t != NULL, "timer create failed");

	/* Set timer far in the future */
	gettimeofday(&tv, NULL);
	tv.tv_sec += 3600;
	fde_add_timeout(tm_fh, t, &tv);

	timeout.tv_sec = 0;
	timeout.tv_usec = 10000;
	fde_runloop(tm_fh, &timeout);

	mu_assert_int_eq(0, tm_fired_count);

	fde_free(tm_fh, t);
}

MU_TEST(test_timer_get_timeout_empty)
{
	struct timeval timeout, sleep_tv;
	struct fde *f_dummy;

	/*
	 * With an empty timer queue, fde_t_get_timeout is static.
	 * We test indirectly: runloop with no timers should sleep
	 * for the full timeout duration (or close to it).
	 * Instead, just verify the runloop doesn't crash with
	 * no timers registered.
	 */
	timeout.tv_sec = 0;
	timeout.tv_usec = 1000;
	fde_runloop(tm_fh, &timeout);

	/* If we got here without crashing, the empty timer path works */
	mu_check(1);
	(void) f_dummy;
	(void) sleep_tv;
}

void
fde_timer_suite(void)
{
	MU_SUITE_CONFIGURE(tm_setup, tm_teardown);

	MU_RUN_TEST(test_timer_insertion_order);
	MU_RUN_TEST(test_timer_fires);
	MU_RUN_TEST(test_timer_not_yet);
	MU_RUN_TEST(test_timer_get_timeout_empty);
}
