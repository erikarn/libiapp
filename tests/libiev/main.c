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
#include <signal.h>
#include <unistd.h>
#include "minunit.h"

#define	TEST_TIMEOUT_SECS	30

static void
alarm_handler(int sig)
{
	(void) sig;
	fprintf(stderr, "\nFAIL: test timed out after %d seconds\n",
	    TEST_TIMEOUT_SECS);
	_exit(99);
}

extern void fde_ctx_suite(void);
extern void fde_create_suite(void);
extern void fde_add_delete_suite(void);
extern void fde_timer_suite(void);
extern void fde_callback_suite(void);
extern void fde_rw_suite(void);
extern void fde_user_suite(void);
extern void fde_shutdown_suite(void);

int
main(int argc, char *argv[])
{
	(void) argc;
	(void) argv;

	signal(SIGALRM, alarm_handler);
	alarm(TEST_TIMEOUT_SECS);

	MU_RUN_SUITE(fde_ctx_suite);
	MU_RUN_SUITE(fde_create_suite);
	MU_RUN_SUITE(fde_add_delete_suite);
	MU_RUN_SUITE(fde_timer_suite);
	MU_RUN_SUITE(fde_callback_suite);
	MU_RUN_SUITE(fde_rw_suite);
	MU_RUN_SUITE(fde_user_suite);
	MU_RUN_SUITE(fde_shutdown_suite);

	MU_REPORT();

	return (MU_EXIT_CODE);
}
