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
#include <sys/types.h>
#include <sys/event.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "libiev/fde.h"
#include "minunit.h"

static struct fde_head *sd_fh;
static int sd_sv[2];

static void
sd_dummy_cb(int fd, struct fde *f, void *arg, fde_cb_status status)
{
	(void) fd;
	(void) f;
	(void) arg;
	(void) status;
}

static void
sd_setup(void)
{
	sd_fh = fde_ctx_new();
	socketpair(AF_UNIX, SOCK_STREAM, 0, sd_sv);
}

static void
sd_teardown(void)
{
	if (sd_fh != NULL) {
		close(sd_fh->kqfd);
		free(sd_fh);
		sd_fh = NULL;
	}
	close(sd_sv[0]);
	close(sd_sv[1]);
}

MU_TEST(test_shutdown_sets_inactive)
{
	mu_check(sd_fh->is_active == true);
	fde_ctx_shutdown(sd_fh);
	mu_check(sd_fh->is_active == false);
}

MU_TEST(test_shutdown_blocks_fde_add_read)
{
	struct fde *f;

	f = fde_create(sd_fh, sd_sv[0], FDE_T_READ, 0, sd_dummy_cb, NULL);
	mu_assert(f != NULL, "fde_create returned NULL");

	fde_ctx_shutdown(sd_fh);

	mu_check(fde_add(sd_fh, f) == false);
	mu_check(f->is_active == 0);

	fde_free(sd_fh, f);
}

MU_TEST(test_shutdown_blocks_fde_add_write)
{
	struct fde *f;

	f = fde_create(sd_fh, sd_sv[0], FDE_T_WRITE, 0, sd_dummy_cb, NULL);
	mu_assert(f != NULL, "fde_create returned NULL");

	fde_ctx_shutdown(sd_fh);

	mu_check(fde_add(sd_fh, f) == false);
	mu_check(f->is_active == 0);

	fde_free(sd_fh, f);
}

MU_TEST(test_shutdown_blocks_fde_add_callback)
{
	struct fde *f;

	f = fde_create(sd_fh, -1, FDE_T_CALLBACK, 0, sd_dummy_cb, NULL);
	mu_assert(f != NULL, "fde_create returned NULL");

	fde_ctx_shutdown(sd_fh);

	mu_check(fde_add(sd_fh, f) == false);
	mu_check(f->is_active == 0);

	fde_free(sd_fh, f);
}

MU_TEST(test_shutdown_blocks_fde_add_timeout)
{
	struct fde *f;
	struct timeval tv;

	f = fde_create(sd_fh, -1, FDE_T_TIMER, 0, sd_dummy_cb, NULL);
	mu_assert(f != NULL, "fde_create returned NULL");

	fde_ctx_shutdown(sd_fh);

	gettimeofday(&tv, NULL);
	tv.tv_sec += 10;
	mu_check(fde_add_timeout(sd_fh, f, &tv) == false);
	mu_check(f->is_active == 0);

	fde_free(sd_fh, f);
}

MU_TEST(test_shutdown_after_add_prevents_readd)
{
	struct fde *f;

	f = fde_create(sd_fh, sd_sv[0], FDE_T_READ, 0, sd_dummy_cb, NULL);
	mu_assert(f != NULL, "fde_create returned NULL");

	mu_check(fde_add(sd_fh, f) == true);
	mu_check(f->is_active == 1);

	fde_ctx_shutdown(sd_fh);

	fde_delete(sd_fh, f);
	mu_check(f->is_active == 0);

	mu_check(fde_add(sd_fh, f) == false);
	mu_check(f->is_active == 0);

	fde_free(sd_fh, f);
}

MU_TEST(test_shutdown_blocks_fde_delete_callback)
{
	struct fde *f;

	f = fde_create(sd_fh, -1, FDE_T_CALLBACK, 0, sd_dummy_cb, NULL);
	mu_assert(f != NULL, "fde_create returned NULL");

	mu_check(fde_add(sd_fh, f) == true);
	mu_check(f->is_active == 1);

	fde_ctx_shutdown(sd_fh);

	mu_check(fde_delete(sd_fh, f) == false);
	mu_check(f->is_active == 1);

	fde_free(sd_fh, f);
}

MU_TEST(test_shutdown_blocks_fde_delete_timer)
{
	struct fde *f;
	struct timeval tv;

	f = fde_create(sd_fh, -1, FDE_T_TIMER, 0, sd_dummy_cb, NULL);
	mu_assert(f != NULL, "fde_create returned NULL");

	gettimeofday(&tv, NULL);
	tv.tv_sec += 10;
	mu_check(fde_add_timeout(sd_fh, f, &tv) == true);
	mu_check(f->is_active == 1);

	fde_ctx_shutdown(sd_fh);

	mu_check(fde_delete(sd_fh, f) == false);
	mu_check(f->is_active == 1);

	fde_free(sd_fh, f);
}

MU_TEST(test_new_ctx_is_active)
{
	mu_check(sd_fh->is_active == true);
}

void
fde_shutdown_suite(void)
{
	MU_SUITE_CONFIGURE(sd_setup, sd_teardown);

	MU_RUN_TEST(test_new_ctx_is_active);
	MU_RUN_TEST(test_shutdown_sets_inactive);
	MU_RUN_TEST(test_shutdown_blocks_fde_add_read);
	MU_RUN_TEST(test_shutdown_blocks_fde_add_write);
	MU_RUN_TEST(test_shutdown_blocks_fde_add_callback);
	MU_RUN_TEST(test_shutdown_blocks_fde_add_timeout);
	MU_RUN_TEST(test_shutdown_after_add_prevents_readd);
	MU_RUN_TEST(test_shutdown_blocks_fde_delete_callback);
	MU_RUN_TEST(test_shutdown_blocks_fde_delete_timer);
}
