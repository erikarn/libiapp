#ifndef	__LIBIAPP_SHM_ALLOC_H__
#define	__LIBIAPP_SHM_ALLOC_H__

struct shm_alloc_slab;

/*
 * This represents the allocator state.
 */
struct shm_alloc_state {
	TAILQ_HEAD(, shm_alloc_slab) slab_list;
	size_t max_size;
	size_t slab_size;
	int do_mlock;
	pthread_mutex_t l;
};

/*
 * This represents a single SHM allocation slab.
 */
struct shm_alloc_slab {
	int shm_fd;
	size_t shm_size;

	TAILQ_ENTRY(shm_alloc_slab) node;

	/*
	 * This is the mmap()'ed address.
	 */
	char *shm_m;

	/*
	 * This is the current offset in the slab that
	 * we've allocated pages from.
	 */
	off_t shm_curofs;
};

extern	void shm_alloc_init(size_t max_size, size_t slab_size, int do_mlock);
extern	struct shm_alloc_slab * shm_alloc_new_slab(size_t size, int do_mlock);

extern	void * shm_alloc_alloc(size_t size);
extern	int shm_alloc_free(void *p, size_t size);

#endif	/* __LIBIAPP_SHM_ALLOC_H__ */
