#ifndef	__THR_H__
#define	__THR_H__

struct libiapp_thr;
struct libiapp_thr_group;

/*
 * A group of worker threads.
 *
 * This represents a group of worker threads doing work.
 *
 * The application will create a thread group with a number of
 * threads, provide callbacks for the lifecycle of said worker
 * threads, and then call to start/stop/pause the worker threads.
 */
struct libiapp_thr_group {
	struct {
		int n_threads;
		struct libiapp_thr **threads;
	} worker_threads;
};

/*
 * A worker thread abstraction.
 *
 * This represents a worker thread in a pool of worker threads.
 * It includes managing the IO being scheduled on said thread,
 * as well as other events/timers and thread/thread communication.
 */

struct libiapp_thr {
	pthread_t thr_id;
	struct libiapp_thr_group *tg;
	int app_id;
	struct fde_head *h;
	struct {
		void *data;
	} cb;

	bool active;

	/* TODO: event for immediate wakeup */

	/* TODO: immediate deferred work */
};

extern	struct libiapp_thr_group * libiapp_thr_group_create(int nthreads);
extern	bool libiapp_thr_group_start(struct libiapp_thr_group *);
extern	bool libiapp_thr_group_stop(struct libiapp_thr_group *);
extern	bool libiapp_thr_group_join(struct libiapp_thr_group *);
extern	void libiapp_thr_group_free(struct libiapp_thr_group *);

#endif
