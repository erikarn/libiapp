/*-
 * Copyright 2026 Adrian Chadd <adrian@FreeBSD.org>.
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
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <err.h>
#include <stdbool.h>
#include <pthread.h>

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include "shm_alloc.h"
#include "netbuf.h"
#include "fde.h"
#include "fd_util.h"
#include "comm.h"
#include "thr.h"

static struct libiapp_thr *
libiapp_thr_create(void)
{
	struct libiapp_thr *t;

	t = calloc(1, sizeof(struct libiapp_thr));
	if (t == NULL) {
		warn("%s: calloc", __func__);
		return (NULL);
	}

	/* fde_head for fd events */
	t->h = fde_ctx_new();
	if (t->h == NULL) {
		free(t);
		return (NULL);
	}

	/* TODO: callback data / state */

	/* TODO: deferred work queue */

	return (t);
}

void
libiapp_thr_destroy(struct libiapp_thr *t)
{
	if (t == NULL)
		return;

	/* TODO: callback data / state */

	/* TODO: fde_head */
	if (t->h != NULL)
		fde_ctx_free(t->h);
	free(t);
}

static void *
libiapp_thr_start(void *arg)
{
	struct timeval tv;
	struct libiapp_thr *t = arg;

	fprintf(stderr, "%s: %p: created\n", __func__, arg);

	while (t->active == true) {
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		fprintf(stderr, "%s: %p: loop\n", __func__, arg);
		fde_runloop(t->h, &tv);
	}

	return (NULL);
}

/**
 * Create a group of worker threads and get them ready to
 * start work.
 */
struct libiapp_thr_group *
libiapp_thr_group_create(int nthreads)
{
	struct libiapp_thr_group *tg;
	int i;

	tg = calloc(1, sizeof(struct libiapp_thr_group));
	if (tg == NULL) {
		warn("%s: calloc", __func__);
		return (NULL);
	}
	tg->worker_threads.n_threads = nthreads;
	tg->worker_threads.threads = calloc(tg->worker_threads.n_threads,
	    sizeof(struct libiapp_thr));
	if (tg->worker_threads.threads == NULL) {
		warn("%s: calloc", __func__);
		free(tg);
		return (NULL);
	}

	for (i = 0; i < tg->worker_threads.n_threads; i++) {
		tg->worker_threads.threads[i] = libiapp_thr_create();
		if (tg->worker_threads.threads[i] == NULL)
			goto error;
		tg->worker_threads.threads[i]->tg = tg; /* XXX */
		tg->worker_threads.threads[i]->active = true; /* XXX */
	}

	return (tg);

error:
	for (i = 0; i < tg->worker_threads.n_threads; i++) {
		if (tg->worker_threads.threads[i] != NULL) {
			libiapp_thr_destroy(tg->worker_threads.threads[i]);
		}
	}
	free(tg->worker_threads.threads);
	free(tg);
	return (NULL);
}

/**
 * Create/start the worker threads.
 *
 * This creates and starts the worker threads.
 * It's a bit badly named; I'll sort through that mess later
 * once this code is stood up and working.
 */
bool
libiapp_thr_group_start(struct libiapp_thr_group *tg)
{
	int i;

	for (i = 0; i < tg->worker_threads.n_threads; i++) {
		struct libiapp_thr *t = tg->worker_threads.threads[i];
		if (pthread_create(&t->thr_id, NULL, libiapp_thr_start,
		    t) != 0) {
			warn("%s: pthread_create", __func__);
			/* XXX better error handling? */
			return (false);
		}
	}

	return (true);
}

bool
libiapp_thr_group_stop(struct libiapp_thr_group *tg)
{
	int i;

	for (i = 0; i < tg->worker_threads.n_threads; i++) {
		struct libiapp_thr *t = tg->worker_threads.threads[i];

		t->active = false;
		/* XXX TODO: send immediate wakeup */
	}
	return (true);
}

bool
libiapp_thr_group_join(struct libiapp_thr_group *tg)
{
	int i;

	for (i = 0; i < tg->worker_threads.n_threads; i++) {
		pthread_join(tg->worker_threads.threads[i]->thr_id, NULL);
	}
	return (true);
}

void
libiapp_thr_group_free(struct libiapp_thr_group *tg)
{
	/* XXX TODO */
}
