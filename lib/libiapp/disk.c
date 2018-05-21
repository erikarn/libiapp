#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <err.h>
#include <aio.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/event.h>
#include <sys/time.h>

#include "fde.h"
#include "disk.h"

/*
 * Disk queue operation functions.
 */
static struct fde_disk_op *
disk_op_create(struct fde_disk *fdd, fde_disk_op_type ot, disk_op_cb *cb, void *cbdata)
{
	struct fde_disk_op *op;

	op = calloc(1, sizeof(*op));
	if (op == NULL) {
		warn("%s: calloc", __func__);
		return (NULL);
	}

	op->fdd = fdd;
	op->is_pending = 0;	/* Not yet submitted to the hardware */
	op->o.cb = cb;
	op->o.cbdata = cbdata;
	op->op = ot;

	return (op);
}

/*
 * Disk queue management functions
 */
void
disk_op_queue(struct fde_disk *fdd, struct fde_disk_op *op)
{

	/* XXX assert it's not pending or queued */
	/* XXX no locking for now! */
	op->is_queued = 1;
	TAILQ_INSERT_TAIL(&fdd->op_queue, op, node);
}

/*
 * Disk handle related methods
 */
static void
disk_cb_cleanup(int fd, struct fde *f, void *arg, fde_cb_status status)
{
	struct fde_disk *fdd = (struct fde_disk *) arg;

	/*
	 * XXX make sure the disk queue for this particular
	 * handle is completely finshed!
	 */

	if (fdd->c.cb != NULL)
		fdd->c.cb(fdd, fdd->c.cbdata);
	if (fdd->do_close)
		close(fdd->fd);

	/*
	 * Tidy up the events.
	 */
	fde_free(fdd->fh_parent, fdd->ev_cleanup);

	/*
	 * Finally, free state.
	 */
	free(fdd);
}

/*
 * Create a disk handle.
 */
struct fde_disk *
disk_create(struct fde_head *fh, disk_close_cb *cb, void *cbdata)
{
	struct fde_disk *fdd;

	fdd = calloc(1, sizeof(*fdd));
	if (fdd == NULL) {
		warn("%s: calloc", __func__);
		return (NULL);
	}

	fdd->fd = -1;
	fdd->do_close = 1;
	fdd->fh_parent = fh;
	TAILQ_INIT(&fdd->op_queue);
	TAILQ_INIT(&fdd->op_pending);

	/*
	 * Create an fde callback for scheduling the final cleanup/close.
	 */
	fdd->ev_cleanup = fde_create(fh, -1, FDE_T_CALLBACK, 0,
	    disk_cb_cleanup, fdd);
	if (fdd->ev_cleanup == NULL)
		goto cleanup;

	/*
	 * Close state.
	 */
	fdd->c.cb = cb;
	fdd->c.cbdata = cbdata;

	/*
	 * Ready!
	 */
	return (fdd);

cleanup:
	if (fdd->ev_cleanup)
		fde_free(fh, fdd->ev_cleanup);
	free(fdd);
	return (NULL);
}

/*
 * Schedule an async disk open.
 *
 * This assumes the disk handle is not already opened; it makes
 * no sense to reopen on an existing handle.
 */
int
disk_open(struct fde_disk *fdd, const char *path, int flags,
    int mode, disk_op_cb *cb, void *cbdata)
{
	struct fde_disk_op *op;

	/*
	 * if the FD is not -1, it's already opened; so error out.
	 */
	if (fdd->fd != -1) {
		fprintf(stderr, "%s: (%s): handle is already opened!\n", __func__, path);
		return (-1);
	}

	/* Create a new disk op; fill in generic state */
	op = disk_op_create(fdd, FDE_DISK_OP_OPEN, cb, cbdata);
	if (op == NULL) {
		return (-1);
	}

	/* Open state */
	op->open_state.filename = strdup(path);
	/* XXX validate the alloc succeeded, fail if needed !*/
	op->open_state.mode = mode;
	op->open_state.flags = flags;

	/* Queue */

	return (0);
}

