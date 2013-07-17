PROG=clt
SRCS=clt.c
CFLAGS+= -I${.CURDIR}/../../lib/libiapp/ -O0
LDFLAGS+= -L${.OBJDIR}/../../lib/libiapp/
LDADD=-lpthread -liapp
NO_MAN=1
DEBUG_FLAGS=-g

.include <bsd.prog.mk>
