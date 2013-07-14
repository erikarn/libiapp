#ifndef	__FD_H__
#define	__FD_H__

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

#define	FDE_HEAD_MAXEVENTS	32

/*
 * FD event queue.  One per thread.
 */
struct fde_head {
	TAILQ_HEAD(, fde) f_head;
	int kqfd;
	struct kevent kev_list[FDE_HEAD_MAXEVENTS];
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
	FDE_T_SIGNAL,		/* XXX not yet implemented */
	FDE_T_TIMER,		/* XXX not yet implemented */
	FDE_T_AIO		/* XXX not yet implemented */
} fde_type;

typedef	void fde_callback(int fd, struct fde *, void *arg,
	    fde_cb_status status);

/*
 * An FD event.
 */
struct fde {
	int fd;
	struct kevent kev;
	TAILQ_ENTRY(fde) node;
	fde_type f_type;
	fde_callback *cb;
	int is_active;
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
	    fde_callback *cb, void *cbdata);

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
 * Remove the event.
 */
extern	void fde_delete(struct fde_head *, struct fde *);

/*
 * Run the kqueue loop check.  This runs kevent() to check what
 * needs to be dispatched, then call the dispatch function.
 */
extern	void fde_runloop(struct fde_head *, const struct timespec *timeout);

#endif	/* __FD_H__ */
