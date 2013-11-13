#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/queue.h>
#include <fcntl.h>
#include <strings.h>
#include <pthread.h>
#include <err.h>

#include "shm_alloc.h"

struct shm_alloc_state s_st;

void
shm_alloc_init(size_t max_size, size_t slab_size, int do_mlock)
{
	struct shm_alloc_slab *sh;

	bzero(&s_st, sizeof(&s_st));

	/*
	 * Initial setup!
	 */
	TAILQ_INIT(&s_st.slab_list);
	s_st.max_size = max_size;
	s_st.slab_size = slab_size;
	s_st.do_mlock = do_mlock;

	pthread_mutex_init(&s_st.l, NULL);

	/*
	 * Allocate our first slab.  If it fails, it's okay,
	 * we'll just do this at the first allocation.
	 */
	pthread_mutex_lock(&s_st.l);
	(void) shm_alloc_new_slab(slab_size, do_mlock);
	pthread_mutex_unlock(&s_st.l);
}

struct shm_alloc_slab *
shm_alloc_new_slab(size_t size, int do_mlock)
{
	int fd;
	void *m;
	int reterror = 0;
	const char *shm_path = SHM_ANON;
	int sys_pagesize;
	int i;
	struct shm_alloc_slab *sh;

	sh = calloc(1, sizeof(*sh));
	if (sh == NULL) {
		warn("%s: calloc", __func__);
		return (NULL);
	}
	sh->shm_fd = -1;

#if 0
	/* Unlink it if it exists */
	(void) shm_unlink(shm_path);
#endif

	/* Open a posix shared memory thing, by name */
	sh->shm_fd = shm_open(shm_path, O_CREAT | O_RDWR, 0600);
	sh->shm_size = size;

	if (sh->shm_fd < 0) {
		sh->shm_fd = -1;
		warn("%s: shm_open", __func__);
		goto cleanup;
	}

	/* Truncate it to the correct size */
	if (ftruncate(sh->shm_fd, sh->shm_size) < 0) {
		warn("%s: ftruncate", __func__);
		reterror = 1;
		goto cleanup;
	}

	/* mmap() the whole range */
	sh->shm_m = mmap(NULL, sh->shm_size,
	    PROT_READ | PROT_WRITE,
	    MAP_ALIGNED_SUPER | MAP_SHARED,
	    sh->shm_fd, 0);
	if (sh->shm_m == MAP_FAILED) {
		warn("%s: mmap", __func__);
		reterror = 1;
		goto cleanup;
	}

	/* mlock() it into memory */
	if (do_mlock) {
		if (mlock(sh->shm_m, sh->shm_size) < 0) {
			warn("%s: mlock", __func__);
			reterror = 1;
			goto cleanup;
		}
	}

	TAILQ_INSERT_TAIL(&s_st.slab_list, sh, node);

	/* Done! Good */
	return (0);

cleanup:
	/* Unlink it if it exists */
	if (sh->shm_m)
		munmap(sh->shm_m, sh->shm_size);

	if (sh->shm_fd != -1)
		close(sh->shm_fd);

	if (shm_path != SHM_ANON)
		(void) shm_unlink(shm_path);
	free(sh);
	return (NULL);
}

void *
shm_alloc_alloc(size_t size)
{
	void *p = NULL;
	struct shm_alloc_slab *sh;

	pthread_mutex_lock(&s_st.l);
	/* Walk the slab list, look for something free */
	TAILQ_FOREACH(sh, &s_st.slab_list, node) {
		fprintf(stderr, "%s: curofs=%lld, size=%lld, shm_size=%lld\n",
		    __func__,
		    (long long) sh->shm_curofs,
		    (long long) size,
		    (long long) sh->shm_size);
		/*
		 * Skip out if we have no free space
		 */
		if (sh->shm_curofs + size > sh->shm_size)
			continue;

		/* There is enough space, so use it */
		p = (sh->shm_m) + sh->shm_curofs;
		sh->shm_curofs += size;
		break;
	}

	pthread_mutex_unlock(&s_st.l);
	return (p);
}

int
shm_alloc_free(void *p, size_t size)
{

	/* XXX ignore for now */
	return (0);
}