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

MU_TEST(test_fde_ctx_new)
{
	struct fde_head *fh;

	fh = fde_ctx_new();
	mu_assert(fh != NULL, "fde_ctx_new returned NULL");
	mu_assert(fh->kqfd >= 0, "kqfd is not a valid fd");
	mu_check(TAILQ_EMPTY(&fh->f_head));
	mu_check(TAILQ_EMPTY(&fh->f_cb_head));
	mu_check(TAILQ_EMPTY(&fh->f_t_head));

	close(fh->kqfd);
	free(fh);
}

MU_TEST(test_fde_ctx_free_no_crash)
{
	struct fde_head *fh;

	fh = fde_ctx_new();
	mu_assert(fh != NULL, "fde_ctx_new returned NULL");

	/*
	 * fde_ctx_free is currently a stub (XXX TODO).
	 * Verify it doesn't crash when called.
	 */
	fde_ctx_free(fh);

	/* Manual cleanup since fde_ctx_free doesn't actually free yet */
	close(fh->kqfd);
	free(fh);
}

void
fde_ctx_suite(void)
{
	MU_RUN_TEST(test_fde_ctx_new);
	MU_RUN_TEST(test_fde_ctx_free_no_crash);
}
