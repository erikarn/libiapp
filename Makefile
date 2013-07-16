SRCS=fde.c comm.c srv.c
PROG=main
NO_MAN=1
LDADD=	-lpthread
CFLAGS=	-O -g -Wall -Werror

.include <bsd.prog.mk>
