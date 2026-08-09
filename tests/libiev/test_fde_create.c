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

static struct fde_head *ctx_fh;
static int ctx_sv[2];

static void
dummy_cb(int fd, struct fde *f, void *arg, fde_cb_status status)
{
	(void) fd;
	(void) f;
	(void) arg;
	(void) status;
}

static void
create_setup(void)
{
	ctx_fh = fde_ctx_new();
	socketpair(AF_UNIX, SOCK_STREAM, 0, ctx_sv);
}

static void
create_teardown(void)
{
	if (ctx_fh != NULL) {
		close(ctx_fh->kqfd);
		free(ctx_fh);
		ctx_fh = NULL;
	}
	close(ctx_sv[0]);
	close(ctx_sv[1]);
}

MU_TEST(test_fde_create_read)
{
	struct fde *f;

	f = fde_create(ctx_fh, ctx_sv[0], FDE_T_READ, 0, dummy_cb, NULL);
	mu_assert(f != NULL, "fde_create READ returned NULL");
	mu_assert_int_eq(FDE_T_READ, f->f_type);
	mu_assert_int_eq(ctx_sv[0], f->fd);
	mu_check(f->cb == dummy_cb);
	mu_check(f->is_active == 0);

	fde_free(ctx_fh, f);
}

MU_TEST(test_fde_create_write)
{
	struct fde *f;

	f = fde_create(ctx_fh, ctx_sv[0], FDE_T_WRITE, 0, dummy_cb, NULL);
	mu_assert(f != NULL, "fde_create WRITE returned NULL");
	mu_assert_int_eq(FDE_T_WRITE, f->f_type);
	mu_assert_int_eq(ctx_sv[0], f->fd);

	fde_free(ctx_fh, f);
}

MU_TEST(test_fde_create_timer)
{
	struct fde *f;

	f = fde_create(ctx_fh, -1, FDE_T_TIMER, 0, dummy_cb, NULL);
	mu_assert(f != NULL, "fde_create TIMER returned NULL");
	mu_assert_int_eq(FDE_T_TIMER, f->f_type);
	mu_assert_int_eq(-1, f->fd);

	fde_free(ctx_fh, f);
}

MU_TEST(test_fde_create_callback)
{
	struct fde *f;

	f = fde_create(ctx_fh, -1, FDE_T_CALLBACK, 0, dummy_cb, NULL);
	mu_assert(f != NULL, "fde_create CALLBACK returned NULL");
	mu_assert_int_eq(FDE_T_CALLBACK, f->f_type);

	fde_free(ctx_fh, f);
}

MU_TEST(test_fde_create_user)
{
	struct fde *f;

	f = fde_create(ctx_fh, -1, FDE_T_USER, FDE_F_PERSIST, dummy_cb, NULL);
	mu_assert(f != NULL, "fde_create USER returned NULL");
	mu_assert_int_eq(FDE_T_USER, f->f_type);
	mu_assert((uintptr_t) f == f->kev.ident,
	    "USER event ident should be (uintptr_t)f");

	fde_free(ctx_fh, f);
}

MU_TEST(test_fde_create_invalid_type)
{
	struct fde *f;

	f = fde_create(ctx_fh, -1, (fde_type) 99, 0, dummy_cb, NULL);
	mu_check(f == NULL);
}

MU_TEST(test_fde_free_inactive)
{
	struct fde *f;

	f = fde_create(ctx_fh, ctx_sv[0], FDE_T_READ, 0, dummy_cb, NULL);
	mu_assert(f != NULL, "fde_create returned NULL");

	/* Free without add; should not crash */
	fde_free(ctx_fh, f);
}

MU_TEST(test_fde_free_active)
{
	struct fde *f;

	f = fde_create(ctx_fh, ctx_sv[0], FDE_T_READ, 0, dummy_cb, NULL);
	mu_assert(f != NULL, "fde_create returned NULL");

	fde_add(ctx_fh, f);
	mu_check(f->is_active == 1);

	/* Free while active; fde_free should call fde_delete first */
	fde_free(ctx_fh, f);
}

void
fde_create_suite(void)
{
	MU_SUITE_CONFIGURE(create_setup, create_teardown);

	MU_RUN_TEST(test_fde_create_read);
	MU_RUN_TEST(test_fde_create_write);
	MU_RUN_TEST(test_fde_create_timer);
	MU_RUN_TEST(test_fde_create_callback);
	MU_RUN_TEST(test_fde_create_user);
	MU_RUN_TEST(test_fde_create_invalid_type);
	MU_RUN_TEST(test_fde_free_inactive);
	MU_RUN_TEST(test_fde_free_active);
}
