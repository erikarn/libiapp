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
iapp_netbuf_alloc(struct shm_alloc_state *sm, netbuf_alloc_type atype,
    size_t minsize)
{
	struct iapp_netbuf *n;

	n = malloc(sizeof(*n));
	if (n == NULL) {
		warn("%s: malloc", __func__);
		return (NULL);
	}

	switch (atype) {
	case NB_ALLOC_MALLOC:
		n->bufptr = malloc(minsize);
		n->nb_type = NB_ALLOC_MALLOC;
		if (n->bufptr == NULL) {
			warn("%s: malloc (buf %d bytes)", __func__, (int) minsize);
			free(n);
			return (NULL);
		}
		break;
	case NB_ALLOC_POSIXSHM:
		n->sa = shm_alloc_alloc(sm, minsize);
		if (n->sa == NULL) {
			warn("%s: malloc (buf %d bytes)", __func__, (int) minsize);
			free(n);
			return (NULL);
		}

		/* Cache buffer pointer */
		n->bufptr = n->sa->sha_ptr;
		n->nb_type = NB_ALLOC_POSIXSHM;
		break;
	default:
		fprintf(stderr, "%s: invalid type (%d)\n", __func__, atype);
		free(n);
		return (NULL);
	}

	n->buf_size = minsize;

	return (n);
}

void
iapp_netbuf_free(struct iapp_netbuf *n)
{

	switch (n->nb_type) {
	case NB_ALLOC_MALLOC:
		free(n->bufptr);
		break;
	case NB_ALLOC_POSIXSHM:
		shm_alloc_free(n->sa);
		break;
	default:
		fprintf(stderr, "%s: %p: invalid type (%d), leaking!\n",
		    __func__,
		    n,
		    n->nb_type);
	}
	free(n);
}
