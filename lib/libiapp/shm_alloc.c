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
#include <assert.h>

#include "shm_alloc.h"

void
shm_alloc_init(struct shm_alloc_state *sm, size_t max_size, size_t slab_size,
	    int do_mlock)
{
	struct shm_alloc_slab *sh;

	bzero(sm, sizeof(*sm));

	/*
	 * Initial setup!
	 */
	TAILQ_INIT(&sm->slab_list);
	sm->max_size = max_size;
	sm->slab_size = slab_size;
	sm->do_mlock = do_mlock;

	pthread_mutex_init(&sm->l, NULL);

	/*
	 * Allocate our first slab.  If it fails, it's okay,
	 * we'll just do this at the first allocation.
	 */
	pthread_mutex_lock(&sm->l);
	(void) shm_alloc_new_slab(sm, slab_size, do_mlock);
	pthread_mutex_unlock(&sm->l);
}

struct shm_alloc_slab *
shm_alloc_new_slab(struct shm_alloc_state *sm, size_t size, int do_mlock)
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

	/* Add it to the list of slabs */
	TAILQ_INSERT_TAIL(&sm->slab_list, sh, node);

	/* Setup the free list */
	TAILQ_INIT(&sh->free_list);
	sh->free_list_cnt = 0;

	/* And link back to the parent */
	sh->sm = sm;

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

/*
 * Add the given allocation to the free list.
 */
void
shm_alloc_add_freelist(struct shm_alloc_state *sm,
    struct shm_alloc_allocation *sa)
{

	assert(sa->sha_isactive == 1);

	/* Don't add an inactive entry onto the free list! */
	if (sa->sha_isactive == 0) {
		fprintf(stderr,
		    "%s: %p: putting inactive item on the freelist?\n",
		    __func__,
		    sa);
		return;
	}

	/* Mark the allocation as inactive */
	sa->sha_isactive = 0;

	/*
	 * Add the entry to the head of the list;
	 * if we're lucky then the next allocation will grab
	 * it and it'll be cache-hot.
	 */
	pthread_mutex_lock(&sm->l);
	TAILQ_INSERT_HEAD(&sa->sha_slab->free_list, sa, node);
	sa->sha_slab->free_list_cnt++;
	/* XXX TODO: also add a top-level free list */
	pthread_mutex_unlock(&sm->l);
}

/*
 * Find a suitable item on the freelist.  Bail out if nothing suitable
 * is found.
 *
 * This assumes the lock is held.
 */
struct shm_alloc_allocation *
shm_alloc_lookup_freelist(struct shm_alloc_state *sm, size_t size)
{
	struct shm_alloc_allocation *sa, *sa_found;
	struct shm_alloc_slab *sh;

	sa_found = NULL;

	/*
	 * XXX TODO: we should just keep the freelist entries on
	 * a top-level list too, in order to make this lookup
	 * quicker!
	 */
	TAILQ_FOREACH(sh, &sm->slab_list, node) {
		TAILQ_FOREACH(sa, &sh->free_list, node) {
			if (sa->sha_len == size) {
				sa_found = sa;
				break;
			}
		}
	}

	/*
	 * If we found something, pull it off of the free list.
	 */
	if (sa_found != NULL) {
		TAILQ_REMOVE(&sa_found->sha_slab->free_list, sa_found, node);
		sa_found->sha_slab->free_list_cnt--;
		sa_found->sha_isactive = 1;
	}

	return (sa_found);
}

struct shm_alloc_allocation *
shm_alloc_alloc(struct shm_alloc_state *sm, size_t size)
{
	struct shm_alloc_slab *sh;
	struct shm_alloc_allocation *sa = NULL;

	pthread_mutex_lock(&sm->l);

	/* Check the freelist first */
	sa = shm_alloc_lookup_freelist(sm, size);
	if (sa != NULL)
		goto out;

	/* Walk the slab list, look for something free */
	TAILQ_FOREACH(sh, &sm->slab_list, node) {
#if 0
		fprintf(stderr, "%s: curofs=%lld, size=%lld, shm_size=%lld\n",
		    __func__,
		    (long long) sh->shm_curofs,
		    (long long) size,
		    (long long) sh->shm_size);
#endif
		/*
		 * Skip out if we have no free space
		 */
		if (sh->shm_curofs + size > sh->shm_size)
			continue;

		/* There is enough space, so use it */

		/*
		 * Allocate the state node; if we fail, don't
		 * bother finishing the shared memory allocation.
		 */
		sa = malloc(sizeof(*sa));
		if (sa == NULL) {
			warn("%s: malloc failed", __func__);
			return (NULL);
		}

		/* OK, we can finish the allocation */

		sa->sha_fd = sh->shm_fd;
		sa->sha_offset = sh->shm_curofs;
		sa->sha_len = size;
		sa->sha_ptr = (sh->shm_m) + sh->shm_curofs;
		sa->sha_isactive = 1;
		sa->sha_slab = sh;

		/* And bump the allocation offset along */
		sh->shm_curofs += size;
		break;
	}

out:
	pthread_mutex_unlock(&sm->l);
	return (sa);
}

int
shm_alloc_free(struct shm_alloc_allocation *sa)
{

	shm_alloc_add_freelist(sa->sha_slab->sm, sa);

	return (0);
}
