PROG=clt
SRCS=clt.c
CFLAGS+= -I${.CURDIR}/../../lib/libiapp/
LDFLAGS+= -L${.OBJDIR}/../../lib/libiapp/
LDADD=-lpthread -liapp
NO_MAN=1

.include <bsd.prog.mk>
