#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/queue.h>
#include <err.h>

#include "shm_alloc.h"
#include "netbuf.h"

void
iapp_netbuf_init(void)
{

}

void
iapp_netbuf_shutdown(void)
{

}

struct iapp_netbuf *
iapp_netbuf_alloc(struct shm_alloc_state *sm, size_t minsize)
{
	struct iapp_netbuf *n;

	n = malloc(sizeof(*n));
	if (n == NULL) {
		warn("%s: malloc", __func__);
		return (NULL);
	}

#if 0
	n->bufptr = malloc(minsize);
	n->nb_type = NB_ALLOC_MALLOC;
	if (n->bufptr == NULL) {
		warn("%s: malloc (buf %d bytes)", __func__, (int) minsize);
		free(n);
		return (NULL);
	}
#else
	n->sa = shm_alloc_alloc(sm, minsize);
	if (n->sa == NULL) {
		warn("%s: malloc (buf %d bytes)", __func__, (int) minsize);
		free(n);
		return (NULL);
	}

	/* Cache buffer pointer */
	n->bufptr = n->sa->sha_ptr;
	n->nb_type = NB_ALLOC_POSIXSHM;
#endif
	n->buf_size = minsize;

	return (n);
}

void
iapp_netbuf_free(struct iapp_netbuf *n)
{

#if 0
	free(n->bufptr);
#else
	shm_alloc_free(n->sa);
#endif
	free(n);
}
