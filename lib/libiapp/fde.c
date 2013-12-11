/*-
 * Copyright (c) 2013 Netflix, Inc.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of Netflix, Inc. nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
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
#include <err.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/queue.h>

#include "fde.h"

struct fde_head *
fde_ctx_new(void)
{
	struct fde_head *fh;

	fh = calloc(1, sizeof(*fh));
	if (fh == NULL) {
		warn("%s: calloc", __func__);
		return (NULL);
	}

	TAILQ_INIT(&fh->f_head);
	TAILQ_INIT(&fh->f_cb_head);
	TAILQ_INIT(&fh->f_t_head);

	fh->kqfd = kqueue();
	if (fh->kqfd == -1) {
		warn("%s: kqueue", __func__);
		free(fh);
		return (NULL);
	}
	return (fh);
}

void
fde_ctx_free(struct fde_head *fh)
{

	/* XXX TODO */
	fprintf(stderr, "%s: not implemented\n", __func__);
}

static int
fde_ev_flags(struct fde *f, uint32_t kev_flags)
{

	/*
	 * For read/write FD events, we either do
	 * oneshot, or clear.  Clear means the event
	 * isn't deleted, but we need a followup
	 * state change (ie, more data arrives or
	 * is written out the network) before another
	 * event is posted.
	 */
	if (f->f_flags & FDE_F_PERSIST) {
		kev_flags |= EV_CLEAR;
	} else {
		kev_flags |= EV_ONESHOT;
	}

	return (kev_flags);
}

struct fde *
fde_create(struct fde_head *fh, int fd, fde_type t, fde_flags fl,
    fde_callback *cb, void *cbdata)
{

	struct fde *f;

	f = calloc(1, sizeof(*f));
	if (f == NULL) {
		warn("%s: calloc\n", __func__);
		return (NULL);
	}

	f->fd = fd;
	f->f_type = t;
	f->f_flags = fl;
	f->cb = cb;
	f->cbdata = cbdata;

	/*
	 * Now, depending upon the node type, initialise it for various
	 * forms of useful event notification.
	 */

	switch (t) {
		case FDE_T_READ:
			/*
			 * NOTE_EOF makes the read note act like select/poll,
			 * where it becomes read-ready for an EOF condition.
			 *
			 * Just note that EOF means the other end has hung up;
			 * there may still be data in the read buffer to
			 * finish up reading.
			 */
			EV_SET(&f->kev, fd, EVFILT_READ,
			    fde_ev_flags(f, EV_ENABLE),
#ifdef	NOTE_EOF
			    NOTE_EOF,
#else
			    0,
#endif
			    0,
			    f);
			break;
		case FDE_T_WRITE:
			EV_SET(&f->kev, fd, EVFILT_WRITE,
			    fde_ev_flags(f, EV_ENABLE),
			    0,
			    0,
			    f);
			break;
		case FDE_T_CALLBACK:
		case FDE_T_TIMER:
			/* Nothing to do here */
			break;
		default:
			warn("%s: event type %d not implemented\n",
			    __func__, t);
			free(f);
			return (NULL);
	}

	/*
	 * All done, return it.
	 */
	return (f);
}

void
fde_free(struct fde_head *fh, struct fde *f)
{

	/*
	 * Make sure we delete the event if it's active, so we don't
	 * get notifications from it.
	 */
	if (f->is_active) {
		fde_delete(fh, f);
	}

	free(f);
}

static void
fde_kq_flush(struct fde_head *fh)
{
	int ret;

	if (fh->pending.n < (FDE_HEAD_MAXEVENTS-1))
		return;

	ret = kevent(fh->kqfd, fh->pending.kev_list, fh->pending.n, NULL, 0, NULL);
	if (ret < 0) {
		warn("%s: kevent", __func__);
		/* XXX Error handling? */
	}

	fh->pending.n = 0;
}

static void
fde_kq_push(struct fde_head *fh, struct kevent *k)
{
	int ret;

#if 1
	/*
	 * If it's full; let's flush it out
	 */
	fde_kq_flush(fh);

	memcpy(&fh->pending.kev_list[fh->pending.n], k, sizeof(struct kevent));
	fh->pending.n++;
#else
	ret = kevent(fh->kqfd, k, 1, NULL, 0, NULL);
	if (ret < 0) {
		warn("%s: kevent", __func__);
		/* XXX Error handling? */
	}
#endif
}

static void
fde_rw_add(struct fde_head *fh, struct fde *f)
{

	if (f->is_active)
		return;

	/*
	 * Assume the event is already setup to be added.
	 */
	f->kev.flags &= (EV_DELETE | EV_ADD | EV_CLEAR | EV_ONESHOT);
	f->kev.flags |= fde_ev_flags(f, EV_ADD | EV_ENABLE);

	fde_kq_push(fh, &f->kev);

	f->is_active = 1;
	TAILQ_INSERT_TAIL(&fh->f_head, f, node);
}

static void
fde_rw_delete(struct fde_head *fh, struct fde *f)
{

	if (! f->is_active)
		return;

	f->kev.flags &= (EV_DELETE | EV_ADD | EV_CLEAR | EV_ONESHOT);
	f->kev.flags |= EV_DELETE;

	fde_kq_push(fh, &f->kev);

	f->is_active = 0;
	TAILQ_REMOVE(&fh->f_head, f, node);
}

static void
fde_cb_add(struct fde_head *fh, struct fde *f)
{

	if (f->is_active)
		return;

	f->is_active = 1;
	f->f_cb_genid = fh->f_cb_genid;
	TAILQ_INSERT_TAIL(&fh->f_head, f, node);
	TAILQ_INSERT_TAIL(&fh->f_cb_head, f, cb_node);
}

static void
fde_cb_delete(struct fde_head *fh, struct fde *f)
{

	if (! f->is_active)
		return;

	f->is_active = 0;
	TAILQ_REMOVE(&fh->f_head, f, node);
	TAILQ_REMOVE(&fh->f_cb_head, f, cb_node);
}

static void
fde_t_delete(struct fde_head *fh, struct fde *f)
{

	if (! f->is_active)
		return;

	f->is_active = 0;
	TAILQ_REMOVE(&fh->f_head, f, node);
	TAILQ_REMOVE(&fh->f_t_head, f, cb_node);
}

void
fde_add(struct fde_head *fh, struct fde *f)
{

	switch (f->f_type) {
		case FDE_T_READ:
		case FDE_T_WRITE:
			fde_rw_add(fh, f);
			break;
		case FDE_T_CALLBACK:
			fde_cb_add(fh, f);
			break;
		default:
			fprintf(stderr, "%s: %p: unknown type (%d)\n",
			    __func__,
			    f,
			    f->f_type);
	}
}

static int
timeval_cmp(const struct timeval *t1, const struct timeval *t2)
{
	uint64_t l, r;

	if (t1->tv_sec != t2->tv_sec) {
		l = t1->tv_sec;
		r = t2->tv_sec;
	} else {
		l = t1->tv_usec;
		r = t2->tv_usec;
	}
	return ((l > r) - (l < r));
}

void
fde_add_timeout(struct fde_head *fh, struct fde *f, struct timeval *tv)
{
	struct fde *n;

	if (f->f_type != FDE_T_TIMER) {
		fprintf(stderr, "%s: %p: wrong type (%d)\n",
		    __func__,
		    f,
		    f->f_type);
	}

	/* XXX should I complain? */
	if (f->is_active)
		return;

	f->is_active = 1;

	/* Insert onto active list */
	TAILQ_INSERT_TAIL(&fh->f_head, f, node);

	/* Set timeout value */
	f->tv = *tv;

	/* Check if list is empty. */
	if (TAILQ_FIRST(&fh->f_t_head) == NULL) {
		TAILQ_INSERT_HEAD(&fh->f_t_head, f, cb_node);
		return;
	}

	/* Check if before first */
	/*
	 * XXX this doesn't preserve order; we'd have to walk
	 * the list and find the first instance of the timer
	 * being _greater_ than this timer and add it there.
	 */
	n = TAILQ_FIRST(&fh->f_t_head);
	if (timeval_cmp(tv, &n->tv) <= 0) {
		TAILQ_INSERT_HEAD(&fh->f_t_head, f, cb_node);
		return;
	}

	/* Check if after last */
	n = TAILQ_LAST(&fh->f_t_head, f_t);
	if (timeval_cmp(tv, &n->tv) >= 0) {
		TAILQ_INSERT_AFTER(&fh->f_t_head, n, f, cb_node);
		return;
	}

	/* Walk the list, insertion sort */
	TAILQ_FOREACH(n, &fh->f_t_head, cb_node) {
		if (timeval_cmp(tv, &n->tv) > 0)
			break;
	}
	TAILQ_INSERT_AFTER(&fh->f_t_head, n, f, cb_node);
}

void
fde_delete(struct fde_head *fh, struct fde *f)
{

	switch (f->f_type) {
		case FDE_T_READ:
		case FDE_T_WRITE:
			fde_rw_delete(fh, f);
			break;
		case FDE_T_CALLBACK:
			fde_cb_delete(fh, f);
			break;
		case FDE_T_TIMER:
			fde_t_delete(fh, f);
			break;
		default:
			fprintf(stderr, "%s: %p: unknown type (%d)\n",
			    __func__,
			    f,
			    f->f_type);
	}
}

static void
fde_cb_runloop(struct fde_head *fh)
{

	struct fde *f, *f_next;
	uint32_t cur_genid;

	cur_genid = fh->f_cb_genid;
	fh->f_cb_genid++;		/* XXX This will wrap; it's ok */

	while ((f = TAILQ_FIRST(&fh->f_cb_head)) != NULL) {
		/*
		 * No, don't process callbacks that we've just scheduled.
		 */
		if (f->f_cb_genid != cur_genid)
			break;
		f_next = TAILQ_NEXT(f, cb_node);
		fde_delete(fh, f);
		f->cb(f->fd, f, f->cbdata, FDE_CB_COMPLETED);
		/* f may be free at this point */
	}
}

static void
fde_t_get_timeout(struct fde_head *fh, const struct timeval *tv_now,
    const struct timeval *tv_timeout, struct timeval *tv_sleep)
{
	struct fde *f;

	f = TAILQ_FIRST(&fh->f_t_head);
	/* If the timeout queue is empty, just allow max timeout */
	if (f == NULL) {
		*tv_sleep = *tv_timeout;
		return;
	}

	/*
	 * Calculate the delta between the current time (tv_now)
	 * and the first timeout value in the list.
	 *
	 * + If tv_now >= f->tv, return '0'.
	 * + If tv_now < f->tv, return the delta, capped by tv_timeout.
	 */
	if (timeval_cmp(tv_now, &f->tv) >= 0) {
		tv_sleep->tv_sec = tv_sleep->tv_usec = 0;
		return;
	}

	/*
	 * We know that tv_now < f->tv here, so take some
	 * shortcuts.
	 *
	 * XXX should methodize this!
	 */
	tv_sleep->tv_sec = f->tv.tv_sec - tv_now->tv_sec;
	if (tv_now->tv_usec > f->tv.tv_usec) {
		/* Borrow from the 'sec' column */
		tv_sleep->tv_usec = f->tv.tv_usec + 1000000 - tv_now->tv_usec;
		tv_sleep->tv_sec --;
	} else {
		tv_sleep->tv_usec = f->tv.tv_usec - tv_now->tv_usec;
	}
}

static void
fde_t_runloop(struct fde_head *fh, const struct timeval *tv)
{

	struct fde *f;

	while ((f = TAILQ_FIRST(&fh->f_t_head)) != NULL) {
		/*
		 * If the current time is less than the event
		 * time, break out; we can't call anything else
		 * in the list.
		 */
#if 0
		fprintf(stderr, "%s: %p: checking %lld.%06lld against %lld.%06lld\n",
		    __func__,
		    fh,
		    (unsigned long long) tv->tv_sec,
		    (unsigned long long) tv->tv_usec,
		    (unsigned long long) f->tv.tv_sec,
		    (unsigned long long) f->tv.tv_usec);
#endif

#if 0
		fprintf(stderr, "%s: %p: timeval_cmp=%d\n", __func__, fh, timeval_cmp(tv, &f->tv));
#endif
		if (timeval_cmp(tv, &f->tv) < 0)
			break;

		fde_delete(fh, f);
		f->cb(f->fd, f, f->cbdata, FDE_CB_COMPLETED);
		/* f may be free at this point */
	}
}

static void
fde_rw_runloop(struct fde_head *fh, const struct timespec *timeout)
{

	int ret, i;
	struct fde *f;

	ret = kevent(fh->kqfd, fh->pending.kev_list, fh->pending.n, fh->kev_list, FDE_HEAD_MAXEVENTS,
	    timeout);

	/*
	 * XXX error handling for pushing events?
	 */
	fh->pending.n = 0;

	if (ret == 0)
		return;

	if (ret < 0) {
		warn("%s: kevent", __func__);
		return;
	}

	for (i = 0; i < ret; i++) {
		f = fh->kev_list[i].udata;
		if (f == NULL) {
			fprintf(stderr, "%s: ident %d: udata==NULL?\n",
			    __func__,
			    fh->kev_list[i].ident);
			continue;
		}

		if (fh->kev_list[i].flags & EV_ERROR) {
			switch (fh->kev_list[i].data) {
			case ENOENT:
			case EINVAL:
			case EBADF:
				continue;
			case EPERM:
			case EPIPE:
				/*
				 * We should notify a registered read callback for
				 * this FD that we received a socket error.
				 * This, fall through
				 */
				break;
			default:
				errno = fh->kev_list[i].data;
				fprintf(stderr, "%s: kevent index %d returned errno %d (%s)\n",
				    __func__,
				    i,
				    errno,
				    strerror(errno));
				continue;
			}
		}

		/*
		 * Callback!
		 *
		 * Assume the kqueue event has already been removed as it's
		 * a one-shot event, so we just mark it inactive here.
		 */

		/*
		 * If it's been marked as inactive here then someone
		 * decided _during this IO loop_ that they weren't
		 * interested in this event any longer.  So, don't call.
		 * the callback.
		 */
		if (f->is_active == 0)
			continue;


		/*
		 * If it's a non-persist callback, mark it as complete.
		 */
		if (! (f->f_flags & FDE_F_PERSIST)) {
			f->is_active = 0;
			TAILQ_REMOVE(&fh->f_head, f, node);
		}

		/*
		 * XXX TODO:
		 *
		 * If it's a read/write socket event, we can store away
		 * the current amount of data available (read) or
		 * space to write (write).. this may change in parallel
		 * between the time the kernel posts the event and the
		 * time we respond to it, but it's better than not knowing
		 * at all.
		 */

		/*
		 * Call the underlying callback.
		 */
		if (f->cb)
			f->cb(f->fd, f, f->cbdata, FDE_CB_COMPLETED);
		else
			fprintf(stderr, "%s: FD %d: no callback?\n",
			    __func__,
			    f->fd);

		/*
		 * XXX at this point, 'f' may be totally invalid, so
		 * we have to ensure we don't reference it.
		 */

		/*
		 * XXX TODO: we /do/ have to lifecycle manage fde events -
		 * if we have a completed event here but someone has
		 * prematurely freed the event before the event has
		 * completed, then we'll have a kqueue event here
		 * for a udata that no longer exists.
		 *
		 * So what we need to actually do here is mark events
		 * as being dying, and then only free them once we've
		 * finished this processing loop.
		 */
	}
}

void
fde_runloop(struct fde_head *fh, const struct timeval *timeout)
{
	struct timespec ts;
	struct timeval tv_now, tv_sleep;

	(void) gettimeofday(&tv_now, NULL);

	/* Run callbacks - this may schedule more callbacks */
	fde_cb_runloop(fh);

	/*
	 * Run timer callbacks - again, this may schedule more
	 * callbacks.
	 */
	fde_t_runloop(fh, &tv_now);

	/*
	 * If there are any scheduled callbacks, make sure we
	 * immediately bail out of the kevent loop.
	 */
	if (TAILQ_FIRST(&fh->f_cb_head) != NULL) {
		ts.tv_sec = ts.tv_nsec = 0;
	} else {
		/*
		 * Check to see whether the timer list has any pending
		 * callbacks; calculate a tv appropriately.
		 */
		fde_t_get_timeout(fh, &tv_now, timeout, &tv_sleep);

		ts.tv_sec = tv_sleep.tv_sec;
		ts.tv_nsec = tv_sleep.tv_usec * 1000;
	}

	/*
	 * Run the read/write IO kqueue loop.
	 */
	fde_rw_runloop(fh, &ts);
}
