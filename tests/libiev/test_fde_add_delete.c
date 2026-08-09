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

static struct fde_head *ad_fh;
static int ad_sv[2];

static void
ad_dummy_cb(int fd, struct fde *f, void *arg, fde_cb_status status)
{
	(void) fd;
	(void) f;
	(void) arg;
	(void) status;
}

static void
ad_setup(void)
{
	ad_fh = fde_ctx_new();
	socketpair(AF_UNIX, SOCK_STREAM, 0, ad_sv);
}

static void
ad_teardown(void)
{
	if (ad_fh != NULL) {
		close(ad_fh->kqfd);
		free(ad_fh);
		ad_fh = NULL;
	}
	close(ad_sv[0]);
	close(ad_sv[1]);
}

MU_TEST(test_fde_add_idempotent)
{
	struct fde *f;

	f = fde_create(ad_fh, ad_sv[0], FDE_T_READ, 0, ad_dummy_cb, NULL);
	mu_assert(f != NULL, "fde_create returned NULL");

	fde_add(ad_fh, f);
	mu_check(f->is_active == 1);

	/* Second add should be a no-op */
	fde_add(ad_fh, f);
	mu_check(f->is_active == 1);

	fde_free(ad_fh, f);
}

MU_TEST(test_fde_delete_idempotent)
{
	struct fde *f;

	f = fde_create(ad_fh, ad_sv[0], FDE_T_READ, 0, ad_dummy_cb, NULL);
	mu_assert(f != NULL, "fde_create returned NULL");

	/* Delete without add should be a no-op */
	fde_delete(ad_fh, f);
	mu_check(f->is_active == 0);

	fde_free(ad_fh, f);
}

MU_TEST(test_fde_add_delete_cycle)
{
	struct fde *f;

	f = fde_create(ad_fh, ad_sv[0], FDE_T_READ, 0, ad_dummy_cb, NULL);
	mu_assert(f != NULL, "fde_create returned NULL");

	/* Add */
	fde_add(ad_fh, f);
	mu_check(f->is_active == 1);

	/* Delete */
	fde_delete(ad_fh, f);
	mu_check(f->is_active == 0);

	/* Re-add */
	fde_add(ad_fh, f);
	mu_check(f->is_active == 1);

	fde_free(ad_fh, f);
}

void
fde_add_delete_suite(void)
{
	MU_SUITE_CONFIGURE(ad_setup, ad_teardown);

	MU_RUN_TEST(test_fde_add_idempotent);
	MU_RUN_TEST(test_fde_delete_idempotent);
	MU_RUN_TEST(test_fde_add_delete_cycle);
}
