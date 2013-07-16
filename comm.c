#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <err.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/event.h>
#include <sys/time.h>

#include <netinet/in.h>

#include "fde.h"
#include "comm.h"

/*
 * This implements the 'socket' logic for sockets, pipes and such.
 * It isn't at all useful for disk IO.
 */

static void
comm_cb_read(int fd, struct fde *f, void *arg, fde_cb_status status)
{

}

static void
comm_cb_write(int fd, struct fde *f, void *arg, fde_cb_status status)
{

}

static void
comm_cb_cleanup(int fd, struct fde *f, void *arg, fde_cb_status status)
{
	struct fde_comm *c = arg;

	/* XXX complain if is_closing / is_cleanup isn't done! */

	/*
	 * Call the close callback if one was registered.
	 */
	if (c->c.cb != NULL)
		c->c.cb(fd, c, c->c.cbdata);

	/*
	 * Close the file descriptor if we're allowed to.
	 */
	if (c->do_close == 1)
		close(c->fd);

	/*
	 * Free this FDE. It's fine to do it in a callback handler
	 * as the callback API guarantees that we can do this.
	 * (Note: it's not guaranteed for freeing IO FDEs from within
	 * an IO callback!)
	 */
	fde_free(c->fh_parent, c->ev_read);
	fde_free(c->fh_parent, c->ev_write);
	fde_free(c->fh_parent, c->ev_cleanup);

	/*
	 * Finally, free the fde_comm state.
	 */
	free(c);
}

/*
 * Create an fde_comm object for the given file descriptor.
 *
 * This assumes the file descriptor is already non-blocking and such.
 */
struct fde_comm *
comm_create(int fd, struct fde_head *fh)
{
	struct fde_comm *fc;

	fc = calloc(1, sizeof(*fc));
	if (fc == NULL) {
		warn("%s: calloc", __func__);
		return (NULL);
	}

	fc->fd = fd;
	fc->do_close = 1;
	fc->fh_parent = fh;

	fc->ev_read = fde_create(fh, fd, FDE_T_READ, comm_cb_read, fc);
	if (fc->ev_read == NULL)
		goto cleanup;

	fc->ev_write = fde_create(fh, fd, FDE_T_WRITE, comm_cb_write, fc);
	if (fc->ev_write == NULL)
		goto cleanup;

	fc->ev_cleanup = fde_create(fh, -1, FDE_T_CALLBACK,
	    comm_cb_cleanup, fc);
	if (fc->ev_cleanup == NULL)
		goto cleanup;

	return (fc);

cleanup:
	if (fc->ev_read)
		fde_free(fh, fc->ev_read);
	if (fc->ev_write)
		fde_free(fh, fc->ev_write);
	if (fc->ev_cleanup)
		fde_free(fh, fc->ev_cleanup);
	free(fc);
	return (NULL);
}

void
comm_mark_nonclose(struct fde_comm *fc)
{

	fc->do_close = 0;
}

static int
comm_is_close_ready(struct fde_comm *fc)
{

	/* XXX should error out if fc->is_closing isn't 1 */
	if (fc->is_closing == 0)
		return (0);

	/*
	 * For now - no active read/write.
	 *
	 * XXX Later, we will need to also disable pending
	 * accept/connect machinery before doing this.
	 */
	return (fc->r.is_active == 0 && fc->w.is_active == 0);
}

static void
comm_start_cleanup(struct fde_comm *fc)
{

	/* XXX complain if we're called when not closing / ready */
	if (fc->is_closing == 0)
		return;

	/* We've already scheduled the cleanup */
	if (fc->is_cleanup == 1)
		return;

	if (! comm_is_close_ready(fc))
		return;

	/* Schedule the cleanup */
	fc->is_cleanup = 1;
	fde_add(fc->fh_parent, fc->ev_cleanup);
}

/*
 * Schedule a comm object to be closed.
 *
 * This will attempt to cancel any pending IO.
 *
 * All cancelled IO will be called with FDE_COMM_CB_CLOSING without the
 * IO occuring.
 *
 * Once all IO on the given socket has completed or cancelled, the close
 * callback will be made, then the state will be freed.  If do_close is
 * set to 1, the FD will be closed.
 */
void
comm_close(struct fde_comm *fc)
{

	/*
	 * If this socket is already closing, don't bother
	 * with the rest of this.
	 */
	if (fc->is_closing == 1)
		return;

	/*
	 * Begin the process of closing.
	 */
	fc->is_closing = 1;

	/*
	 * XXX complain if is_cleanup is set, it shouldn't be!
	 */

	/*
	 * Check to see if there's any pending IO.  If there is,
	 * let it complete (for now) - we'll later on add some
	 * stuff to abort IO that we can.
	 *
	 * If there isn't, we can just schedule the close callback.
	 */
	if (! comm_is_close_ready(fc))
		return;

	/*
	 * Ready to close!
	 */
	comm_start_cleanup(fc);
}

int
comm_read(struct fde_comm *fc, char *buf, int len)
{

	return (-1);
}

int
comm_write(struct fde_comm *fc, char *buf, int len)
{

	return (-1);
}
