SRCS=test.c fde.c
PROG=main
NO_MAN=1
LDADD=	-lpthread

.include <bsd.prog.mk>
