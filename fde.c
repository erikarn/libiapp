#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <err.h>
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

struct fde *
fde_create(struct fde_head *fh, int fd, fde_type t, fde_callback *cb,
    void *cbdata)
{

	struct fde *f;

	f = calloc(1, sizeof(*f));
	if (f == NULL) {
		warn("%s: calloc\n", __func__);
		return (NULL);
	}

	f->fd = fd;
	f->f_type = t;
	f->cb = cb;
	f->cbdata = cbdata;

	/*
	 * Now, depending upon the node type, initialise it for various
	 * forms of useful event notification.
	 */

	switch (t) {
		case FDE_T_READ:
			EV_SET(&f->kev, fd, EVFILT_READ, EV_ENABLE | EV_ONESHOT, 0, 0, f);
			break;
		case FDE_T_WRITE:
			EV_SET(&f->kev, fd, EVFILT_WRITE, EV_ENABLE | EV_ONESHOT, 0, 0, f);
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

void
fde_add(struct fde_head *fh, struct fde *f)
{

	if (f->is_active)
		return;

	/*
	 * Assume the event is already setup to be added.
	 */
	f->kev.flags &= (EV_DELETE | EV_ADD | EV_ONESHOT);
	f->kev.flags |= EV_ADD | EV_ONESHOT;

	if (kevent(fh->kqfd, &f->kev, 1, NULL, 0, NULL) < 0) {
		warn("%s: kqueue", __func__);
		/* XXX return error? */
		return;
	}

	f->is_active = 1;
	TAILQ_INSERT_TAIL(&fh->f_head, f, node);
}

void
fde_delete(struct fde_head *fh, struct fde *f)
{

	if (! f->is_active)
		return;

	f->kev.flags &= (EV_DELETE | EV_ADD | EV_ONESHOT);
	f->kev.flags |= EV_DELETE;

	if (kevent(fh->kqfd, &f->kev, 1, NULL, 0, NULL) < 0) {
		warn("%s: kqueue", __func__);
		/* XXX return error? */
		return;
	}

	f->is_active = 0;
	TAILQ_REMOVE(&fh->f_head, f, node);
}

void
fde_runloop(struct fde_head *fh, const struct timespec *timeout)
{

	int ret, i;
	struct fde *f;

	ret = kevent(fh->kqfd, NULL, 0, fh->kev_list, FDE_HEAD_MAXEVENTS,
	    timeout);

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

		/*
		 * Callback!
		 *
		 * Assume the event has already been removed as it's
		 * a one-shot event, so we just mark it inactive here.
		 */
		f->is_active = 0;
		TAILQ_REMOVE(&fh->f_head, f, node);
		if (f->cb)
			f->cb(f->fd, f, f->cbdata, FDE_CB_COMPLETED);
	}
}
