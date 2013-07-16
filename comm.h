#ifndef	__COMM_H__
#define	__COMM_H__

struct fde_comm;

typedef enum {
	FDE_COMM_CB_NONE,
	FDE_COMM_CB_COMPLETED,
	FDE_COMM_CB_CLOSING,
	FDE_COMM_CB_ERROR,
	FDE_COMM_CB_EOF,
	FDE_COMM_CB_ABORTED
} fde_comm_cb_status;

typedef void	comm_accept_cb(int fd, struct fde_comm *fc, void *arg,
		    fde_comm_cb_status status, int newfd,
		    struct sockaddr *saddr, socklen_t slen, int xerrno);
typedef void	comm_read_cb(int fd, struct fde_comm *fc, void *arg,
		    fde_comm_cb_status status, int retval);
typedef void	comm_write_cb(int fd, struct fde_comm *fc, void *arg,
		    fde_comm_cb_status status, int nwritten);
typedef void	comm_close_cb(int fd, struct fde_comm *fc, void *arg);

struct fde_comm {
	int fd;
	int do_close;		/* Whether to close the FD */
	struct fde_head *fh_parent;

	/* Events */
	struct fde *ev_read;
	struct fde *ev_write;
	struct fde *ev_accept;
	struct fde *ev_connect;
	struct fde *ev_cleanup;

	/* General state */
	int is_closing;		/* Are we getting ready to close? */
	int is_cleanup;		/* cleanup has been scheduled */

	/*
	 * Read state
	 */
	struct {
		int is_active;
		char *buf;	/* buffer to read into */
		int len;	/* buffer length */
		comm_read_cb *cb;
		void *cbdata;
	} r;

	/*
	 * Write state
	 */
	struct {
		int is_active;
		char *buf;
		int len;
		int offset;
		comm_write_cb *cb;
		void *cbdata;
	} w;

	/*
	 * Close state
	 */
	struct {
		comm_close_cb *cb;
		void *cbdata;
	} c;

	/*
	 * Accept state
	 */
	struct {
		int is_active;
		comm_accept_cb *cb;
		void *cbdata;
	} a;

	/*
	 * Connect state
	 */
	struct {
		int is_active;
	} co;
};

/*
 * Create a comm struct for an already-created FD.
 */
extern	struct fde_comm * comm_create(int fd, struct fde_head *fh);

/*
 * Mark the comm struct as non-closing
 */
extern	void comm_mark_nonclose(struct fde_comm *fc);

/*
 * Schedule to have the given comm struct closed.
 *
 * + Any existing queued IO that can be cancelled will be cancelled
 *   and IO handlers will be called with FDE_COMM_CB_CLOSING.
 * + The close handler will then be called just before the comm
 *   state is freed.
 */
extern	void comm_close(struct fde_comm *fc);

/*
 * Set or clear the non-block flag on an open comm object.
 */
extern	int comm_fd_set_nonblocking(int fd, int enable);
extern	int comm_set_nonblocking(struct fde_comm *c, int enable);

/*
 * Schedule some data to be read.
 *
 * The buffer must stay valid for the lifetime of the read.
 */
extern	int comm_read(struct fde_comm *fc, char *buf, int len,
	    comm_read_cb *cb, void *cbdata);

/*
 * Schedule some data to be written.
 *
 * The buffer must stay valid for the lifetime of the write.
 */
extern	int comm_write(struct fde_comm *fc, char *buf, int len,
	    comm_write_cb *cb, void *cbdata);

/*
 * Start accept()ing on the given socket.
 *
 * This assumes that the socket is setup and listening.
 */
extern	int comm_listen(struct fde_comm *fc, comm_accept_cb *cb,
	    void *cbdata);

#endif	/* __COMM_H__ */
