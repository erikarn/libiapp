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

#ifndef	__FDE_H__
#define	__FDE_H__

/*
 * This is a lightweight, kqueue specific set of wrappers around kqueue
 * management for FDs, AIO events, signals and other things.
 *
 * It's kind of but not quite like libevent.  Specifically, at this layer
 * there's no thread-safe behaviour.  Any thread-safe stuff should be done
 * at a higher layer.
 *
 * The other thing I'm not yet doing here is trying to handle extended
 * object lifecycles.  Eg, close here will free things and cancel events.
 * I'll see about implementing this behaviour at a higher layer.
 */

struct fde_head;
struct fde;

#define	FDE_HEAD_MAXEVENTS	128

/*
 * FD event queue.  One per thread.
 */
struct fde_head {
	TAILQ_HEAD(, fde) f_head;	/* list of all active entries */
	TAILQ_HEAD(, fde) f_cb_head;	/* list of callbacks to perform */
	TAILQ_HEAD(f_t, fde) f_t_head;	/* list of timer events to perform */
	int kqfd;
	struct kevent kev_list[FDE_HEAD_MAXEVENTS];
	struct {
		struct kevent kev_list[FDE_HEAD_MAXEVENTS];
		int n;
	} pending;
};

typedef enum {
	FDE_CB_NONE,
	FDE_CB_COMPLETED,
	FDE_CB_ABORTED,
	FDE_CB_CLOSING
} fde_cb_status;

typedef enum {
	FDE_T_NONE,
	FDE_T_READ,
	FDE_T_WRITE,
	FDE_T_CALLBACK,		/* Immediate callback */
	FDE_T_SIGNAL,		/* XXX not yet implemented */
	FDE_T_TIMER,		/* XXX not yet implemented */
	FDE_T_AIO		/* XXX not yet implemented */
} fde_type;

typedef enum {
	FDE_F_PERSIST	= 0x00000001	/* stay registered */
} fde_flags;

typedef	void fde_callback(int fd, struct fde *, void *arg,
	    fde_cb_status status);

/*
 * An FD event.
 */
struct fde {
	int fd;
	struct kevent kev;
	TAILQ_ENTRY(fde) node;
	TAILQ_ENTRY(fde) cb_node;
	fde_type f_type;
	uint32_t f_flags;
	fde_callback *cb;
	int is_active;
	struct timeval tv;		/* time to fire this event */
	void *cbdata;
};

/*
 * Create a FDE list.
 */
extern	struct fde_head * fde_ctx_new(void);

/*
 * Free an FDE list, complete with shutting things down by calling
 * the callbacks with an error/shutdown method.
 */
extern	void fde_ctx_free(struct fde_head *);

/*
 * Create an FD struct for a given FD.
 */
extern	struct fde * fde_create(struct fde_head *, int fd, fde_type t,
		uint32_t flags, fde_callback *cb, void *cbdata);

/*
 * Free the state associated with this FD.
 *
 * For now, this removes all of the queued state, thus can only
 * be done from the current thread.
 */
extern	void fde_free(struct fde_head *, struct fde *);

/*
 * Add the event.
 */
extern	void fde_add(struct fde_head *, struct fde *);

/*
 * Add the event, but with a timeout.  This is only applicable
 * for timer callbacks; any attempt to add a normal event
 * this way will result in things blowing up.
 *
 * The callout will occur at or after 'tv'.
 */
extern	void fde_add_timeout(struct fde_head *, struct fde *,
	    struct timeval *tv);

/*
 * Remove the event.
 */
extern	void fde_delete(struct fde_head *, struct fde *);

/*
 * Run the kqueue loop check.  This runs kevent() to check what
 * needs to be dispatched, then call the dispatch function.
 */
extern	void fde_runloop(struct fde_head *, const struct timeval *timeout);

#endif	/* __FDE_H__ */
