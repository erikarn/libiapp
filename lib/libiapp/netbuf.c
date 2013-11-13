#include <stdio.h>
#include <stdlib.h>
#include <err.h>

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
iapp_netbuf_alloc(size_t minsize)
{
	struct iapp_netbuf *n;

	n = malloc(sizeof(*n));
	if (n == NULL) {
		warn("%s: malloc", __func__);
		return (NULL);
	}

	n->bufptr = malloc(minsize);
	if (n->bufptr == NULL) {
		warn("%s: malloc (buf %d bytes)", __func__, (int) minsize);
		free(n);
		return (NULL);
	}
	n->buf_size = minsize;

	return (n);
}

void
iapp_netbuf_free(struct iapp_netbuf *n)
{

	free(n->bufptr);
	free(n);
}
