#ifndef	__LIBIAPP_DISK_H__
#define	__LIBIAPP_DISK_H__

/*
 * This represents a disk descriptor.
 *
 * It differs from a comm descriptor in that we can do multiple parallel
 * read/write operations at different offsets, as well as various things
 * such as fsync.
 */

struct fde_disk;
struct fde_disk_op;

/*
 * Callback types
 */
typedef void	disk_close_cb(struct fde_disk *fdd, void *cbdata);
typedef void	disk_op_cb(struct fde_disk *fdd, struct fde_disk_op *op,
		    int retval, int xerrno, void *cbdata);

typedef enum {
	FDE_DISK_OP_NONE,
	FDE_DISK_OP_READ,
	FDE_DISK_OP_WRITE,
	FDE_DISK_OP_FSYNC,
	FDE_DISK_OP_OPEN,
	FDE_DISK_OP_CLOSE
} fde_disk_op_type;

struct fde_disk_op {
	struct fde_disk *fdd;

	/* AIO state for events requiring it */
	struct aiocb aio;

	/* Op type! */
	fde_disk_op_type op;

	/* Node on the relevant list */
	TAILQ_ENTRY(fde_disk_op) node;

	/*
	 * For asynchronous operations; this indicates that
	 * the operation has been queued to the OS and is
	 * awaiting a response.
	 */
	int is_pending;

	/*
	 * This indiciates the item is on the operation queue,
	 * not the pending (OS) queue.
	 */
	int is_queued;

	struct {
		disk_op_cb *cb;
		void *cbdata;
		int retval;
		int xerrno;
	} o;

	/*
	 * For FDE_DISK_OP_OPEN events, this is the pathname of the file
	 * to open.
	 */
	struct {
		const char *filename;
		int mode;	/* UNIX permissions to open the file */
		int flags;	/* UNIX O_* flags */
	} open_state;
};

struct fde_disk {
	int fd;
	int do_close;
	struct fde_head *fh_parent;

	/*
	 * Queue of events which have not yet been serviced.
	 */
	TAILQ_HEAD(, fde_disk_op) op_queue;

	/*
	 * Queue of events which have been sent to the OS
	 * and are waiting for a response.
	 */
	TAILQ_HEAD(, fde_disk_op) op_pending;

	struct fde * ev_cleanup;

	struct {
		disk_close_cb *cb;
		void *cbdata;
	} c;
};

/*
 * Create a new disk file handle.
 */
extern	struct fde_disk * disk_create(struct fde_head *fh,
	    disk_close_cb *cb, void *cbdata);

/*
 * Schedule an asynchronous file open.
 */
extern	int disk_open(struct fde_disk *fdd, const char *path,
	    int flags, int mode, disk_op_cb *cb, void *cbdata);

/*
 * Schedule an asynchronous file close.
 *
 * This will occur after all pending operations complete.
 * It will not abort existing operations.
 * It will mark the disk state as "closing" so any further
 * queued operations fail.
 */
extern	int disk_close(struct fde_disk *fdd);

#endif	/* __LIBIAPP_DISK_H__ */
