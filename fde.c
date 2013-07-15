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
			/*
			 * NOTE_EOF makes the read note act like select/poll,
			 * where it becomes read-ready for an EOF condition.
			 */
			EV_SET(&f->kev, fd, EVFILT_READ,
			    EV_ENABLE | EV_ONESHOT,
#ifdef	NOTE_EOF
			    NOTE_EOF,
#else
			    0,
#endif
			    0, f);
			break;
		case FDE_T_WRITE:
			EV_SET(&f->kev, fd, EVFILT_WRITE,
			    EV_ENABLE | EV_ONESHOT, 0, 0, f);
			break;
		case FDE_T_CALLBACK:
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
fde_rw_add(struct fde_head *fh, struct fde *f)
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

static void
fde_rw_delete(struct fde_head *fh, struct fde *f)
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

static void
fde_cb_add(struct fde_head *fh, struct fde *f)
{

	if (f->is_active)
		return;

	f->is_active = 1;
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
		default:
			fprintf(stderr, "%s: %p: unknown type (%d)\n",
			    __func__,
			    f,
			    f->f_type);
	}
}

static void
fde_cb_runloop(struct fde_head *fh, const struct timespec *timeout)
{

	struct fde *f, *f_next;

	while ((f = TAILQ_FIRST(&fh->f_cb_head)) != NULL) {
		f_next = TAILQ_NEXT(f, cb_node);
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
		 * Mark the event as inactive and call the callback.
		 */
		f->is_active = 0;
		TAILQ_REMOVE(&fh->f_head, f, node);
		if (f->cb)
			f->cb(f->fd, f, f->cbdata, FDE_CB_COMPLETED);
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
fde_runloop(struct fde_head *fh, const struct timespec *timeout)
{

	fde_cb_runloop(fh, timeout);
	fde_rw_runloop(fh, timeout);
}


