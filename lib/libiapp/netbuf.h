#ifndef	__NETBUF_H__
#define	__NETBUF_H__


typedef enum {
	NB_ALLOC_NONE		= 0,
	NB_ALLOC_MALLOC,
	NB_ALLOC_POSIXSHM,
} netbuf_alloc_type;

/*
 * Representation of a single network buffer entry.
 *
 * This specifically represents _just_ a buffer allocated
 * from the netbuf API.  Later on a container for storing
 * lists of netbuf iovecs will pop into existence to do
 * the IO and the comm API will use that instead of
 * individual netbufs.
 */
struct iapp_netbuf {
	struct shm_alloc_allocation *sa;
	char *bufptr;
	int buf_size;
	netbuf_alloc_type nb_type;
};

extern	void iapp_netbuf_init(void);
extern	struct iapp_netbuf * iapp_netbuf_alloc(struct shm_alloc_state *sm,
	    netbuf_alloc_type atype, size_t minsize);
extern	void iapp_netbuf_free(struct iapp_netbuf *);
extern	void iapp_netbuf_shutdown(void);

static inline const char *
iapp_netbuf_buf(struct iapp_netbuf *n)
{

	return (n->bufptr);
}

static inline char *
iapp_netbuf_buf_nonconst(struct iapp_netbuf *n)
{

	return (n->bufptr);
}

static inline size_t
iapp_netbuf_size(struct iapp_netbuf *n)
{

	return (n->buf_size);
}

#endif	/* __NETBUF_H__ */
