#ifndef	__LIBIAPP_SHM_ALLOC_H__
#define	__LIBIAPP_SHM_ALLOC_H__

struct shm_alloc_slab;
struct shm_alloc_allocation;

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
	struct shm_alloc_state *sm;

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

	/*
	 * This is the current free list state.
	 */
	TAILQ_HEAD(, shm_alloc_allocation) free_list;
	int free_list_cnt;
};

/*
 * This represents a shared memory allocation.
 */
struct shm_alloc_allocation {
	struct shm_alloc_slab *sha_slab;
	int sha_fd;
	off_t sha_offset;
	size_t sha_len;
	char *sha_ptr;
	TAILQ_ENTRY(shm_alloc_allocation) node;
	int sha_isactive;
};

extern	void shm_alloc_init(struct shm_alloc_state *sm,
	    size_t max_size, size_t slab_size, int do_mlock);
extern	struct shm_alloc_slab * shm_alloc_new_slab(struct shm_alloc_state *sm,
	    size_t size, int do_mlock);
extern	struct shm_alloc_allocation * shm_alloc_alloc(struct shm_alloc_state *sm,
	    size_t size);
extern	int shm_alloc_free(struct shm_alloc_allocation *);

#endif	/* __LIBIAPP_SHM_ALLOC_H__ */
